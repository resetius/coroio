#pragma once

#include <memory>
#include <sstream>
#include <cstring>
#include <type_traits>

namespace NNet {
namespace NActors {

struct TBlob {
    using TRawPtr = std::shared_ptr<void>;
    TRawPtr Data = nullptr;
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

template<typename T, typename TAllocator>
typename std::enable_if_t<is_pod_v<T>, TBlob>
SerializePodNear(T&& message, TAllocator& alloc)
{
    constexpr uint32_t size = sizeof_data<T>();

    TBlob::TRawPtr rawPtr = nullptr;

    if constexpr (size > 0) {
        auto* data = alloc.Acquire(size);
        new (data) T(std::move(message));

        rawPtr = TBlob::TRawPtr(data, [&alloc](void* ptr) {
            alloc.Release(ptr);
        });
    }

    return TBlob{std::move(rawPtr), size, TBlob::PointerType::Near};
}

template<typename T, typename TAllocator>
typename std::enable_if_t<!is_pod_v<T>, TBlob>
SerializeNonPodNear(T&& message, TAllocator& alloc)
{
    T* obj = new T(std::forward<T>(message));

    auto rawPtr = TBlob::TRawPtr(obj, [](void* ptr) {
        delete reinterpret_cast<T*>(ptr);
    });

    return TBlob{std::move(rawPtr), sizeof(T), TBlob::PointerType::Near};
}

template<typename T, typename TAllocator>
TBlob SerializeNear(T&& message, TAllocator& alloc)
{
    if constexpr (is_pod_v<T>) {
        return SerializePodNear<T>(std::forward<T>(message), alloc);
    } else {
        return SerializeNonPodNear<T>(std::forward<T>(message), alloc);
    }
}

template<typename T>
void SerializeToStream(const T& obj, std::ostringstream& oss)
{
    static_assert(sizeof(T) == 0, "Serialization not implemented for this type");
}

template<typename T, typename TAllocator>
TBlob SerializeFar(TBlob blob, TAllocator& alloc)
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
        auto rawPtr = TBlob::TRawPtr(data, [](void* ptr) {
            ::operator delete(ptr);
        });
        return TBlob{std::move(rawPtr), static_cast<uint32_t>(oss.str().size()), TBlob::PointerType::Far};
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
auto DeserializeFar(const TBlob& blob) -> std::conditional_t<is_pod_v<T>, T&, T> {
    if constexpr (is_pod_v<T>) {
        return *reinterpret_cast<T*>(blob.Data.get());
    } else {
        std::istringstream iss(std::string(reinterpret_cast<const char*>(blob.Data.get()), blob.Size));
        T obj;
        DeserializeFromStream(obj, iss);
        return obj;
    }
}

} // namespace NActors
} // namespace NNet
