set logscale x 2
set logscale y 2
set xlabel "Number of the descriptors"
set ylabel "Time in microseconds"
set title "One Active Connection (i5-11400F, WSL2)"
set terminal pngcairo dashed size 800,600
set output 'bench_11400F.png'
plot \
    "netlib_11400F_epoll.txt" with lines dt 2 lw 3 title "netlib epoll",\
    "netlib_11400F_poll.txt" with lines dt 3 lw 3 title "netlib poll",\
    "netlib_11400F_select.txt" with lines dt 4 lw 3 title "netlib select",\
    "netlib_11400F_uring.txt" with lines dt 5 lw 3 title "netlib uring",\
    "libevent_11400F_epoll.txt" with lines dt ".. " lw 3 lc rgb '#4488bb' title "libevent epoll",\
    "libevent_11400F_poll.txt" with lines dt "-- " lw 3 title "libevent poll",\
    "libevent_11400F_select.txt" with lines dt ". . -" lw 3 title "libevent select"

