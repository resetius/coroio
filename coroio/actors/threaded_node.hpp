#pragma once

#include "node.hpp"

#include <coroio/mpsc_queue.hpp>
#include <coroio/notifier.hpp>

#include <atomic>
#include <mutex>
#include <vector>

namespace NNet {
namespace NActors {

#ifndef _WIN32

/// Producer lock of `TThreadedNode` for a single producer thread: locks nothing.
struct TNullLock {
    void lock() {
    }

    void unlock() {
    }
};

/**
 * @brief `INode` that writes to its socket on another thread.
 *
 * The node has two sides:
 *
 * - **Producer side** — the thread of the actor system. `Send`, `Acquire`, `Commit`
 *   and `Offset` put frames into a segment in memory, exactly as `TNode` puts them
 *   into its buffer. `Drain` hands the segment to the I/O side and takes an empty one.
 * - **I/O side** — a coroutine on `poller`, which belongs to another thread (see
 *   `TLoopThread`). It connects, writes the segments to the socket in the order of
 *   `Drain` calls and returns them for reuse. At most `MaxSpareSegments` written
 *   segments are kept, each with the memory of its buffer; the rest are deleted.
 *
 * The byte stream is the one `TNode` writes: reconnects with a 1 s back-off, a failed
 * write drops the segment being written, and a reconnected stream starts with a filler
 * frame once a frame with `PayloadAlignment` has been written (see `WriteFiller`).
 * Unlike `TNode`, committed bytes are not sent until the next `Drain`.
 *
 * `Send` defers the serialization of a Near object of a registered non-POD type (see
 * `TMessagesFactory::HasPayloadWriter`). The producer side takes the size of its payload
 * and frames it, so the frame keeps its place among the frames written around it and the
 * stream stays that of `TNode`. The I/O side writes the payload into the frame with
 * `SerializeTo` before the segment goes to the socket, and destroys the object there.
 * The object must therefore own, or keep alive, everything that `SerializeTo` reads.
 * A `SerializeTo` that throws fails its segment as a failed write does. POD payloads
 * and Far blobs are copied at once, as by `TNode`.
 *
 * `TLock` guards the producer side. With the default `TNullLock` it has one producer
 * thread. With a real lock (`std::mutex`) any thread may produce: `Send`, `StartConnect`
 * and `Drain` take the lock themselves, and a caller of `Offset`, `Acquire` and `Commit`
 * holds `ProducerLock()` across the three calls that write one frame.
 *
 * @code
 * TLoopThread<TDefaultPoller> io;
 * TResolver resolver(io.Poller());
 * auto& poller = io.Poller();
 * sys.AddNode(2, std::make_unique<TThreadedNode<TDefaultPoller>>(
 *     poller, factory, resolver,
 *     [&poller](const TAddress& addr) { return TDefaultPoller::TSocket(poller, addr.Domain()); },
 *     THostPort("host", 9002)));
 * io.Start();
 * ...
 * io.Stop();
 * io.Join();
 * // the actor system and its nodes are destroyed here, before io
 * @endcode
 *
 * @tparam TPoller Poller type of the I/O thread; determines the socket type.
 * @tparam TLock   BasicLockable guarding the producer side.
 *
 * @note Construct the node before the loop of `poller` starts and destroy it after
 *       that loop has stopped: both touch the poller. `resolver` and the sockets of
 *       `socketFactory` must belong to the same poller.
 * @note Not available on Windows: it needs `TNotifier`.
 */
template<typename TPoller, typename TLock = TNullLock>
class TThreadedNode final : public INode {
public:
    using TSocket = typename TPoller::TSocket;

    /// Written segments kept for reuse. One goes around while the I/O side keeps up;
    /// the others let a short backlog pass without allocating.
    static constexpr size_t MaxSpareSegments = 4;

