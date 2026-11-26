#pragma once

#include <coroio/actors/actorsystem.hpp>

#include <memory>

/**
 * The transport of --io-threads: the nodes write on the first I/O thread, the inbound
 * connections are read on the last one.
 *
 * It lives in a translation unit of its own, so the code of the actors is compiled as
 * without it: gcc devirtualizes the calls through INode speculatively when the unit
 * defines one implementation, and TThreadedNode would be a second one.
 */
class IIoThreads {
public:
    virtual ~IIoThreads() = default;

    virtual std::unique_ptr<NNet::NActors::INode> MakeNode(NNet::NActors::TMessagesFactory& factory, const NNet::THostPort& hostPort) = 0;
    /// Reads the inbound connections of `sys` on the last thread, with reader 0 (v1) or 1 (V2)
    virtual void ServeInbound(NNet::NActors::TActorSystem& sys, NNet::NActors::TNodeId nodeId, const NNet::TAddress& address, int readerType) = 0;
    virtual void Start() = 0;
    /// Stops and joins the threads, then destroys the inbound; the nodes go with the actor system
    virtual void Stop() = 0;
};

#ifndef _WIN32
template<typename TPoller>
std::unique_ptr<IIoThreads> MakeIoThreads(int threads);
#endif
