#!/bin/bash

N=${1:-10}  # Number of nodes
INFL=${2:-2} # Inflight
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
  ./examples/ping_actors --node-id $i $NODE_ARGS > ping_node_$i.log 2>&1 &
done

# Run first node in foreground
echo "Starting node 1 in foreground"
./examples/ping_actors --inflight $INFL --messages 10000000 --node-id 1 $NODE_ARGS
