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

/**
 * @brief Opaque message payload with Near/Far duality.
 *
 * Wraps an arbitrary message value for routing through the actor system.
 * Two modes exist depending on whether the message stays in-process or
 * crosses a node boundary:
 *
 * - **Near** — a live C++ object pointer. Zero-copy for local delivery.
 *   POD types use a pool allocator; non-POD types use `new`.
 * - **Far** — a flat byte buffer for network transmission. POD: the Near
 *   pointer is reused as-is (bytes are already contiguous). Non-POD:
 *   produced by `SerializeToStream` / `SerializeFarInplace`.
 *
 * Move-only; owns the payload via `TRawPtr`.
 */
struct TBlob {
    using TRawPtr = std::unique_ptr<void, TBlobDeleter>;
    TRawPtr Data = {};   ///< Owned payload (Near: object ptr; Far: byte buffer)
    uint32_t Size = 0;   ///< Payload size in bytes (0 for empty/sentinel blobs)
    enum class PointerType {
        Near,  ///< Live object pointer — valid only within the same process
        Far    ///< Serialized byte buffer — safe to copy across the network
    } Type;
};

/**
 * @brief `true` if `T` qualifies for zero-overhead Near↔Far conversion.
 *
 * Requires `std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>`.
 * For POD types `SerializeFar` is a no-op and `SerializeFarInplace` writes
 * raw bytes directly into the network buffer without any extra allocation.
 */
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

/**
 * @brief Creates a Near blob containing a newly constructed `T`.
 *
 * POD types are placed into `alloc` (arena/pool); non-POD types use `new`.
 * The returned blob is the canonical form for local actor message delivery.
 *
 * @tparam T          Message type.
 * @tparam TAllocator Allocator with `Acquire(size_t) → void*` / `Release(void*)`.
 * @param alloc       Allocator used for POD objects (non-POD always use `new`).
 * @param args        Constructor arguments forwarded to `T`.
 */
template<typename T, typename TAllocator, typename... Args>
TBlob SerializeNear(TAllocator& alloc, Args&&... args)
{
    if constexpr (is_pod_v<T>) {
        return SerializePodNear<T>(alloc, std::forward<Args>(args)...);
    } else {
        return SerializeNonPodNear<T>(alloc, std::forward<Args>(args)...);
    }
}

/**
 * @brief User-provided serialization hook for non-POD message types.
 *
 * Specialize this function to enable non-POD messages to cross node boundaries.
 * The default (unspecialized) template triggers a `static_assert` at compile
 * time if a non-POD type is sent to a remote node without a specialization.
 *
 * Must be specialized alongside `DeserializeFromStream<T>`.
 *
 * @code
 * template<>
 * void SerializeToStream<MyMsg>(const MyMsg& msg, std::ostringstream& oss) {
 *     oss << msg.field1 << ' ' << msg.field2;
 * }
 * @endcode
 */
template<typename T>
void SerializeToStream(const T& obj, std::ostringstream& oss)
{
    static_assert(sizeof(T) == 0, "Serialization not implemented for this type");
}

/**
 * @brief Converts a Near blob to a Far (wire-safe) blob.
 *
 * POD: marks the existing blob as Far without copying — raw bytes are already
 * contiguous and safe to transmit. Non-POD: calls `SerializeToStream<T>` and
 * copies the result into a new heap buffer.
 *
 * @param blob Near blob to convert (moved in).
 * @return Far blob ready for sending over the network.
 */
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

/**
 * @brief Serializes a message directly into a stream buffer with a `THeader` prefix.
 *
 * Used by `TActorSystem::Send` when routing to a remote node. Writes
 * `THeader + payload` into the node's outbound buffer in one contiguous
 * region — no intermediate allocation.
 *
 * POD: copies raw bytes. Non-POD: calls `SerializeToStream<T>`.
 *
 * @tparam T       Message type.
 * @tparam TStream Stream with `Acquire(size_t) → span<char>` / `Commit(size_t)`.
 * @param stream    Output stream (e.g. `TNode`'s outbound buffer).
 * @param sender    Actor ID written into the `THeader`.
 * @param recipient Actor ID written into the `THeader`.
 * @param args      Constructor arguments forwarded to `T`.
 */
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

/**
 * @brief Returns a reference (or value for empty types) to `T` inside a Near blob.
 *
 * For types with data members: returns `T&` — zero-copy cast from `blob.Data.get()`.
 * For empty types (no data members): returns a default-constructed `T` by value
 * (no storage is allocated for empty messages).
 *
 * @param blob Near blob to read from.
 * @return `T&` if `T` has data members; `T` by value if `T` is empty.
 */
template<typename T>
auto DeserializeNear(const TBlob& blob) -> std::conditional_t<sizeof_data<T>() == 0, T, T&> {
    if constexpr (sizeof_data<T>() == 0) {
        return T{};
    } else {
        return *reinterpret_cast<T*>(blob.Data.get());
    }
}

/**
 * @brief User-provided deserialization hook for non-POD message types.
 *
 * Specialize alongside `SerializeToStream<T>` to enable non-POD messages to
 * cross node boundaries. The default (unspecialized) template triggers a
 * `static_assert` at compile time if called without a specialization.
 *
 * @code
 * template<>
 * void DeserializeFromStream<MyMsg>(MyMsg& msg, std::istringstream& iss) {
 *     iss >> msg.field1 >> msg.field2;
 * }
 * @endcode
 */
template<typename T>
void DeserializeFromStream(T& obj, std::istringstream& iss) {
    static_assert(sizeof(T) == 0, "Deserialization not implemented for this type");
}

/**
 * @brief Deserializes a Far blob back into a `T`.
 *
 * POD with data members: returns `T&` — same as `DeserializeNear` (bytes are
 * already valid C++ objects). Non-POD or empty POD: calls
 * `DeserializeFromStream<T>` and returns `T` by value.
 *
 * @param blob Far blob received from the network.
 * @return `T&` for POD types with data; `T` by value for non-POD or empty types.
 */
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
