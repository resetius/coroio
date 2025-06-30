#pragma once

#include "actor.hpp"

namespace NNet {
namespace NActors {

class TActorSystem
{
public:
    TActorSystem() = default;

    TActorId Register(TActor::TPtr actor);
    void Send(TMessage::TPtr message);

    template<typename T>
    auto Ask(TMessage::TPtr message) {    
        class TAskAwaiter
        {
        public:
            TAskAwaiter(const std::shared_ptr<TAskState>& state)
                : State(state)
            { }

            bool await_ready() const noexcept {
                return State->Answer != nullptr;
            }

            void await_suspend(THandle h) {
                State->Handle = h;
            }

            T await_resume() {
                return State->Answer;
            }

        private:
            std::shared_ptr<TAskState> State;
        };

        auto state = std::make_shared<TAskState>();
        auto* askActor = new TAsk(state);
        auto actorId = Register(askActor);
        message->From = actorId;
        Send(message);
        return TAskAwaiter{state};
    }

private:
    template<typename T>
    class TAskState
    {
    public:
        THandle Handle = nullptr;
        T* Answer = nullptr;
    };

    template<typename T>
    class TAsk : public IActor {
    public:
        TAsk(const std::shared_ptr<TAskState>& state)
            : State(state)
        { }

        TFuture<void> Receive(TMessage::TPtr message) override {
            State->Answer = dynamic_cast<T>(message);
            State->Handle.resume();
            Send(SelfActorId, new TPoisonPill);
            co_return;
        }

    private:
        std::shared_ptr<TAskState> State;
    };
};

} // namespace NActors
} // namespace NNet

