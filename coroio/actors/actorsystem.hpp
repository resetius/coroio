#pragma once

#include "actor.hpp"

#include <unordered_map>
#include <unordered_set>
#include <queue>

namespace NNet {
namespace NActors {

enum class ESystemMessages : uint64_t {
    PoisonPill = 1
};

class TPoisonPill : public TMessage
{
public:
    TPoisonPill() {
        MessageId = static_cast<uint64_t>(ESystemMessages::PoisonPill);
    }
};

template<typename T>
class TAskState
{
public:
    THandle Handle = nullptr;
    std::unique_ptr<T> Answer = nullptr;
};

template<typename T>
class TAsk : public IActor {
public:
    TAsk(const std::shared_ptr<TAskState<T>>& state)
        : State(state)
    { }

    TFuture<void> Receive(TMessage::TPtr message) override;

private:
    std::shared_ptr<TAskState<T>> State;
};


class TActorSystem
{
public:
    TActorSystem(TPollerBase* poller)
        : Poller(poller)
    { }

    TActorId Register(IActor::TPtr actor);

    void Schedule(TMessage::TPtr message, TTime when);

    auto Sleep(TTime until) {
        return Poller->Sleep(until);
    }

    template<typename Rep, typename Period>
    auto Sleep(std::chrono::duration<Rep,Period> duration) {
        return Poller->Sleep(duration);
    }

    void Send(TMessage::TPtr message);

    template<typename T>
    auto Ask(TMessage::TPtr message) {
        class TAskAwaiter
        {
        public:
            TAskAwaiter(const std::shared_ptr<TAskState<T>>& state)
                : State(state)
            { }

            bool await_ready() const noexcept {
                return State->Answer != nullptr;
            }

            void await_suspend(THandle h) {
                State->Handle = h;
            }

            std::unique_ptr<T> await_resume() {
                return std::move(State->Answer);
            }

        private:
            std::shared_ptr<TAskState<T>> State;
        };

        auto state = std::make_shared<TAskState<T>>();
        auto askActor = std::make_unique<TAsk<T>>(state);
        auto actorId = Register(std::move(askActor));
        message->From = actorId;
        Send(std::move(message));
        return TAskAwaiter{state};
    }

    void MaybeNotify() {
        if (ExecuteAwait_) {
            ExecuteAwait_.resume();
        }
    }

    void GcIterationSync() {
        CleanupMessages.clear();
        for (auto actorId : CleanupActors) {
            ShutdownActor(actorId);
        }
        CleanupActors.clear();
    }

    TFuture<void> WaitExecute();

    size_t ActorsSize() const {
        return Actors.size();
    }

private:
    void ShutdownActor(uint64_t actorId) {
        ReadyActors.erase(actorId);
        Mailboxes.erase(actorId);
        Actors.erase(actorId);
        Pending.erase(actorId);
    }

    TPollerBase* Poller;

    std::unordered_set<uint64_t> ReadyActors;
    std::queue<std::pair<uint64_t, std::queue<TMessage::TPtr>*>> ReadyQueues;
    std::unordered_map<uint64_t, std::queue<TMessage::TPtr>> Mailboxes;
    std::unordered_map<uint64_t, IActor::TPtr> Actors;
    std::unordered_map<uint64_t, TFuture<void>> Pending;
    std::vector<TFuture<void>> CleanupMessages;
    std::vector<uint64_t> CleanupActors;
    uint64_t NextActorId_ = 1;
    uint64_t NodeId_ = 1;
    THandle ExecuteAwait_{};
};

template<typename T>
TFuture<void> TAsk<T>::Receive(TMessage::TPtr message) {
    State->Answer = std::unique_ptr<T>(static_cast<T*>(message.release()));
    State->Handle.resume();
    auto command = std::make_unique<TPoisonPill>();
    command->To = SelfActorId;
    ActorSystem->Send(std::move(command));
    co_return;
}

} // namespace NActors
} // namespace NNet

