#include <chrono>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <random>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#ifndef _WIN32
#include <netinet/tcp.h>
#endif
#include <signal.h>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <coroio/all.hpp>
#include <coroio/actors/actorsystem.hpp>
#include <coroio/actors/messages.hpp>
#include <coroio/actors/messages_factory.hpp>
#include <coroio/actors/queue.hpp>
#include <coroio/actors/intrusive_list.hpp>

#include "testlib.h"
#include "perf.h"

extern "C" {
#include <cmocka.h>
}

using namespace NNet;
using namespace NNet::NActors;

void test_actor_id_equality(void**) {
    const TActorId actor(1, 64, 2);
    const TActorId same(1, 64, 2);
    const TActorId otherNode(2, 64, 2);
    const TActorId otherActor(1, 65, 2);
    const TActorId otherCookie(1, 64, 3);
    assert_true(actor == same);
    assert_false(actor != same);
    assert_true(TActorId{} == TActorId{});
    assert_true(TActorId{} != actor);
    for (const auto& other : {otherNode, otherActor, otherCookie}) {
        assert_false(actor == other);
        assert_false(other == actor);
        assert_true(actor != other);
        assert_true(other != actor);
    }

    struct TCollisionHash {
        size_t operator()(const TActorId&) const { return 0; }
    };
    std::unordered_map<TActorId, int, TCollisionHash> values{
        {actor, 1}, {otherNode, 2}, {otherActor, 3}, {otherCookie, 4},
    };
    assert_int_equal(values.size(), 4);
    assert_int_equal(values.at(same), 1);
    assert_int_equal(values.at(otherNode), 2);
    assert_int_equal(values.at(otherActor), 3);
    assert_int_equal(values.at(otherCookie), 4);
    assert_int_equal(values.erase(same), 1);
    assert_int_equal(values.size(), 3);
}

void test_actor_id_hash(void**) {
    static_assert(noexcept(std::hash<TActorId>{}(TActorId{})));
    const TActorId actor(1, 64, 2);
    const TActorId same(1, 64, 2);
    assert_true(std::hash<TActorId>{}(actor) == std::hash<TActorId>{}(same));

    std::unordered_map<TActorId, int> values;
    std::unordered_set<TActorId> actors;
    const std::array ids{
        TActorId{}, actor, TActorId(2, 64, 2), TActorId(1, 65, 2), TActorId(1, 64, 3),
        TActorId(0xffff, 0xffffffff, 0xffff),
    };
    for (size_t i = 0; i < ids.size(); ++i) {
        assert_true(values.emplace(ids[i], i).second);
        assert_true(actors.insert(ids[i]).second);
    }
    assert_false(values.emplace(same, -1).second);
    assert_false(actors.insert(same).second);
    assert_int_equal(values.size(), ids.size());
    assert_int_equal(actors.size(), ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        assert_int_equal(values.at(ids[i]), i);
        assert_int_equal(actors.count(ids[i]), 1);
    }
    assert_int_equal(values.erase(same), 1);
    assert_int_equal(actors.erase(same), 1);
    assert_int_equal(values.count(actor), 0);
    assert_int_equal(actors.count(actor), 0);
}

#ifndef _WIN32
void test_actor_system_notifier_factory(void**) {
    TLoop<TDefaultPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TActorSystem system(&loop.Poller());
    auto first = system.CreateNotifier();
    auto second = system.CreateNotifier();
    auto firstWait = first->Wait();
    auto secondWait = second->Wait();
    for (int i = 0; i < 1000; ++i) {
        first->Notify();
    }
    const auto deadline = TClock::now() + std::chrono::seconds(5);
    while (!firstWait.done() && TClock::now() < deadline) {
        loop.Step();
    }
    assert_true(firstWait.done());
    firstWait.await_resume();
    assert_false(secondWait.done());
    auto next = first->Wait();
    assert_false(next.done());
    second->Notify();
    while (!secondWait.done() && TClock::now() < deadline) {
        loop.Step();
    }
    assert_true(secondWait.done());
    secondWait.await_resume();
    assert_false(next.done());
}

struct TNotifierActorState {
    std::unique_ptr<INotifier> Notifier;
    bool Waiting = false;
    int Payload = 0;
    int Observed = 0;
};

class TNotifierActor : public ICoroActor {
public:
    explicit TNotifierActor(TNotifierActorState& state)
        : State_(state)
    {}

    void AfterRegister(TActorContext::TPtr ctx) override {
        State_.Notifier = ctx->CreateNotifier();
    }

    TFuture<void> CoReceive(TMessageId, TBlob, TActorContext::TPtr ctx) override {
        State_.Waiting = true;
        co_await State_.Notifier->Wait();
        State_.Observed = State_.Payload;
        ctx->Send<TPoison>(ctx->Self());
    }

private:
    TNotifierActorState& State_;
};

void test_actor_context_notifier_factory(void**) {
    TLoop<TDefaultPoller> loop;
    loop.Poller().SetMaxDuration(std::chrono::milliseconds(1));
    TNotifierActorState first;
    TNotifierActorState second;
    TActorSystem system(&loop.Poller());
    const auto firstId = system.Register(std::make_unique<TNotifierActor>(first));
    const auto secondId = system.Register(std::make_unique<TNotifierActor>(second));
    system.Send(firstId, firstId, 100, TBlob{});
    system.Send(secondId, secondId, 100, TBlob{});
    system.Serve();
    auto stepUntil = [&](auto ready) {
        const auto deadline = TClock::now() + std::chrono::seconds(5);
        while (!ready() && TClock::now() < deadline) {
            loop.Step();
        }
        assert_true(ready());
    };
    stepUntil([&] { return first.Waiting && second.Waiting; });
    std::thread firstProducer([&] {
        first.Payload = 42;
        for (int i = 0; i < 1000; ++i) {
            first.Notifier->Notify();
        }
    });
    firstProducer.join();
    stepUntil([&] { return first.Observed == 42 && system.ActorsSize() == 1; });
    assert_int_equal(second.Observed, 0);
    std::thread secondProducer([&] {
        second.Payload = 17;
        second.Notifier->Notify();
    });
    secondProducer.join();
    stepUntil([&] { return second.Observed == 17 && system.ActorsSize() == 0; });
}
#endif

struct TState {
    bool usePerf = false;
    int maxIterations = 1000000;
    int messageSize = 0;
};

struct TPingMessage {
    static constexpr TMessageId MessageId = 100;
};

struct TPongMessage {
    static constexpr TMessageId MessageId = 200;
};

struct TAllocator {
    void* Acquire(size_t size) {
        return ::operator new(size);
    }

    void Release(void* ptr) {
        ::operator delete(ptr);
    }
};

class TPingActor : public ICoroActor {
public:
    TFuture<void> CoReceive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        std::cerr << "Received Pong message from: " << ctx->Sender().ToString() << ", message: " << counter++ << "\n";
        co_await ctx->Sleep(std::chrono::milliseconds(1000));
        ctx->Send<TPingMessage>(ctx->Sender());
        if (counter == 4) {
            ctx->Send<TPoison>(ctx->Self());
        }
        co_return;
    }

    int counter = 0;
};

class TPongActor : public IActor {
public:
    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        std::cerr << "Received Ping message from: " << ctx->Sender().ToString() << ", message: " << counter++ << "\n";
        ctx->Send<TPongMessage>(ctx->Sender());
        if (counter == 5) {
            ctx->Send<TPoison>(ctx->Self());
        }
    }

    int counter = 0;
};

class TAskerActor : public ICoroActor {
    TFuture<void> CoReceive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        std::cerr << "Asker Received message from: " << ctx->Sender().ToString() << "\n";
        auto result = co_await ctx->Ask<TPongMessage>(ctx->Sender(), TPingMessage{});
        std::cerr << "Reply received from " << ctx->Sender().ToString() << ", message: " << result.MessageId << "\n";

        ctx->Send<TPoison>(ctx->Self());
        std::cerr << "PoisonPill sent\n";

        co_return;
    }
};

class TResponderActor : public ICoroActor {
    TFuture<void> CoReceive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        std::cerr << "Responder Received message from: " << ctx->Sender().ToString() << "\n";
        co_await ctx->Sleep(std::chrono::milliseconds(1000));
        ctx->Send<TPongMessage>(ctx->Sender());

        ctx->Send<TPoison>(ctx->Self());
        std::cerr << "PoisonPill sent\n";

        co_return;
    }
};

void test_ping_pong(void**) {
    TLoop<TDefaultPoller> loop;

    TActorSystem actorSystem(&loop.Poller());

    auto pingActorId = actorSystem.Register(std::move(std::make_unique<TPingActor>()));
    auto pongActorId = actorSystem.Register(std::move(std::make_unique<TPongActor>()));
    std::cerr << "PingActor: " << pingActorId.ToString() << "\n";
    std::cerr << "PongActor: " << pongActorId.ToString() << "\n";

    actorSystem.Send<TPingMessage>(pingActorId, pongActorId);

    actorSystem.Serve();

    while (actorSystem.ActorsSize() > 0) {
        loop.Step();
    }
}

void test_ask_respond(void**) {
    TLoop<TDefaultPoller> loop;

    TActorSystem actorSystem(&loop.Poller());

    auto askerActorId = actorSystem.Register(std::move(std::make_unique<TAskerActor>()));
    auto responderActorId = actorSystem.Register(std::move(std::make_unique<TResponderActor>()));
    std::cerr << "AskerActor: " << askerActorId.ToString() << "\n";
    std::cerr << "ResponderActor: " << responderActorId.ToString() << "\n";

    actorSystem.Send<TPingMessage>(responderActorId, askerActorId);

    actorSystem.Serve();

    while (actorSystem.ActorsSize() > 0) {
        loop.Step();
    }
}

struct TAsyncWork { static constexpr TMessageId MessageId = 400; };
struct TAsyncDone { static constexpr TMessageId MessageId = 401; };
struct TAsyncGo { static constexpr TMessageId MessageId = 402; };

struct TAsyncWorker : IBehaviorActor, TBehavior<TAsyncWorker, TAsyncWork> {
    TAsyncWorker() { Become(this); }
    TFuture<void> Receive(TAsyncWork&&, TBlob, TActorContext::TPtr ctx) {
        co_await ctx->Sleep(std::chrono::milliseconds(5));
        ctx->Send<TAsyncDone>(ctx->Sender());
    }
    void HandleUnknownMessage(TMessageId, TBlob, TActorContext::TPtr) {}
};

