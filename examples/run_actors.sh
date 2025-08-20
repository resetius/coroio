#!/bin/bash

N=${1:-10}  # Number of nodes
INFL=${2:-2} # Inflight
MESSIZE=${3:-0} # Message Size
READERTYPE=${4:-0} # Reader Type
METHOD=${5:-default} # Poller Type
BASE_PORT=2001

NODE_ARGS=""
for ((i=1; i<=N; ++i)); do
  NODE_ARGS+=" --node localhost:$((BASE_PORT + i - 1)):$i"
done

pkill -9 -f ping_actors

ulimit -c unlimited

# Start all nodes in background
echo $NODE_ARGS
for ((i=2; i<=N; ++i)); do
  echo "Starting node $i in background"
  ./examples/ping_actors --method $METHOD --reader-type $READERTYPE --message-size $MESSIZE --node-id $i $NODE_ARGS > ping_node_$i.log 2>&1 &
done

# Run first node in foreground
echo "Starting node 1 in foreground"
#sudo perf record -g ./examples/ping_actors --inflight $INFL --message-size $MESSIZE --messages 10000000 --node-id 1 $NODE_ARGS
./examples/ping_actors --inflight $INFL --method $METHOD --reader-type $READERTYPE --message-size $MESSIZE --messages 10000000 --node-id 1 $NODE_ARGS
