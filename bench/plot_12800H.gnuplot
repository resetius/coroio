set logscale x 2
set logscale y 2
set xlabel "Number of the descriptors"
set ylabel "Time in microseconds"
set title "One Active Connection (i7-12800H)"
set terminal pngcairo dashed size 800,600
set output 'bench_12800H.png'
plot \
    "netlib_12800H_epoll.txt" with lines dt 2 lw 3 title "netlib epoll",\
    "netlib_12800H_poll.txt" with lines dt 3 lw 3 title "netlib poll",\
    "netlib_12800H_select.txt" with lines dt 4 lw 3 title "netlib select",\
    "netlib_12800H_uring.txt" with lines dt 5 lw 3 title "netlib uring",\
    "libevent_12800H_epoll.txt" with lines dt ".. " lw 3 lc rgb '#4488bb' title "libevent epoll",\
    "libevent_12800H_poll.txt" with lines dt "-- " lw 3 title "libevent poll",\
    "libevent_12800H_select.txt" with lines dt ". . -" lw 3 title "libevent select"

