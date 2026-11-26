#include "ping_actors_io.h"

#ifndef _WIN32

#include <coroio/all.hpp>
#include <coroio/actors/threaded_inbound.hpp>
#include <coroio/actors/threaded_node.hpp>
#include <coroio/loop_thread.hpp>

#include <chrono>
#include <thread>
#include <vector>

using namespace NNet;
using namespace NNet::NActors;

namespace {

template<typename TPoller>
class TIoThreads final : public IIoThreads {
public:
    using TSocket = typename TPoller::TSocket;

    explicit TIoThreads(int threads) {
        for (int i = 0; i < threads; ++i) {
            Threads_.push_back(std::make_unique<TLoopThread<TPoller>>());
        }
        Resolver_ = std::make_unique<TResolver>(Threads_.front()->Poller());
    }

    std::unique_ptr<INode> MakeNode(TMessagesFactory& factory, const THostPort& hostPort) override {
        auto& poller = Threads_.front()->Poller();
        return std::make_unique<TThreadedNode<TPoller>>(poller, factory, *Resolver_,
            [&poller](const TAddress& addr) { return TSocket(poller, addr.Domain()); }, hostPort);
    }

    void ServeInbound(TActorSystem& sys, TNodeId nodeId, const TAddress& address, int readerType) override {
        auto& poller = Threads_.back()->Poller();
        TSocket listener(poller, address.Domain());
        listener.Bind(address);
        listener.Listen();
        Delivery_ = std::make_unique<TQueuedDelivery>(sys);
        if (readerType == 0) {
            Inbound_ = std::make_shared<TThreadedInbound<TPoller, TQueuedDelivery, TZeroCopyEnvelopeReader>>(
                poller, std::move(listener), nodeId, *Delivery_);
        } else {
            Inbound_ = std::make_shared<TThreadedInbound<TPoller, TQueuedDelivery, TZeroCopyEnvelopeReaderV2>>(
                poller, std::move(listener), nodeId, *Delivery_);
        }
    }

    void Start() override {
        for (auto& thread : Threads_) {
            thread->Start();
        }
    }

    void Stop() override {
        // Lets the nodes write what the actors sent last: the poison pills
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (auto& thread : Threads_) {
            thread->Stop();
        }
        for (auto& thread : Threads_) {
            thread->Join();
        }
        Inbound_.reset();
        Delivery_.reset();
    }

private:
    std::vector<std::unique_ptr<TLoopThread<TPoller>>> Threads_;
    std::unique_ptr<TResolver> Resolver_;
    std::unique_ptr<TQueuedDelivery> Delivery_;
    std::shared_ptr<void> Inbound_;
};

} // namespace

template<typename TPoller>
std::unique_ptr<IIoThreads> MakeIoThreads(int threads) {
    return std::make_unique<TIoThreads<TPoller>>(threads);
}

template std::unique_ptr<IIoThreads> MakeIoThreads<TSelect>(int);
template std::unique_ptr<IIoThreads> MakeIoThreads<TPoll>(int);
#ifdef HAVE_EPOLL
template std::unique_ptr<IIoThreads> MakeIoThreads<TEPoll>(int);
#endif
#ifdef HAVE_URING
template std::unique_ptr<IIoThreads> MakeIoThreads<TUring>(int);
#endif
#ifdef HAVE_KQUEUE
template std::unique_ptr<IIoThreads> MakeIoThreads<TKqueue>(int);
#endif

#endif // _WIN32
