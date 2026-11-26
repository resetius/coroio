#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <coroio/all.hpp>
#include <coroio/actors/actorsystem.hpp>
#include <coroio/actors/threaded_node.hpp>
#include <coroio/loop_thread.hpp>

#include "testlib.h"

using namespace NNet;
using namespace NNet::NActors;

#ifndef _WIN32

struct TText {
    static constexpr TMessageId MessageId = 50;
    std::string Value;
};

struct TAlignedPod {
    static constexpr TMessageId MessageId = 51;
    static constexpr size_t PayloadAlignment = 16;
    uint64_t Values[3];
};

struct TAlignedBytes {
    static constexpr TMessageId MessageId = 52;
    static constexpr size_t PayloadAlignment = 16;
    std::string Value;
};

struct TPlainPod {
    static constexpr TMessageId MessageId = 53;
    uint64_t Values[2];
};

struct TPing {
    static constexpr TMessageId MessageId = 54;
    static constexpr size_t PayloadAlignment = 16;
    uint64_t Seq;
    uint64_t Check;
};

// Where the payloads of TProbe are sized, written and destroyed
struct TProbeLog {
    void Add(std::vector<std::thread::id>& to) {
        std::lock_guard guard(Mutex);
        to.push_back(std::this_thread::get_id());
    }

    std::mutex Mutex;
    std::vector<std::thread::id> Sized;
    std::vector<std::thread::id> Written;
    std::vector<std::thread::id> Destroyed;
};

// A non-POD aligned message: TThreadedNode defers its payload once it has a payload writer
struct TProbe {
    static constexpr TMessageId MessageId = 56;
    static constexpr size_t PayloadAlignment = 16;

    TProbe(TProbeLog* log, std::string value, bool sizeFails = false)
        : Log(log)
        , Value(std::move(value))
        , SizeFails(sizeFails)
    { }

    ~TProbe() {
        Log->Add(Log->Destroyed);
    }

    TProbe(const TProbe&) = delete;
    TProbe& operator=(const TProbe&) = delete;

    TProbeLog* Log;
    std::string Value;
    bool SizeFails;
};

// Registered while a node runs; never sent
template<TMessageId Id>
struct TLate {
    static constexpr TMessageId MessageId = Id;
    std::string Value;
};

namespace NNet::NActors {

template<>
size_t SerializedSize<TProbe>(const TProbe& obj) {
    obj.Log->Add(obj.Log->Sized);
    if (obj.SizeFails) {
        throw std::runtime_error("probe: no size");
    }
    return obj.Value.size();
}

template<>
void SerializeTo<TProbe>(const TProbe& obj, std::span<char> out) {
    obj.Log->Add(obj.Log->Written);
    std::memcpy(out.data(), obj.Value.data(), out.size());
}

template<>
size_t SerializedSize<TLate<1000>>(const TLate<1000>& obj) {
    return obj.Value.size();
}

template<>
void SerializeTo<TLate<1000>>(const TLate<1000>& obj, std::span<char> out) {
    std::memcpy(out.data(), obj.Value.data(), out.size());
}

template<>
size_t SerializedSize<TLate<2000>>(const TLate<2000>& obj) {
    return obj.Value.size();
}

template<>
void SerializeTo<TLate<2000>>(const TLate<2000>& obj, std::span<char> out) {
    std::memcpy(out.data(), obj.Value.data(), out.size());
}

template<>
size_t SerializedSize<TLate<3000>>(const TLate<3000>& obj) {
    return obj.Value.size();
}

template<>
void SerializeTo<TLate<3000>>(const TLate<3000>& obj, std::span<char> out) {
    std::memcpy(out.data(), obj.Value.data(), out.size());
}

template<>
size_t SerializedSize<TText>(const TText& obj) {
    return obj.Value.size();
}

template<>
void SerializeTo<TText>(const TText& obj, std::span<char> out) {
    std::memcpy(out.data(), obj.Value.data(), out.size());
}

template<>
void DeserializeFrom<TText>(TText& obj, TBlob& blob) {
    obj.Value.assign(static_cast<const char*>(blob.Data.get()), blob.Size);
}

template<>
size_t SerializedSize<TAlignedBytes>(const TAlignedBytes& obj) {
    return obj.Value.size();
}

template<>
void SerializeTo<TAlignedBytes>(const TAlignedBytes& obj, std::span<char> out) {
    std::memcpy(out.data(), obj.Value.data(), out.size());
}

} // namespace NNet::NActors

namespace {

const TActorId Sender(1, 1, 1);
const TActorId Recipient(2, 2, 2);
constexpr TMessageId EmptyMessageId = 55;
const std::string EndText = "END-OF-STREAM";

struct TAllocator {
    void* Acquire(size_t size) {
        return ::operator new(size);
    }

    void Release(void* ptr) {
        ::operator delete(ptr);
    }
};

struct TVectorStream {
    std::string Data;

    std::span<char> Acquire(size_t size) {
        const size_t committed = Data.size();
        Data.resize(committed + size);
        return {Data.data() + committed, size};
    }

    void Commit(size_t) {
    }

    uint64_t Offset() const {
        return Data.size();
    }
};

// The bytes of a TText frame: they do not depend on the stream offset
std::string TextFrame(const std::string& text) {
    TVectorStream stream;
    SerializeFarInplace<TText>(stream, Sender, Recipient, text);
    return stream.Data;
}

// The Near objects of the two non-POD types are serialized on the I/O thread
void RegisterTypes(TMessagesFactory& factory) {
    factory.RegisterPayloadWriter<TText>();
    factory.RegisterSerializer<TAlignedPod>();
    factory.RegisterPayloadWriter<TAlignedBytes>();
    factory.RegisterSerializer<TPlainPod>();
    factory.RegisterSerializer<TPing>();
}

template<typename TPoller, typename TPredicate>
bool Pump(TLoop<TPoller>& loop, TPredicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
    const auto deadline = TClock::now() + timeout;
    while (!predicate()) {
        if (TClock::now() > deadline) {
            return false;
        }
        loop.Step();
    }
    return true;
}

template<typename TPoller>
void PumpFor(TLoop<TPoller>& loop, std::chrono::milliseconds duration) {
    Pump(loop, [] { return false; }, duration);
}

/**
 * The remote side of a node, on the loop of the test thread. Accepts every connection
 * and reads it up to its limit; keeps the bytes or passes them to OnData.
 */
template<typename TPoller>
class TPeer {
public:
    using TSocket = typename TPoller::TSocket;
    static constexpr size_t NoLimit = static_cast<size_t>(-1);

    TPeer(TPoller& poller, int port)
        : Listener_(poller, TAddress{"127.0.0.1", port}.Domain())
    {
        Listener_.Bind(TAddress{"127.0.0.1", port});
        Listener_.Listen();
        Acceptor_ = Accept();
    }

    ~TPeer() {
        Acceptor_ = TFuture<void>();
        Connections_.clear();
    }

    /// Bytes to read from the connection number `index`, set before it is accepted
    void SetReadLimit(size_t index, size_t limit) {
        if (Limits_.size() <= index) {
            Limits_.resize(index + 1, NoLimit);
        }
        Limits_[index] = limit;
    }

    size_t Accepted() const {
        return Connections_.size();
    }

    const std::string& Bytes(size_t index) const {
        return Connections_[index]->Bytes;
    }

