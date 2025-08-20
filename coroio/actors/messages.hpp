#pragma once

#include <memory>
#include <sstream>
#include <cstring>
#include <type_traits>
#include <functional>

#include "actorid.hpp"

namespace NNet {
namespace NActors {

struct TContextDeleter {
    TContextDeleter() = default;

    TContextDeleter(void (*func)(void*, void*), void* ctx)
        : Release(func)
        , Context(ctx)
    { }

    void operator()(void* ptr) {
        Release(Context, ptr);
    }

    void (*Release)(void*, void*) = nullptr;
    void* Context = nullptr;
};

template<typename TLambda>
struct TLambdaToContextDeleter {
    static_assert(
        std::is_empty_v<TLambda> || sizeof(TLambda) <= sizeof(void*),
        "Lambda must be stateless or capture only one pointer-sized value");

    // Stateless lambda to context deleter conversion
    template<typename F>
    static std::enable_if_t<std::is_empty_v<std::decay_t<F>>, TContextDeleter>
    Convert(F&& lambda) {
        auto func_ptr = +[](void* /*unused*/, void* ptr) {
            std::decay_t<F>{}(ptr);
        };
        return TContextDeleter{func_ptr, nullptr};
    }

    template<typename F>
    static std::enable_if_t<!std::is_empty_v<std::decay_t<F>>, TContextDeleter>
    Convert(F&& lambda) {
        void* captured_ptr = ExtractCapturedPointer(lambda);

        auto func_ptr = +[](void* ctx, void* ptr) {
            using LambdaType = std::decay_t<F>;
            alignas(LambdaType) char buffer[sizeof(LambdaType)];
            *reinterpret_cast<void**>(buffer) = ctx;
            auto* lambda_ptr = reinterpret_cast<LambdaType*>(buffer);
            (*lambda_ptr)(ptr);
        };

        return TContextDeleter{func_ptr, captured_ptr};
    }

private:
    template<typename F>
    static void* ExtractCapturedPointer(const F& lambda) {
        return *reinterpret_cast<void* const*>(&lambda);
    }
};

struct TBlobDeleter {
    TBlobDeleter() = default;

    template<typename TFunc>
    TBlobDeleter(TFunc&& func)
        : Release(TLambdaToContextDeleter<TFunc>::Convert(std::forward<TFunc>(func)))
    { }

    void operator()(void* ptr) {
        Release(ptr);
    }

