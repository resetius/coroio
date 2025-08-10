#include <chrono>
#include <array>
#include <exception>
#include <sstream>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>
#include <iostream>
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
        ctx->Send(ctx->Sender(), TPingMessage{});
        if (counter == 4) {
            ctx->Send(ctx->Self(), TPoison{});
        }
        co_return;
    }

    int counter = 0;
};

class TPongActor : public IActor {
public:
    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        std::cerr << "Received Ping message from: " << ctx->Sender().ToString() << ", message: " << counter++ << "\n";
        ctx->Send(ctx->Sender(), TPongMessage{});
        if (counter == 5) {
            ctx->Send(ctx->Self(), TPoison{});
        }
    }

    int counter = 0;
};

class TAskerActor : public ICoroActor {
    TFuture<void> CoReceive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        std::cerr << "Asker Received message from: " << ctx->Sender().ToString() << "\n";
        auto result = co_await ctx->Ask<TPongMessage>(ctx->Sender(), TPingMessage{});
        std::cerr << "Reply received from " << ctx->Sender().ToString() << ", message: " << result.MessageId << "\n";

        ctx->Send(ctx->Self(), TPoison{});
        std::cerr << "PoisonPill sent\n";

        co_return;
    }
};

class TResponderActor : public ICoroActor {
    TFuture<void> CoReceive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        std::cerr << "Responder Received message from: " << ctx->Sender().ToString() << "\n";
        co_await ctx->Sleep(std::chrono::milliseconds(1000));
        ctx->Send(ctx->Sender(), TPongMessage{});

        ctx->Send(ctx->Self(), TPoison{});
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

    actorSystem.Send(pingActorId, pongActorId, TPingMessage{});

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

    actorSystem.Send(responderActorId, askerActorId, TPingMessage{});

    actorSystem.Serve();

    while (actorSystem.ActorsSize() > 0) {
        loop.Step();
    }
}

class TSingleShotActor : public IActor {
public:
    void Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx) override {
        std::cerr << "Received Pong message from: " << ctx->Sender().ToString() << "\n";
        ctx->Send(ctx->Sender(), TPingMessage{});
        ctx->Send(ctx->Self(), TPoison{});
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
    actorSystem.Schedule(later, pingActorId, pingActorId, TPingMessage{});

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
    auto timer = actorSystem.Schedule(later, pingActorId, pingActorId, TPingMessage{});
    actorSystem.Cancel(timer);
    later = now + std::chrono::milliseconds(10);

    actorSystem.Schedule(later, pingActorId, pingActorId, TPingMessage{});

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
    auto blob = SerializeNear(TEmptyMessage{}, allocator);
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
    auto blob = NNet::NActors::SerializeNear(std::move(msg), allocator);
    assert_true(blob.Size == sizeof(TPodMessage));

    // Deserialize
    auto& deserialized = NNet::NActors::DeserializeNear<TPodMessage>(blob);
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
void SerializeToStream<std::string>(const std::string& obj, std::ostringstream& oss) {
    oss << obj;
}

template<>
void DeserializeFromStream<std::string>(std::string& obj, std::istringstream& iss) {
    obj = iss.str();
}

template<>
void SerializeToStream<TWrappedString>(const TWrappedString& obj, std::ostringstream& oss) {
    oss << obj.Value;
}

template<>
void DeserializeFromStream<TWrappedString>(TWrappedString& obj, std::istringstream& iss) {
    obj.Value = iss.str();
}

} // namespace NNet::NActors

void test_serialize_non_pod(void**) {
    TAllocator allocator;
    std::string str = "Hello, World!";
    auto size = str.size();
    auto blob = SerializeNear<std::string>(std::move(str), allocator);

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

    auto blob = SerializeNear(TPodMessage{1, 2.0, 'c', 4.0, 5.0}, alloc);
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

    auto blob = SerializeNear(TWrappedString{"Hello, World!"}, alloc);
    auto blobSize = blob.Size;
    auto farBlob = factory.SerializeFar(TWrappedString::MessageId, std::move(blob));

    auto mes = DeserializeFar<TWrappedString>(farBlob);
    assert_string_equal(mes.Value.c_str(), "Hello, World!");
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
    auto strNearBlob = SerializeNear(TWrappedString{"Hello, World!"}, alloc);

    TActorContext::TPtr ctx;
    ctx.reset(new (&actorSystem) TMockActorContext(TActorId(), TActorId(), &actorSystem));
    behavior->Receive(TWrappedString::MessageId, std::move(strNearBlob), std::move(ctx));

    auto podNearBlob = SerializeNear(TPodMessage{42, 3.14, 'x', 2.71, 1.618}, alloc);
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
    auto strNearBlob = SerializeNear(TWrappedString{"Hello, World!"}, alloc);
    actor->Receive(TWrappedString::MessageId, std::move(strNearBlob), std::move(ctx));

    auto podNearBlob = SerializeNear(TPodMessage{42, 3.14, 'x', 2.71, 1.618}, alloc);
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

    podNearBlob = SerializeNear(TPodMessage{41, 1.14, 'y', 1.71, 2.618}, alloc);
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
        auto nearBlob = SerializeNear(TWrappedString{"Message " + std::to_string(i)}, alloc);
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
        auto nearBlob = SerializeNear(TWrappedString{"Message " + std::to_string(i)}, alloc);
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

    ADD_TEST(cmocka_unit_test, test_ping_pong);
    ADD_TEST(cmocka_unit_test, test_ask_respond);
    ADD_TEST(cmocka_unit_test, test_schedule);
    ADD_TEST(cmocka_unit_test, test_schedule_cancel);
    ADD_TEST(cmocka_unit_test, test_serialize_zero_size);
    ADD_TEST(cmocka_unit_test, test_serialize_pod);
    ADD_TEST(cmocka_unit_test, test_serialize_non_pod);
    ADD_TEST(cmocka_unit_test, test_serialize_messages_factory);
    ADD_TEST(cmocka_unit_test, test_envelope_reader);
    ADD_TEST(cmocka_unit_test, test_envelope_reader_v2);
    ADD_TEST(cmocka_unit_test_prestate, test_envelope_reader_microbenchmark, &state);
    ADD_TEST(cmocka_unit_test, test_serialize_messages_factory_non_pod);
    ADD_TEST(cmocka_unit_test, test_unbounded_vector_queue);
    ADD_TEST(cmocka_unit_test, test_behavior);
    ADD_TEST(cmocka_unit_test, test_behavior_actor);
    ADD_TEST(cmocka_unit_test, test_intrusive_list);

    return _cmocka_run_group_tests("test_actors", tests.data(), tests.size(), NULL, NULL);
}