    /// An abortive close: the next writes of the node fail
    void Reset(size_t index) {
        auto& connection = *Connections_[index];
        connection.Reader = TFuture<void>();
        linger reset{};
        reset.l_onoff = 1;
        reset.l_linger = 0;
        setsockopt(connection.Socket.Fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        connection.Socket.Close();
    }

    std::function<void(size_t index, const char* data, size_t size)> OnData;

private:
    struct TConnection {
        TSocket Socket;
        size_t Index = 0;
        size_t Limit = NoLimit;
        size_t Received = 0;
        std::string Bytes;
        std::vector<char> Buffer = std::vector<char>(256 * 1024);
        TFuture<void> Reader;
    };

    TFuture<void> Accept() {
        while (true) {
            auto client = co_await Listener_.Accept();
            if (!client) {
                continue;
            }
            const size_t index = Connections_.size();
            auto connection = std::make_unique<TConnection>();
            connection->Socket = std::move(*client);
            connection->Index = index;
            connection->Limit = index < Limits_.size() ? Limits_[index] : NoLimit;
            Connections_.push_back(std::move(connection));
            Connections_.back()->Reader = Read(*Connections_.back());
        }
    }

    TFuture<void> Read(TConnection& connection) {
        while (connection.Received < connection.Limit) {
            const size_t want = std::min(connection.Buffer.size(), connection.Limit - connection.Received);
            const auto size = co_await connection.Socket.ReadSome(connection.Buffer.data(), want);
            if (size < 0) {
                continue;
            }
            if (size == 0) {
                break;
            }
            connection.Received += size;
            if (OnData) {
                OnData(connection.Index, connection.Buffer.data(), size);
            } else {
                connection.Bytes.append(connection.Buffer.data(), size);
            }
        }
    }

    TSocket Listener_;
    std::vector<size_t> Limits_;
    std::vector<std::unique_ptr<TConnection>> Connections_;
    TFuture<void> Acceptor_;
};

template<typename TPoller>
std::function<typename TPoller::TSocket(const TAddress&)> SocketFactory(TPoller& poller) {
    return [&poller](const TAddress& addr) {
        return typename TPoller::TSocket(poller, addr.Domain());
    };
}

// TNode on the loop of the test thread: the reference
template<typename TPoller>
struct TPlainSetup {
    TPlainSetup(TLoop<TPoller>& loop, TMessagesFactory& factory, int port)
        : Resolver(TAddress{"127.0.0.1", 53}, loop.Poller())
        , Node(loop.Poller(), factory, Resolver, SocketFactory(loop.Poller()), THostPort("127.0.0.1", port))
    { }

    void Stop() {
    }

    TResolver Resolver;
    TNode<TPoller> Node;
};

// TThreadedNode on its own I/O thread; the test thread is the producer
template<typename TPoller, typename TLock = TNullLock>
struct TThreadedSetup {
    TThreadedSetup(TLoop<TPoller>&, TMessagesFactory& factory, int port)
        : Resolver(TAddress{"127.0.0.1", 53}, Io.Poller())
        , Node(Io.Poller(), factory, Resolver, SocketFactory(Io.Poller()), THostPort("127.0.0.1", port))
    {
        Io.Start();
    }

    // The loop stops before the node and the resolver are destroyed
    ~TThreadedSetup() {
        Stop();
    }

    void Stop() {
        Io.Stop();
        Io.Join();
    }