struct TAsyncBoss : IBehaviorActor, TBehavior<TAsyncBoss, TAsyncGo, TAsyncDone> {
    TAsyncBoss(std::vector<TActorId> workers, int& done) : Workers(std::move(workers)), Done(done) { Become(this); }
    void Receive(TAsyncGo&&, TBlob, TActorContext::TPtr ctx) {
        for (auto w : Workers) {
            ctx->Send<TAsyncWork>(w);
        }
    }
    void Receive(TAsyncDone&&, TBlob, TActorContext::TPtr) { ++Done; }
    void HandleUnknownMessage(TMessageId, TBlob, TActorContext::TPtr) {}
    std::vector<TActorId> Workers;
    int& Done;
};

// Async handlers of several actors serving one sender must all complete:
// the pending future belongs to the receiving actor, not to the sender.
void test_async_handler_scatter_gather(void**) {
    TLoop<TDefaultPoller> loop;
    TActorSystem actorSystem(&loop.Poller());

    std::vector<TActorId> workers;
    for (int i = 0; i < 4; ++i) {
        workers.push_back(actorSystem.Register(std::make_unique<TAsyncWorker>()));
    }
    int done = 0;
    auto boss = actorSystem.Register(std::make_unique<TAsyncBoss>(workers, done));
    actorSystem.Send<TAsyncGo>(boss, boss);
    actorSystem.Serve();

    const auto until = TClock::now() + std::chrono::milliseconds(200);
    while (TClock::now() < until && done < 4) {
        loop.Step();
    }
    assert_int_equal(done, 4);
}

class TSingleShotActor : public IActor {
public:
    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        std::cerr << "Received Pong message from: " << ctx->Sender().ToString() << "\n";
        ctx->Send<TPingMessage>(ctx->Sender());
        ctx->Send<TPoison>(ctx->Self());
    }
};

void test_schedule(void**) {
    TLoop<TDefaultPoller> loop;

    TActorSystem actorSystem(&loop.Poller());

    auto pingActorId = actorSystem.Register(std::move(std::make_unique<TSingleShotActor>()));
    std::cerr << "PingActor: " << pingActorId.ToString() << "\n";

    auto now = std::chrono::steady_clock::now();
    auto later = now + std::chrono::milliseconds(1000);

    actorSystem.Serve();
    actorSystem.Schedule<TPingMessage>(later, pingActorId, pingActorId);

    while (actorSystem.ActorsSize() > 0) {
        loop.Step();
    }

    auto elapsed = std::chrono::steady_clock::now() - now;
    std::cerr << "Elapsed time: " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms\n";
    assert_true(elapsed >= std::chrono::milliseconds(1000));
}

void test_schedule_cancel(void**) {
    TLoop<TDefaultPoller> loop;

    TActorSystem actorSystem(&loop.Poller());

    auto pingActorId = actorSystem.Register(std::move(std::make_unique<TSingleShotActor>()));
    std::cerr << "PingActor: " << pingActorId.ToString() << "\n";

    auto now = std::chrono::steady_clock::now();
    auto later = now + std::chrono::milliseconds(1000);

    actorSystem.Serve();
    auto timer = actorSystem.Schedule<TPingMessage>(later, pingActorId, pingActorId);
    actorSystem.Cancel(timer);
    later = now + std::chrono::milliseconds(10);

    actorSystem.Schedule<TPingMessage>(later, pingActorId, pingActorId);

    while (actorSystem.ActorsSize() > 0) {
        loop.Step();
    }

    auto elapsed = std::chrono::steady_clock::now() - now;
    std::cerr << "Elapsed time: " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms\n";
    assert_true(elapsed >= std::chrono::milliseconds(10) && elapsed < std::chrono::milliseconds(1000));
}

struct TEmptyMessage {
    bool operator==(const TEmptyMessage&) const = default;
};

void test_serialize_zero_size(void**) {
    TAllocator allocator;

    // Serialize
    auto blob = SerializeNear<TEmptyMessage>(allocator);
    assert_true(blob.Size == 0);
    assert_true(blob.Data.get() == nullptr);

    // Deserialize
    auto deserialized = DeserializeNear<TEmptyMessage>(blob);
    assert_true(deserialized == TEmptyMessage{});

    void* ptr = blob.Data.get();
    auto farBlob = SerializeFar<TEmptyMessage>(std::move(blob));
    assert_true(farBlob.Data.get() == ptr);
    assert_true(farBlob.Size == 0);

    auto deserializedFar = DeserializeFar<TEmptyMessage>(farBlob);
    assert_true(deserializedFar == TEmptyMessage{});
}

struct TPodMessage {
    static constexpr TMessageId MessageId = 42;
    int field1;
    double field2;
    char field3;
    double field4;
    double field5;
};

void test_serialize_pod(void**) {
    TPodMessage msg{1, 2.0, 'c', 4.0, 5.0};
    TAllocator allocator;

    // Serialize
    auto blob = SerializeNear<TPodMessage>(allocator, std::move(msg));
    assert_true(blob.Size == sizeof(TPodMessage));

    // Deserialize
    auto& deserialized = DeserializeNear<TPodMessage>(blob);
    assert_true(deserialized.field1 == 1);
    assert_true(deserialized.field2 == 2.0);
    assert_true(deserialized.field3 == 'c');
    assert_true(deserialized.field4 == 4.0);
    assert_true(deserialized.field5 == 5.0);

    void* ptr = blob.Data.get();
    auto farBlob = SerializeFar<TPodMessage>(std::move(blob));
    assert_true(farBlob.Data.get() == ptr);

    auto& deserializedFar = DeserializeFar<TPodMessage>(farBlob);
    assert_true(deserializedFar.field1 == 1);
    assert_true(deserializedFar.field2 == 2.0);
    assert_true(deserializedFar.field3 == 'c');
    assert_true(deserializedFar.field4 == 4.0);
    assert_true(deserializedFar.field5 == 5.0);
}

struct TWrappedString {
    static constexpr TMessageId MessageId = 43;
    std::string Value;
};

namespace NNet::NActors {

template<>
size_t SerializedSize<std::string>(const std::string& obj) {
    return obj.size();
}

template<>
void SerializeTo<std::string>(const std::string& obj, std::span<char> out) {
    std::memcpy(out.data(), obj.data(), out.size());
}

template<>
void DeserializeFrom<std::string>(std::string& obj, TBlob& blob) {
    obj.assign(static_cast<const char*>(blob.Data.get()), blob.Size);
}

template<>
size_t SerializedSize<TWrappedString>(const TWrappedString& obj) {
    return obj.Value.size();
}

template<>
void SerializeTo<TWrappedString>(const TWrappedString& obj, std::span<char> out) {
    std::memcpy(out.data(), obj.Value.data(), out.size());
}

template<>
void DeserializeFrom<TWrappedString>(TWrappedString& obj, TBlob& blob) {
    obj.Value.assign(static_cast<const char*>(blob.Data.get()), blob.Size);
}

} // namespace NNet::NActors

void test_serialize_non_pod(void**) {
    TAllocator allocator;
    std::string str = "Hello, World!";
    auto size = str.size();
    auto blob = SerializeNear<std::string>(allocator, std::move(str));

    std::string& deserialized = DeserializeNear<std::string>(blob);
    assert_string_equal(deserialized.c_str(), "Hello, World!");

    void* ptr = blob.Data.get();
    auto farBlob = SerializeFar<std::string>(std::move(blob));
    assert_true(farBlob.Data.get() != ptr);
    assert_true(farBlob.Size == size);

    std::string deserializedFar = DeserializeFar<std::string>(farBlob);
    assert_string_equal(deserializedFar.c_str(), "Hello, World!");
}

void test_serialize_messages_factory(void**) {
    TMessagesFactory factory;
    TAllocator alloc;

    factory.RegisterSerializer<TPodMessage>();

    auto blob = SerializeNear<TPodMessage>(alloc, 1, 2.0, 'c', 4.0, 5.0);
    auto blobSize = blob.Size;
    auto farBlob = factory.SerializeFar(TPodMessage::MessageId, std::move(blob));
    assert_true(farBlob.Size == blobSize);

    auto& mes = DeserializeFar<TPodMessage>(farBlob);
    assert_true(mes.field1 == 1);
    assert_true(mes.field2 == 2.0);
    assert_true(mes.field3 == 'c');
    assert_true(mes.field4 == 4.0);
    assert_true(mes.field5 == 5.0);
}

void test_serialize_messages_factory_non_pod(void**) {
    TMessagesFactory factory;
    TAllocator alloc;

    factory.RegisterSerializer<TWrappedString>();

    auto blob = SerializeNear<TWrappedString>(alloc, "Hello, World!");
    auto blobSize = blob.Size;
    auto farBlob = factory.SerializeFar(TWrappedString::MessageId, std::move(blob));

    auto mes = DeserializeFar<TWrappedString>(farBlob);
    assert_string_equal(mes.Value.c_str(), "Hello, World!");
}

// An output stream like TNode's outbound buffer: Acquire reserves, Commit publishes.
struct TVectorStream {
    std::vector<char> Data;
    size_t Committed = 0;

    std::span<char> Acquire(size_t size) {
        if (Data.size() < Committed + size) {
            Data.resize(Committed + size);
        }
        return {Data.data() + Committed, size};
    }

    void Commit(size_t size) {
        Committed += size;
    }

    uint64_t Offset() const {
        return Committed;
    }
};

void test_serialize_far_inplace_round_trip(void**) {
    TVectorStream stream;
    const TActorId sender(1, 2, 3);
    const TActorId recipient(2, 5, 7);
    SerializeFarInplace<TWrappedString>(stream, sender, recipient, std::string("Hello, World!"));
    SerializeFarInplace<TWrappedString>(stream, sender, recipient, std::string());

    // The payload is exactly SerializedSize bytes after the header.
    assert_int_equal(stream.Committed, 2 * sizeof(THeader) + 13);
    THeader header;
    std::memcpy(&header, stream.Data.data(), sizeof(header));
    assert_true(header.Sender == sender);
    assert_true(header.Recipient == recipient);
    assert_int_equal(header.MessageId, TWrappedString::MessageId);
    assert_int_equal(header.Size, 13);

    TZeroCopyEnvelopeReader reader;
    reader.Push(stream.Data.data(), stream.Committed);
    auto first = reader.Pop();
    assert_true(first.has_value());
    assert_string_equal(DeserializeFar<TWrappedString>(first->Blob).Value.c_str(), "Hello, World!");
    auto second = reader.Pop();
    assert_true(second.has_value());
    assert_int_equal(second->Blob.Size, 0);
    assert_true(DeserializeFar<TWrappedString>(second->Blob).Value.empty());
    assert_false(reader.Pop().has_value());
}

// Keeps the received bytes instead of copying them.
struct TBlobView {
    static constexpr TMessageId MessageId = 44;
    TBlob::TRawPtr Bytes;
    uint32_t Size = 0;
};