    /**
     * @brief Constructs the node and its I/O coroutine; does not connect.
     *
     * @param poller        Poller of the I/O thread.
     * @param factory       Factory for serializing messages to their Far (wire) form; its payload
     *                      writers run on the I/O side.
     * @param resolver      DNS resolver on `poller`.
     * @param socketFactory Callable that creates a socket on `poller` given a resolved `TAddress`.
     * @param hostPort      Remote host and port to connect to.
     */
    TThreadedNode(TPoller& poller, TMessagesFactory& factory, TResolver& resolver, const std::function<TSocket(const TAddress&)>& socketFactory, const THostPort& hostPort)
        : Poller_(poller)
        , Factory_(factory)
        , Resolver_(resolver)
        , SocketFactory_(socketFactory)
        , HostPort_(hostPort)
        , Notifier_(poller)
        , Current_(std::make_unique<TSegment>(Segments_))
        , Server_(Serve())
    {
        Spare_.reserve(MaxSpareSegments);
    }

    /// Destroys the coroutine first: a pending write reads `Writing_` and a pending
    /// connect uses `Socket_`, see `~TNode`.
    ~TThreadedNode() {
        Server_ = TFuture<void>();
    }

    /// The lock of the producer side. Hold it across `Offset`, `Acquire` and `Commit` of one frame.
    TLock& ProducerLock() {
        return Lock_;
    }

    /// Segments alive now: the current one, the queued ones, the one being written and
    /// the spare ones. An idle node has at most `1 + MaxSpareSegments`. Any thread.
    size_t Segments() const {
        return Segments_.load(std::memory_order_relaxed);
    }

    // The framing is the one of TNode::Send; a deferred payload is left to the I/O side
    void Send(TEnvelope&& envelope) override {
        // Empty blobs skip the factory and its compile-time check of the id
        if (envelope.MessageId > MaxMessageId) [[unlikely]] {
            throw std::invalid_argument("message id " + std::to_string(envelope.MessageId)
                + " overlaps the padding bits of THeader::MessageId");
        }
        std::lock_guard guard(Lock_);
        auto blob = std::move(envelope.Blob);
        uint32_t padding = 0;
        uint32_t payload = 0;
        bool deferred = false;
        if (blob.Size > 0) {
            deferred = blob.Type == TBlob::PointerType::Near && Factory_.HasPayloadWriter(envelope.MessageId);
            if (deferred) {
                // Sized here: an invalid object throws to the sender, as with TNode
                payload = Factory_.PayloadSize(envelope.MessageId, blob);
            } else {
                blob = Factory_.SerializeFar(envelope.MessageId, std::move(blob));
                payload = blob.Size;
            }
            const size_t alignment = Factory_.PayloadAlignment(envelope.MessageId);
            if (alignment > 1) {
                padding = PayloadPadding(Offset(), alignment);
            }
        }
        THeader data{
            .Sender = envelope.Sender,
            .Recipient = envelope.Recipient,
            .MessageId = envelope.MessageId | (padding << HeaderPaddingShift),
            .Size = payload
        };
        const size_t size = sizeof(data) + padding + payload;
        auto buf = Acquire(size);
        std::memcpy(buf.data(), &data, sizeof(data));
        std::memset(buf.data() + sizeof(data), 0, padding);
        if (deferred) {
            Current_->Deferred.push_back(TDeferredPayload{
                .Offset = Current_->Size + sizeof(data) + padding,
                .Size = payload,
                .MessageId = envelope.MessageId,
                .Blob = std::move(blob),
            });
        } else if (payload > 0) {
            std::memcpy(buf.data() + sizeof(data) + padding, blob.Data.get(), payload);
        }
        Commit(size);
    }

    std::span<char> Acquire(size_t size) override {
        auto& segment = *Current_;
        if (segment.Size + size > segment.Data.size()) {
            segment.Data.resize(segment.Size + size);
        }
        return std::span<char>(segment.Data.data() + segment.Size, size);
    }

    void Commit(size_t size) override {
        Current_->Size += size;
    }

    uint64_t Offset() const override {
        if (!WroteAligned_) [[unlikely]] {
            // One atomic step against the CAS in Connect(): either that moved the base first
            // and this reads the new one, or it sees the bit and keeps the base. The word
            // carries all the two sides share here, so nothing is ordered by it: relaxed.
            Base_ = AlignState_.fetch_or(AlignedBit, std::memory_order_relaxed) >> 1;
            WroteAligned_ = true;
        }
        return SealedBytes_ + Current_->Size - Base_;
    }

    void StartConnect() override {
        std::lock_guard guard(Lock_);
        Activate();
    }