    TLoopThread<TPoller> Io;
    TResolver Resolver;
    TThreadedNode<TPoller, TLock> Node;
};

// A string of 0..40 bytes, an aligned POD, aligned bytes of 1..60 bytes, a plain POD
template<typename TStream>
void WriteRound(TStream& stream, std::mt19937& rng, int r) {
    SerializeFarInplace<TText>(stream, Sender, Recipient, std::string(rng() % 41, static_cast<char>('a' + r % 26)));
    SerializeFarInplace<TAlignedPod>(stream, Sender, Recipient, TAlignedPod{{uint64_t(r), uint64_t(r) * 3, uint64_t(r) * 7}});
    SerializeFarInplace<TAlignedBytes>(stream, Sender, Recipient, TAlignedBytes{std::string(1 + rng() % 60, static_cast<char>('A' + r % 26))});
    SerializeFarInplace<TPlainPod>(stream, Sender, Recipient, TPlainPod{{uint64_t(r), uint64_t(r) + 1}});
}

// The same kinds of frames through INode::Send, and a frame without a payload
void SendRound(INode& node, TAllocator& alloc, std::mt19937& rng, int r) {
    auto send = [&](TMessageId messageId, TBlob blob) {
        node.Send(TEnvelope{.Sender = Sender, .Recipient = Recipient, .MessageId = messageId, .Blob = std::move(blob)});
    };
    send(TAlignedBytes::MessageId, SerializeNear<TAlignedBytes>(alloc, std::string(1 + rng() % 60, static_cast<char>('0' + r % 10))));
    send(TPlainPod::MessageId, SerializeNear<TPlainPod>(alloc, TPlainPod{{uint64_t(r), 5}}));
    send(EmptyMessageId, {});
    send(TText::MessageId, SerializeNear<TText>(alloc, std::string(rng() % 41, static_cast<char>('k' + r % 10))));
    send(TAlignedPod::MessageId, SerializeNear<TAlignedPod>(alloc, TAlignedPod{{uint64_t(r), 1, 2}}));
}

template<typename TStream>
void WriteEnd(TStream& stream) {
    SerializeFarInplace<TText>(stream, Sender, Recipient, EndText);
}

bool Ended(const std::string& bytes) {
    return bytes.ends_with(TextFrame(EndText));
}

bool IsAlignedType(TMessageId messageId) {
    return messageId == TAlignedPod::MessageId || messageId == TAlignedBytes::MessageId || messageId == TPing::MessageId;
}

// Reads the stream as an inbound connection with TZeroCopyEnvelopeReaderV2 does: every
// payload of an aligned type must be 16-aligned in memory, and the END frame comes last.
void CheckStream(const std::string& bytes, size_t minFrames) {
    TZeroCopyEnvelopeReaderV2 reader(256, 8);
    size_t frames = 0;
    bool ended = false;
    size_t pos = 0;
    while (pos < bytes.size()) {
        auto buf = reader.Acquire(64);
        const size_t n = std::min(buf.size(), bytes.size() - pos);
        std::memcpy(buf.data(), bytes.data() + pos, n);
        reader.Commit(n);
        pos += n;
        while (auto envelope = reader.Pop()) {
            ASSERT_FALSE(ended);
            ++frames;
            ASSERT_TRUE(envelope->MessageId >= TText::MessageId && envelope->MessageId <= EmptyMessageId);
            if (IsAlignedType(envelope->MessageId)) {
                ASSERT_TRUE(envelope->Blob.Size > 0);
                ASSERT_EQ(reinterpret_cast<uintptr_t>(envelope->Blob.Data.get()) % 16, 0u);
            }
            ended = envelope->MessageId == TText::MessageId && envelope->Blob.Size == EndText.size()
                && std::memcmp(envelope->Blob.Data.get(), EndText.data(), EndText.size()) == 0;
        }
    }
    ASSERT_TRUE(ended);
    ASSERT_TRUE(frames >= minFrames);
}

// One connection: every kind of frame, with Drain() at random points
template<typename TPoller, typename TSetup>
void RunSingleStream(std::string& stream) {
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TMessagesFactory factory;
    RegisterTypes(factory);
    const int port = GetFreePort();
    TPeer<TPoller> peer(loop.Poller(), port);
    TSetup setup(loop, factory, port);
    INode& node = setup.Node;

    TAllocator alloc;
    std::mt19937 rng(17);
    for (int batch = 0; batch < 30; ++batch) {
        // A reservation that is not committed is reused by the next Acquire
        node.Acquire(100 + batch);
        const int rounds = 1 + rng() % 5;
        for (int r = 0; r < rounds; ++r) {
            WriteRound(node, rng, batch * 10 + r);
            if (rng() % 3 == 0) {
                node.Drain();
            }
            SendRound(node, alloc, rng, batch * 10 + r);
        }
        node.Drain();
        // Some batches reach the socket one by one and some together
        if (rng() % 2) {
            PumpFor(loop, std::chrono::milliseconds(2));
        }
    }
    WriteEnd(node);
    node.Drain();
    ASSERT_TRUE(Pump(loop, [&] { return peer.Accepted() == 1 && Ended(peer.Bytes(0)); }));
    setup.Stop();
    ASSERT_EQ(peer.Accepted(), 1u);
    stream = peer.Bytes(0);
}

// A write blocked by a peer that does not read, aligned frames queued behind it, a reset.
// The second connection carries a filler, the queued frames and the frames written later.
template<typename TPoller, typename TSetup>
void RunAlignedReconnect(std::string& stream) {
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TMessagesFactory factory;
    RegisterTypes(factory);
    const int port = GetFreePort();
    TPeer<TPoller> peer(loop.Poller(), port);
    peer.SetReadLimit(0, 0);
    TSetup setup(loop, factory, port);
    INode& node = setup.Node;

    // A frame too large for the socket buffers; its size is not a multiple of 16
    const uint32_t large = (64 << 20) + 3;
    auto buf = node.Acquire(sizeof(THeader) + large);
    const THeader header{Sender, Recipient, TText::MessageId, large};
    std::memcpy(buf.data(), &header, sizeof(header));
    node.Commit(sizeof(THeader) + large);
    node.Drain();
    ASSERT_TRUE(Pump(loop, [&] { return peer.Accepted() == 1; }));
    PumpFor(loop, std::chrono::milliseconds(300));

    // Queued behind the blocked write, padded for the old stream
    std::mt19937 rng(21);
    TAllocator alloc;
    for (int r = 0; r < 20; ++r) {
        WriteRound(node, rng, r);
        SendRound(node, alloc, rng, r);
    }
    node.Drain();
    PumpFor(loop, std::chrono::milliseconds(50));

    peer.Reset(0);
    ASSERT_TRUE(Pump(loop, [&] { return peer.Accepted() == 2; }));
    for (int r = 20; r < 30; ++r) {
        WriteRound(node, rng, r);
        SendRound(node, alloc, rng, r);
    }
    WriteEnd(node);
    node.Drain();
    ASSERT_TRUE(Pump(loop, [&] { return Ended(peer.Bytes(1)); }));
    setup.Stop();
    ASSERT_EQ(peer.Accepted(), 2u);
    stream = peer.Bytes(1);
}

// A node without aligned frames reconnects, and only then writes its first aligned frame.
// The second connection starts with `probes` 32-byte frames, `tail` is what follows them.
template<typename TPoller, typename TSetup>
void RunUnalignedReconnect(std::string& stream, size_t& probes, std::string& tail) {
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TMessagesFactory factory;
    RegisterTypes(factory);
    const int port = GetFreePort();
    TPeer<TPoller> peer(loop.Poller(), port);
    // A 27-byte frame, then 32-byte ones: the old stream offset stays 11 mod 16
    const std::string head = TextFrame("abc");
    const std::string probe = TextFrame("12345678");
    ASSERT_EQ(head.size(), 27u);
    ASSERT_EQ(probe.size(), 32u);
    peer.SetReadLimit(0, head.size());
    TSetup setup(loop, factory, port);
    INode& node = setup.Node;

    SerializeFarInplace<TText>(node, Sender, Recipient, std::string("abc"));
    node.Drain();
    ASSERT_TRUE(Pump(loop, [&] { return peer.Accepted() == 1 && peer.Bytes(0).size() == head.size(); }));
    ASSERT_TRUE(peer.Bytes(0) == head);
    peer.Reset(0);

    // The node sees the reset when a write fails; the frames written until then are lost
    const auto deadline = TClock::now() + std::chrono::seconds(60);
    while (!(peer.Accepted() == 2 && peer.Bytes(1).size() >= probe.size())) {
        ASSERT_TRUE(TClock::now() < deadline);
        SerializeFarInplace<TText>(node, Sender, Recipient, std::string("12345678"));
        node.Drain();
        PumpFor(loop, std::chrono::milliseconds(10));
    }

    std::mt19937 rng(23);
    TAllocator alloc;
    for (int r = 0; r < 10; ++r) {
        WriteRound(node, rng, r);
        SendRound(node, alloc, rng, r);
    }
    WriteEnd(node);
    node.Drain();
    ASSERT_TRUE(Pump(loop, [&] { return Ended(peer.Bytes(1)); }));
    setup.Stop();
    ASSERT_EQ(peer.Accepted(), 2u);
    stream = peer.Bytes(1);
    probes = 0;
    while (stream.compare(probes * probe.size(), probe.size(), probe) == 0) {
        ++probes;
    }
    tail = stream.substr(probes * probe.size());
}

// The payload of a stress frame: the producer, its sequence number and a pattern
std::string StressPayload(uint32_t producer, uint32_t seq, size_t size) {
    std::string payload(2 * sizeof(uint32_t) + size, '\0');
    std::memcpy(payload.data(), &producer, sizeof(producer));
    std::memcpy(payload.data() + sizeof(producer), &seq, sizeof(seq));
    for (size_t i = 0; i < size; ++i) {
        payload[2 * sizeof(uint32_t) + i] = static_cast<char>(seq * 31 + i * 7 + producer);
    }
    return payload;
}

// Reads the connections of a peer like InboundConnection and checks the stress frames.
// Used by the test thread only.
class TStressChecker {
public:
    TStressChecker(size_t producers, bool lossless)
        : Next_(producers, 0)
        , Lossless_(lossless)
    { }

    void Feed(size_t index, const char* data, size_t size) {
        if (Readers_.size() <= index) {
            Readers_.resize(index + 1);
        }
        if (!Readers_[index]) {
            Readers_[index] = std::make_unique<TZeroCopyEnvelopeReaderV2>();
        }
        auto& reader = *Readers_[index];
        while (size > 0) {
            auto buf = reader.Acquire(size);
            const size_t n = std::min(buf.size(), size);
            std::memcpy(buf.data(), data, n);
            reader.Commit(n);
            data += n;
            size -= n;
            while (auto envelope = reader.Pop()) {
                Check(*envelope);
            }
        }
    }

    size_t Frames = 0;
    size_t Ends = 0;
    uint32_t MaxSeq = 0;
    std::string Error;

private:
    void Check(const TEnvelope& envelope) {
        if (!Error.empty()) {
            return;
        }
        ++Frames;
        const char* data = static_cast<const char*>(envelope.Blob.Data.get());
        if (envelope.MessageId == TText::MessageId && envelope.Blob.Size == EndText.size()
            && std::memcmp(data, EndText.data(), EndText.size()) == 0)
        {
            ++Ends;
            return;
        }
        if (envelope.MessageId != TText::MessageId && envelope.MessageId != TAlignedBytes::MessageId) {
            Error = "unexpected message id " + std::to_string(envelope.MessageId);
            return;
        }
        if (envelope.MessageId == TAlignedBytes::MessageId && reinterpret_cast<uintptr_t>(data) % 16 != 0) {
            Error = "payload is not aligned";
            return;
        }
        uint32_t producer = 0;
        uint32_t seq = 0;
        if (envelope.Blob.Size < 2 * sizeof(uint32_t)) {
            Error = "short payload";
            return;
        }
        std::memcpy(&producer, data, sizeof(producer));
        std::memcpy(&seq, data + sizeof(producer), sizeof(seq));
        if (producer >= Next_.size() || envelope.Sender != TActorId(1, producer + 1, 1)) {
            Error = "unknown producer";
            return;
        }
        if (Lossless_ ? seq != Next_[producer] : seq < Next_[producer]) {
            Error = "producer " + std::to_string(producer) + ": frame " + std::to_string(seq)
                + " after " + std::to_string(Next_[producer]);
            return;
        }
        Next_[producer] = seq + 1;
        MaxSeq = std::max(MaxSeq, seq);
        if (StressPayload(producer, seq, envelope.Blob.Size - 2 * sizeof(uint32_t)).compare(0, std::string::npos, data, envelope.Blob.Size) != 0) {
            Error = "payload of frame " + std::to_string(seq) + " is damaged";
        }
    }