namespace NNet::NActors {

template<>
void DeserializeFrom<TBlobView>(TBlobView& obj, TBlob& blob) {
    obj.Size = blob.Size;
    obj.Bytes = std::move(blob.Data);
}

} // namespace NNet::NActors

void test_deserialize_far_takes_blob(void**) {
    const std::string text = "kept, not copied";
    TBlob blob{
        .Data = TBlob::TRawPtr(::operator new(text.size()), TBlobDeleter{[](void* ptr) {
            ::operator delete(ptr);
        }}),
        .Size = static_cast<uint32_t>(text.size()),
        .Type = TBlob::PointerType::Far,
    };
    std::memcpy(blob.Data.get(), text.data(), text.size());
    void* bytes = blob.Data.get();

    auto message = DeserializeFar<TBlobView>(blob);
    assert_true(message.Bytes.get() == bytes);
    assert_int_equal(message.Size, text.size());
    assert_true(blob.Data.get() == nullptr);
    assert_int_equal(blob.Size, 0);
    assert_memory_equal(message.Bytes.get(), text.data(), text.size());
}

// Claims a payload one byte over the 32-bit size field of THeader.
struct TOversizedMessage {
    static constexpr TMessageId MessageId = 46;
    std::string Text;
};

namespace NNet::NActors {

template<>
size_t SerializedSize<TOversizedMessage>(const TOversizedMessage&) {
    return size_t(1) << 32;
}

template<>
void SerializeTo<TOversizedMessage>(const TOversizedMessage&, std::span<char>) {
    fail_msg("SerializeTo must not run for an oversized payload");
}

} // namespace NNet::NActors

void test_serialize_far_rejects_oversized_payload(void**) {
    TVectorStream stream;
    bool thrown = false;
    try {
        SerializeFarInplace<TOversizedMessage>(stream, TActorId(1, 2, 3), TActorId(2, 5, 7), std::string("x"));
    } catch (const std::length_error&) {
        thrown = true;
    }
    assert_true(thrown);
    // The size is checked before anything is reserved.
    assert_true(stream.Data.empty());
    assert_int_equal(stream.Committed, 0);
}

void test_unbounded_vector_queue(void**) {
    NNet::NActors::TUnboundedVectorQueue<int> queue(8);

    for (int i = 0; i < 10; ++i) {
        queue.Push(int(i));
    }

    assert_true(queue.Size() == 10);
    assert_false(queue.Empty());

    assert_true(queue.Front() == 0);

    for (int i = 0; i < 10; ++i) {
        assert_true(queue.Front() == i);
        queue.Pop();
    }

    assert_true(queue.Size() == 0);
    assert_true(queue.Empty());


    for (int j = 5; j < 15; ++j) {
        for (int i = 0; i < j; ++i) {
            queue.Push(int(i));
        }

        assert_true(queue.Size() == j);

        for (int i = 0; i < j; ++i) {
            assert_true(queue.Front() == i);
            queue.Pop();
        }

        assert_true(queue.Size() == 0);
        assert_true(queue.Empty());
    }
}

struct TLocalMessage {
    using LocalMessage = void;
    static constexpr TMessageId MessageId = 300;
    std::unique_ptr<int> Value;
};

static_assert(is_local_message_v<TLocalMessage>);
static_assert(!is_pod_v<TLocalMessage>);

struct TLocalBehavior : public TBehavior<TLocalBehavior, TLocalMessage> {
    int Received = 0;
    int Unknown = 0;

    void Receive(TLocalMessage&& message, TBlob, TActorContext::TPtr) {
        Received = *message.Value;
    }

    void HandleUnknownMessage(TMessageId, TBlob, TActorContext::TPtr) {
        ++Unknown;
    }
};

void test_local_message(void**) {
    TAllocator alloc;
    TMessagesFactory factory;
    factory.RegisterSerializer<TLocalMessage>();

    TLocalBehavior behavior;
    IBehavior& dispatcher = behavior;
    dispatcher.Receive(TLocalMessage::MessageId,
        SerializeNear<TLocalMessage>(alloc, std::make_unique<int>(42)), {});
    assert_int_equal(behavior.Received, 42);

    dispatcher.Receive(TLocalMessage::MessageId,
        TBlob{nullptr, 0, TBlob::PointerType::Far}, {});
    assert_int_equal(behavior.Unknown, 1);

    bool rejected = false;
    try {
        factory.SerializeFar(TLocalMessage::MessageId,
            SerializeNear<TLocalMessage>(alloc, std::make_unique<int>(7)));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert_true(rejected);

    TActorSystem actorSystem(nullptr);
    actorSystem.Send<TLocalMessage>(TActorId(), TActorId(2, 1, 1), std::make_unique<int>(7));
}

struct TRichBehavior : public TBehavior<TRichBehavior, TWrappedString, TPodMessage> {
    TPodMessage PodReceived;
    TWrappedString StrReceived;

    void Receive(TWrappedString&& message, TBlob blob, TActorContext::TPtr ctx) {
        StrReceived = std::move(message);
    }

    TFuture<void> Receive(TPodMessage&& message, TBlob blob, TActorContext::TPtr ctx) {
        PodReceived = std::move(message);
        co_return;
    }

    void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) {
        std::cerr << "Unknown message received: " << messageId << "\n";
    }
};

void test_behavior(void**) {
    TAllocator alloc;
    TActorSystem actorSystem(nullptr);
    auto behavior = std::unique_ptr<IBehavior>(new TRichBehavior());
    auto strNearBlob = SerializeNear<TWrappedString>(alloc, "Hello, World!");

    TActorContext::TPtr ctx;
    ctx.reset(new (&actorSystem) TMockActorContext(TActorId(), TActorId(), &actorSystem));
    behavior->Receive(TWrappedString::MessageId, std::move(strNearBlob), std::move(ctx));

    auto podNearBlob = SerializeNear<TPodMessage>(alloc, 42, 3.14, 'x', 2.71, 1.618);
    ctx.reset(new (&actorSystem) TMockActorContext(TActorId(), TActorId(), &actorSystem));
    behavior->Receive(TPodMessage::MessageId, std::move(podNearBlob), std::move(ctx));

    auto& richBehavior = static_cast<TRichBehavior&>(*behavior);
    assert_true(richBehavior.StrReceived.Value == "Hello, World!");
    assert_true(richBehavior.PodReceived.field1 == 42);
    assert_true(richBehavior.PodReceived.field2 == 3.14);
    assert_true(richBehavior.PodReceived.field3 == 'x');
    assert_true(richBehavior.PodReceived.field4 == 2.71);
    assert_true(richBehavior.PodReceived.field5 == 1.618);
}

struct TMyActor : public IBehaviorActor,
                  public TBehavior<TMyActor, TWrappedString, TPodMessage> {

    TPodMessage PodReceived;
    TWrappedString StrReceived;

    struct TEmptyBehavior : public TBehavior<TEmptyBehavior> {
        void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) {
            std::cerr << "EmptyBehavior received unknown message: " << messageId << "\n";
        }
    };

    TEmptyBehavior EmptyBehavior;

    TMyActor() {
        Become(this);
    }

    void Receive(TWrappedString&& message, TBlob blob, TActorContext::TPtr ctx) {
        StrReceived = std::move(message);
    }
    TFuture<void> Receive(TPodMessage&& message, TBlob blob, TActorContext::TPtr ctx) {
        PodReceived = std::move(message);
        Become(&EmptyBehavior);
        co_return;
    }
    void HandleUnknownMessage(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) {}
};

void test_behavior_actor(void**) {
    TAllocator alloc;
    TActorSystem actorSystem(nullptr);

    IActor::TPtr actor = std::make_unique<TMyActor>();

    TActorContext::TPtr ctx;
    ctx.reset(new (&actorSystem) TMockActorContext(TActorId(), TActorId(), &actorSystem));
    auto strNearBlob = SerializeNear<TWrappedString>(alloc, "Hello, World!");
    actor->Receive(TWrappedString::MessageId, std::move(strNearBlob), std::move(ctx));

    auto podNearBlob = SerializeNear<TPodMessage>(alloc, 42, 3.14, 'x', 2.71, 1.618);
    ctx.reset(new (&actorSystem) TMockActorContext(TActorId(), TActorId(), &actorSystem));
    actor->Receive(TPodMessage::MessageId, std::move(podNearBlob), std::move(ctx));

    auto check = [&] () {
        auto& myActor = static_cast<TMyActor&>(*actor);
        assert_true(myActor.StrReceived.Value == "Hello, World!");
        assert_true(myActor.PodReceived.field1 == 42);
        assert_true(myActor.PodReceived.field2 == 3.14);
        assert_true(myActor.PodReceived.field3 == 'x');
        assert_true(myActor.PodReceived.field4 == 2.71);
        assert_true(myActor.PodReceived.field5 == 1.618);
    };

    check();

    podNearBlob = SerializeNear<TPodMessage>(alloc, 41, 1.14, 'y', 1.71, 2.618);
    ctx.reset(new (&actorSystem) TMockActorContext(TActorId(), TActorId(), &actorSystem));
    actor->Receive(TPodMessage::MessageId, std::move(podNearBlob), std::move(ctx));

    check();
}

void test_envelope_reader(void**) {
    TAllocator alloc;
    TZeroCopyEnvelopeReader reader(64);
    assert_true(reader.Size() == 0);

    for (int i = 0; i < 2; ++i) {
        THeader header {
            .Sender = TActorId(1, 1, 1),
            .Recipient = TActorId(1, 2, 2),
            .MessageId = TMessageId(i),
        };
        auto buffer = reader.Acquire(sizeof(THeader));
        assert_true(buffer.size() == sizeof(THeader));
        std::memcpy(buffer.data(), &header, sizeof(THeader));
        reader.Commit(sizeof(THeader));
    }

    auto envelope = reader.Pop();
    assert_true(envelope.has_value());
    assert_true(envelope->Sender == TActorId(1, 1, 1));
    assert_true(envelope->Recipient == TActorId(1, 2, 2));
    assert_true(envelope->MessageId == 0);
    assert_true(envelope->Blob.Size == 0);

    envelope = reader.Pop();
    assert_true(envelope.has_value());
    assert_true(envelope->Sender == TActorId(1, 1, 1));
    assert_true(envelope->Recipient == TActorId(1, 2, 2));
    assert_true(envelope->MessageId == 1);
    assert_true(envelope->Blob.Size == 0);

    {
        THeader header {
            .Sender = TActorId(1, 1, 1),
            .Recipient = TActorId(1, 2, 2),
            .MessageId = TMessageId(2),
            .Size = 0
        };
        auto buffer = reader.Acquire(sizeof(THeader));
        assert_true(buffer.size() == 16);
        std::memcpy(buffer.data(), &header, 16);
        reader.Commit(buffer.size());

        buffer = reader.Acquire(8);
        assert_true(buffer.size() == 8);
        std::memcpy(buffer.data(), ((char*)&header) + 16, 8);
        reader.Commit(buffer.size());
    }

    envelope = reader.Pop();
    assert_true(envelope.has_value());
    assert_true(envelope->Sender == TActorId(1, 1, 1));
    assert_true(envelope->Recipient == TActorId(1, 2, 2));
    assert_true(envelope->MessageId == 2);
    assert_true(envelope->Blob.Size == 0);

    for (int i = 0; i < 10; ++i) {
        auto nearBlob = SerializeNear<TWrappedString>(alloc, "Message " + std::to_string(i));
        auto farBlob = SerializeFar<TWrappedString>(std::move(nearBlob));
        THeader header {
            .Sender = TActorId(1, 1, 1),
            .Recipient = TActorId(1, 2, 2),
            .MessageId = TMessageId(i),
            .Size = farBlob.Size
        };
        reader.Push(reinterpret_cast<const char*>(&header), sizeof(THeader));
        reader.Push(static_cast<const char*>(farBlob.Data.get()), farBlob.Size);
    }

    for (int i = 0; i < 10; ++i) {
        auto envelope = reader.Pop();
        assert_true(envelope.has_value());
        assert_true(envelope->MessageId == i);
        auto&& str = DeserializeFar<TWrappedString>(envelope->Blob);
        assert_string_equal(str.Value.c_str(), ("Message " + std::to_string(i)).c_str());
    }
}