    void Drain() override {
        std::lock_guard guard(Lock_);
        Activate();
        if (Current_->Size == 0) {
            return;
        }
        auto next = TakeSpare();
        next->Size = 0;
        SealedBytes_ += Current_->Size;
        Current_.swap(next);
        if (Queue_.Push(std::move(next))) {
            Notifier_.Notify();
        }
    }

    THostPort GetHostPort() const override {
        return HostPort_;
    }

private:
    // A payload that the I/O side writes into its frame, at Offset of the segment
    struct TDeferredPayload {
        size_t Offset = 0;
        uint32_t Size = 0;
        TMessageId MessageId = 0;
        TBlob Blob;
    };

    // Frames committed between two Drain calls. The buffer keeps its memory while the
    // segment goes around: producer, Queue_, I/O side, Returned_, Spare_, producer.
    struct TSegment : TMpscQueueNode<TSegment> {
        // Counts itself: Segments() is what the node really holds, whatever path a segment took
        explicit TSegment(std::atomic<size_t>& live)
            : Live(live)
        {
            Live.fetch_add(1, std::memory_order_relaxed);
        }

        ~TSegment() {
            Live.fetch_sub(1, std::memory_order_relaxed);
        }

        TSegment(const TSegment&) = delete;
        TSegment& operator=(const TSegment&) = delete;

        std::atomic<size_t>& Live;
        std::vector<char> Data;
        size_t Size = 0;
        // In the order of their frames; empty again once the I/O side has written them
        std::vector<TDeferredPayload> Deferred;
    };

    // AlignState_ is the stream base shifted left by one, with this bit set once
    // a frame with PayloadAlignment has been written
    static constexpr uint64_t AlignedBit = 1;
    static constexpr size_t MaxBytesBeforeYield = 2 * 1024 * 1024;

    // Producer side
    void Activate() {
        if (!Active_) {
            Active_ = true;
            // Release pairs with the acquire load in Serve(): the notification alone
            // tells the I/O side to look, not what to see.
            ConnectRequested_.store(true, std::memory_order_release);
            Notifier_.Notify();
        }
    }

    // Producer side. The segment returned last comes first: a backlog that has passed
    // does not send every spare buffer around again.
    std::unique_ptr<TSegment> TakeSpare() {
        while (auto segment = Returned_.Pop()) {
            Spare_.push_back(std::move(segment));
        }
        if (Spare_.empty()) {
            return std::make_unique<TSegment>(Segments_);
        }
        auto segment = std::move(Spare_.back());
        Spare_.pop_back();
        SpareCount_.fetch_sub(1, std::memory_order_relaxed);
        return segment;
    }

    // I/O side. Only this side adds to SpareCount_, so the check keeps it at the cap.
    // Relaxed: the count orders nothing, the segment itself goes through Returned_.
    void Recycle(std::unique_ptr<TSegment> segment) {
        if (SpareCount_.load(std::memory_order_relaxed) < MaxSpareSegments) {
            SpareCount_.fetch_add(1, std::memory_order_relaxed);
            Returned_.Push(std::move(segment));
        }
    }

    // I/O side. The objects die here, also when a writer throws: a recycled segment
    // must not bring them back to the producer.
    void WritePayloads(TSegment& segment) {
        try {
            for (const auto& payload : segment.Deferred) {
                Factory_.WritePayload(payload.MessageId, payload.Blob,
                    std::span<char>(segment.Data.data() + payload.Offset, payload.Size));
            }
        } catch (...) {
            segment.Deferred.clear();
            throw;
        }
        segment.Deferred.clear();
    }

    // I/O side. A failed wait is logged and tried again after the back-off: the node
    // then looks for work once a second instead of never.
    TFuture<void> WaitForWork() {
        try {
            co_await Notifier_.Wait();
            co_return;
        } catch (const std::exception& ex) {
            std::cerr << "Error waiting for frames to " << HostPort_.ToString() << ": " << ex.what() << "\n";
        }
        co_await Poller_.Sleep(std::chrono::milliseconds(1000));
    }