    std::vector<std::unique_ptr<TZeroCopyEnvelopeReaderV2>> Readers_;
    std::vector<uint32_t> Next_;
    bool Lossless_;
};

// One stress frame through one of the two ways into the node
template<typename TNodeType>
void WriteStressFrame(TNodeType& node, TAllocator& alloc, std::mt19937& rng, uint32_t producer, uint32_t seq) {
    const size_t size = rng() % 10 == 0 ? 1 + rng() % (256 * 1024) : rng() % 200;
    std::string payload = StressPayload(producer, seq, size);
    const TActorId sender(1, producer + 1, 1);
    const bool aligned = rng() % 2;
    if (rng() % 2) {
        // What TActorSystem::Send<T> does, under the lock of the node
        std::lock_guard guard(node.ProducerLock());
        if (aligned) {
            SerializeFarInplace<TAlignedBytes>(node, sender, Recipient, TAlignedBytes{std::move(payload)});
        } else {
            SerializeFarInplace<TText>(node, sender, Recipient, std::move(payload));
        }
    } else {
        node.Send(TEnvelope{
            .Sender = sender,
            .Recipient = Recipient,
            .MessageId = aligned ? TAlignedBytes::MessageId : TText::MessageId,
            .Blob = aligned ? SerializeNear<TAlignedBytes>(alloc, std::move(payload)) : SerializeNear<TText>(alloc, std::move(payload)),
        });
    }
    if (rng() % 4 == 0) {
        node.Drain();
    }
}

void SendProbe(INode& node, TAllocator& alloc, TProbeLog& log, std::string value, bool sizeFails = false) {
    node.Send(TEnvelope{
        .Sender = Sender,
        .Recipient = Recipient,
        .MessageId = TProbe::MessageId,
        .Blob = SerializeNear<TProbe>(alloc, &log, std::move(value), sizeFails),
    });
}

struct TProbeRun {
    // TProbe gets a payload writer; else only a serializer
    bool Writer = true;
    // Registers new types with payload writers during the run, on the producer thread
    bool LateTypes = false;
};

// Probes between small frames that are framed at once, as a TFinish after the last chunk;
// every 50th round also a probe that cannot be sized. `thrown` counts the rejected sends.
template<typename TPoller, typename TSetup>
void RunProbes(std::string& stream, TProbeLog& log, int& thrown, TProbeRun run = {}) {
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TMessagesFactory factory;
    RegisterTypes(factory);
    if (run.Writer) {
        factory.RegisterPayloadWriter<TProbe>();
    } else {
        factory.RegisterSerializer<TProbe>();
    }
    const int port = GetFreePort();
    TPeer<TPoller> peer(loop.Poller(), port);
    TSetup setup(loop, factory, port);
    INode& node = setup.Node;

    TAllocator alloc;
    std::mt19937 rng(31);
    for (int r = 0; r < 200; ++r) {
        SerializeFarInplace<TPlainPod>(node, Sender, Recipient, TPlainPod{{uint64_t(r), 1}});
        const size_t size = r % 10 == 9 ? 64 * 1024 + rng() % 1000 : rng() % 100;
        SendProbe(node, alloc, log, std::string(size, static_cast<char>('a' + r % 26)));
        SerializeFarInplace<TAlignedPod>(node, Sender, Recipient, TAlignedPod{{uint64_t(r), 2, 3}});
        if (r % 50 == 25) {
            try {
                SendProbe(node, alloc, log, "bad", true);
            } catch (const std::runtime_error&) {
                ++thrown;
            }
        }
        // Each one grows the vector of writers while the I/O side writes payloads
        if (run.LateTypes && r == 60) {
            factory.RegisterPayloadWriter<TLate<1000>>();
        }
        if (run.LateTypes && r == 120) {
            factory.RegisterPayloadWriter<TLate<2000>>();
        }
        if (run.LateTypes && r == 180) {
            factory.RegisterPayloadWriter<TLate<3000>>();
        }
        if (rng() % 4 == 0) {
            node.Drain();
        }
    }
    WriteEnd(node);
    node.Drain();
    ASSERT_TRUE(Pump(loop, [&] { return peer.Accepted() == 1 && Ended(peer.Bytes(0)); }));
    setup.Stop();
    ASSERT_EQ(peer.Accepted(), 1u);
    stream = peer.Bytes(0);
}

} // namespace

// TThreadedNode writes the bytes TNode writes for the same calls.
template<typename TPoller>
void test_threaded_node_stream_equals_node() {
    std::string plain;
    std::string threaded;
    ASSERT_NO_FATAL_FAILURE((RunSingleStream<TPoller, TPlainSetup<TPoller>>(plain)));
    ASSERT_NO_FATAL_FAILURE((RunSingleStream<TPoller, TThreadedSetup<TPoller>>(threaded)));
    ASSERT_TRUE(plain == threaded);
    ASSERT_NO_FATAL_FAILURE(CheckStream(threaded, 300));
}

// The same with the producer side under a mutex
template<typename TPoller>
void test_threaded_node_stream_equals_node_with_mutex() {
    std::string plain;
    std::string threaded;
    ASSERT_NO_FATAL_FAILURE((RunSingleStream<TPoller, TPlainSetup<TPoller>>(plain)));
    ASSERT_NO_FATAL_FAILURE((RunSingleStream<TPoller, TThreadedSetup<TPoller, std::mutex>>(threaded)));
    ASSERT_TRUE(plain == threaded);
}

template<typename TPoller>
void test_threaded_node_filler_on_reconnect_equals_node() {
    std::string plain;
    std::string threaded;
    ASSERT_NO_FATAL_FAILURE((RunAlignedReconnect<TPoller, TPlainSetup<TPoller>>(plain)));
    ASSERT_NO_FATAL_FAILURE((RunAlignedReconnect<TPoller, TThreadedSetup<TPoller>>(threaded)));
    ASSERT_TRUE(plain == threaded);
    // The old stream offset is that of the dropped frame: 24 + 64 MiB + 3 = 11 mod 16
    THeader filler;
    ASSERT_TRUE(threaded.size() > sizeof(filler));
    std::memcpy(&filler, threaded.data(), sizeof(filler));
    ASSERT_EQ(filler.MessageId & MaxMessageId, FillerMessageId);
    ASSERT_EQ(HeaderPadding(filler), 11u);
    ASSERT_NO_FATAL_FAILURE(CheckStream(threaded, 250));
}

template<typename TPoller>
void test_threaded_node_unaligned_reconnect_equals_node() {
    std::string plain;
    std::string plainTail;
    size_t plainProbes = 0;
    std::string threaded;
    std::string threadedTail;
    size_t threadedProbes = 0;
    ASSERT_NO_FATAL_FAILURE((RunUnalignedReconnect<TPoller, TPlainSetup<TPoller>>(plain, plainProbes, plainTail)));
    ASSERT_NO_FATAL_FAILURE((RunUnalignedReconnect<TPoller, TThreadedSetup<TPoller>>(threaded, threadedProbes, threadedTail)));
    // No filler: the stream starts with a plain frame, as a writer from before PayloadAlignment
    ASSERT_TRUE(plainProbes > 0);
    ASSERT_TRUE(threadedProbes > 0);
    // The number of probes depends on timing; the aligned frames after them do not
    ASSERT_TRUE(plainTail == threadedTail);
    ASSERT_NO_FATAL_FAILURE(CheckStream(threaded, 90));
}

// The connect is refused until the listener appears; the node retries after its back-off
// and the only Drain() is enough to deliver the frame.
template<typename TPoller>
void test_threaded_node_reconnects_to_dead_peer() {
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TMessagesFactory factory;
    const int port = GetFreePort();
    TThreadedSetup<TPoller> setup(loop, factory, port);
    setup.Node.Send(TEnvelope{.Sender = Sender, .Recipient = Recipient, .MessageId = 42});
    setup.Node.Drain();

    PumpFor(loop, std::chrono::milliseconds(300));
    TPeer<TPoller> peer(loop.Poller(), port);
    ASSERT_TRUE(Pump(loop, [&] { return peer.Accepted() == 1 && peer.Bytes(0).size() >= sizeof(THeader); }));
    setup.Stop();
    // The first bytes on the wire are the message, not a connection probe
    THeader received;
    ASSERT_EQ(peer.Bytes(0).size(), sizeof(received));
    std::memcpy(&received, peer.Bytes(0).data(), sizeof(received));
    ASSERT_TRUE(received.Sender == Sender);
    ASSERT_TRUE(received.Recipient == Recipient);
    ASSERT_EQ(received.MessageId, 42u);
    ASSERT_EQ(received.Size, 0u);
}

// Like TNode, the node connects at StartConnect() and not before
template<typename TPoller>
void test_threaded_node_connects_on_request() {
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TMessagesFactory factory;
    const int port = GetFreePort();
    TPeer<TPoller> peer(loop.Poller(), port);
    TThreadedSetup<TPoller> setup(loop, factory, port);
    // Committed but not drained
    setup.Node.Send(TEnvelope{.Sender = Sender, .Recipient = Recipient, .MessageId = 42});
    PumpFor(loop, std::chrono::milliseconds(100));
    ASSERT_EQ(peer.Accepted(), 0u);

    setup.Node.StartConnect();
    setup.Node.StartConnect();
    ASSERT_TRUE(Pump(loop, [&] { return peer.Accepted() == 1; }));
    PumpFor(loop, std::chrono::milliseconds(50));
    ASSERT_EQ(peer.Bytes(0).size(), 0u);

    setup.Node.Drain();
    ASSERT_TRUE(Pump(loop, [&] { return peer.Bytes(0).size() == sizeof(THeader); }));
    setup.Stop();
    ASSERT_EQ(peer.Accepted(), 1u);
}

// A frame of the stream as the test expects to receive it
struct TWireFrame {
    TMessageId MessageId;
    std::string Payload;
    bool Aligned;
};

// Walks the raw stream frame by frame from `pos`, up to an incomplete frame. The padding
// must be zeros, and the payload of an aligned frame must start at a multiple of 16.
static void ParseWire(const std::string& wire, size_t& pos, std::vector<TWireFrame>& frames) {
    while (wire.size() - pos >= sizeof(THeader)) {
        THeader header;
        std::memcpy(&header, wire.data() + pos, sizeof(header));
        const size_t padding = HeaderPadding(header);
        const size_t size = sizeof(header) + padding + header.Size;
        if (wire.size() - pos < size) {
            return;
        }
        for (size_t i = 0; i < padding; ++i) {
            ASSERT_EQ(wire[pos + sizeof(header) + i], 0);
        }
        const size_t payload = pos + sizeof(header) + padding;
        frames.push_back({HeaderMessageId(header), wire.substr(payload, header.Size), payload % 16 == 0});
        pos += size;
    }
}

// The segments grow through many sizes, are reused with stale bytes, and keep the frames:
// the padding is zeros and the holes of the deferred payloads are written, over segments
// that were filled with 0xff first
template<typename TPoller>
void test_threaded_node_segments_grow_keeping_frames() {
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TMessagesFactory factory;
    RegisterTypes(factory);
    const int port = GetFreePort();
    TPeer<TPoller> peer(loop.Poller(), port);
    TThreadedSetup<TPoller> setup(loop, factory, port);
    INode& node = setup.Node;
    TAllocator alloc;
    std::mt19937 rng(31);

    std::vector<TWireFrame> frames;
    std::vector<TWireFrame> received;
    size_t parsed = 0;
    // Sends the segment and waits until all the frames are in `received`
    auto flush = [&]() {
        node.Drain();
        ASSERT_TRUE(Pump(loop, [&] {
            if (peer.Accepted() > 0) {
                ParseWire(peer.Bytes(0), parsed, received);
            }
            return received.size() >= frames.size();
        }));
    };
    auto send = [&](TMessageId messageId, TBlob blob) {
        node.Send(TEnvelope{.Sender = Sender, .Recipient = Recipient, .MessageId = messageId, .Blob = std::move(blob)});
    };
    // Near objects: their payloads are written by the I/O side, into holes
    auto sendText = [&](size_t size, char fill) {
        std::string text(size, fill);
        send(TText::MessageId, SerializeNear<TText>(alloc, text));
        frames.push_back({TText::MessageId, text, false});
    };
    auto sendBytes = [&](size_t size, char fill) {
        std::string bytes(size, fill);
        send(TAlignedBytes::MessageId, SerializeNear<TAlignedBytes>(alloc, bytes));
        frames.push_back({TAlignedBytes::MessageId, bytes, true});
    };
    // The same frames written in place by the producer
    auto writeShifted = [&]() {
        std::string text(rng() % 41, 'l');
        SerializeFarInplace<TText>(node, Sender, Recipient, text);
        frames.push_back({TText::MessageId, text, false});
        std::string bytes(1 + rng() % 60, 'm');
        SerializeFarInplace<TAlignedBytes>(node, Sender, Recipient, TAlignedBytes{bytes});
        frames.push_back({TAlignedBytes::MessageId, bytes, true});
        const TAlignedPod pod{{rng(), rng(), rng()}};
        SerializeFarInplace<TAlignedPod>(node, Sender, Recipient, pod);
        frames.push_back({TAlignedPod::MessageId, std::string(reinterpret_cast<const char*>(&pod), sizeof(pod)), true});
    };
    // A text of 0..40 bytes moves the stream offset, so the padding of the aligned frame has any length
    auto sendShifted = [&]() {
        sendText(rng() % 41, 's');
        sendBytes(1 + rng() % 60, 't');
    };
    // A frame with a payload of 0xff bytes, written without padding: leaves them in the segment
    auto writeDirty = [&](size_t size) {
        auto buf = node.Acquire(sizeof(THeader) + size);
        const THeader header{Sender, Recipient, TText::MessageId, static_cast<uint32_t>(size)};
        std::memcpy(buf.data(), &header, sizeof(header));
        std::memset(buf.data() + sizeof(header), 0xff, size);
        node.Commit(sizeof(THeader) + size);
        frames.push_back({TText::MessageId, std::string(size, '\xff'), false});
    };

    // Phase 1: a segment of about 1 MiB grows from empty, a few bytes at a time
    for (int i = 0; i < 2000; ++i) {
        writeShifted();
        sendShifted();
    }
    ASSERT_NO_FATAL_FAILURE(flush());

    // Phase 2: every segment of the node is filled with 0xff
    for (int i = 0; i < 8; ++i) {
        writeDirty(256 << 10);
        ASSERT_NO_FATAL_FAILURE(flush());
    }

    // Phase 3: frames over the stale bytes; their padding and holes are written explicitly
    for (int batch = 0; batch < 4; ++batch) {
        for (int i = 0; i < 100; ++i) {
            writeShifted();
            sendShifted();
        }
        ASSERT_NO_FATAL_FAILURE(flush());
    }

    // Phase 4: segments of several MiB, grown over the stale bytes, with a large hole after each shift
    for (int batch = 0; batch < 3; ++batch) {
        for (int i = 0; i < 6; ++i) {
            sendText(rng() % 41, 'x');
            sendBytes((1 << 20) + 5, static_cast<char>('a' + i));
        }
        ASSERT_NO_FATAL_FAILURE(flush());
    }
    setup.Stop();

    ASSERT_EQ(received.size(), frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        ASSERT_EQ(received[i].MessageId, frames[i].MessageId);
        ASSERT_TRUE(received[i].Payload == frames[i].Payload);
        if (frames[i].Aligned) {
            ASSERT_TRUE(received[i].Aligned);
        }
    }
}

// An id in the padding bits would break the stream, even with an empty blob
template<typename TPoller>
void test_threaded_node_rejects_message_id_with_padding_bits() {
    TLoop<TPoller> loop;
    TMessagesFactory factory;
    TThreadedSetup<TPoller> setup(loop, factory, GetFreePort());
    auto& node = setup.Node;
    bool thrown = false;
    try {
        node.Send(TEnvelope{.Sender = Sender, .Recipient = Recipient, .MessageId = MaxMessageId + 1});
    } catch (const std::invalid_argument&) {
        thrown = true;
    }
    ASSERT_TRUE(thrown);
    ASSERT_EQ(node.Offset(), 0u);
    node.Send(TEnvelope{.Sender = Sender, .Recipient = Recipient, .MessageId = MaxMessageId});
    ASSERT_EQ(node.Offset(), sizeof(THeader));
    // The offset does not go back when the segment is handed to the I/O side
    node.Drain();
    ASSERT_EQ(node.Offset(), sizeof(THeader));
}

// The node is destroyed with a write blocked in the socket and segments in its queues
template<typename TPoller>
void test_threaded_node_destroyed_during_blocked_write() {
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TMessagesFactory factory;
    RegisterTypes(factory);
    const int port = GetFreePort();
    TPeer<TPoller> peer(loop.Poller(), port);
    peer.SetReadLimit(0, 0);
    {
        TThreadedSetup<TPoller> setup(loop, factory, port);
        INode& node = setup.Node;
        const uint32_t large = 64 << 20;
        auto buf = node.Acquire(sizeof(THeader) + large);
        const THeader header{Sender, Recipient, TText::MessageId, large};
        std::memcpy(buf.data(), &header, sizeof(header));
        node.Commit(sizeof(THeader) + large);
        node.Drain();
        ASSERT_TRUE(Pump(loop, [&] { return peer.Accepted() == 1; }));
        PumpFor(loop, std::chrono::milliseconds(200));
        std::mt19937 rng(29);
        for (int r = 0; r < 10; ++r) {
            WriteRound(node, rng, r);
            node.Drain();
        }
        WriteRound(node, rng, 10);
    }
    ASSERT_EQ(peer.Accepted(), 1u);
}

// Small segments pile up while the peer is down, then large batches go out one per
// Drain(). Segments() counts the segments that exist, not the node's own count of spare
// ones: once the backlog is written, only the current segment and the spare ones remain.
template<typename TPoller>
void test_threaded_node_bounds_spare_segments() {
    using TNodeType = TThreadedNode<TPoller>;
    constexpr size_t count = 200;
    constexpr size_t idle = 1 + TNodeType::MaxSpareSegments;
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TMessagesFactory factory;
    RegisterTypes(factory);
    const int port = GetFreePort();
    TThreadedSetup<TPoller> setup(loop, factory, port);
    auto& node = setup.Node;
    ASSERT_EQ(node.Segments(), 1u);

    size_t expected = 0;
    for (size_t i = 0; i < count; ++i) {
        SerializeFarInplace<TText>(node, Sender, Recipient, std::string("small"));
        node.Drain();
        expected += TextFrame("small").size();
    }
    // Nothing is written before the node connects: the whole backlog and the current segment
    ASSERT_EQ(node.Segments(), count + 1);

    size_t received = 0;
    TPeer<TPoller> peer(loop.Poller(), port);
    peer.OnData = [&](size_t, const char*, size_t size) {
        received += size;
    };
    // Every queued segment is written and comes back; all but the spare ones are deleted
    ASSERT_TRUE(Pump(loop, [&] { return received == expected && node.Segments() == idle; }, std::chrono::seconds(20)));

    const std::string large(64 * 1024, 'x');
    for (size_t i = 0; i < count; ++i) {
        SerializeFarInplace<TText>(node, Sender, Recipient, large);
        node.Drain();
        expected += TextFrame(large).size();
    }
    ASSERT_TRUE(Pump(loop, [&] { return received == expected && node.Segments() <= idle; }, std::chrono::seconds(20)));
    setup.Stop();
    ASSERT_TRUE(node.Segments() <= idle);
    ASSERT_EQ(peer.Accepted(), 1u);
}

// A Near probe is sized on the producer side and written and destroyed on the I/O side,
// in its place among the frames around it: the stream is the one TNode writes. A probe
// that cannot be sized throws to the sender and leaves no frame.
template<typename TPoller>
void test_threaded_node_defers_payloads() {
    TProbeLog plainLog;
    TProbeLog threadedLog;
    std::string plain;
    std::string threaded;
    int plainThrown = 0;
    int threadedThrown = 0;
    ASSERT_NO_FATAL_FAILURE((RunProbes<TPoller, TPlainSetup<TPoller>>(plain, plainLog, plainThrown)));
    ASSERT_NO_FATAL_FAILURE((RunProbes<TPoller, TThreadedSetup<TPoller>>(threaded, threadedLog, threadedThrown)));
    ASSERT_TRUE(plain == threaded);
    ASSERT_EQ(plainThrown, 4);
    ASSERT_EQ(threadedThrown, 4);

    const auto producer = std::this_thread::get_id();
    ASSERT_EQ(threadedLog.Sized.size(), 204u);
    for (const auto& id : threadedLog.Sized) {
        ASSERT_TRUE(id == producer);
    }
    ASSERT_EQ(threadedLog.Written.size(), 200u);
    const auto io = threadedLog.Written.front();
    ASSERT_TRUE(io != producer);
    for (const auto& id : threadedLog.Written) {
        ASSERT_TRUE(id == io);
    }
    // The rejected probes die in Send, the others after their write
    ASSERT_EQ(threadedLog.Destroyed.size(), 204u);
    ASSERT_EQ(std::count(threadedLog.Destroyed.begin(), threadedLog.Destroyed.end(), producer), 4);
    ASSERT_EQ(std::count(threadedLog.Destroyed.begin(), threadedLog.Destroyed.end(), io), 200);
}

// A non-POD type without a payload writer is serialized in Send, as by TNode
template<typename TPoller>
void test_threaded_node_serializes_other_payloads_at_once() {
    TProbeLog plainLog;
    TProbeLog threadedLog;
    std::string plain;
    std::string threaded;
    int plainThrown = 0;
    int threadedThrown = 0;
    const TProbeRun run{.Writer = false};
    ASSERT_NO_FATAL_FAILURE((RunProbes<TPoller, TPlainSetup<TPoller>>(plain, plainLog, plainThrown, run)));
    ASSERT_NO_FATAL_FAILURE((RunProbes<TPoller, TThreadedSetup<TPoller>>(threaded, threadedLog, threadedThrown, run)));
    ASSERT_TRUE(plain == threaded);
    ASSERT_EQ(threadedThrown, 4);
    const auto producer = std::this_thread::get_id();
    ASSERT_EQ(threadedLog.Written.size(), 200u);
    ASSERT_EQ(std::count(threadedLog.Written.begin(), threadedLog.Written.end(), producer), 200);
    ASSERT_EQ(std::count(threadedLog.Destroyed.begin(), threadedLog.Destroyed.end(), producer), 204);
}

// Types registered on the producer thread while the I/O side writes deferred payloads:
// the I/O side does not read the factory, so the stream is still that of TNode
template<typename TPoller>
void test_threaded_node_registers_types_while_running() {
    TProbeLog plainLog;
    TProbeLog threadedLog;
    std::string plain;
    std::string threaded;
    int plainThrown = 0;
    int threadedThrown = 0;
    const TProbeRun run{.LateTypes = true};
    ASSERT_NO_FATAL_FAILURE((RunProbes<TPoller, TPlainSetup<TPoller>>(plain, plainLog, plainThrown, run)));
    ASSERT_NO_FATAL_FAILURE((RunProbes<TPoller, TThreadedSetup<TPoller>>(threaded, threadedLog, threadedThrown, run)));
    ASSERT_TRUE(plain == threaded);
    ASSERT_EQ(threadedLog.Written.size(), 200u);
    ASSERT_TRUE(threadedLog.Written.front() != std::this_thread::get_id());
}

// The node is destroyed while a write blocks: the probes in its queue and in its current
// segment are destroyed with it, none of them written
template<typename TPoller>
void test_threaded_node_destroys_deferred_payloads() {
    TProbeLog log;
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TMessagesFactory factory;
    RegisterTypes(factory);
    factory.RegisterPayloadWriter<TProbe>();
    const int port = GetFreePort();
    TPeer<TPoller> peer(loop.Poller(), port);
    peer.SetReadLimit(0, 0);
    {
        TThreadedSetup<TPoller> setup(loop, factory, port);
        INode& node = setup.Node;
        const uint32_t large = 64 << 20;
        auto buf = node.Acquire(sizeof(THeader) + large);
        const THeader header{Sender, Recipient, TText::MessageId, large};
        std::memcpy(buf.data(), &header, sizeof(header));
        node.Commit(sizeof(THeader) + large);
        node.Drain();
        ASSERT_TRUE(Pump(loop, [&] { return peer.Accepted() == 1; }));
        PumpFor(loop, std::chrono::milliseconds(200));
        TAllocator alloc;
        for (int r = 0; r < 10; ++r) {
            SendProbe(node, alloc, log, std::string(64 * 1024, 'p'));
            node.Drain();
        }
        SendProbe(node, alloc, log, "current");
    }
    ASSERT_TRUE(log.Written.empty());
    ASSERT_EQ(log.Destroyed.size(), 11u);
}

class TPonger : public IBehaviorActor, public TBehavior<TPonger, TPing> {
public:
    TPonger() {
        Become(this);
    }

