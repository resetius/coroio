#include <chrono>
#include <array>
#include <exception>
#include <sstream>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>
#include <iostream>

#include <coroio/all.hpp>
#include <coroio/actors/actorsystem.hpp>
#include <coroio/actors/messages.hpp>
#include <coroio/actors/messages_factory.hpp>
#include <coroio/actors/queue.hpp>

extern "C" {
#include <cmocka.h>
}

using namespace NNet;
using namespace NNet::NActors;

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

    auto farBlob = SerializeFar<TEmptyMessage>(blob);
    assert_true(farBlob.Data.get() == blob.Data.get());
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

    auto farBlob = SerializeFar<TPodMessage>(blob);
    assert_true(farBlob.Data.get() == blob.Data.get());

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

    auto farBlob = SerializeFar<std::string>(blob);
    assert_true(farBlob.Data.get() != blob.Data.get());
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

int main() {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_ping_pong),
        cmocka_unit_test(test_ask_respond),
        cmocka_unit_test(test_schedule),
        cmocka_unit_test(test_schedule_cancel),
        cmocka_unit_test(test_serialize_zero_size),
        cmocka_unit_test(test_serialize_pod),
        cmocka_unit_test(test_serialize_non_pod),
        cmocka_unit_test(test_serialize_messages_factory),
        cmocka_unit_test(test_serialize_messages_factory_non_pod),
        cmocka_unit_test(test_unbounded_vector_queue)
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
