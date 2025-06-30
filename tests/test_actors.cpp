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
    TFuture<void> Receive(TMessage::TPtr message) {
        std::cerr << "Received Pong message from: " << message->From.ToString() << ", message: " << counter++ << "\n";
        auto reply = std::make_unique<TPingMessage>();
        reply->From = SelfActorId;
        reply->To = message->From;
        co_await ActorSystem->Sleep(std::chrono::milliseconds(1000));
        ActorSystem->Send(std::move(reply));
        if (counter == 4) {
            auto command = std::make_unique<TPoisonPill>();
            command->From = SelfActorId;
            command->To = SelfActorId;
            ActorSystem->Send(std::move(command));
        }
        co_return;
    }

    int counter = 0;
};

class TPongActor : public IActor {
public:
    TFuture<void> Receive(TMessage::TPtr message) {
        std::cerr << "Received Ping message from: " << message->From.ToString() << ", message: " << counter++ << "\n";
        auto reply = std::make_unique<TPongMessage>();
        reply->From = SelfActorId;
        reply->To = message->From;
        ActorSystem->Send(std::move(reply));
        if (counter == 5) {
            auto command = std::make_unique<TPoisonPill>();
            command->From = SelfActorId;
            command->To = SelfActorId;
            ActorSystem->Send(std::move(command));
        }
        co_return;
    }

    int counter = 0;
};

class TAskerActor : public IActor {
    TFuture<void> Receive(TMessage::TPtr message) {
        std::cerr << "Asker Received message from: " << message->From.ToString() << "\n";
        auto question = std::make_unique<TPingMessage>();
        question->From = SelfActorId;
        question->To = message->From;
        std::cerr << "Ask\n";
        auto result = co_await ActorSystem->Ask<TPongMessage>(std::move(question));
        std::cerr << "Reply received from " << result->From.ToString() << ", message: " << result->MessageId << "\n";

        auto command = std::make_unique<TPoisonPill>();
        command->From = SelfActorId;
        command->To = SelfActorId;
        ActorSystem->Send(std::move(command));
        std::cerr << "PoisonPill sent\n";

        co_return;
    }
};

class TResponderActor : public IActor {
    TFuture<void> Receive(TMessage::TPtr message) {
        std::cerr << "Responder Received message from: " << message->From.ToString() << "\n";
        co_await ActorSystem->Sleep(std::chrono::milliseconds(1000));
        auto reply = std::make_unique<TPongMessage>();
        reply->From = SelfActorId;
        reply->To = message->From;
        ActorSystem->Send(std::move(reply));

        auto command = std::make_unique<TPoisonPill>();
        command->From = SelfActorId;
        command->To = SelfActorId;
        ActorSystem->Send(std::move(command));
        std::cerr << "PoisonPill sent\n";

        co_return;
    }
};

// need to delete completed receives via eventloop iteration to avoid read of deleted memory
TFuture<void> Gc(TActorSystem* actorSystem) {
    while (true) {
        co_await actorSystem->Sleep(std::chrono::milliseconds(1000));
        actorSystem->GcIterationSync();
    }
}

TFuture<void> Fetcher(TActorSystem* actorSystem) {
    while (true) {
        co_await actorSystem->WaitExecute();
    }
}

void test_ping_pong(void**) {
    TLoop<TDefaultPoller> loop;

    TActorSystem actorSystem(&loop.Poller());

    auto pingActorId = actorSystem.Register(std::move(std::make_unique<TPingActor>()));
    auto pongActorId = actorSystem.Register(std::move(std::make_unique<TPongActor>()));
    std::cerr << "PingActor: " << pingActorId.ToString() << "\n";
    std::cerr << "PongActor: " << pongActorId.ToString() << "\n";

    {
        auto ping = std::make_unique<TPingMessage>();
        ping->From = pingActorId;
        ping->To = pongActorId;
        actorSystem.Send(std::move(ping));
    }

    auto fetcher = Fetcher(&actorSystem);
    auto gc = Gc(&actorSystem);

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
        ping->From = responderActorId;
        ping->To = askerActorId;
        actorSystem.Send(std::move(ping));
    }

    auto fetcher = Fetcher(&actorSystem);
    auto gc = Gc(&actorSystem);

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