    void Receive(TPing&& ping, TBlob, TActorContext::TPtr ctx) {
        ctx->Send<TText>(ctx->Sender(), std::to_string(ping.Seq) + ":" + std::to_string(ping.Check));
    }

    void HandleUnknownMessage(TMessageId, TBlob, TActorContext::TPtr) {
    }
};

// Sends `sequential` pings one after another, then `burst` pings at once
class TPinger : public IBehaviorActor, public TBehavior<TPinger, TText> {
public:
    TPinger(TActorId ponger, uint64_t sequential, uint64_t burst, std::vector<std::string>& replies)
        : Ponger(ponger)
        , Sequential(sequential)
        , Burst(burst)
        , Replies(replies)
    {
        Become(this);
    }

    void Receive(TText&& reply, TBlob, TActorContext::TPtr ctx) {
        Replies.push_back(std::move(reply.Value));
        const uint64_t seq = Replies.size();
        if (seq < Sequential) {
            ctx->Send<TPing>(Ponger, TPing{seq, seq * 3});
        } else if (seq == Sequential) {
            for (uint64_t i = 0; i < Burst; ++i) {
                ctx->Send<TPing>(Ponger, TPing{seq + i, (seq + i) * 3});
            }
        }
    }

    void HandleUnknownMessage(TMessageId, TBlob, TActorContext::TPtr) {
    }

