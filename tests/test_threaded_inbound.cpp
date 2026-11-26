#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <coroio/all.hpp>
#include <coroio/actors/actorsystem.hpp>
#include <coroio/actors/threaded_inbound.hpp>
#include <coroio/actors/threaded_node.hpp>
#include <coroio/loop_thread.hpp>

#include "testlib.h"

using namespace NNet;
using namespace NNet::NActors;

#ifndef _WIN32

struct TText {
    static constexpr TMessageId MessageId = 60;
    std::string Value;
};

struct TAlignedText {
    static constexpr TMessageId MessageId = 61;
    static constexpr size_t PayloadAlignment = 16;
    std::string Value;
};

struct TPing {
    static constexpr TMessageId MessageId = 62;
    static constexpr size_t PayloadAlignment = 16;
    uint64_t Seq;
    uint64_t Check;
};

struct TPong {
    static constexpr TMessageId MessageId = 63;
    uint64_t Seq;
    uint64_t Check;
};

namespace NNet::NActors {

template<>
size_t SerializedSize<TText>(const TText& obj) {
    return obj.Value.size();
}

template<>
void SerializeTo<TText>(const TText& obj, std::span<char> out) {
    std::memcpy(out.data(), obj.Value.data(), out.size());
}

template<>
size_t SerializedSize<TAlignedText>(const TAlignedText& obj) {
    return obj.Value.size();
}

template<>
void SerializeTo<TAlignedText>(const TAlignedText& obj, std::span<char> out) {
    std::memcpy(out.data(), obj.Value.data(), out.size());
}

} // namespace NNet::NActors

namespace {

constexpr TNodeId LocalNode = 1;
constexpr TNodeId RemoteNode = 2;
constexpr TNodeId OtherNode = 3;
// The bound of one read of an inbound connection
constexpr size_t ReadSize = 512 * 1024;

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

// Mostly small payloads; every 64th one is between 64 and 256 KiB
size_t MixedSize(uint32_t seq) {
    return seq % 64 == 63 ? 64 * 1024 + (seq * 7919) % (192 * 1024) : (seq * 31) % 200;
}

std::string MakePayload(uint32_t connection, uint32_t seq, size_t size) {
    return std::to_string(connection) + ":" + std::to_string(seq) + ":"
        + std::string(size, static_cast<char>('a' + (connection * 7 + seq) % 26));
}

// Odd frames ask for an aligned payload
TMessageId MessageIdOf(uint32_t seq) {
    return seq % 2 ? TAlignedText::MessageId : TText::MessageId;
}

TActorId SenderOf(uint32_t connection) {
    return TActorId(RemoteNode, connection + 1, 1);
}

void WriteFrame(TVectorStream& stream, TActorId recipient, uint32_t connection, uint32_t seq, size_t size) {
    auto payload = MakePayload(connection, seq, size);
    if (MessageIdOf(seq) == TAlignedText::MessageId) {
        SerializeFarInplace<TAlignedText>(stream, SenderOf(connection), recipient, TAlignedText{std::move(payload)});
    } else {
        SerializeFarInplace<TText>(stream, SenderOf(connection), recipient, TText{std::move(payload)});
    }
}

// The stream of one connection: frames 0, 1, ... in order
std::string MakeStream(TActorId recipient, uint32_t connection, uint32_t frames, size_t (*size)(uint32_t) = MixedSize) {
    TVectorStream stream;
    for (uint32_t seq = 0; seq < frames; ++seq) {
        WriteFrame(stream, recipient, connection, seq, size(seq));
    }
    return stream.Data;
}

// What TRecorder has seen of the connections
struct TRecord {
    TRecord(size_t connections, bool aligned, size_t (*size)(uint32_t) = MixedSize)
        : Next(connections)
        , Aligned(aligned)
        , Size(size)
    { }

    void Fail(const std::string& error) {
        if (Error.empty()) {
            Error = error;
        }
    }

    std::vector<uint32_t> Next;
    bool Aligned;
    size_t (*Size)(uint32_t);
    size_t Received = 0;
    std::string Error;
};

// Checks that every connection delivers its frames intact and in order
class TRecorder : public IActor {
public:
    explicit TRecorder(TRecord& record)
        : Record(record)
    { }

    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        const uint32_t connection = ctx->Sender().ActorId() - 1;
        if (connection >= Record.Next.size()) {
            Record.Fail("unknown sender " + ctx->Sender().ToString());
            return;
        }
        const uint32_t seq = Record.Next[connection]++;
        const std::string expected = MakePayload(connection, seq, Record.Size(seq));
        const std::string frame = "frame " + std::to_string(seq) + " of connection " + std::to_string(connection);
        if (messageId != MessageIdOf(seq) || blob.Size != expected.size()
            || std::memcmp(blob.Data.get(), expected.data(), expected.size()) != 0)
        {
            Record.Fail(frame + " differs");
        }
        if (Record.Aligned && messageId == TAlignedText::MessageId && reinterpret_cast<uintptr_t>(blob.Data.get()) % 16 != 0) {
            Record.Fail(frame + " is not aligned");
        }
        ++Record.Received;
    }