    // I/O side
    TFuture<void> Serve() {
        // Like TNode, the node does not connect before StartConnect() or Drain()
        while (!ConnectRequested_.load(std::memory_order_acquire)) {
            co_await WaitForWork();
        }
        while (true) {
            if (!Connected_) {
                co_await Connect();
            }
            Writing_ = Queue_.Pop();
            if (!Writing_) {
                co_await WaitForWork();
                continue;
            }
            // Counted before the write: a failed write drops the rest of the segment, as in TNode
            SentBytes_ += Writing_->Size;
            try {
                WritePayloads(*Writing_);
                co_await TByteWriter(Socket_).Write(Writing_->Data.data(), Writing_->Size);
            } catch (const std::exception& ex) {
                std::cerr << "Error during draining: " << ex.what() << "\n";
                Connected_ = false;
            }
            WrittenBytes_ += Writing_->Size;
            Recycle(std::move(Writing_));
            // A write that does not block does not suspend: without this a busy producer
            // would keep the loop from its other coroutines. Not Yield(): resumed from
            // a timer, it is resumed again in the same pass, before the poller polls.
            if (WrittenBytes_ >= MaxBytesBeforeYield) {
                WrittenBytes_ = 0;
                co_await Poller_.Sleep(std::chrono::nanoseconds(1));
            }
        }
    }

    // I/O side
    TFuture<void> Connect() {
        // Without aligned frames the new stream starts at 0 with no filler, as TNode does:
        // the base moves to the first byte of the new stream. Once the bit is set the base
        // stays, and the filler below gives the queued frames their old offsets.
        uint64_t state = AlignState_.load(std::memory_order_relaxed);
        while (!(state & AlignedBit) && !AlignState_.compare_exchange_weak(state, SentBytes_ << 1,
            std::memory_order_relaxed, std::memory_order_relaxed))
        {
        }
        const uint64_t offset = (state & AlignedBit) ? SentBytes_ - (state >> 1) : 0;
        std::cout << "Connecting to " << HostPort_.ToString() << "\n";
        while (!Connected_) {
            try {
                TAddress addr = co_await HostPort_.Resolve(Resolver_);
                Socket_ = SocketFactory_(addr);
                // Small messages (acks) must not wait for Nagle and delayed ACK.
                Socket_.SetNoDelay();
                co_await Socket_.Connect(addr);
                char filler[MaxFillerSize];
                if (const size_t size = WriteFiller(filler, offset)) {
                    co_await TByteWriter(Socket_).Write(filler, size);
                }
                Connected_ = true;
                std::cout << "Connected to " << HostPort_.ToString() << "\n";
            } catch (const std::exception& ex) {
                std::cerr << "Error connecting to " << HostPort_.ToString() << ": " << ex.what() << "\n";
                Connected_ = false;
            }
            if (!Connected_) {
                co_await Poller_.Sleep(std::chrono::milliseconds(1000));
            }
        }
    }

    TPoller& Poller_;
    TMessagesFactory& Factory_;
    TResolver& Resolver_;
    std::function<TSocket(TAddress&)> SocketFactory_;
    THostPort HostPort_;

    // Shared by the two sides
    TNotifier<TPoller> Notifier_;
    // Declared before every owner of a segment, so it outlives the segments
    std::atomic<size_t> Segments_ = 0;
    TMpscQueue<TSegment> Queue_;
    TMpscQueue<TSegment> Returned_;
    std::atomic<bool> ConnectRequested_ = false;
    // Segments in Returned_ and Spare_
    std::atomic<size_t> SpareCount_ = 0;
    mutable std::atomic<uint64_t> AlignState_ = 0;

    // Producer side, under Lock_
    TLock Lock_;
    std::unique_ptr<TSegment> Current_;
    // Taken from Returned_, newest last
    std::vector<std::unique_ptr<TSegment>> Spare_;
    // Bytes of the segments handed to the I/O side
    uint64_t SealedBytes_ = 0;
    bool Active_ = false;
    // Set by Offset(), with the base it took from AlignState_
    mutable bool WroteAligned_ = false;
    mutable uint64_t Base_ = 0;

    // I/O side
    bool Connected_ = false;
    TSocket Socket_;
    std::unique_ptr<TSegment> Writing_;
    // Bytes of the segments taken from Queue_, written or dropped: the count the producer
    // side keeps in SealedBytes_, as the I/O side has seen it
    uint64_t SentBytes_ = 0;
    // Since the I/O coroutine last let the loop poll
    size_t WrittenBytes_ = 0;
    TFuture<void> Server_;
};

#endif // _WIN32

} // namespace NActors
} // namespace NNet