void test_envelope_reader_v2(void**) {
    TAllocator alloc;
    TZeroCopyEnvelopeReaderV2 reader(64, 32);
    assert_true(reader.Size() == 0);

    for (int i = 0; i < 2; ++i) {
        THeader header {
            .Sender = TActorId(1, 1, 1),
            .Recipient = TActorId(1, 2, 2),
            .MessageId = TMessageId(i),
        };
        auto buffer = reader.Acquire(sizeof(THeader));
        assert_true(buffer.size() == sizeof(THeader));
        std::memcpy(buffer.data(), &header, sizeof(THeader));
        reader.Commit(sizeof(THeader));
    }

    assert_true(reader.Size() == 2*sizeof(THeader));

    assert_true(reader.UsedChunksCount() == 0);
    auto envelope = reader.Pop();
    assert_true(reader.UsedChunksCount() == 0);

    assert_true(envelope.has_value());
    assert_true(envelope->Sender == TActorId(1, 1, 1));
    assert_true(envelope->Recipient == TActorId(1, 2, 2));
    assert_true(envelope->MessageId == 0);
    assert_true(envelope->Blob.Size == 0);

    envelope = reader.Pop();
    assert_true(reader.UsedChunksCount() == 0);

    assert_true(envelope.has_value());
    assert_true(envelope->Sender == TActorId(1, 1, 1));
    assert_true(envelope->Recipient == TActorId(1, 2, 2));
    assert_true(envelope->MessageId == 1);
    assert_true(envelope->Blob.Size == 0);

    {
        THeader header {
            .Sender = TActorId(1, 1, 1),
            .Recipient = TActorId(1, 2, 2),
            .MessageId = TMessageId(2),
            .Size = 0
        };
        auto buffer = reader.Acquire(sizeof(THeader));
        assert_true(buffer.size() == 24);
        std::memcpy(buffer.data(), &header, sizeof(THeader));
        reader.Commit(buffer.size());
    }

    envelope = reader.Pop();
    assert_true(reader.UsedChunksCount() == 0);
    assert_true(envelope.has_value());
    assert_true(envelope->Sender == TActorId(1, 1, 1));
    assert_true(envelope->Recipient == TActorId(1, 2, 2));
    assert_true(envelope->MessageId == 2);
    assert_true(envelope->Blob.Size == 0);

    for (int i = 0; i < 10; ++i) {
        auto nearBlob = SerializeNear<TWrappedString>(alloc, "Message " + std::to_string(i));
        auto farBlob = SerializeFar<TWrappedString>(std::move(nearBlob));
        THeader header {
            .Sender = TActorId(1, 1, 1),
            .Recipient = TActorId(1, 2, 2),
            .MessageId = TMessageId(i),
            .Size = farBlob.Size
        };
        reader.Push(reinterpret_cast<const char*>(&header), sizeof(THeader));
        reader.Push(static_cast<const char*>(farBlob.Data.get()), farBlob.Size);
    }

    for (int i = 0; i < 10; ++i) {
        auto envelope = reader.Pop();
        if (i == 0) {
            assert_int_equal(reader.UsedChunksCount(), 1);
        }
        assert_true(envelope.has_value());
        assert_true(envelope->MessageId == i);
        auto&& str = DeserializeFar<TWrappedString>(envelope->Blob);
        assert_string_equal(str.Value.c_str(), ("Message " + std::to_string(i)).c_str());
    }
}

void test_envelope_reader_microbenchmark(void** arg) {
    TState* state = static_cast<TState*>(*arg);
    TAllocator alloc;
    TZeroCopyEnvelopeReader v1;
    TZeroCopyEnvelopeReaderV2 v2(1024 * 1024 * 1024, 2 * sizeof(THeader));
    const int maxIterations = state->maxIterations;

    std::vector<char> testBuffer(state->messageSize);

    auto pushLambda = [&](auto& reader) {
        for (int i = 0; i < maxIterations; ++i) {
            THeader header {
                .Sender = TActorId(1, 1, 1),
                .Recipient = TActorId(1, 2, 2),
                .MessageId = 1000,
                .Size = static_cast<uint32_t>(state->messageSize)
            };
            reader.Push(reinterpret_cast<const char*>(&header), sizeof(THeader));
            reader.Push(reinterpret_cast<const char*>(testBuffer.data()), state->messageSize);
        }
    };

    auto popLambda = [&](auto& reader) {
        for (int i = 0; i < maxIterations; ++i) {
            auto envelope = reader.Pop();
        }
    };

    TPerfWrapper perf1("v1_pop.data", {});

    auto t1 = std::chrono::steady_clock::now();
    pushLambda(v1);
    auto t2 = std::chrono::steady_clock::now();
    if (state->usePerf) {
        perf1.Profile([&]() {
            popLambda(v1);
        });
    } else {
        popLambda(v1);
    }
    auto t3 = std::chrono::steady_clock::now();

    auto elapsedV1 = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    auto elapsedV1Pop = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
    std::cerr << "V1 Push: " << elapsedV1 << " ms, Pop: " << elapsedV1Pop << " ms\n";

    TPerfWrapper perf2("v2_pop.data", {});

    auto t4 = std::chrono::steady_clock::now();
    pushLambda(v2);
    auto t5 = std::chrono::steady_clock::now();
    if (state->usePerf) {
        perf2.Profile([&]() {
            popLambda(v2);
        });
    } else {
        popLambda(v2);
    }
    auto t6 = std::chrono::steady_clock::now();

    auto elapsedV2 = std::chrono::duration_cast<std::chrono::milliseconds>(t5 - t4).count();
    auto elapsedV2Pop = std::chrono::duration_cast<std::chrono::milliseconds>(t6 - t5).count();
    std::cerr << "V2 Push: " << elapsedV2 << " ms, Pop: " << elapsedV2Pop << " ms\n";
}

void test_envelope_reader_v2_recycled_chunk(void**) {
    constexpr size_t payloadSize = 32;
    // A chunk holds exactly one header and one payload
    TZeroCopyEnvelopeReaderV2 reader(sizeof(THeader) + payloadSize, 16);

    auto feed = [&](int id) {
        THeader header {
            .Sender = TActorId(1, 1, 1),
            .Recipient = TActorId(1, 2, 2),
            .MessageId = TMessageId(id),
            .Size = payloadSize,
        };
        char buf[sizeof(THeader) + payloadSize];
        std::memset(buf, 'x', sizeof(buf));
        std::memcpy(buf, &header, sizeof(THeader));
        reader.Push(buf, sizeof(buf));
    };

    feed(1);
    auto e1 = reader.Pop();
    assert_true(e1.has_value());
    feed(2);
    assert_true(reader.UsedChunksCount() == 1);
    e1.reset();
    assert_true(reader.UsedChunksCount() == 0);
    feed(3);
    auto e2 = reader.Pop();
    assert_true(e2.has_value());
    auto e3 = reader.Pop();
    assert_true(e3.has_value());
    assert_true(e3->MessageId == 3);
    e3.reset();
    e2.reset();
    assert_true(reader.UsedChunksCount() == 0);
}

void test_envelope_reader_v2_copyout_live_blob(void**) {
    constexpr size_t H = sizeof(THeader);
    constexpr size_t P = 32;

    auto makeMessage = [](int id, char fill) {
        THeader header {
            .Sender = TActorId(1, 1, 1),
            .Recipient = TActorId(1, 2, 2),
            .MessageId = TMessageId(id),
            .Size = P,
        };
        std::string msg(H + P, fill);
        std::memcpy(msg.data(), &header, H);
        return msg;
    };

    auto check = [](const TEnvelope& e, char expected) {
        const char* data = static_cast<const char*>(e.Blob.Data.get());
        for (size_t i = 0; i < P; ++i) {
            assert_true(data[i] == expected);
        }
    };

    // The header is split across chunks, CopyOut drains a chunk with a live blob
    {
        TZeroCopyEnvelopeReaderV2 r(H + P + 8, 4);
        auto m1 = makeMessage(1, 'A');
        auto m2 = makeMessage(2, 'B');
        auto m3 = makeMessage(3, 'C');
        r.Push(m1.data(), m1.size());
        r.Push(m2.data(), 8);
        auto e1 = r.Pop();
        assert_true(e1.has_value());
        r.Push(m2.data() + 8, m2.size() - 8);
        auto e2 = r.Pop();
        assert_true(e2.has_value());
        r.Push(m3.data(), m3.size());
        r.Push(m3.data(), m3.size());
        check(*e1, 'A');
        assert_true(r.UsedChunksCount() == 1);
        e1.reset();
        assert_true(r.UsedChunksCount() == 0);
    }

    // The payload is split across chunks, the copying CopyOut drains a chunk with a live blob
    {
        TZeroCopyEnvelopeReaderV2 r(H + P + H + 8, 4);
        auto m1 = makeMessage(1, 'A');
        auto m2 = makeMessage(2, 'B');
        auto m3 = makeMessage(3, 'C');
        r.Push(m1.data(), m1.size());
        r.Push(m2.data(), H + 8);
        auto e1 = r.Pop();
        assert_true(e1.has_value());
        assert_true(!r.Pop().has_value());
        r.Push(m2.data() + H + 8, P - 8);
        auto e2 = r.Pop();
        assert_true(e2.has_value());
        check(*e2, 'B');
        for (int i = 0; i < 3; ++i) {
            r.Push(m3.data(), m3.size());
        }
        check(*e1, 'A');
        assert_true(r.UsedChunksCount() == 1);
        e1.reset();
        assert_true(r.UsedChunksCount() == 0);
    }

    // The header ends exactly at the chunk end, the template CopyOut drains a chunk with a live blob
    {
        TZeroCopyEnvelopeReaderV2 r(H + P + H, 4);
        auto m1 = makeMessage(1, 'A');
        auto m2 = makeMessage(2, 'B');
        auto m3 = makeMessage(3, 'C');
        r.Push(m1.data(), m1.size());
        r.Push(m2.data(), H);
        auto e1 = r.Pop();
        assert_true(e1.has_value());
        r.Push(m2.data() + H, P);
        auto e2 = r.Pop();
        assert_true(e2.has_value());
        check(*e2, 'B');
        r.Push(m3.data(), m3.size());
        r.Push(m3.data(), m3.size());
        check(*e1, 'A');
        assert_true(r.UsedChunksCount() == 1);
        e1.reset();
        assert_true(r.UsedChunksCount() == 0);
    }
}