    TRecord& Record;
};

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

// Waits on the test thread without running any loop
template<typename TPredicate>
bool WaitUntil(TPredicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
    const auto deadline = TClock::now() + timeout;
    while (!predicate()) {
        if (TClock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

template<typename TPoller>
typename TPoller::TSocket Listen(TPoller& poller, const TAddress& address) {
    typename TPoller::TSocket listener(poller, address.Domain());
    listener.Bind(address);
    listener.Listen();
    return listener;
}

/**
 * An actor system on the loop of the test thread whose inbound connections are read
 * on an I/O thread. The inbound counts the connections it saw closed and the envelopes
 * it dropped.
 */
template<typename TPoller, typename TReader>
struct TInboundSetup {
    explicit TInboundSetup(size_t limit = TQueuedDelivery::DefaultLimit)
        : Address("127.0.0.1", GetFreePort())
        , Delivery(System, limit)
        , Inbound(Io.Poller(), Listen(Io.Poller(), Address), LocalNode, Delivery, [this](const std::string& message) {
            Closed += message.find("InboundConnection") != std::string::npos;
            Dropped += message.find("different node") != std::string::npos;
        })
    {
        Loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
        System.Serve();
        Io.Start();
    }

    // The I/O loop stops before the inbound is destroyed
    ~TInboundSetup() {
        Stop();
    }

    void Stop() {
        Io.Stop();
        Io.Join();
    }

    TLoop<TPoller> Loop;
    TActorSystem System{&Loop.Poller(), LocalNode};
    TAddress Address;
    TLoopThread<TPoller> Io;
    std::atomic<int> Closed = 0;
    std::atomic<int> Dropped = 0;
    TQueuedDelivery Delivery;
    TThreadedInbound<TPoller, TQueuedDelivery, TReader> Inbound;
};

/// Writes prepared streams to `address` from a thread of its own, one connection per
/// stream, in pieces of 64 KiB. After its stream a connection shuts its side down and
/// waits for the inbound to close the other one.
template<typename TPoller>
class TClients {
public:
    using TSocket = typename TPoller::TSocket;

    TClients(const TAddress& address, std::vector<std::string> streams)
        : Streams_(std::move(streams))
        , Written_(Streams_.size())
        , Ended_(Streams_.size())
    {
        Sockets_.reserve(Streams_.size());
        for (size_t i = 0; i < Streams_.size(); ++i) {
            Sockets_.emplace_back(Io_.Poller(), address.Domain());
        }
        for (size_t i = 0; i < Streams_.size(); ++i) {
            Writers_.push_back(Write(i, address));
        }
        Io_.Start();
    }

    ~TClients() {
        Stop();
    }

    void Stop() {
        Io_.Stop();
        Io_.Join();
    }

    /// Bytes of stream `index` written so far. Any thread.
    size_t Written(size_t index) const {
        return Written_[index].load();
    }

    size_t Size(size_t index) const {
        return Streams_[index].size();
    }

    /// The inbound has closed connection `index`. Any thread.
    bool Ended(size_t index) const {
        return Ended_[index].load();
    }

private:
    TFuture<void> Write(size_t index, TAddress address) {
        auto& socket = Sockets_[index];
        const auto& stream = Streams_[index];
        co_await socket.Connect(address);
        constexpr size_t piece = 64 * 1024;
        for (size_t pos = 0; pos < stream.size(); pos += piece) {
            const size_t size = std::min(piece, stream.size() - pos);
            co_await TByteWriter(socket).Write(stream.data() + pos, size);
            Written_[index] += size;
        }
        ::shutdown(socket.Fd(), SHUT_WR);
        char byte;
        while (co_await socket.ReadSome(&byte, 1) != 0) {
        }
        Ended_[index] = true;
        socket.Close();
    }

    TLoopThread<TPoller> Io_;
    std::vector<std::string> Streams_;
    std::vector<std::atomic<size_t>> Written_;
    std::vector<std::atomic<bool>> Ended_;
    std::vector<TSocket> Sockets_;
    std::vector<TFuture<void>> Writers_;
};

} // namespace

// Three connections at once, small and large frames: every connection delivers its
// frames intact and in order, and the V2 reader keeps the aligned payloads aligned
template<typename TPoller, typename TReader>
void test_threaded_inbound_delivers_in_order() {
    constexpr uint32_t connections = 3;
    constexpr uint32_t frames = 2000;
    TInboundSetup<TPoller, TReader> setup;
    TRecord record(connections, std::is_same_v<TReader, TZeroCopyEnvelopeReaderV2>);
    const auto recorder = setup.System.Register(std::make_unique<TRecorder>(record));
    std::vector<std::string> streams;
    for (uint32_t c = 0; c < connections; ++c) {
        streams.push_back(MakeStream(recorder, c, frames));
    }
    TClients<TPoller> clients(setup.Address, std::move(streams));

    const bool complete = Pump(setup.Loop, [&] { return record.Received == connections * frames || !record.Error.empty(); });
    const bool closed = Pump(setup.Loop, [&] { return setup.Closed.load() == int(connections); });
    const bool ended = WaitUntil([&] { return clients.Ended(0) && clients.Ended(1) && clients.Ended(2); });
    clients.Stop();
    setup.Stop();
    ASSERT_TRUE(record.Error.empty()) << record.Error;
    ASSERT_TRUE(complete);
    ASSERT_TRUE(closed);
    ASSERT_TRUE(ended);
    ASSERT_EQ(setup.Delivery.QueuedBytes(), 0u);
}

size_t LargeSize(uint32_t) {
    return 64 * 1024;
}

// The actor loop does not run: the delivery fills up to its limit plus at most one read,
// the inbound stops reading, and TCP stops the client. Then the actor loop runs, and
// everything arrives in order.
template<typename TPoller>
void test_threaded_inbound_backpressure() {
    constexpr size_t limit = 1024 * 1024;
    constexpr uint32_t frames = 1024;
    TInboundSetup<TPoller, TZeroCopyEnvelopeReaderV2> setup(limit);
    TRecord record(1, true, LargeSize);
    const auto recorder = setup.System.Register(std::make_unique<TRecorder>(record));
    TClients<TPoller> clients(setup.Address, {MakeStream(recorder, 0, frames, LargeSize)});

    const bool filled = WaitUntil([&] { return setup.Delivery.QueuedBytes() >= limit; });
    // The client is stopped once it writes nothing for a while
    size_t written = clients.Written(0);
    auto since = TClock::now();
    const bool stopped = WaitUntil([&] {
        if (clients.Written(0) != written) {
            written = clients.Written(0);
            since = TClock::now();
        }
        return TClock::now() - since > std::chrono::milliseconds(300);
    });
    const size_t queued = setup.Delivery.QueuedBytes();

    const bool complete = Pump(setup.Loop, [&] { return record.Received == frames || !record.Error.empty(); });
    clients.Stop();
    setup.Stop();
    ASSERT_TRUE(filled);
    ASSERT_TRUE(stopped);
    ASSERT_GE(queued, limit);
    // One read beyond the limit, and the frame that the previous read began
    ASSERT_LE(queued, limit + ReadSize + LargeSize(0) + 64);
    ASSERT_LT(written, clients.Size(0));
    ASSERT_TRUE(record.Error.empty()) << record.Error;
    ASSERT_TRUE(complete);
}

// Envelopes for another node are dropped and logged; the others arrive
template<typename TPoller>
void test_threaded_inbound_drops_envelopes_for_other_nodes() {
    TInboundSetup<TPoller, TZeroCopyEnvelopeReaderV2> setup;
    TRecord record(1, true);
    const auto recorder = setup.System.Register(std::make_unique<TRecorder>(record));
    const TActorId other(OtherNode, recorder.ActorId(), recorder.Cookie());
    TVectorStream stream;
    for (uint32_t seq = 0; seq < 3; ++seq) {
        WriteFrame(stream, recorder, 0, seq, MixedSize(seq));
        WriteFrame(stream, other, 0, 100 + seq, 10);
    }
    TClients<TPoller> clients(setup.Address, {stream.Data});

    const bool closed = Pump(setup.Loop, [&] { return setup.Closed.load() == 1; });
    // The last envelopes may still be on their way
    Pump(setup.Loop, [&] { return record.Received == 3; }, std::chrono::seconds(10));
    clients.Stop();
    setup.Stop();
    ASSERT_TRUE(closed);
    ASSERT_TRUE(record.Error.empty()) << record.Error;
    ASSERT_EQ(record.Received, 3u);
    ASSERT_EQ(setup.Dropped.load(), 3);
}

// A connection that has finished closes its socket at once; the inbound forgets it
// when it accepts the next one
template<typename TPoller>
void test_threaded_inbound_closes_and_forgets_connections() {
    constexpr uint32_t connections = 6;
    TInboundSetup<TPoller, TZeroCopyEnvelopeReaderV2> setup;
    TRecord record(connections, true);
    const auto recorder = setup.System.Register(std::make_unique<TRecorder>(record));
    for (uint32_t c = 0; c < connections; ++c) {
        TClients<TPoller> clients(setup.Address, {MakeStream(recorder, c, 10)});
        ASSERT_TRUE(Pump(setup.Loop, [&] { return setup.Closed.load() == int(c + 1) && record.Received == (c + 1) * 10; }));
        ASSERT_TRUE(WaitUntil([&] { return clients.Ended(0); }, std::chrono::seconds(10)));
    }
    setup.Stop();
    ASSERT_TRUE(record.Error.empty()) << record.Error;
    ASSERT_EQ(setup.Inbound.Connections(), 1u);
}

class TPonger : public IBehaviorActor, public TBehavior<TPonger, TPing> {
public:
    TPonger() {
        Become(this);
    }

    void Receive(TPing&& ping, TBlob, TActorContext::TPtr ctx) {
        ctx->Send<TPong>(ctx->Sender(), TPong{ping.Seq, ping.Check});
    }

    void HandleUnknownMessage(TMessageId, TBlob, TActorContext::TPtr) {
    }
};

// Sends `sequential` pings one after another, then `burst` pings at once
class TPinger : public IBehaviorActor, public TBehavior<TPinger, TPong> {
public:
    TPinger(TActorId ponger, uint64_t sequential, uint64_t burst, std::vector<TPong>& replies)
        : Ponger(ponger)
        , Sequential(sequential)
        , Burst(burst)
        , Replies(replies)
    {
        Become(this);
    }

    void Receive(TPong&& reply, TBlob, TActorContext::TPtr ctx) {
        Replies.push_back(reply);
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
    std::vector<TPong>& Replies;
};

// Two actor systems on the test thread: their nodes write on one I/O thread, their
// inbound connections are read on another one, and ServeOutbound() starts the nodes
template<typename TPoller, typename TReader>
void test_threaded_inbound_actor_systems() {
    using TSocket = typename TPoller::TSocket;
    constexpr uint64_t sequential = 500;
    constexpr uint64_t burst = 2000;
    TLoop<TPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    auto nodesIo = std::make_unique<TLoopThread<TPoller>>();
    auto inboundIo = std::make_unique<TLoopThread<TPoller>>();
    auto resolver = std::make_unique<TResolver>(TAddress{"127.0.0.1", 53}, nodesIo->Poller());
    TMessagesFactory factory;
    factory.RegisterSerializer<TPing>();
    factory.RegisterSerializer<TPong>();
    TActorSystem first(&loop.Poller(), 1);
    TActorSystem second(&loop.Poller(), 2);
    TActorSystem* systems[] = {&first, &second};

    const int ports[] = {GetFreePort(), GetFreePort()};
    std::unique_ptr<TQueuedDelivery> deliveries[2];
    std::unique_ptr<TThreadedInbound<TPoller, TQueuedDelivery, TReader>> inbounds[2];
    for (int i = 0; i < 2; ++i) {
        auto& poller = nodesIo->Poller();
        systems[i]->AddNode(2 - i, std::make_unique<TThreadedNode<TPoller>>(
            poller, factory, *resolver,
            [&poller](const TAddress& addr) { return TSocket(poller, addr.Domain()); },
            THostPort("127.0.0.1", ports[1 - i])));
        deliveries[i] = std::make_unique<TQueuedDelivery>(*systems[i]);
        inbounds[i] = std::make_unique<TThreadedInbound<TPoller, TQueuedDelivery, TReader>>(
            inboundIo->Poller(), Listen(inboundIo->Poller(), TAddress{"127.0.0.1", ports[i]}), i + 1, *deliveries[i]);
    }
    std::vector<TPong> replies;
    const auto ponger = second.RegisterWellKnown(1, std::make_unique<TPonger>());
    const auto pinger = first.Register(std::make_unique<TPinger>(ponger, sequential, burst, replies));
    first.ServeOutbound();
    second.ServeOutbound();
    nodesIo->Start();
    inboundIo->Start();

    first.Send<TPing>(pinger, ponger, TPing{0, 0});
    const bool complete = Pump(loop, [&] { return replies.size() == sequential + burst; });
    nodesIo->Stop();
    inboundIo->Stop();
    nodesIo->Join();
    inboundIo->Join();
    // Everything that belongs to an I/O thread goes before it
    first.AddNode(2, nullptr);
    second.AddNode(1, nullptr);
    for (int i = 0; i < 2; ++i) {
        inbounds[i].reset();
        deliveries[i].reset();
    }
    resolver.reset();
    nodesIo.reset();
    inboundIo.reset();

    ASSERT_TRUE(complete);
    for (uint64_t seq = 0; seq < sequential + burst; ++seq) {
        ASSERT_EQ(replies[seq].Seq, seq);
        ASSERT_EQ(replies[seq].Check, seq * 3);
    }
}

// The I/O loop stops while clients write; then the inbound and the delivery are destroyed
// with envelopes in the queue, and with V2 the reader goes before the blobs of its chunks.
// Whether the actor side has emptied the queue by then is a matter of timing: at least one
// iteration must destroy a queue that is not empty.
template<typename TPoller, typename TReader>
void test_threaded_inbound_stop_under_load() {
    int nonEmpty = 0;
    for (int iteration = 0; iteration < 10; ++iteration) {
        TInboundSetup<TPoller, TReader> setup(4 * 1024 * 1024);
        TRecord record(2, std::is_same_v<TReader, TZeroCopyEnvelopeReaderV2>);
        const auto recorder = setup.System.Register(std::make_unique<TRecorder>(record));
        TClients<TPoller> clients(setup.Address, {MakeStream(recorder, 0, 5000), MakeStream(recorder, 1, 5000)});
        const bool running = Pump(setup.Loop, [&] { return record.Received > size_t(100 + iteration * 200); });
        setup.Stop();
        // The actor side takes some of what was handed over before the stop
        Pump(setup.Loop, [] { return false; }, std::chrono::milliseconds(iteration % 3));
        clients.Stop();
        nonEmpty += setup.Delivery.QueuedBytes() > 0;
        ASSERT_TRUE(running);
        ASSERT_TRUE(record.Error.empty()) << record.Error;
    }
    ASSERT_GT(nonEmpty, 0);
}

#endif // _WIN32

#define my_unit_test(f, a) RegisterTest(#f, #a, f<a>)
#define my_unit_test2(f, a, b) my_unit_test(f, a); my_unit_test(f, b)
#define my_unit_test3(f, a, b, c) my_unit_test2(f, a, b); my_unit_test(f, c)
#define my_unit_test4(f, a, b, c, d) my_unit_test3(f, a, b, c); my_unit_test(f, d)

#define my_reader_test(f, a) \
    RegisterTest(#f, #a "_v1", f<a, TZeroCopyEnvelopeReader>); \
    RegisterTest(#f, #a "_v2", f<a, TZeroCopyEnvelopeReaderV2>)
#define my_reader_test2(f, a, b) my_reader_test(f, a); my_reader_test(f, b)
#define my_reader_test3(f, a, b, c) my_reader_test2(f, a, b); my_reader_test(f, c)
#define my_reader_test4(f, a, b, c, d) my_reader_test3(f, a, b, c); my_reader_test(f, d)

#ifdef __linux__
#ifdef HAVE_URING
#define my_unit_poller(f) my_unit_test4(f, TSelect, TPoll, TEPoll, TUring)
#define my_reader_poller(f) my_reader_test4(f, TSelect, TPoll, TEPoll, TUring)
#else
#define my_unit_poller(f) my_unit_test3(f, TSelect, TPoll, TEPoll)
#define my_reader_poller(f) my_reader_test3(f, TSelect, TPoll, TEPoll)
#endif
#elif defined(__APPLE__) || defined(__FreeBSD__)
#define my_unit_poller(f) my_unit_test3(f, TSelect, TPoll, TKqueue)
#define my_reader_poller(f) my_reader_test3(f, TSelect, TPoll, TKqueue)
#else
#define my_unit_poller(f) my_unit_test2(f, TSelect, TPoll)
#define my_reader_poller(f) my_reader_test2(f, TSelect, TPoll)
#endif

int main(int argc, char* argv[]) {
    TInitializer init;

    testing::InitGoogleTest(&argc, argv);

#ifndef _WIN32
    ADD_TEST(my_reader_poller, test_threaded_inbound_delivers_in_order);
    ADD_TEST(my_unit_poller, test_threaded_inbound_backpressure);
    ADD_TEST(my_unit_poller, test_threaded_inbound_drops_envelopes_for_other_nodes);
    ADD_TEST(my_unit_poller, test_threaded_inbound_closes_and_forgets_connections);
    ADD_TEST(my_reader_poller, test_threaded_inbound_actor_systems);
    ADD_TEST(my_reader_poller, test_threaded_inbound_stop_under_load);
#endif

    return RUN_ALL_TESTS();
}
