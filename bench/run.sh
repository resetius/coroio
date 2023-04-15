#!/bin/bash
BINARY=../build/bench
# isolcpus=10,11 nohz_full=10,11
# 12th Gen Intel(R) Core(TM) i7-12800H
# echo performance | sudo tee /sys/devices/system/cpu/cpu10/cpufreq/scaling_governor
CPU="12800H"
TASKSET="taskset -c 10"
BACKENDS="poll epoll select"

# COMMAND="../build/bench -m epoll -n 100 | grep p99 | awk '{print $2}'"

for backend in $BACKENDS
do
    i=1
    file=netlib_"$CPU"_"$backend".txt
    echo "writing $file"
    > $file
    while (( i <= 200000 ))
    do
        echo $backend $i
        t=`$TASKSET $BINARY -m $backend -n $i 2>/dev/null | grep p50 | awk '{print $2}'`
        echo $i $t >> $file
        ((i=$i * 2))
    done
done
