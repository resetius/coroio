# Actors on top of coroio

Actor-based concurrency for single-process and distributed systems. Actors communicate only through messages — no shared state, no locks required. Each actor runs strictly on one thread.

Origins: [Erlang](https://en.wikipedia.org/wiki/Erlang_(programming_language)), [Akka](https://en.wikipedia.org/wiki/Akka_(toolkit)).

---

## Architecture

```
TActorSystem
  ├── Actors[]               per-actor state: mailbox + pending future + IActor ptr
  │     └── TActorInternalState
  │           ├── Mailbox    TUnboundedVectorQueue<TEnvelope>
  │           ├── Pending    TFuture<void>  (live async handler, if any)
  │           └── Actor      IActor*
  │
  ├── ReadyActors            queue of actor IDs with pending messages
  ├── DelayedMessages        priority queue of scheduled envelopes
  │
  └── Nodes[]                remote node connections (distributed mode only)
        └── TNode<TPoller>
              ├── OutputBuffer   serialized outbound bytes
              └── Connector/Drainer  TFuture<void> coroutines

Loop: loop.Step()
  → TActorSystem::ExecuteSync()   drain ReadyActors, call Receive per message
  → TActorSystem::GcIterationSync() retire completed futures, free dead actors
  → DrainReadyNodes()             flush outbound buffers to remote nodes
```

### TActorId

```
TActorId = { NodeId: uint16, LocalActorId: uint32, Cookie: uint16 }
```

- `NodeId` — which process/machine; `1` for local by default
- `LocalActorId` — slot in the `Actors[]` array
- `Cookie` — incremented on each reuse; prevents stale messages from reaching a new actor in a recycled slot
- Default-constructed `TActorId{}` is falsy (`operator bool` returns false)

### Message envelope

```
TEnvelope
  ├── Sender     TActorId
  ├── Recipient  TActorId
  ├── MessageId  uint32
  └── Blob       TBlob
        ├── Data  unique_ptr<void>
        ├── Size  uint32
        └── Type  Near | Far
```

`TBlob::PointerType::Near` — pointer to a live C++ object; used for in-process delivery.
`TBlob::PointerType::Far` — flat byte buffer; used when the message crosses a process boundary over the network.

---

## Serialization

### POD messages (automatic)

A type is treated as POD when `std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>`.

```cpp
struct TCounterUpdate {
    static constexpr TMessageId MessageId = 10;
    int64_t Delta;
    uint32_t SourceId;
};

struct THeartbeat {
    static constexpr TMessageId MessageId = 11;
    // empty struct — zero allocation, zero copy
};
```

- In-process (`Near`): placement-new into a pool-allocated buffer; recipient gets a direct `T&`.
- Over network (`Far`): bytes copied as-is into the wire buffer; `Far` is a no-op conversion for POD.
- Empty structs (`sizeof` = 0 after layout): no allocation at all, `DeserializeNear` returns a default-constructed `T` by value.

No specializations needed. No `RegisterSerializer` needed for local-only use.

### Non-POD messages (user-provided)

Any type that is not trivially copyable + standard layout must provide both specializations inside `namespace NNet::NActors`:

```cpp
struct TLogEntry {
    static constexpr TMessageId MessageId = 20;
    std::string Text;       // std::string → not POD
    std::vector<int> Tags;  // vector → not POD
};

namespace NNet::NActors {

template<>
void SerializeToStream<TLogEntry>(const TLogEntry& obj, std::ostringstream& oss) {
    // wire format: 4-byte tag count, tag values, then text
    uint32_t n = obj.Tags.size();
    oss.write(reinterpret_cast<const char*>(&n), sizeof(n));
    oss.write(reinterpret_cast<const char*>(obj.Tags.data()), n * sizeof(int));
    oss << obj.Text;
}

template<>
void DeserializeFromStream<TLogEntry>(TLogEntry& obj, std::istringstream& iss) {
    uint32_t n = 0;
    iss.read(reinterpret_cast<char*>(&n), sizeof(n));
    obj.Tags.resize(n);
    iss.read(reinterpret_cast<char*>(obj.Tags.data()), n * sizeof(int));
    obj.Text = iss.str().substr(iss.tellg());
}

} // namespace NNet::NActors
```

- In-process (`Near`): `new T(args...)`, pointer stored in `TBlob`. Recipient gets the object by rvalue ref.
- Over network (`Far`): calls `SerializeToStream` → copies bytes to flat buffer. Receiving end calls `DeserializeFromStream`.

**Forgetting to specialize both functions is a compile-time error** (`static_assert` fires inside the template stubs).

### TMessagesFactory (distributed mode only)

For remote delivery the sending node must know how to convert a `Near` blob to `Far`. `TMessagesFactory` holds one function pointer per `MessageId` that calls `SerializeFar<T>`.

```cpp
TMessagesFactory factory;
factory.RegisterSerializer<TLogEntry>();   // non-POD: stores SerializeFar<TLogEntry>
factory.RegisterSerializer<TCounterUpdate>(); // POD: SerializeFar is a no-op but still register
factory.RegisterSerializer<THeartbeat>();

// Pass factory to every TNode
auto node = std::make_unique<TNode<Poller>>(
    loop.Poller(), factory, resolver,
    [&](const TAddress& addr){ return Poller::TSocket(loop.Poller(), addr.Domain()); },
    THostPort("peer.host", 9000)
);
```

For **local-only** deployments, `TMessagesFactory` is not needed.

---

## Actor types

```
IActor
  ├── synchronous; Receive() must complete without suspending
  └── ICoroActor
        └── async; override CoReceive() → TFuture<void>; can co_await

IBehaviorActor
  ├── delegates Receive() to a swappable IBehavior*
  ├── call Become(behavior*) to switch at any time
  └── behaviors are plain objects — not registered with TActorSystem

TBehavior<Derived, Msg1, Msg2, ...>   (mixin, not an actor by itself)
  ├── inherits IBehavior
  ├── typed dispatch: one Receive(MsgN&&, TBlob, ctx) per listed type
  ├── handler return void → sync; return TFuture<void> → async
  └── HandleUnknownMessage() required for unlisted messages
```

The most common pattern is `IBehaviorActor + TBehavior` as a single class for simple state machines, or `IBehaviorActor` with separate `TBehavior` objects for more complex multi-state actors.

---

### IActor

**When to use:** the handler is pure computation — no I/O, no timers, no waiting for a reply. The mailbox is drained in a tight loop; `Receive` is called once per message and must return before the next one is dispatched.

**Constraints:**
- No `co_await` inside `Receive` (not a coroutine).
- Must not block the thread (no `sleep`, no blocking I/O).
- All message type checking and deserialization is manual.

```cpp
class TCounter : public IActor {
public:
    void Receive(TMessageId id, TBlob blob, TActorContext::TPtr ctx) override {
        if (id == TIncrement::MessageId) {
            auto& msg = DeserializeNear<TIncrement>(blob);  // T& for POD
            count_ += msg.Delta;
        } else if (id == TGetCount::MessageId) {
            ctx->Send<TCountValue>(ctx->Sender(), count_);
        } else if (id == TPoison::MessageId) {
            // no-op: system removes actor after this Receive returns
        }
    }
private:
    int64_t count_ = 0;
};
```

`DeserializeNear<T>(blob)` returns `T&` for non-empty POD, `T` (by value) for empty structs. For non-POD types arriving from a remote node (`blob.Type == Far`) use `DeserializeFar<T>(blob)` instead, or use `TBehavior` which handles the distinction automatically.

---

### ICoroActor

**When to use:** the handler needs to wait — for a timer, for a reply from another actor, or for any other async event. Override `CoReceive` instead of `Receive`.

**Constraints:**
- Only one async handler per actor can be active at a time. If `CoReceive` suspends (i.e. the returned `TFuture<void>` is not immediately done), new messages arriving for the actor are queued in the mailbox but not dispatched until the current handler completes.
- `CoReceive` receives `ctx` by value (`TActorContext::TPtr`). The ctx is only valid during the synchronous part and through `co_await` points — do not store it past the coroutine's lifetime.

```cpp
class TPingActor : public ICoroActor {
public:
    TFuture<void> CoReceive(TMessageId id, TBlob blob, TActorContext::TPtr ctx) override {
        if (id == TPong::MessageId) {
            co_await ctx->Sleep(std::chrono::seconds(1));   // suspend, don't block
            ctx->Send<TPing>(ctx->Sender());
            if (++count_ == 4)
                ctx->Send<TPoison>(ctx->Self());            // shut down self
        }
        co_return;
    }
private:
    int count_ = 0;
};
```

**Execution model:** `ICoroActor::Receive` (the non-virtual bridge) calls `CoReceive` and checks `future.done()`. If already complete, nothing is stored. If still running, the future is registered as `Pending` on the actor's internal state and the mailbox drain stops for this actor. When the future completes, a continuation checks for queued messages and re-schedules the actor.

```
message arrives
    │
    ▼
CoReceive() called
    │
    ├── returns immediately (done) ──► next message from mailbox
    │
    └── suspends at co_await ──► future stored as Pending
                                 mailbox drain paused
                                      │
                                      ▼
                                 event fires, future resumes
                                      │
                                      ▼
                                 future completes
                                      │
                                      ▼
                                 Pending cleared
                                 mailbox checked ──► next message (if any)
```

---

### IBehaviorActor

**When to use:** the actor needs to fundamentally change what messages it accepts and how it responds based on its current state — i.e., a state machine. `Become(behavior*)` switches the active handler atomically between messages.

**Constraints:**
- `Become` takes a raw pointer; the behavior object must outlive the actor. Behaviors are usually stored as members of the actor itself.
- Behavior switch takes effect on the next message. The current `Receive` call completes with the old behavior.
- `IBehaviorActor` does not have `TBehavior` built in. You either pair it with `TBehavior` (most common) or implement `IBehavior` manually.

```cpp
// simplest form: actor IS its own behavior
struct TMyActor : public IBehaviorActor,
                  public TBehavior<TMyActor, TStart, TWork> {
    TMyActor() { Become(this); }  // initial behavior = self

    void Receive(TStart&&, TBlob, TActorContext::TPtr ctx) {
        started_ = true;
        // no behavior switch — stays on self
    }
    void Receive(TWork&& msg, TBlob, TActorContext::TPtr ctx) {
        if (!started_) return;
        process(msg);
    }
    void HandleUnknownMessage(TMessageId, TBlob, TActorContext::TPtr) {}

    bool started_ = false;
};
```

```cpp
// multiple behaviors as separate objects
struct TProtocolActor : public IBehaviorActor {
    struct THandshake : TBehavior<THandshake, THello, TTimeout> {
        THandshake(TProtocolActor* self) : Self(self) {}
        void Receive(THello&&, TBlob, TActorContext::TPtr ctx) {
            ctx->Send<TAck>(ctx->Sender());
            Self->Become(&Self->Active);      // switch to Active state
        }
        void Receive(TTimeout&&, TBlob, TActorContext::TPtr ctx) {
            ctx->Send<TPoison>(ctx->Self());  // timeout: shut down
        }
        void HandleUnknownMessage(TMessageId, TBlob, TActorContext::TPtr) {}
        TProtocolActor* Self;
    };

    struct TActive : TBehavior<TActive, TData, TClose> {
        TActive(TProtocolActor* self) : Self(self) {}
        void Receive(TData&& msg, TBlob, TActorContext::TPtr ctx) { process(msg); }
        void Receive(TClose&&, TBlob, TActorContext::TPtr ctx) {
            ctx->Send<TPoison>(ctx->Self());
        }
        void HandleUnknownMessage(TMessageId, TBlob, TActorContext::TPtr) {}
        TProtocolActor* Self;
    };

    TProtocolActor() : Handshake(this), Active(this) { Become(&Handshake); }

    THandshake Handshake;
    TActive    Active;
};
```

---

### TBehavior\<Derived, Msg1, Msg2, ...\>

`TBehavior` is a **mixin** (inherits `IBehavior`) that provides typed dispatch. It is not an actor by itself — it is used as a base class for a behavior object that is then plugged into `IBehaviorActor` via `Become`.

**Dispatch mechanics:**
1. Fold-expression over the message type list: first type whose `MessageId` matches wins.
2. Checks `blob.Type` (Near or Far) and calls the appropriate deserializer.
3. Checks the return type of `Derived::Receive(Msg&&, ...)` at compile time:
   - `void` → call directly, return immediately.
   - `TFuture<void>` → start async via `ctx->StartAsync().Commit(future)` (same path as `ICoroActor`).
4. If no type matched: calls `Derived::HandleUnknownMessage(id, blob, ctx)` — **required**, no default**.

**Mixed sync/async handlers in one behavior:**

```cpp
struct TWorkerBehavior : TBehavior<TWorkerBehavior, TFastJob, TSlowJob> {
    // sync handler — returns before next message is dispatched
    void Receive(TFastJob&& job, TBlob, TActorContext::TPtr ctx) {
        auto result = compute(job.Data);
        ctx->Send<TResult>(ctx->Sender(), result);
    }

    // async handler — suspends; mailbox paused until this completes
    TFuture<void> Receive(TSlowJob&& job, TBlob, TActorContext::TPtr ctx) {
        co_await ctx->Sleep(job.Delay);
        auto result = co_await ctx->Ask<TData>(dbActor_, TQuery{job.Key});
        ctx->Send<TResult>(ctx->Sender(), result.Value);
    }

    void HandleUnknownMessage(TMessageId id, TBlob, TActorContext::TPtr) {
        std::cerr << "Unexpected message: " << id << "\n";
    }

    TActorId dbActor_;
};
```

**`TBehavior` with no message types** — a catch-all / dead state:

```cpp
struct TDeadBehavior : TBehavior<TDeadBehavior> {
    void HandleUnknownMessage(TMessageId id, TBlob, TActorContext::TPtr ctx) {
        // silently drop all messages (actor is shutting down)
    }
};
```

---

### Choosing the right type

```
Need co_await in the handler?
  Yes → ICoroActor  (or TBehavior with TFuture<void> handlers inside IBehaviorActor)
  No  → IActor  (or TBehavior with void handlers inside IBehaviorActor)

Need to change message handling based on state?
  Yes → IBehaviorActor + TBehavior
  No  → IActor or ICoroActor directly

Single coherent state machine where actor IS its behavior?
  → IBehaviorActor + TBehavior<Self, Msgs...>, Become(this) in constructor

Multiple distinct states with different message sets?
  → IBehaviorActor with separate TBehavior member objects per state

Need typed dispatch without writing if/else chains?
  → TBehavior (works regardless of sync/async)

Need fine control over Near/Far deserialization?
  → IActor with manual DeserializeNear / DeserializeFar calls
```

---

## TActorContext API

Available inside every `Receive` / `CoReceive`:

| Method | Description |
|---|---|
| `ctx->Self()` | this actor's `TActorId` |
| `ctx->Sender()` | sender of the current message (may be falsy for system messages) |
| `ctx->Send<T>(to, args...)` | construct and deliver `T{args...}` to `to`; non-blocking |
| `ctx->Forward<T>(to, args...)` | same but preserves original `Sender()` |
| `ctx->Schedule<T>(when, from, to, args...)` | deliver at `when`; returns `TEvent` |
| `ctx->Cancel(event)` | cancel a previously scheduled message |
| `ctx->Sleep(duration\|time_point)` | `co_await`-able; requires `ICoroActor` or async `TBehavior` handler |
| `ctx->Ask<TReply>(to, question)` | send `question`, suspend until `TReply` arrives; returns `TFuture<TReply>` |
| `TPoison` | send to any actor to shut it down |

`Send` and `Schedule` both accept constructor arguments, not a pre-built object:
```cpp
ctx->Send<TLogEntry>(loggerActor, "error text", std::vector<int>{1, 2});
ctx->Schedule<TTick>(steady_clock::now() + 1s, ctx->Self(), ctx->Self());
```

---

## Local setup

```cpp
#include <coroio/all.hpp>
#include <coroio/actors/actorsystem.hpp>
using namespace NNet; using namespace NNet::NActors;

TLoop<TDefaultPoller> loop;
TActorSystem sys(&loop.Poller());           // nodeId defaults to 1

auto id = sys.Register(std::make_unique<TWorker>());
sys.Send<TJob>(TActorId{}, id, "work item"); // sender = null

sys.Serve();                                 // start scheduler coroutine (no socket)

while (sys.ActorsSize() > 0)
    loop.Step();
```

---

## Distributed setup

Each process is a **node** identified by a `TNodeId`. Nodes connect to each other over TCP. Messages to remote actors are serialized to `Far` and written to the outbound buffer; the receiving side deserializes and delivers locally.

```
Process A (nodeId=1)              Process B (nodeId=2)
┌─────────────────────┐           ┌─────────────────────┐
│ TActorSystem(1)     │           │ TActorSystem(2)      │
│   Actor[local_id]   │           │   Actor[local_id]    │
│                     │  TCP      │                      │
│ TNode→node 2 ───────┼──────────►│ InboundServe         │
│ InboundServe◄───────┼──────────── TNode→node 1         │
└─────────────────────┘           └─────────────────────┘

Wire message layout:
  [ THeader: Sender | Recipient | MessageId | Size ]
  [ payload bytes (Far-serialized)                 ]
```

```cpp
TInitializer init;
TLoop<TDefaultPoller> loop;
TResolver resolver(loop.Poller());
TMessagesFactory factory;

factory.RegisterSerializer<TJob>();
factory.RegisterSerializer<TDone>();

TActorSystem sys(&loop.Poller(), /*myNodeId=*/1);

// register remote peer
sys.AddNode(2, std::make_unique<TNode<TDefaultPoller>>(
    loop.Poller(), factory, resolver,
    [&](const TAddress& addr){ return TDefaultPoller::TSocket(loop.Poller(), addr.Domain()); },
    THostPort("peer.host", 9001)
));

// local listen socket
TAddress addr{"::", 9000};
TDefaultPoller::TSocket sock(loop.Poller(), addr.Domain());
sock.Bind(addr); sock.Listen();
sys.Serve(std::move(sock));   // starts inbound + outbound coroutines

// send to actor on node 2: construct TActorId with remote nodeId
auto remoteActor = TActorId{2, knownLocalId, knownCookie};
sys.Send<TJob>(TActorId{}, remoteActor, "work item");

while (sys.ActorsSize() > 0)
    loop.Step();
```

`TNode` reconnects automatically on connection failure (with 1 s backoff). See `examples/behavior_actors.cpp` and `examples/ping_actors.cpp` for complete runnable examples.
