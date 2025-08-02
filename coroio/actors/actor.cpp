#include "actor.hpp"
#include "actorsystem.hpp"

namespace NNet {
namespace NActors {

void ICoroActor::Receive(TMessageId messageId, TBlob blob, TActorContext::TPtr ctx)
{
    auto async = ctx->StartAsync();
    auto future = CoReceive(messageId, std::move(blob), std::move(ctx));
    if (!future.done()) {
        async.Commit(std::move(future));
    }
}

TActorContext::TAsync TActorContext::StartAsync()
{
    return TAsync{ActorSystem, Sender_.ActorId()};
}

void TActorContext::TAsync::Commit(TFuture<void>&& future)
{
    ActorSystem_->AddPendingFuture(ActorId_, std::move(future));
}

} // namespace NActors
} // namespace NNet