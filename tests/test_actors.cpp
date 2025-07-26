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

extern "C" {
#include <cmocka.h>
}

using namespace NNet;
using namespace NNet::NActors;

class TPingMessage : public TMessage {
public:
    TPingMessage() {
        MessageId = 10;
    }
};

class TPongMessage : public TMessage {
public:
    TPongMessage() {
        MessageId = 20;
    }
};

class TPingActor : public IActor {
public:
    TFuture<void> Receive(TMessage::TPtr message, TActorContext::TPtr ctx) override {
        std::cerr << "Received Pong message from: " << ctx->Sender().ToString() << ", message: " << counter++ << "\n";
        auto reply = std::make_unique<TPingMessage>();
        co_await ctx->Sleep(std::chrono::milliseconds(1000));
        ctx->Send(ctx->Sender(), std::move(reply));
        if (counter == 4) {
            auto command = std::make_unique<TPoisonPill>();
            ctx->Send(ctx->Self(), std::move(command));
        }
        co_return;
    }

    int counter = 0;
};

class TPongActor : public IActor {
public:
    TFuture<void> Receive(TMessage::TPtr message, TActorContext::TPtr ctx) {
        std::cerr << "Received Ping message from: " << ctx->Sender().ToString() << ", message: " << counter++ << "\n";
        auto reply = std::make_unique<TPongMessage>();
        ctx->Send(ctx->Sender(), std::move(reply));
        if (counter == 5) {
            auto command = std::make_unique<TPoisonPill>();
            ctx->Send(ctx->Self(), std::move(command));
        }
        co_return;
    }

    int counter = 0;
};

class TAskerActor : public IActor {
    TFuture<void> Receive(TMessage::TPtr message, TActorContext::TPtr ctx) {
        std::cerr << "Asker Received message from: " << ctx->Sender().ToString() << "\n";
        auto question = std::make_unique<TPingMessage>();
        std::cerr << "Ask\n";
        auto result = co_await ctx->Ask<TPongMessage>(ctx->Sender(), std::move(question));
        std::cerr << "Reply received from " << ctx->Sender().ToString() << ", message: " << result->MessageId << "\n";

        auto command = std::make_unique<TPoisonPill>();
        ctx->Send(ctx->Self(), std::move(command));
        std::cerr << "PoisonPill sent\n";

        co_return;
    }
};

class TResponderActor : public IActor {
    TFuture<void> Receive(TMessage::TPtr message, TActorContext::TPtr ctx) {
        std::cerr << "Responder Received message from: " << ctx->Sender().ToString() << "\n";
        co_await ctx->Sleep(std::chrono::milliseconds(1000));
        auto reply = std::make_unique<TPongMessage>();
        ctx->Send(ctx->Sender(), std::move(reply));

        auto command = std::make_unique<TPoisonPill>();
        ctx->Send(ctx->Self(), std::move(command));
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

    {
        auto ping = std::make_unique<TPingMessage>();
        actorSystem.Send(pingActorId, pongActorId, std::move(ping));
    }

    actorSystem.Serve();
    actorSystem.MaybeNotify();

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

    {
        auto ping = std::make_unique<TPingMessage>();
        actorSystem.Send(responderActorId, askerActorId, std::move(ping));
    }

    actorSystem.Serve();
    actorSystem.MaybeNotify();

    while (actorSystem.ActorsSize() > 0) {
        loop.Step();
    }
}

struct TPodMessage {
    int field1;
    double field2;
    char field3;
    double field4;
    double field5;
};

struct TAllocator {
    void* Acquire(size_t size) {
        return ::operator new(size);
    }

    void Release(void* ptr) {
        ::operator delete(ptr);
    }
};

void test_serialize_pod(void**) {
    TPodMessage msg{1, 2.0, 'c', 4.0, 5.0};
    TAllocator allocator;

    // Serialize
    auto blob = NNet::NActors::SerializeNear(std::move(msg), allocator);
    assert_true(blob.IsPOD);

    // Deserialize
    auto& deserialized = NNet::NActors::DeserializeNear<TPodMessage>(blob);
    assert_true(deserialized.field1 == 1);
    assert_true(deserialized.field2 == 2.0);
    assert_true(deserialized.field3 == 'c');
    assert_true(deserialized.field4 == 4.0);
    assert_true(deserialized.field5 == 5.0);

    auto farBlob = NNet::NActors::SerializeFar<TPodMessage>(blob, allocator);
    assert_true(farBlob.Data.get() == blob.Data.get());

    auto& deserializedFar = NNet::NActors::DeserializeFar<TPodMessage>(farBlob);
    assert_true(deserializedFar.field1 == 1);
    assert_true(deserializedFar.field2 == 2.0);
    assert_true(deserializedFar.field3 == 'c');
    assert_true(deserializedFar.field4 == 4.0);
    assert_true(deserializedFar.field5 == 5.0);
}

namespace NNet::NActors {

template<>
void SerializeToStream<std::string>(const std::string& obj, std::ostringstream& oss) {
    oss << obj;
}

template<>
void DeserializeFromStream<std::string>(std::string& obj, std::istringstream& iss) {
    obj = iss.str();
}

} // namespace NNet::NActors

void test_serialize_non_pod(void**) {
    TAllocator allocator;
    std::string str = "Hello, World!";
    auto size = str.size();
    NNet::NActors::TBlob blob = NNet::NActors::SerializeNear<std::string>(std::move(str), allocator);
    assert_true(blob.IsPOD == false);

    std::string& deserialized = NNet::NActors::DeserializeNear<std::string>(blob);
    assert_string_equal(deserialized.c_str(), "Hello, World!");

    auto farBlob = NNet::NActors::SerializeFar<std::string>(blob, allocator);
    assert_true(farBlob.Data.get() != blob.Data.get());
    assert_true(farBlob.Size == size);

    std::string deserializedFar = NNet::NActors::DeserializeFar<std::string>(farBlob);
    assert_string_equal(deserializedFar.c_str(), "Hello, World!");
}

int main() {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_ping_pong),
        cmocka_unit_test(test_ask_respond),
        cmocka_unit_test(test_serialize_pod),
        cmocka_unit_test(test_serialize_non_pod),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