void test_envelope_reader_v2_blob_outlives_reader(void**) {
    constexpr size_t H = sizeof(THeader);
    constexpr size_t P = 32;

    auto makeMessage = [](int id, char fill) {
        std::vector<char> m(H + P, fill);
        THeader header {
            .Sender = TActorId(1, 1, 1),
            .Recipient = TActorId(1, 2, 2),
            .MessageId = TMessageId(id),
            .Size = P,
        };
        std::memcpy(m.data(), &header, H);
        return m;
    };
    auto check = [](const TEnvelope& e, char fill) {
        assert_true(e.Blob.Size == P);
        const char* d = static_cast<const char*>(e.Blob.Data.get());
        for (size_t i = 0; i < P; ++i) {
            assert_true(d[i] == fill);
        }
    };

    // One chunk per message: blobs live in the used, sealed and current chunks
    std::optional<TEnvelope> e1, e2, e3;
    {
        TZeroCopyEnvelopeReaderV2 r(H + P, 4);
        auto m1 = makeMessage(1, 'A');
        auto m2 = makeMessage(2, 'B');
        auto m3 = makeMessage(3, 'C');
        r.Push(m1.data(), m1.size());
        e1 = r.Pop();
        r.Push(m2.data(), m2.size());
        e2 = r.Pop();
        r.Push(m3.data(), m3.size());
        e3 = r.Pop();
        assert_true(e1 && e2 && e3);
    }
    check(*e3, 'C');
    check(*e1, 'A');
    e1.reset();
    check(*e2, 'B');
    e2.reset();
    e3.reset();
}

void test_envelope_reader_v2_sealed_blob_outlives_reader(void**) {
    constexpr size_t H = sizeof(THeader);
    constexpr size_t P = 32;

    auto makeMessage = [](int id, char fill) {
        std::vector<char> m(H + P, fill);
        THeader header {
            .Sender = TActorId(1, 1, 1),
            .Recipient = TActorId(1, 2, 2),
            .MessageId = TMessageId(id),
            .Size = P,
        };
        std::memcpy(m.data(), &header, H);
        return m;
    };

    // Two messages per chunk: the third one seals the chunk with the unread second message
    std::optional<TEnvelope> e1;
    {
        TZeroCopyEnvelopeReaderV2 r(2 * (H + P), 4);
        auto m1 = makeMessage(1, 'A');
        auto m2 = makeMessage(2, 'B');
        auto m3 = makeMessage(3, 'C');
        r.Push(m1.data(), m1.size());
        r.Push(m2.data(), m2.size());
        e1 = r.Pop();
        assert_true(e1.has_value());
        r.Push(m3.data(), m3.size());
    }
    assert_true(e1->Blob.Size == P);
    const char* d = static_cast<const char*>(e1->Blob.Data.get());
    for (size_t i = 0; i < P; ++i) {
        assert_true(d[i] == 'A');
    }
    e1.reset();
}

// Payload of message `id`: V2PayloadSize(id) bytes equal to char(id)
static size_t V2PayloadSize(uint32_t id) {
    return (id * 37) % 301;
}

static void AppendV2Message(std::vector<char>& stream, uint32_t id) {
    THeader header {
        .Sender = TActorId(1, 1, 1),
        .Recipient = TActorId(1, 2, 2),
        .MessageId = TMessageId(id),
        .Size = static_cast<uint32_t>(V2PayloadSize(id)),
    };
    const char* p = reinterpret_cast<const char*>(&header);
    stream.insert(stream.end(), p, p + sizeof(header));
    stream.insert(stream.end(), header.Size, static_cast<char>(id));
}

static bool V2PayloadIsValid(const TEnvelope& e) {
    if (e.Blob.Size != V2PayloadSize(e.MessageId)) {
        return false;
    }
    const char* d = static_cast<const char*>(e.Blob.Data.get());
    for (size_t i = 0; i < e.Blob.Size; ++i) {
        if (d[i] != static_cast<char>(e.MessageId)) {
            return false;
        }
    }
    return true;
}

// Commits bytes in pieces of random size, like socket reads
static size_t FeedV2(TZeroCopyEnvelopeReaderV2& reader, const std::vector<char>& stream, size_t pos, std::mt19937& rng) {
    size_t piece = std::min<size_t>(1 + rng() % 1500, stream.size() - pos);
    auto buf = reader.Acquire(piece);
    size_t size = std::min(piece, buf.size());
    std::memcpy(buf.data(), stream.data() + pos, size);
    reader.Commit(size);
    return pos + size;
}

void test_envelope_reader_v2_blobs_released_on_other_threads(void**) {
    constexpr int messages = 100000;
    constexpr int workers = 3;
    // Blobs held by each worker, so they outlive chunk rotations
    constexpr size_t held = 64;

    std::vector<char> stream;
    for (int i = 0; i < messages; ++i) {
        AppendV2Message(stream, i);
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<TEnvelope> queue;
    bool done = false;
    std::atomic<int> received = 0;
    std::atomic<int> invalid = 0;

    std::vector<std::thread> threads;
    for (int w = 0; w < workers; ++w) {
        threads.emplace_back([&] {
            std::deque<TEnvelope> blobs;
            while (true) {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return done || !queue.empty(); });
                if (queue.empty()) {
                    break;
                }
                blobs.push_back(std::move(queue.front()));
                queue.pop_front();
                lock.unlock();
                cv.notify_all();
                if (!V2PayloadIsValid(blobs.back())) {
                    ++invalid;
                }
                ++received;
                if (blobs.size() > held) {
                    blobs.pop_front();
                }
            }
        });
    }

    TZeroCopyEnvelopeReaderV2 reader(4096, 64);
    std::mt19937 rng(42);
    size_t pos = 0;
    while (pos < stream.size()) {
        pos = FeedV2(reader, stream, pos, rng);
        while (auto e = reader.Pop()) {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return queue.size() < 1024; });
            queue.push_back(std::move(*e));
            lock.unlock();
            cv.notify_all();
        }
    }
    {
        std::lock_guard lock(mutex);
        done = true;
    }
    cv.notify_all();
    for (auto& t : threads) {
        t.join();
    }
    assert_int_equal(received.load(), messages);
    assert_int_equal(invalid.load(), 0);
    assert_int_equal(reader.UsedChunksCount(), 0);
}

void test_envelope_reader_v2_reader_dies_while_blobs_released(void**) {
    constexpr int rounds = 200;
    constexpr int workers = 3;

    std::vector<char> stream;
    for (int i = 0; i < 400; ++i) {
        AppendV2Message(stream, i);
    }

    std::mt19937 rng(7);
    std::atomic<int> invalid = 0;
    for (int round = 0; round < rounds; ++round) {
        auto reader = std::make_unique<TZeroCopyEnvelopeReaderV2>(2048, 64);
        // Cut the stream inside a message, so a sealed chunk may keep unread bytes
        std::vector<char> prefix(stream.begin(), stream.begin() + stream.size() / 2 + rng() % (stream.size() / 2));
        std::vector<std::vector<TEnvelope>> batches(workers);
        size_t pos = 0;
        int count = 0;
        while (pos < prefix.size()) {
            pos = FeedV2(*reader, prefix, pos, rng);
            while (auto e = reader->Pop()) {
                batches[count++ % workers].push_back(std::move(*e));
            }
        }

        std::atomic<bool> start = false;
        std::vector<std::thread> threads;
        for (auto& batch : batches) {
            threads.emplace_back([&] {
                while (!start.load()) {
                    std::this_thread::yield();
                }
                for (auto& e : batch) {
                    if (!V2PayloadIsValid(e)) {
                        ++invalid;
                    }
                    e = {};
                }
            });
        }
        start = true;
        reader.reset();
        for (auto& t : threads) {
            t.join();
        }
    }
    assert_int_equal(invalid.load(), 0);
}

struct TMyNode : public TIntrusiveListNode<TMyNode> {
    int Value;

    TMyNode(int value) : Value(value) {}
};

void test_intrusive_list(void**) {
    using TList = TIntrusiveList<TMyNode>;
    TList list;

    for (int i = 0; i < 10; ++i) {
        list.PushBack(std::make_unique<TMyNode>(i));
    }

    int i = 0;
    while (list.Front()) {
        auto node = list.Erase(list.Front());
        assert_int_equal(node->Value, i);
        i++;
    }
    assert_true(list.Size() == 0);
}

void test_node_reconnects_to_dead_peer(void**) {
    using TPoller = TDefaultPoller;
    TLoop<TPoller> loop;
    auto& poller = loop.Poller();
    TResolver resolver(TAddress{"127.0.0.1", 53}, poller);
    TMessagesFactory factory;
    int port = GetFreePort();

    TNode<TPoller> node(poller, factory, resolver,
        [&poller](const TAddress& addr) { return TPoller::TSocket(poller, addr.Domain()); },
        THostPort("127.0.0.1", port));

    const TActorId sender(1, 2, 3);
    const TActorId recipient(2, 5, 7);
    node.Send(TEnvelope{.Sender = sender, .Recipient = recipient, .MessageId = 42});
    // Nobody listens yet: the connect is refused and the node keeps retrying.
    node.Drain();

    THeader received;
    bool done = false;
    // The listener appears only after the first attempts have failed.
    auto server = [](TPoller& poller, int port, THeader& received, bool& done) -> TFuture<void> {
        co_await poller.Sleep(TClock::now() + std::chrono::milliseconds(300));
        TAddress addr{"127.0.0.1", port};
        TPoller::TSocket listener(poller, addr.Domain());
        listener.Bind(addr);
        listener.Listen();
        auto client = (co_await listener.Accept()).value();
        TByteReader reader(client);
        co_await reader.Read(&received, sizeof(received));
        done = true;
    }(poller, port, received, done);

    auto deadline = TClock::now() + std::chrono::seconds(10);
    while (!done && TClock::now() < deadline) {
        loop.Step();
        node.Drain();
    }

    assert_true(done);
    // The first bytes on the wire are the real message, not a connection probe.
    assert_true(received.Sender == sender);
    assert_true(received.Recipient == recipient);
    assert_int_equal(received.MessageId, 42);
    assert_int_equal(received.Size, 0);
}