    TActorId Ponger;
    uint64_t Sequential;
    uint64_t Burst;
    std::vector<std::string>& Replies;
};

// Two actor systems on the test thread talk through two TThreadedNode on one I/O thread
template<typename TPoller>
void test_threaded_node_actor_systems() {
    using TSocket = typename TPoller::TSocket;
    constexpr uint64_t sequential = 500;
    constexpr uint64_t burst = 2000;
    TLoop<TPoller> loop;
    auto& poller = loop.Poller();
    poller.SetMaxDuration(std::chrono::milliseconds(1));
    auto io = std::make_unique<TLoopThread<TPoller>>();
    auto resolver = std::make_unique<TResolver>(TAddress{"127.0.0.1", 53}, io->Poller());
    TMessagesFactory factory;
    RegisterTypes(factory);
    int closed = 0;
    auto logger = [&](const std::string& message) {
        closed += message.find("InboundConnection") != std::string::npos;
    };
    TActorSystem first(&poller, 1, logger);
    TActorSystem second(&poller, 2, logger);

    const int ports[] = {GetFreePort(), GetFreePort()};
    TActorSystem* systems[] = {&first, &second};
    for (int i = 0; i < 2; ++i) {
        systems[i]->AddNode(2 - i, std::make_unique<TThreadedNode<TPoller>>(
            io->Poller(), factory, *resolver, SocketFactory(io->Poller()), THostPort("127.0.0.1", ports[1 - i])));
    }
    std::vector<std::string> replies;
    const auto ponger = second.RegisterWellKnown(1, std::make_unique<TPonger>());
    const auto pinger = first.Register(std::make_unique<TPinger>(ponger, sequential, burst, replies));
    for (int i = 0; i < 2; ++i) {
        TAddress address{"127.0.0.1", ports[i]};
        TSocket listener(poller, address.Domain());
        listener.Bind(address);
        listener.Listen();
        systems[i]->template Serve<TSocket, TZeroCopyEnvelopeReaderV2>(std::move(listener));
    }
    io->Start();

    first.Send<TPing>(pinger, ponger, TPing{0, 0});
    const bool complete = Pump(loop, [&] { return replies.size() == sequential + burst; });
    io->Stop();
    io->Join();
    // Destroys the nodes and then their poller, which closes the sockets: the inbound
    // connections of both systems see the end of their streams and finish before the
    // systems are destroyed
    first.AddNode(2, nullptr);
    second.AddNode(1, nullptr);
    resolver.reset();
    io.reset();
    const bool finished = Pump(loop, [&] { return closed == 2; }, std::chrono::seconds(10));

    ASSERT_TRUE(complete);
    ASSERT_TRUE(finished);
    for (uint64_t seq = 0; seq < sequential + burst; ++seq) {
        ASSERT_TRUE(replies[seq] == std::to_string(seq) + ":" + std::to_string(seq * 3));
    }
}

// Producers write numbered frames, small ones and frames up to 256 KiB, while the I/O
// thread sends them. Every frame arrives intact, aligned and in the order of its producer.
template<typename TPoller, typename TLock>
void RunStress(uint32_t producerCount, uint32_t perProducer) {
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TMessagesFactory factory;
    RegisterTypes(factory);
    const int port = GetFreePort();
    TStressChecker checker(producerCount, true);
    TPeer<TPoller> peer(loop.Poller(), port);
    peer.OnData = [&](size_t index, const char* data, size_t size) {
        checker.Feed(index, data, size);
    };
    TThreadedSetup<TPoller, TLock> setup(loop, factory, port);
    auto& node = setup.Node;

    std::vector<std::thread> producers;
    for (uint32_t p = 0; p < producerCount; ++p) {
        producers.emplace_back([&node, p, perProducer] {
            TAllocator alloc;
            std::mt19937 rng(100 + p);
            for (uint32_t seq = 0; seq < perProducer; ++seq) {
                WriteStressFrame(node, alloc, rng, p, seq);
            }
            node.Drain();
        });
    }
    const size_t total = size_t(producerCount) * perProducer;
    const bool complete = Pump(loop, [&] { return checker.Frames == total || !checker.Error.empty(); }, std::chrono::seconds(300));
    for (auto& producer : producers) {
        producer.join();
    }
    setup.Stop();
    ASSERT_TRUE(checker.Error.empty()) << checker.Error;
    ASSERT_TRUE(complete);
    ASSERT_EQ(peer.Accepted(), 1u);
}

template<typename TPoller>
void test_threaded_node_stress() {
    RunStress<TPoller, TNullLock>(1, 6000);
}

template<typename TPoller>
void test_threaded_node_stress_many_producers() {
    RunStress<TPoller, std::mutex>(4, 2000);
}

// The peer resets the connection again and again while a producer writes. A reset loses
// frames, but every new stream starts at a frame boundary and keeps the payloads aligned.
template<typename TPoller>
void test_threaded_node_stress_reconnects() {
    constexpr size_t resets = 6;
    constexpr size_t bytesBeforeReset = 2 * 1024 * 1024;
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TMessagesFactory factory;
    RegisterTypes(factory);
    const int port = GetFreePort();
    TStressChecker checker(1, false);
    std::vector<size_t> received;
    TPeer<TPoller> peer(loop.Poller(), port);
    peer.OnData = [&](size_t index, const char* data, size_t size) {
        checker.Feed(index, data, size);
        received.resize(std::max(received.size(), index + 1));
        received[index] += size;
    };
    TThreadedSetup<TPoller> setup(loop, factory, port);
    auto& node = setup.Node;

    std::atomic<bool> finish = false;
    std::atomic<bool> done = false;
    std::atomic<uint32_t> seen = 0;
    std::thread producer([&node, &finish, &done, &seen] {
        TAllocator alloc;
        std::mt19937 rng(200);
        for (uint32_t seq = 0; !finish.load(); ++seq) {
            WriteStressFrame(node, alloc, rng, 0, seq);
            // Bounds the backlog by the progress of the peer, but not forever: a reset loses frames
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
            while (seq > seen.load() + 2000 && !finish.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
        // Any frame may be lost, the last one too
        while (!done.load()) {
            WriteEnd(node);
            node.Drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    size_t reset = 0;
    const bool complete = Pump(loop, [&] {
        seen.store(checker.MaxSeq);
        if (reset < resets && reset < received.size() && received[reset] >= bytesBeforeReset) {
            peer.Reset(reset++);
        }
        if (reset == resets) {
            finish.store(true);
        }
        return checker.Ends > 0 || !checker.Error.empty();
    }, std::chrono::seconds(300));
    finish.store(true);
    done.store(true);
    producer.join();
    setup.Stop();
    ASSERT_TRUE(checker.Error.empty()) << checker.Error;
    ASSERT_TRUE(complete);
    ASSERT_EQ(reset, resets);
    ASSERT_TRUE(peer.Accepted() > resets);
}

// The I/O loop stops while a producer keeps writing and draining; then the node is
// destroyed with whatever is left in its queues and its socket.
template<typename TPoller>
void test_threaded_node_stop_under_load() {
    for (int iteration = 0; iteration < 10; ++iteration) {
        TLoop<TPoller> loop;
        loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
        TMessagesFactory factory;
        RegisterTypes(factory);
        const int port = GetFreePort();
        size_t received = 0;
        TPeer<TPoller> peer(loop.Poller(), port);
        peer.OnData = [&](size_t, const char*, size_t size) {
            received += size;
        };
        std::atomic<bool> done = false;
        std::thread producer;
        {
            TThreadedSetup<TPoller> setup(loop, factory, port);
            producer = std::thread([&node = setup.Node, &done] {
                TAllocator alloc;
                std::mt19937 rng(300);
                for (uint32_t seq = 0; !done.load(); ++seq) {
                    WriteStressFrame(node, alloc, rng, 0, seq);
                }
            });
            const bool running = Pump(loop, [&] { return received > size_t(1 + iteration % 3) * 1024 * 1024; });
            setup.Stop();
            // The producer still hands segments over and notifies the stopped loop
            PumpFor(loop, std::chrono::milliseconds(5));
            done.store(true);
            producer.join();
            ASSERT_TRUE(running);
        }
    }
}

#endif // _WIN32

#define my_unit_test(f, a) RegisterTest(#f, #a, f<a>)
#define my_unit_test2(f, a, b) my_unit_test(f, a); my_unit_test(f, b)
#define my_unit_test3(f, a, b, c) my_unit_test2(f, a, b); my_unit_test(f, c)
#define my_unit_test4(f, a, b, c, d) my_unit_test3(f, a, b, c); my_unit_test(f, d)

#ifdef __linux__
#ifdef HAVE_URING
#define my_unit_poller(f) my_unit_test4(f, TSelect, TPoll, TEPoll, TUring)
#else
#define my_unit_poller(f) my_unit_test3(f, TSelect, TPoll, TEPoll)
#endif
#elif defined(__APPLE__) || defined(__FreeBSD__)
#define my_unit_poller(f) my_unit_test3(f, TSelect, TPoll, TKqueue)
#else
#define my_unit_poller(f) my_unit_test2(f, TSelect, TPoll)
#endif

int main(int argc, char* argv[]) {
    TInitializer init;

    testing::InitGoogleTest(&argc, argv);

#ifndef _WIN32
    ADD_TEST(my_unit_poller, test_threaded_node_stream_equals_node);
    ADD_TEST(my_unit_poller, test_threaded_node_stream_equals_node_with_mutex);
    ADD_TEST(my_unit_poller, test_threaded_node_filler_on_reconnect_equals_node);
    ADD_TEST(my_unit_poller, test_threaded_node_unaligned_reconnect_equals_node);
    ADD_TEST(my_unit_poller, test_threaded_node_reconnects_to_dead_peer);
    ADD_TEST(my_unit_poller, test_threaded_node_connects_on_request);
    ADD_TEST(my_unit_poller, test_threaded_node_segments_grow_keeping_frames);
    ADD_TEST(my_unit_poller, test_threaded_node_rejects_message_id_with_padding_bits);
    ADD_TEST(my_unit_poller, test_threaded_node_destroyed_during_blocked_write);
    ADD_TEST(my_unit_poller, test_threaded_node_bounds_spare_segments);
    ADD_TEST(my_unit_poller, test_threaded_node_defers_payloads);
    ADD_TEST(my_unit_poller, test_threaded_node_serializes_other_payloads_at_once);
    ADD_TEST(my_unit_poller, test_threaded_node_registers_types_while_running);
    ADD_TEST(my_unit_poller, test_threaded_node_destroys_deferred_payloads);
    ADD_TEST(my_unit_poller, test_threaded_node_actor_systems);
    ADD_TEST(my_unit_poller, test_threaded_node_stress);
    ADD_TEST(my_unit_poller, test_threaded_node_stress_many_producers);
    ADD_TEST(my_unit_poller, test_threaded_node_stress_reconnects);
    ADD_TEST(my_unit_poller, test_threaded_node_stop_under_load);
#endif

    return RUN_ALL_TESTS();
}
