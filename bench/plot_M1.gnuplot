set logscale x 2
set logscale y 2
set xlabel "Number of the descriptors"
set ylabel "Time in microseconds"
set title "One Active Connection (M1)"
set terminal pngcairo dashed size 800,600
set output 'bench_M1.png'
plot \
    "netlib_M1_kqueue.txt" with lines dt 2 lw 3 title "netlib kqueue",\
    "netlib_M1_poll.txt" with lines dt 3 lw 3 title "netlib poll",\
    "netlib_M1_select.txt" with lines dt 4 lw 3 title "netlib select",\
    "libevent_M1_kqueue.txt" with lines dt ".. " lw 3 lc rgb '#4488bb' title "libevent kqueue",\
    "libevent_M1_poll.txt" with lines dt "-- " lw 3 title "libevent poll",\
    "libevent_M1_select.txt" with lines dt ". . -" lw 3 title "libevent select"