    TContextDeleter Release;
};

struct TBlob {
    using TRawPtr = std::unique_ptr<void, TBlobDeleter>;
    //using TRawPtr = std::shared_ptr<void>;
    TRawPtr Data = {};
    uint32_t Size = 0;
    enum class PointerType {
        Near,  // Pointer to the object (for actor communication)
        Far    // Serialized representation (for network transmission)
    } Type;
};

template<typename T>
constexpr bool is_pod_v = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

template<typename T, typename = void>
struct has_data_members : std::true_type {};

template<typename T>
struct has_data_members<T, std::enable_if_t<
    std::is_empty_v<T> &&
    std::is_trivial_v<T> &&
    std::is_standard_layout_v<T>
>> : std::false_type {};

template<typename T>
constexpr bool has_data_members_v = has_data_members<T>::value;

template<typename T>
constexpr size_t sizeof_data() {
    if constexpr (has_data_members_v<T>) {
        return sizeof(T);
    } else {
        return 0;
    }
}

template<typename T, typename TAllocator, typename... Args>
typename std::enable_if_t<is_pod_v<T>, TBlob>
SerializePodNear(TAllocator& alloc, Args&&... args)
{
    constexpr uint32_t size = sizeof_data<T>();

    TBlob::TRawPtr rawPtr = nullptr;

    if constexpr (size > 0) {
        auto* data = alloc.Acquire(size);
        new (data) T(std::forward<Args>(args)...);

        rawPtr = TBlob::TRawPtr(data, TBlobDeleter{[&alloc](void* ptr) {
            alloc.Release(ptr);
        }});
    }

    return TBlob{std::move(rawPtr), size, TBlob::PointerType::Near};
}

template<typename T, typename TAllocator, typename... Args>
typename std::enable_if_t<!is_pod_v<T>, TBlob>
SerializeNonPodNear(TAllocator& alloc, Args&&... args)
{
    T* obj = new T(std::forward<Args>(args)...);

    auto rawPtr = TBlob::TRawPtr(obj, TBlobDeleter{[](void* ptr) {
        delete reinterpret_cast<T*>(ptr);
    }});

    return TBlob{std::move(rawPtr), sizeof(T), TBlob::PointerType::Near};
}

template<typename T, typename TAllocator, typename... Args>
TBlob SerializeNear(TAllocator& alloc, Args&&... args)
{
    if constexpr (is_pod_v<T>) {
        return SerializePodNear<T>(alloc, std::forward<Args>(args)...);
    } else {
        return SerializeNonPodNear<T>(alloc, std::forward<Args>(args)...);
    }
}

template<typename T>
void SerializeToStream(const T& obj, std::ostringstream& oss)
{
    static_assert(sizeof(T) == 0, "Serialization not implemented for this type");
}

template<typename T>
TBlob SerializeFar(TBlob blob)
{
    if constexpr (is_pod_v<T>) {
        blob.Type = TBlob::PointerType::Far;
        return blob; // For POD, far == near, just share the pointer
    } else {
        T* obj = reinterpret_cast<T*>(blob.Data.get());
        std::ostringstream oss;
        SerializeToStream(*obj, oss);
        void* data = ::operator new(oss.str().size());
        std::memcpy(data, oss.str().data(), oss.str().size());
        auto rawPtr = TBlob::TRawPtr(data, TBlobDeleter{[](void* ptr) {
            ::operator delete(ptr);
        }});
        return TBlob{std::move(rawPtr), static_cast<uint32_t>(oss.str().size()), TBlob::PointerType::Far};
    }
}

template<typename T, typename TStream, typename... Args>
void SerializeFarInplace(TStream& stream, TActorId sender, TActorId recipient, Args&&... args)
{
    if constexpr (is_pod_v<T>) {
        constexpr auto size = sizeof_data<T>() + sizeof(THeader);
        auto buf = stream.Acquire(size);
        char* p = static_cast<char*>(buf.data());
        new (p) THeader {sender, recipient, T::MessageId, sizeof_data<T>()};
        p += sizeof(THeader);
        new (p) T(std::forward<Args>(args)...);
        stream.Commit(size);
    } else {
        // TODO: optimize:
        // 1. estimate size of serialized T
        // 2. allocate enough space in stream
        // 3. serialize T to stream
        // 4. write header with size
        // 5. commit the whole buffer
        std::ostringstream oss;
        SerializeToStream(T(std::forward<Args>(args)...), oss);
        auto size = oss.str().size() + sizeof(THeader);
        auto buf = stream.Acquire(size);
        char* p = static_cast<char*>(buf.data());
        new (p) THeader {sender, recipient, T::MessageId, static_cast<uint32_t>(oss.str().size())};
        p += sizeof(THeader);
        std::memcpy(p, oss.str().data(), oss.str().size());
        stream.Commit(size);
    }
}

template<typename T>
auto DeserializeNear(const TBlob& blob) -> std::conditional_t<sizeof_data<T>() == 0, T, T&> {
    if constexpr (sizeof_data<T>() == 0) {
        return T{};
    } else {
        return *reinterpret_cast<T*>(blob.Data.get());
    }
}

template<typename T>
void DeserializeFromStream(T& obj, std::istringstream& iss) {
    static_assert(sizeof(T) == 0, "Deserialization not implemented for this type");
}

template<typename T>
auto DeserializeFar(const TBlob& blob) -> std::conditional_t<is_pod_v<T> && (sizeof_data<T>()>0), T&, T> {
    if constexpr (is_pod_v<T>) {
        return DeserializeNear<T>(blob);
    } else {
        std::istringstream iss(std::string(reinterpret_cast<const char*>(blob.Data.get()), blob.Size));
        T obj;
        DeserializeFromStream(obj, iss);
        return obj;
    }
}

} // namespace NActors
} // namespace NNet
