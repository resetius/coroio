# Actors on top of coroio

This directory contains an implementation of the actor paradigm on top of coroio. The paradigm simplifies concurrent programming, including distributed systems. The idea is to write interacting agents (actors):

- An actor can send a message to another actor (Send).
- An actor can create a new actor (Spawn/Register).
- An actor can send a Poison message to itself or another actor to terminate it.
- Every actor implements Receive, which handles incoming messages.

Each actor has an ActorId. You can send a message to any actor by its ActorId. Inside Receive you can get the sender’s ActorId.

Actors can communicate within a single process or across processes. In the latter case, after Send the message will be put on the network eventually (Send is non‑blocking). Each actor runs strictly on one thread, so no synchronization is required.

Origins: the actor model comes from Erlang (see Wikipedia) and is also popular in Akka for Java/Scala (see Wikipedia). This implementation is inspired primarily by Akka.

- Erlang: https://en.wikipedia.org/wiki/Erlang_(programming_language)
- Akka: https://en.wikipedia.org/wiki/Akka_(toolkit)

## Example: behaviors, custom messages, and networking

Below are excerpts from `examples/behavior_actors.cpp` showing how to define messages, provide serialization, and write a behavior‑based actor. Boilerplate CLI parsing is omitted.

### 1) Define messages

Each message type must have a unique static `MessageId`. POD messages serialize “as is”. For non‑POD messages, provide `SerializeToStream` and `DeserializeFromStream` specializations.

```cpp
struct TMessage {
	static constexpr TMessageId MessageId = 100;
	std::string Text;
};

struct TPing {
	static constexpr TMessageId MessageId = 101;
};

namespace NNet::NActors {

template<>
void SerializeToStream<TMessage>(const TMessage& obj, std::ostringstream& oss) {
	oss << obj.Text;
}

template<>
void DeserializeFromStream<TMessage>(TMessage& obj, std::istringstream& iss) {
	obj.Text = iss.str();
}

} // namespace NNet::NActors
```

Notes:
- POD messages need no custom (de)serialization.
- Non‑POD messages must provide both specializations shown above.

### 2) Write a behavior‑based actor

Use `IBehaviorActor` together with the `TBehavior` helper to get type‑safe message dispatch and the ability to switch behavior at runtime with `Become`.

```cpp
struct TBehaviorActor : public IBehaviorActor {
	struct TRichBehavior : public TBehavior<TRichBehavior, TPing, TMessage> {
		TRichBehavior(TBehaviorActor* parent, const std::string& message)
			: Parent(parent), Message(message) {}

		void Receive(TPing&&, TBlob, TActorContext::TPtr ctx) {
			TMessage msg{Message};
			std::cout << "Sending message: " << msg.Text << "\n";

			auto nodeId = Parent->NodeIds.front();
			if (Parent->NodeIds.size() > 1) {
				nodeId = Parent->NodeIds[1 + rand() % (Parent->NodeIds.size() - 1)];
			}
			auto nextActorId = TActorId{nodeId, ctx->Self().ActorId(), ctx->Self().Cookie()};

			// schedule next tick and send a message
			ctx->Schedule<TPing>(std::chrono::steady_clock::now() + std::chrono::milliseconds(1000), ctx->Self(), ctx->Self());
			ctx->Send<TMessage>(nextActorId, msg);
		}

		void Receive(TMessage&& message, TBlob, TActorContext::TPtr ctx) {
			std::cout << "Received message: " << message.Text << "\n";
			Parent->Become(Parent->Behaviors[++Parent->Index % Parent->Behaviors.size()]);
		}

		void HandleUnknownMessage(TMessageId messageId, TBlob, TActorContext::TPtr) {
			std::cerr << "Unknown message received: " << messageId << "\n";
		}

		TBehaviorActor* Parent;
		std::string Message;
	};

	TBehaviorActor(int myIdx, const std::vector<TNodeId>& nodeIds)
		: NodeIds(nodeIds), Hello(this, "Hello"), World(this, "World!") {
		std::swap(NodeIds[myIdx], NodeIds[0]);
		Become(&Hello);
	}

	std::vector<TNodeId> NodeIds;
	TRichBehavior Hello;
	TRichBehavior World;
	int Index = 0;
	std::vector<IBehavior*> Behaviors = {&Hello, &World};
};
```

Key points:
- `Receive` is the main handler called for each message.
- `Schedule` lets you send messages at a specific time (here, a self‑tick every second).
- `Become` switches the current behavior dynamically.

### 3) Networking and ActorSystem

To enable network interaction, pass a list of nodes to `TActorSystem`. A node is a triple `(hostname, port, nodeId)`. The example uses `TNode<Poller>` and `sys.AddNode(nodeId, std::unique_ptr<INode>)` to register remote nodes. Messages to actors on other nodes are serialized and sent over the network (Send is non‑blocking; delivery is eventual).

Minimal setup sketch (without CLI parsing):

```cpp
using Poller = TDefaultPoller; // or another poller
TInitializer init;
TLoop<Poller> loop;
TResolver resolver(loop.Poller());
TMessagesFactory factory;

factory.RegisterSerializer<TMessage>();
factory.RegisterSerializer<TPing>();

TActorSystem sys(&loop.Poller(), /*myNodeId=*/1);

// Add a remote node (hostname, port, nodeId)
auto node = std::make_unique<TNode<Poller>>(
	loop.Poller(), factory, resolver,
	[&](const TAddress& addr){ return Poller::TSocket(loop.Poller(), addr.Domain()); },
	THostPort("host.example", 9000)
);
sys.AddNode(/*nodeId=*/2, std::move(node));

// Create a listening socket and start serving local + remote
TAddress address{"::", /*port*/ 9000};
Poller::TSocket socket(loop.Poller(), address.Domain());
socket.Bind(address);
socket.Listen();
sys.Serve(std::move(socket));

// Spawn an actor and kick it off
auto pingActor = std::make_unique<TBehaviorActor>(/*myIdx*/0, std::vector<TNodeId>{1,2});
auto pingActorId = sys.Register(std::move(pingActor));
sys.Send<TPing>(TActorId{}, pingActorId);

while (sys.ActorsSize() > 0) {
	loop.Step();
}
```

## API highlights

- `ctx->Send<T>(to, args...)`: send a typed message T to `to`.
- `ctx->Forward<T>(to, args...)`: forward a message, preserving the original sender.
- `ctx->Schedule<T>(when, sender, recipient, args...)`: schedule a message for later delivery.
- `ctx->Cancel(event)`: cancel a scheduled message.
- `ctx->Sleep(duration)` / `ctx->Sleep(until)`: coroutine sleeps.
- `ctx->Ask<T>(recipient, question)`: request‑reply pattern; returns a future.
- `TPoison`: system message; sending it to an actor shuts it down.
- `Become(newBehavior)`: switch the active behavior at runtime (for `IBehaviorActor`).

See `examples/behavior_actors.cpp` for a complete, runnable example.

