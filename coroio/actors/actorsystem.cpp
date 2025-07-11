#include <coroio/actors/actorsystem.hpp>

namespace NNet {
namespace NActors {

TActorId TActorSystem::Register(IActor::TPtr actor) {
    AliveActors++;
    uint64_t id = 0;
    if (!FreeActorIds.empty()) {
        id = FreeActorIds.top();
        FreeActorIds.pop();
    } else {
        id = NextActorId_++;
    }
    auto cookie = NextCookie_++;
    TActorId actorId{NodeId_, id, cookie};
    actor->Attach(this, actorId); // TODO: remove me

    TActorInternalState state = TActorInternalState {
        .Cookie = cookie,
        .Mailbox = std::make_unique<std::queue<TMessage::TPtr>>(),
        .Actor = std::move(actor)
    };

    if (id >= Actors.size()) {
        Actors.resize(id + 1);
    }
    Actors[id] = std::move(state);

    return actorId;
}

void TActorSystem::Send(TMessage::TPtr message) {
    auto to = message->To.ActorId();
    if (to == 0 || to >= Actors.size()) {
        std::cerr << "Cannot send message to actor with id: " << to << "\n";
        return;
    }
    auto& state = Actors[to];
    if (message->To.Cookie() != state.Cookie) {
        std::cerr << "Message cookie mismatch for actor with id: " << to << "\n";
        return;
    }
    auto& mailbox = state.Mailbox;
    if (!mailbox) {
        return;
    }
    mailbox->push(std::move(message));
    if (!state.Flags.IsReady && !state.Pending.raw()) {
        state.Flags.IsReady = 1;
        ReadyActors.push({to});
    }
}

TFuture<void> TActorSystem::WaitExecute() {
    if (ReadyActors.empty()) {
        ExecuteAwait_ = co_await Self();
        co_await std::suspend_always();
        ExecuteAwait_ = {};
    }

    while (!ReadyActors.empty()) {
        auto actorId = ReadyActors.front();
        ReadyActors.pop();
        if (actorId >= Actors.size()) {
            std::cerr << "Actor with id: " << actorId << " does not exist\n";
            continue;
        }
        auto& state = Actors[actorId];
        auto& actor = state.Actor;
        if (!actor) {
            std::cerr << "Actor with id: " << actorId << " is not registered\n";
            continue;
        }
        auto& mailbox = state.Mailbox;
        if (!mailbox) {
            std::cerr << "Mailbox for actor with id: " << actorId << " is not initialized\n";
            continue;
        }
        while (!mailbox->empty()) {
            auto message = std::move(mailbox->front()); mailbox->pop();
            if (message->MessageId == static_cast<uint64_t>(ESystemMessages::PoisonPill)) {
                CleanupActors.emplace_back(actorId);
                break;
            }
            auto future = actor->Receive(std::move(message)).Accept([this, actorId=actorId](){
                // if we were in pending
                // we need to try restart ActorSystem loop
                auto& pending = Actors[actorId].Pending;
                if (pending.raw()) {
                    CleanupMessages.emplace_back(std::move(pending)); // we cannot delete coroutine from itself, need to do "gc"
                    pending = {};
                    Actors[actorId].Flags.IsReady = 0;

                    // if Sent to actorId was called, we need to check if it has any messages in mailbox
                    auto& mailbox = Actors[actorId].Mailbox;
                    if (!mailbox->empty()) {
                        Actors[actorId].Flags.IsReady = 1;
                        ReadyActors.push({actorId});
                    }

                    MaybeNotify();
                }
            });
            if (!future.done()) {
                Actors[actorId].Pending = std::move(future);
                break;
            }
        }

        if (mailbox->empty()) {
            Actors[actorId].Flags.IsReady = 0;
        }
    }
    co_return;
}


} // namespace NNet
} // namespace NActors
