#include <coroio/actors/actorsystem.hpp>

#ifdef Yield
#undef Yield
#endif

namespace NNet {
namespace NActors {

TActorSystem::~TActorSystem() {
    for (auto handle : Handles) {
        handle.destroy();
    }
}

void TActorContext::Send(TActorId to, TMessageId messageId, TBlob blob)
{
    ActorSystem->Send(Self(), to, messageId, std::move(blob));
}

void TActorContext::Forward(TActorId to, TMessageId messageId, TBlob blob)
{
    ActorSystem->Send(Sender(), to, messageId, std::move(blob));
}

TEvent TActorContext::Schedule(TTime when, TActorId sender, TActorId recipient, TMessageId messageId, TBlob blob)
{
    return ActorSystem->Schedule(when, sender, recipient, messageId, std::move(blob));
}

void TActorContext::Cancel(TEvent event)
{
    ActorSystem->Cancel(event);
}

TActorId TActorSystem::Register(IActor::TPtr actor) {
    AliveActors++;
    TLocalActorId id = 0;
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
        .Mailbox = std::make_unique<TUnboundedVectorQueue<TEnvelope>>(),
        .Actor = std::move(actor)
    };

    if (id >= Actors.size()) {
        Actors.resize(id + 1);
    }
    Actors[id] = std::move(state);

    return actorId;
}

void TActorSystem::Send(TActorId sender, TActorId recipient, TMessageId messageId, TBlob blob)
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
        if (maybeRemote.Flags.IsReady == 0) {
            maybeRemote.Flags.IsReady = 1;
            ReadyNodes.Push(recipient.NodeId());
        }

        YieldNotify();
        return;
    }
    auto to = recipient.ActorId();
    if (to == 0 || to >= Actors.size()) {
        std::cerr << "Cannot send message to actor with id: " << to << "\n";
        return;
    }
    auto& state = Actors[to];
    if (recipient.Cookie() != state.Cookie) {
        // std::cerr << "Message cookie mismatch for actor with id: " << to << "\n";
        return;
    }
    auto& mailbox = state.Mailbox;
    if (!mailbox) {
        return;
    }
    mailbox->Push(TEnvelope{
        .Sender = sender,
        .Recipient = recipient,
        .MessageId = messageId,
        .Blob = std::move(blob)
    });
    if (!state.Flags.IsReady && !state.Pending.raw()) {
        state.Flags.IsReady = 1;
        ReadyActors.Push(uint64_t{to});
    }

    YieldNotify();
}

void TActorSystem::ExecuteSync() {
    while (!ReadyActors.Empty()) {
        auto actorId = ReadyActors.Front();
        ReadyActors.Pop();
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

        Actors[actorId].Flags.IsReady = 0;

        while (!mailbox->Empty()) {
            auto envelope = std::move(mailbox->Front());
            mailbox->Pop();
            auto messageId = envelope.MessageId;
            if (messageId == static_cast<TMessageId>(ESystemMessages::PoisonPill)) [[unlikely]] {
                ShutdownActor(actorId);
                break;
            }
            auto ctx = std::unique_ptr<TActorContext>(
                new (this) TActorContext(envelope.Sender, envelope.Recipient, this)
            );
            actor->Receive(envelope.MessageId, std::move(envelope.Blob), std::move(ctx));
            if (Actors[actorId].Pending.raw()) [[unlikely]] {
                break;
            }
        }
    }
}

void TActorSystem::GcIterationSync() {
    CleanupMessages.clear();
}

void TActorSystem::DrainReadyNodes() {
    while (!ReadyNodes.Empty()) {
        auto nodeId = ReadyNodes.Front();
        ReadyNodes.Pop();
        if (nodeId >= Nodes.size()) {
            std::cerr << "Node with id: " << nodeId << " does not exist\n";
            continue;
        }
        auto& nodeState = Nodes[nodeId];
        if (!nodeState.Node) {
            std::cerr << "Node with id: " << nodeId << " is not registered\n";
            continue;
        }
        nodeState.Flags.IsReady = 0;
        if (nodeState.Pending) {
            nodeState.Pending.resume();
        }
    }
}

void TActorSystem::Serve()
{
    YieldCoroutine_ = [](TActorSystem* self) -> TVoidTask {
        while (true) {
            if (self->ReadyActors.Empty()) {
                self->IsYielding_ = false;
                co_await std::suspend_always{};
            }
            self->IsYielding_ = true;
            co_await self->Poller->Yield();

            try {
                self->ExecuteSync();
                self->DrainReadyNodes();
                self->GcIterationSync();
            } catch (const std::exception& ex) {
                std::cerr << "Error during actor system execution: " << ex.what() << "\n";
            }
        }
    }(this);

    ScheduleCoroutine_ = [](TActorSystem* self) -> TVoidTask {
        while (true) {
            co_await std::suspend_always{};

            auto now = TClock::now();
            unsigned prevId = 0;
            bool first = true;
            while (!self->DelayedMessages.empty() && self->DelayedMessages.top().When <= now) {
                TBlob blob = std::move(const_cast<TDelayed&>(self->DelayedMessages.top()).Blob);
                TActorId sender = self->DelayedMessages.top().Sender;
                TActorId recipient = self->DelayedMessages.top().Recipient;
                TMessageId messageId = self->DelayedMessages.top().MessageId;
                auto timerId = self->DelayedMessages.top().TimerId;
                auto valid = self->DelayedMessages.top().valid;
                self->DelayedMessages.pop();
                if ((first || prevId != timerId) && valid) { // skip removed timers
                    self->Send(sender, recipient, messageId, std::move(blob));
                }

                first = false;
                prevId = timerId;
            }
        }
    }(this);

    Handles.emplace_back(ScheduleCoroutine_);
    Handles.emplace_back(YieldCoroutine_);

    YieldNotify();
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

void TActorSystem::YieldNotify() {
    if (IsYielding_ == false) {
        YieldCoroutine_.resume();
    }
}

TEvent TActorSystem::Schedule(TTime when, TActorId sender, TActorId recipient, TMessageId messageId, TBlob blob)
{
    auto timerId = Poller->AddTimer(when, ScheduleCoroutine_);

    DelayedMessages.push({
        .When = when,
        .TimerId = timerId,
        .Sender = sender,
        .Recipient = recipient,
        .MessageId = messageId,
        .Blob = std::move(blob)
    });
    return {timerId, when};
}

void TActorSystem::Cancel(TEvent event) {
    DelayedMessages.push({
        .When = event.second,
        .TimerId = event.first,
        .valid = false
    });
    Poller->RemoveTimer(event.first, event.second);
}

void TActorSystem::AddPendingFuture(TLocalActorId actorId, TFuture<void>&& future)
{
    assert(!Actors[actorId].Pending.raw() && "Actor already has a pending future");
    assert(actorId < Actors.size() && "ActorId out of range");

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
            if (!mailbox->Empty()) {
                Actors[actorId].Flags.IsReady = 1;
                ReadyActors.Push(TLocalActorId{actorId});
            }

            YieldNotify();
        }
    };

    Actors[actorId].Pending = std::move(future.Accept(pendingLambda));
}

} // namespace NNet
} // namespace NActors