// Bytes queued while the node connects are flushed when the connection completes,
// without another Drain() call.
void test_node_flushes_bytes_queued_while_connecting(void**) {
    using TPoller = TDefaultPoller;
    TLoop<TPoller> loop;
    auto& poller = loop.Poller();
    TResolver resolver(TAddress{"127.0.0.1", 53}, poller);
    TMessagesFactory factory;
    int port = GetFreePort();

    TNode<TPoller> node(poller, factory, resolver,
        [&poller](const TAddress& addr) { return TPoller::TSocket(poller, addr.Domain()); },
        THostPort("127.0.0.1", port));

    const TActorId sender(1, 2, 3);
    const TActorId recipient(2, 5, 7);
    node.Send(TEnvelope{.Sender = sender, .Recipient = recipient, .MessageId = 42});
    // The only Drain(): it starts connecting while nobody listens yet.
    node.Drain();

    THeader received;
    bool done = false;
    auto server = [](TPoller& poller, int port, THeader& received, bool& done) -> TFuture<void> {
        co_await poller.Sleep(TClock::now() + std::chrono::milliseconds(300));
        TAddress addr{"127.0.0.1", port};
        TPoller::TSocket listener(poller, addr.Domain());
        listener.Bind(addr);
        listener.Listen();
        auto client = (co_await listener.Accept()).value();
        TByteReader reader(client);
        co_await reader.Read(&received, sizeof(received));
        done = true;
    }(poller, port, received, done);

    auto deadline = TClock::now() + std::chrono::seconds(10);
    while (!done && TClock::now() < deadline) {
        loop.Step();
    }

    assert_true(done);
    assert_int_equal(received.MessageId, 42);
}

void test_node_sets_tcp_nodelay(void**) {
    using TPoller = TDefaultPoller;
    TLoop<TPoller> loop;
    auto& poller = loop.Poller();
    TResolver resolver(TAddress{"127.0.0.1", 53}, poller);
    TMessagesFactory factory;
    const int port = GetFreePort();
    TAddress address{"127.0.0.1", port};
    TPoller::TSocket listener(poller, address.Domain());
    listener.Bind(address);
    listener.Listen();

    int fd = -1;
    TNode<TPoller> node(poller, factory, resolver,
        [&poller, &fd](const TAddress& addr) {
            TPoller::TSocket socket(poller, addr.Domain());
            fd = socket.Fd();
            return socket;
        },
        THostPort("127.0.0.1", port));
    node.Send(TEnvelope{.Sender = TActorId(1, 2, 3), .Recipient = TActorId(2, 5, 7), .MessageId = 42});

    bool done = false;
    auto server = [](TPoller::TSocket& listener, bool& done) -> TFuture<void> {
        auto client = (co_await listener.Accept()).value();
        THeader header;
        TByteReader reader(client);
        co_await reader.Read(&header, sizeof(header));
        done = true;
    }(listener, done);

    const auto deadline = TClock::now() + std::chrono::seconds(10);
    while (!done && TClock::now() < deadline) {
        loop.Step();
        node.Drain();
    }
    assert_true(done);

    int value = 0;
    socklen_t len = sizeof(value);
    assert_int_equal(getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char*) &value, &len), 0);
    assert_true(value != 0);
}

// Destroying a node whose write is blocked must cancel the write before the send buffer
// is freed (the kernel reads it on io_uring); run under ASan on every poller.
void test_node_destroyed_during_blocked_write(void**) {
    using TPoller = TDefaultPoller;
    TLoop<TPoller> loop;
    auto& poller = loop.Poller();
    TResolver resolver(TAddress{"127.0.0.1", 53}, poller);
    TMessagesFactory factory;
    const int port = GetFreePort();
    TAddress address{"127.0.0.1", port};
    TPoller::TSocket listener(poller, address.Domain());
    listener.Bind(address);
    listener.Listen();

    std::optional<TPoller::TSocket> accepted;
    auto server = [](TPoller::TSocket& listener, std::optional<TPoller::TSocket>& accepted) -> TFuture<void> {
        // The peer accepts and never reads, so the node's write stays blocked.
        accepted.emplace((co_await listener.Accept()).value());
    }(listener, accepted);

    {
        TNode<TPoller> node(poller, factory, resolver,
            [&poller](const TAddress& addr) { return TPoller::TSocket(poller, addr.Domain()); },
            THostPort("127.0.0.1", port));
        node.Acquire(64 << 20);
        node.Commit(64 << 20);
        const auto deadline = TClock::now() + std::chrono::milliseconds(500);
        while (TClock::now() < deadline) {
            node.Drain();
            loop.Step();
        }
        assert_true(accepted.has_value());
    }
    loop.Step();
}

// Its serializer fails after the node reserved room for it.
struct TFailingMessage {
    static constexpr TMessageId MessageId = 45;
    std::string Text;
};

namespace NNet::NActors {

template<>
size_t SerializedSize<TFailingMessage>(const TFailingMessage&) {
    return 64;
}

template<>
void SerializeTo<TFailingMessage>(const TFailingMessage&, std::span<char>) {
    throw std::runtime_error("cannot serialize");
}

} // namespace NNet::NActors

