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

int main() {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_ping_pong),
        cmocka_unit_test(test_ask_respond),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
