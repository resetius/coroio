#include <coroio/actors/actorsystem.hpp>

namespace NNet {
namespace NActors {

TActorSystem::~TActorSystem() {
    for (auto handle : Handles) {
        handle.destroy();
    }
}

void TActorContext::Send(TActorId to, TMessage::TPtr message)
{
    ActorSystem->Send(Self(), to, std::move(message));
}

void TActorContext::Forward(TActorId to, TMessage::TPtr message) {
    ActorSystem->Send(Sender(), to, std::move(message));
}

void TActorContext::Send(TActorId to, uint32_t messageId, TBlob blob)
{
    ActorSystem->Send(Self(), to, messageId, std::move(blob));
}

void TActorContext::Forward(TActorId to, uint32_t messageId, TBlob blob)
{
    ActorSystem->Send(Sender(), to, messageId, std::move(blob));
}

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

    TActorInternalState state = TActorInternalState {
        .Cookie = cookie,
        .Mailbox = std::make_unique<std::queue<TEnvelope>>(),
        .Actor = std::move(actor)
    };

    if (id >= Actors.size()) {
        Actors.resize(id + 1);
    }
    Actors[id] = std::move(state);

    return actorId;
}

// TODO: remove
void TActorSystem::Send(TActorId sender, TActorId recipient, TMessage::TPtr message) {
    if (recipient.NodeId() != NodeId_) {
        auto& maybeRemote = Nodes[recipient.NodeId()];
        if (!maybeRemote.Node) {
            std::cerr << "Cannot send message to actor on different node: " << recipient.ToString() << "\n";
            return;
        }
        maybeRemote.Node->Send(TEnvelope{
            .Sender = sender,
            .Recipient = recipient,
            .Message = std::move(message)
        });
        if (maybeRemote.Pending) {
            maybeRemote.Pending.resume();
        }
        return;
    }
    auto to = recipient.ActorId();
    if (to == 0 || to >= Actors.size()) {
        std::cerr << "Cannot send message to actor with id: " << to << "\n";
        return;
    }
    auto& state = Actors[to];
    if (recipient.Cookie() != state.Cookie) {
        std::cerr << "Message cookie mismatch for actor with id: " << to << "\n";
        return;
    }
    auto& mailbox = state.Mailbox;
    if (!mailbox) {
        return;
    }
    mailbox->push(TEnvelope{
        .Sender = sender,
        .Recipient = recipient,
        .Message = std::move(message)
    });
    if (!state.Flags.IsReady && !state.Pending.raw()) {
        state.Flags.IsReady = 1;
        ReadyActors.push({to});
    }

    MaybeNotify();
}

void TActorSystem::Send(TActorId sender, TActorId recipient, uint32_t messageId, TBlob blob)
{
    if (recipient.NodeId() != NodeId_) {
        auto& maybeRemote = Nodes[recipient.NodeId()];
        if (!maybeRemote.Node) {
            std::cerr << "Cannot send message to actor on different node: " << recipient.ToString() << "\n";
            return;
        }
        maybeRemote.Node->Send(TEnvelope{
            .Sender = sender,
            .Recipient = recipient,
            .MessageId = messageId,
            .Blob = std::move(blob)
        });
        if (maybeRemote.Pending) {
            maybeRemote.Pending.resume();
        }
        return;
    }
    auto to = recipient.ActorId();
    if (to == 0 || to >= Actors.size()) {
        std::cerr << "Cannot send message to actor with id: " << to << "\n";
        return;
    }
    auto& state = Actors[to];
    if (recipient.Cookie() != state.Cookie) {
        std::cerr << "Message cookie mismatch for actor with id: " << to << "\n";
        return;
    }
    auto& mailbox = state.Mailbox;
    if (!mailbox) {
        return;
    }
    std::cerr << "Sending message with id: " << messageId << " to actor: " << recipient.ToString() << "\n";
    mailbox->push(TEnvelope{
        .Sender = sender,
        .Recipient = recipient,
        .MessageId = messageId,
        .Blob = std::move(blob)
    });
    if (!state.Flags.IsReady && !state.Pending.raw()) {
        state.Flags.IsReady = 1;
        ReadyActors.push({to});
    }

    MaybeNotify();
}

TFuture<void> TActorSystem::WaitExecute() {
    if (ReadyActors.empty()) {
        co_await SuspendExecution();
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

        auto pendingLambda = [this, actorId]() {
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
        };

        while (!mailbox->empty()) {
            auto envelope = std::move(mailbox->front()); mailbox->pop();
            auto messageId = envelope.Message
                ? envelope.Message->MessageId
                : envelope.MessageId;
            if (messageId == static_cast<uint64_t>(ESystemMessages::PoisonPill)) {
                CleanupActors.emplace_back(actorId);
                break;
            }
            auto ctx = std::unique_ptr<TActorContext>(
                new (this) TActorContext(envelope.Sender, envelope.Recipient, this)
            );
            auto future = envelope.Message
                ? actor->Receive(std::move(envelope.Message), std::move(ctx)).Accept(pendingLambda) // TODO: remove
                : actor->Receive(envelope.MessageId, std::move(envelope.Blob), std::move(ctx)).Accept(pendingLambda);
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

void TActorSystem::GcIterationSync() {
    CleanupMessages.clear();
    for (auto actorId : CleanupActors) {
        ShutdownActor(actorId);
    }
    CleanupActors.clear();
}

void TActorSystem::Serve()
{
    Handles.emplace_back([](TActorSystem* self) -> TVoidTask {
        while (true) {
            co_await self->WaitExecute();
        }
    }(this));

    Handles.emplace_back([](TActorSystem* self) -> TVoidTask {
        while (true) {
            co_await self->Sleep(std::chrono::milliseconds(1000));
            self->GcIterationSync();
        }
    }(this));
}

void TActorSystem::AddNode(int id, std::unique_ptr<INode> node)
{
    if (id == NodeId_) {
        std::cerr << "Cannot add node with the same id as current node: " << id << "\n";
        return;
    }
    if (Nodes.size() <= id) {
        Nodes.resize(id + 1);
    }
    Nodes[id].Node = std::move(node);
}

size_t TActorSystem::ActorsSize() const {
    return AliveActors;
}

void TActorSystem::MaybeNotify() {
    if (ExecuteAwait_) {
        ExecuteAwait_.resume();
    }
}

} // namespace NNet
} // namespace NActors