void test_node_drops_failed_serialization(void**) {
    using TPoller = TDefaultPoller;
    TLoop<TPoller> loop;
    auto& poller = loop.Poller();
    TResolver resolver(TAddress{"127.0.0.1", 53}, poller);
    TMessagesFactory factory;
    const int port = GetFreePort();
    TAddress address{"127.0.0.1", port};
    TPoller::TSocket listener(poller, address.Domain());
    listener.Bind(address);
    listener.Listen();

    TNode<TPoller> node(poller, factory, resolver,
        [&poller](const TAddress& addr) { return TPoller::TSocket(poller, addr.Domain()); },
        THostPort("127.0.0.1", port));
    const TActorId sender(1, 2, 3);
    const TActorId recipient(2, 5, 7);
    bool thrown = false;
    try {
        SerializeFarInplace<TFailingMessage>(node, sender, recipient, std::string("x"));
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    node.Send(TEnvelope{.Sender = sender, .Recipient = recipient, .MessageId = 42});

    THeader received;
    bool done = false;
    auto server = [](TPoller::TSocket& listener, THeader& received, bool& done) -> TFuture<void> {
        auto client = (co_await listener.Accept()).value();
        TByteReader reader(client);
        co_await reader.Read(&received, sizeof(received));
        done = true;
    }(listener, received, done);

    const auto deadline = TClock::now() + std::chrono::seconds(10);
    while (!done && TClock::now() < deadline) {
        node.Drain();
        loop.Step();
    }

    assert_true(thrown);
    assert_true(done);
    // The bytes reserved for the failed message never reach the wire.
    assert_int_equal(received.MessageId, 42);
    assert_int_equal(received.Size, 0);
}

class TRecordingActor : public IActor {
public:
    explicit TRecordingActor(std::vector<TMessageId>& received)
        : Received(received)
    { }

    void Receive(TMessageId messageId, TBlob, TActorContext::TPtr) override {
        Received.push_back(messageId);
    }

    std::vector<TMessageId>& Received;
};

// A read that stops at the per-turn byte budget may leave complete envelopes
// buffered; they must be delivered without waiting for more bytes.
void test_inbound_envelope_after_yield(void**) {
    using TPoller = TDefaultPoller;
    TLoop<TPoller> loop;
    auto& poller = loop.Poller();
    // The inbound connection reports the peer's close here when it ends.
    bool inboundClosed = false;
    TActorSystem sys(&poller, 1, [&](const std::string& message) {
        inboundClosed = inboundClosed || message.find("InboundConnection") != std::string::npos;
    });
    std::vector<TMessageId> received;
    const auto recipient = sys.Register(std::make_unique<TRecordingActor>(received));

    TAddress address{"127.0.0.1", GetFreePort()};
    TPoller::TSocket listener(poller, address.Domain());
    listener.Bind(address);
    listener.Listen();
    sys.Serve(std::move(listener));

    std::vector<char> bytes;
    auto append = [&](TMessageId messageId, uint32_t size) {
        const THeader header{.Sender = TActorId(2, 1, 1), .Recipient = recipient, .MessageId = messageId, .Size = size};
        const auto* p = reinterpret_cast<const char*>(&header);
        bytes.insert(bytes.end(), p, p + sizeof(header));
        bytes.insert(bytes.end(), size, 'x');
    };
    // Above the 2 MiB budget of one turn.
    append(100, 3 * 1024 * 1024);
    append(200, 16);
    const size_t small = sizeof(THeader) + 16;

    TPoller::TSocket client(poller, address.Domain());
    bool written = false;
    // The tail of the large envelope arrives in one read with the small one.
    auto writer = [](TPoller& poller, TPoller::TSocket& client, TAddress address, std::vector<char>& bytes, size_t tail, bool& written) -> TFuture<void> {
        co_await client.Connect(address);
        const size_t head = bytes.size() - tail;
        co_await TByteWriter(client).Write(bytes.data(), head);
        co_await poller.Sleep(std::chrono::milliseconds(200));
        co_await TByteWriter(client).Write(bytes.data() + head, tail);
        written = true;
    }(poller, client, address, bytes, small + 64, written);

    const auto deadline = TClock::now() + std::chrono::seconds(5);
    while (received.size() < 2 && TClock::now() < deadline) {
        loop.Step();
    }
    // Let the inbound coroutine see EOF and free itself before the system goes away.
    client.Close();
    const auto closeDeadline = TClock::now() + std::chrono::seconds(5);
    while (!inboundClosed && TClock::now() < closeDeadline) {
        loop.Step();
    }

    assert_true(written);
    assert_true(inboundClosed);
    assert_int_equal(received.size(), 2);
    assert_int_equal(received[0], 100);
    assert_int_equal(received[1], 200);
}

class TStringCollector : public IBehaviorActor, public TBehavior<TStringCollector, TWrappedString> {
public:
    explicit TStringCollector(std::vector<std::string>& received)
        : Received(received)
    {
        Become(this);
    }

    void Receive(TWrappedString&& message, TBlob, TActorContext::TPtr) {
        Received.push_back(std::move(message.Value));
    }

    void HandleUnknownMessage(TMessageId, TBlob, TActorContext::TPtr) { }

    std::vector<std::string>& Received;
};

// A far message larger than the per-turn read budget is written in place by
// SerializeFarInplace and read back by TBehavior through DeserializeFrom.
void test_far_message_across_yield(void**) {
    using TPoller = TDefaultPoller;
    TLoop<TPoller> loop;
    auto& poller = loop.Poller();
    bool inboundClosed = false;
    TActorSystem sys(&poller, 1, [&](const std::string& message) {
        inboundClosed = inboundClosed || message.find("InboundConnection") != std::string::npos;
    });
    std::vector<std::string> received;
    const auto recipient = sys.Register(std::make_unique<TStringCollector>(received));

    TAddress address{"127.0.0.1", GetFreePort()};
    TPoller::TSocket listener(poller, address.Domain());
    listener.Bind(address);
    listener.Listen();
    sys.Serve(std::move(listener));

    // Above the 2 MiB budget of one turn.
    std::string large(5 * 1024 * 1024, '\0');
    for (size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<char>('a' + i % 23);
    }
    TVectorStream stream;
    SerializeFarInplace<TWrappedString>(stream, TActorId(2, 1, 1), recipient, large);
    SerializeFarInplace<TWrappedString>(stream, TActorId(2, 1, 1), recipient, std::string("tail"));

    TPoller::TSocket client(poller, address.Domain());
    bool written = false;
    auto writer = [](TPoller::TSocket& client, TAddress address, TVectorStream& stream, bool& written) -> TFuture<void> {
        co_await client.Connect(address);
        co_await TByteWriter(client).Write(stream.Data.data(), stream.Committed);
        written = true;
    }(client, address, stream, written);

    const auto deadline = TClock::now() + std::chrono::seconds(10);
    while (received.size() < 2 && TClock::now() < deadline) {
        loop.Step();
    }
    client.Close();
    const auto closeDeadline = TClock::now() + std::chrono::seconds(5);
    while (!inboundClosed && TClock::now() < closeDeadline) {
        loop.Step();
    }

    assert_true(written);
    assert_true(inboundClosed);
    assert_int_equal(received.size(), 2);
    assert_true(received[0] == large);
    assert_string_equal(received[1].c_str(), "tail");
}

// Payloads that ask for 16-byte alignment on the wire
struct TAlignedPod {
    static constexpr TMessageId MessageId = 47;
    static constexpr size_t PayloadAlignment = 16;
    uint64_t Values[3];
};

struct TAlignedBytes {
    static constexpr TMessageId MessageId = 48;
    static constexpr size_t PayloadAlignment = 16;
    std::string Value;
};

struct TPlainPod {
    static constexpr TMessageId MessageId = 49;
    uint64_t Values[2];
};

namespace NNet::NActors {

template<>
size_t SerializedSize<TAlignedBytes>(const TAlignedBytes& obj) {
    return obj.Value.size();
}

template<>
void SerializeTo<TAlignedBytes>(const TAlignedBytes& obj, std::span<char> out) {
    std::memcpy(out.data(), obj.Value.data(), out.size());
}

} // namespace NNet::NActors

struct TExpectedFrame {
    TMessageId MessageId;
    std::string Payload;
    bool Aligned;
};

// Rounds of: a string of 0..40 bytes, an aligned POD, aligned bytes of 1..60 bytes
template<typename TStream>
static void WriteAlignedFrames(TStream& stream, int rounds, std::mt19937& rng, std::vector<TExpectedFrame>& frames) {
    const TActorId sender(1, 1, 1);
    const TActorId recipient(1, 2, 2);
    for (int r = 0; r < rounds; ++r) {
        std::string text(rng() % 41, static_cast<char>('a' + r % 26));
        SerializeFarInplace<TWrappedString>(stream, sender, recipient, text);
        frames.push_back({TWrappedString::MessageId, text, false});
        const TAlignedPod pod{{uint64_t(r), uint64_t(r) * 3, uint64_t(r) * 7}};
        SerializeFarInplace<TAlignedPod>(stream, sender, recipient, pod);
        frames.push_back({TAlignedPod::MessageId, std::string(reinterpret_cast<const char*>(&pod), sizeof(pod)), true});
        std::string bytes(1 + rng() % 60, static_cast<char>('A' + r % 26));
        SerializeFarInplace<TAlignedBytes>(stream, sender, recipient, TAlignedBytes{bytes});
        frames.push_back({TAlignedBytes::MessageId, bytes, true});
    }
}

static void CheckAlignedFrames(const std::vector<TEnvelope>& envelopes, const std::vector<TExpectedFrame>& frames) {
    assert_int_equal(envelopes.size(), frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& e = envelopes[i];
        assert_int_equal(e.MessageId, frames[i].MessageId);
        assert_int_equal(e.Blob.Size, frames[i].Payload.size());
        if (e.Blob.Size > 0) {
            assert_memory_equal(e.Blob.Data.get(), frames[i].Payload.data(), e.Blob.Size);
            if (frames[i].Aligned) {
                assert_int_equal(reinterpret_cast<uintptr_t>(e.Blob.Data.get()) % 16, 0);
            }
        }
    }
}

// Commits the stream in pieces of 1..maxPiece bytes and pops all envelopes
template<typename TReader>
static std::vector<TEnvelope> ReadInPieces(TReader& reader, const char* data, size_t size, size_t maxPiece, std::mt19937& rng) {
    std::vector<TEnvelope> envelopes;
    size_t pos = 0;
    while (pos < size) {
        const size_t piece = std::min<size_t>(1 + rng() % maxPiece, size - pos);
        auto buf = reader.Acquire(piece);
        const size_t n = std::min(piece, buf.size());
        std::memcpy(buf.data(), data + pos, n);
        reader.Commit(n);
        pos += n;
        while (auto e = reader.Pop()) {
            envelopes.push_back(std::move(*e));
        }
    }
    return envelopes;
}

void test_payload_alignment_v2(void**) {
    std::mt19937 rng(11);
    for (size_t chunkSize : {64, 100, 129, 256, 1000, 4096}) {
        // A leading string of 0..15 bytes shifts every later frame
        for (int shift = 0; shift < 16; ++shift) {
            TVectorStream stream;
            std::vector<TExpectedFrame> frames;
            std::string lead(shift, 'z');
            SerializeFarInplace<TWrappedString>(stream, TActorId(1, 1, 1), TActorId(1, 2, 2), lead);
            frames.push_back({TWrappedString::MessageId, lead, false});
            WriteAlignedFrames(stream, 50, rng, frames);

            TZeroCopyEnvelopeReaderV2 reader(chunkSize, 8);
            auto envelopes = ReadInPieces(reader, stream.Data.data(), stream.Committed, chunkSize, rng);
            CheckAlignedFrames(envelopes, frames);
        }
    }
}

void test_payload_alignment_v1_and_plain_reader(void**) {
    std::mt19937 rng(12);
    TVectorStream stream;
    std::vector<TExpectedFrame> frames;
    WriteAlignedFrames(stream, 200, rng, frames);

    TZeroCopyEnvelopeReader v1(64);
    CheckAlignedFrames(ReadInPieces(v1, stream.Data.data(), stream.Committed, 300, rng), frames);

    TEnvelopeReader plain;
    plain.Push(stream.Data.data(), stream.Committed);
    std::vector<TEnvelope> envelopes;
    while (auto e = plain.Pop()) {
        envelopes.push_back(std::move(*e));
    }
    CheckAlignedFrames(envelopes, frames);
}

void test_payload_alignment_wire_format(void**) {
    const TActorId sender(1, 2, 3);
    const TActorId recipient(2, 5, 7);
    TVectorStream stream;
    // The stream offset is not a multiple of 16
    stream.Acquire(3);
    stream.Commit(3);

    // A type without PayloadAlignment: the header and the payload, as before
    SerializeFarInplace<TWrappedString>(stream, sender, recipient, std::string("abc"));
    const THeader plain{sender, recipient, TWrappedString::MessageId, 3};
    assert_int_equal(stream.Committed, 3 + sizeof(THeader) + 3);
    assert_memory_equal(stream.Data.data() + 3, &plain, sizeof(plain));
    assert_memory_equal(stream.Data.data() + 3 + sizeof(THeader), "abc", 3);

    const TPlainPod pod{{1, 2}};
    const size_t podStart = stream.Committed;
    SerializeFarInplace<TPlainPod>(stream, sender, recipient, pod);
    const THeader podHeader{sender, recipient, TPlainPod::MessageId, sizeof(TPlainPod)};
    assert_int_equal(stream.Committed, podStart + sizeof(THeader) + sizeof(TPlainPod));
    assert_memory_equal(stream.Data.data() + podStart, &podHeader, sizeof(podHeader));
    assert_memory_equal(stream.Data.data() + podStart + sizeof(THeader), &pod, sizeof(pod));

    // An aligned type: zero padding up to the next multiple of 16, its length in the top bits of the id
    const size_t start = stream.Committed;
    SerializeFarInplace<TAlignedBytes>(stream, sender, recipient, TAlignedBytes{"xyz"});
    const uint32_t padding = (16 - (start + sizeof(THeader)) % 16) % 16;
    assert_true(padding > 0);
    const THeader aligned{sender, recipient, TAlignedBytes::MessageId | (padding << 28), 3};
    assert_int_equal(stream.Committed, start + sizeof(THeader) + padding + 3);
    assert_memory_equal(stream.Data.data() + start, &aligned, sizeof(aligned));
    for (uint32_t i = 0; i < padding; ++i) {
        assert_int_equal(stream.Data[start + sizeof(THeader) + i], 0);
    }
    assert_int_equal((start + sizeof(THeader) + padding) % 16, 0);
    assert_memory_equal(stream.Data.data() + start + sizeof(THeader) + padding, "xyz", 3);
}

// TNode pads relative to its own stream offset, across several drains and both send paths
void test_node_aligns_payloads(void**) {
    using TPoller = TDefaultPoller;
    TLoop<TPoller> loop;
    auto& poller = loop.Poller();
    TResolver resolver(TAddress{"127.0.0.1", 53}, poller);
    TMessagesFactory factory;
    factory.RegisterSerializer<TAlignedBytes>();
    TAllocator alloc;
    const int port = GetFreePort();
    TAddress address{"127.0.0.1", port};
    TPoller::TSocket listener(poller, address.Domain());
    listener.Bind(address);
    listener.Listen();

    TNode<TPoller> node(poller, factory, resolver,
        [&poller](const TAddress& addr) { return TPoller::TSocket(poller, addr.Domain()); },
        THostPort("127.0.0.1", port));
    INode& stream = node;

    std::mt19937 rng(13);
    std::vector<TExpectedFrame> frames;
    for (int batch = 0; batch < 3; ++batch) {
        WriteAlignedFrames(stream, 10, rng, frames);
        std::string bytes(1 + rng() % 60, 'e');
        node.Send(TEnvelope{
            .Sender = TActorId(1, 1, 1),
            .Recipient = TActorId(1, 2, 2),
            .MessageId = TAlignedBytes::MessageId,
            .Blob = SerializeNear<TAlignedBytes>(alloc, bytes),
        });
        frames.push_back({TAlignedBytes::MessageId, bytes, true});
        node.Drain();
        const auto until = TClock::now() + std::chrono::milliseconds(100);
        while (TClock::now() < until) {
            loop.Step();
        }
    }

    // Reads like InboundConnection, in small pieces
    TZeroCopyEnvelopeReaderV2 reader(256, 8);
    std::vector<TEnvelope> envelopes;
    auto server = [](TPoller::TSocket& listener, TZeroCopyEnvelopeReaderV2& reader, std::vector<TEnvelope>& envelopes, size_t count) -> TFuture<void> {
        auto client = (co_await listener.Accept()).value();
        while (envelopes.size() < count) {
            auto buf = reader.Acquire(64);
            auto size = co_await client.ReadSome(buf.data(), buf.size());
            if (size < 0) {
                continue;
            }
            if (size == 0) {
                break;
            }
            reader.Commit(size);
            while (auto e = reader.Pop()) {
                envelopes.push_back(std::move(*e));
            }
        }
    }(listener, reader, envelopes, frames.size());

    const auto deadline = TClock::now() + std::chrono::seconds(10);
    while (envelopes.size() < frames.size() && TClock::now() < deadline) {
        loop.Step();
    }
    CheckAlignedFrames(envelopes, frames);
}

// Frames queued during a write that fails are re-padded for the next connection.
// Not on Windows: its loopback takes the whole 64 MiB without a reading peer, so no write stays blocked.
void test_node_realigns_queue_on_reconnect(void**) {
    using TPoller = TDefaultPoller;
    TLoop<TPoller> loop;
    auto& poller = loop.Poller();
    TResolver resolver(TAddress{"127.0.0.1", 53}, poller);
    TMessagesFactory factory;
    factory.RegisterSerializer<TAlignedPod>();
    factory.RegisterSerializer<TAlignedBytes>();
    const int port = GetFreePort();
    TAddress address{"127.0.0.1", port};
    TPoller::TSocket listener(poller, address.Domain());
    listener.Bind(address);
    listener.Listen();

    std::optional<TPoller::TSocket> first;
    auto stall = [](TPoller::TSocket& listener, std::optional<TPoller::TSocket>& first) -> TFuture<void> {
        // Never reads, so the node's write stays blocked
        first.emplace((co_await listener.Accept()).value());
    }(listener, first);

    TNode<TPoller> node(poller, factory, resolver,
        [&poller](const TAddress& addr) { return TPoller::TSocket(poller, addr.Domain()); },
        THostPort("127.0.0.1", port));
    INode& stream = node;

    // A frame too large for the socket buffers; its size is not a multiple of 16
    const uint32_t large = (64 << 20) + 3;
    auto buf = node.Acquire(sizeof(THeader) + large);
    const THeader header{TActorId(1, 1, 1), TActorId(1, 2, 2), TWrappedString::MessageId, large};
    std::memcpy(buf.data(), &header, sizeof(header));
    node.Commit(sizeof(THeader) + large);
    node.Drain();
    const auto blocked = TClock::now() + std::chrono::milliseconds(500);
    while (TClock::now() < blocked) {
        loop.Step();
    }
    assert_true(first.has_value());
    assert_int_equal(node.Offset() % 16, (sizeof(THeader) + large) % 16);

    // Queued behind the blocked write, padded for the old stream
    std::mt19937 rng(14);
    std::vector<TExpectedFrame> frames;
    WriteAlignedFrames(stream, 20, rng, frames);

    // Reading resumes on the second connection
    TZeroCopyEnvelopeReaderV2 reader(256, 8);
    std::vector<TEnvelope> envelopes;
    auto server = [](TPoller::TSocket& listener, TZeroCopyEnvelopeReaderV2& reader, std::vector<TEnvelope>& envelopes, size_t count) -> TFuture<void> {
        auto client = (co_await listener.Accept()).value();
        while (envelopes.size() < count) {
            auto buf = reader.Acquire(64);
            auto size = co_await client.ReadSome(buf.data(), buf.size());
            if (size < 0) {
                continue;
            }
            if (size == 0) {
                break;
            }
            reader.Commit(size);
            while (auto e = reader.Pop()) {
                envelopes.push_back(std::move(*e));
            }
        }
    }(listener, reader, envelopes, frames.size());

    // An abortive close resets the connection on every platform, the blocked write fails
    linger reset{};
    reset.l_onoff = 1;
    reset.l_linger = 0;
    assert_int_equal(setsockopt(first->Fd(), SOL_SOCKET, SO_LINGER, (const char*) &reset, sizeof(reset)), 0);
    first->Close();
    const auto deadline = TClock::now() + std::chrono::seconds(10);
    while (envelopes.size() < frames.size() && TClock::now() < deadline) {
        loop.Step();
    }
    CheckAlignedFrames(envelopes, frames);
}

void test_intrusive_list_stale_tag(void**) {
    using TList = TIntrusiveList<TMyNode>;
    TList first(1);
    TList second(2);

    first.PushBack(std::make_unique<TMyNode>(0));
    first.PushBack(std::make_unique<TMyNode>(1));

    auto erased = first.Erase(first.Front());
    assert_true(erased->Tag == 0);
    auto popped = first.PopFront();
    assert_true(popped->Tag == 0);

    TMyNode* erasedPtr = erased.get();
    TMyNode* poppedPtr = popped.get();
    second.PushBack(std::move(erased));
    second.PushBack(std::move(popped));

    assert_true(first.Erase(erasedPtr) == nullptr);
    assert_true(first.Erase(poppedPtr) == nullptr);
    assert_true(second.Size() == 2);
}

int main(int argc, char** argv) {
    TInitializer init;

    TState state;

    std::vector<CMUnitTest> tests;
    std::unordered_set<std::string> filters;
    tests.reserve(500);

    parse_filters(argc, argv, filters);

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--perf")) {
            state.usePerf = true;
        } else if (!strcmp(argv[i], "--max-iterations")) {
            if (i + 1 < argc) {
                state.maxIterations = std::atoi(argv[++i]);
            } else {
                std::cerr << "Error: --max-iterations requires a value\n";
                return 1;
            }
        } else if (!strcmp(argv[i], "--message-size")) {
            if (i + 1 < argc) {
                state.messageSize = std::atoi(argv[++i]);
            } else {
                std::cerr << "Error: --message-size requires a value\n";
                return 1;
            }
        }
    }

    ADD_TEST(cmocka_unit_test, test_actor_id_equality);
    ADD_TEST(cmocka_unit_test, test_actor_id_hash);
