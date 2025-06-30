#include <coroio/actors/actorsystem.hpp>

namespace NNet {
namespace NActors {

TActorId TActorSystem::Register(IActor::TPtr actor) {
    auto id = NextActorId_++;
    TActorId actorId{NodeId_, id};
    [[maybe_unused]] auto& mailbox = Mailboxes[id];
    actor->Attach(this, actorId);
    Actors[id] = std::move(actor);
    return actorId;
}

void TActorSystem::Send(TMessage::TPtr message) {
    auto to = message->To.ActorId();
    auto maybeMailbox = Mailboxes.find(to);
    if (maybeMailbox == Mailboxes.end()) {
        std::cerr << "Cannot find mailbox for " << to << "\n";
        return;
    }
    auto* mailbox = &maybeMailbox->second;
    mailbox->push(std::move(message));
    if (!Pending.contains(to) &&
        !ReadyActors.contains(to))
    {
        ReadyActors.insert(to);
        ReadyQueues.push({to, mailbox});
    }
}

TFuture<void> TActorSystem::WaitExecute() {
    if (ReadyQueues.empty()) {
        ExecuteAwait_ = co_await Self();
        co_await std::suspend_always();
        ExecuteAwait_ = {};
    }

    while (!ReadyQueues.empty()) {
        auto [actorId, queue] = ReadyQueues.front();
        ReadyQueues.pop();
        auto maybeActor = Actors.find(actorId);
        if (maybeActor == Actors.end()) {
            continue;
        }
        auto& actor = maybeActor->second;
        while (!queue->empty()) {
            auto message = std::move(queue->front()); queue->pop();
            if (message->MessageId == static_cast<uint64_t>(ESystemMessages::PoisonPill)) {
                CleanupActors.emplace_back(actorId);
                break;
            }
            auto future = actor->Receive(std::move(message)).Accept([this, actorId=actorId](){
                // if we were in pending
                // we need to try restart ActorSystem loop
                auto maybeSelfFuture = Pending.find(actorId);
                if (maybeSelfFuture != Pending.end()) {
                    CleanupMessages.emplace_back(std::move(maybeSelfFuture->second)); // we cannot delete coroutine from itself, need to do "gc"
                    Pending.erase(maybeSelfFuture);
                    ReadyActors.erase(actorId);

                    // if Sent to actorId was called, we need to check if it has any messages in mailbox
                    auto maybeMailbox = Mailboxes.find(actorId);
                    if (maybeMailbox != Mailboxes.end() && !maybeMailbox->second.empty()) {
                        auto* mailbox = &maybeMailbox->second;
                        ReadyActors.insert(actorId);
                        ReadyQueues.push({actorId, mailbox});
                    }

                    MaybeNotify();
                }
            });
            if (!future.done()) {
                Pending[actorId] = std::move(future);
                break;
            }
        }

        if (queue->empty()) {
            ReadyActors.erase(actorId);
        }
    }
    co_return;
}


} // namespace NNet
} // namespace NActors