#ifndef _WIN32
    ADD_TEST(cmocka_unit_test, test_actor_system_notifier_factory);
    ADD_TEST(cmocka_unit_test, test_actor_context_notifier_factory);
#endif
    ADD_TEST(cmocka_unit_test, test_ping_pong);
    ADD_TEST(cmocka_unit_test, test_ask_respond);
    ADD_TEST(cmocka_unit_test, test_async_handler_scatter_gather);
    ADD_TEST(cmocka_unit_test, test_schedule);
    ADD_TEST(cmocka_unit_test, test_schedule_cancel);
    ADD_TEST(cmocka_unit_test, test_serialize_zero_size);
    ADD_TEST(cmocka_unit_test, test_serialize_pod);
    ADD_TEST(cmocka_unit_test, test_serialize_non_pod);
    ADD_TEST(cmocka_unit_test, test_serialize_messages_factory);
    ADD_TEST(cmocka_unit_test, test_serialize_far_inplace_round_trip);
    ADD_TEST(cmocka_unit_test, test_deserialize_far_takes_blob);
    ADD_TEST(cmocka_unit_test, test_serialize_far_rejects_oversized_payload);
    ADD_TEST(cmocka_unit_test, test_envelope_reader);
    ADD_TEST(cmocka_unit_test, test_envelope_reader_v2);
    ADD_TEST(cmocka_unit_test, test_envelope_reader_v2_recycled_chunk);
    ADD_TEST(cmocka_unit_test, test_envelope_reader_v2_copyout_live_blob);
    ADD_TEST(cmocka_unit_test, test_envelope_reader_v2_blob_outlives_reader);
    ADD_TEST(cmocka_unit_test, test_envelope_reader_v2_sealed_blob_outlives_reader);
    ADD_TEST(cmocka_unit_test, test_envelope_reader_v2_blobs_released_on_other_threads);
    ADD_TEST(cmocka_unit_test, test_envelope_reader_v2_reader_dies_while_blobs_released);
    ADD_TEST(cmocka_unit_test_prestate, test_envelope_reader_microbenchmark, &state);
    ADD_TEST(cmocka_unit_test, test_serialize_messages_factory_non_pod);
    ADD_TEST(cmocka_unit_test, test_unbounded_vector_queue);
    ADD_TEST(cmocka_unit_test, test_behavior);
    ADD_TEST(cmocka_unit_test, test_local_message);
    ADD_TEST(cmocka_unit_test, test_behavior_actor);
    ADD_TEST(cmocka_unit_test, test_intrusive_list);
    ADD_TEST(cmocka_unit_test, test_node_reconnects_to_dead_peer);
    ADD_TEST(cmocka_unit_test, test_node_drops_failed_serialization);
    ADD_TEST(cmocka_unit_test, test_node_flushes_bytes_queued_while_connecting);
    ADD_TEST(cmocka_unit_test, test_node_sets_tcp_nodelay);
    ADD_TEST(cmocka_unit_test, test_node_destroyed_during_blocked_write);
    ADD_TEST(cmocka_unit_test, test_inbound_envelope_after_yield);
    ADD_TEST(cmocka_unit_test, test_far_message_across_yield);
    ADD_TEST(cmocka_unit_test, test_intrusive_list_stale_tag);
    ADD_TEST(cmocka_unit_test, test_payload_alignment_v2);
    ADD_TEST(cmocka_unit_test, test_payload_alignment_v1_and_plain_reader);
    ADD_TEST(cmocka_unit_test, test_payload_alignment_wire_format);
    ADD_TEST(cmocka_unit_test, test_node_aligns_payloads);
#ifndef _WIN32
    ADD_TEST(cmocka_unit_test, test_node_realigns_queue_on_reconnect);
#endif

    return _cmocka_run_group_tests("test_actors", tests.data(), tests.size(), NULL, NULL);
}
