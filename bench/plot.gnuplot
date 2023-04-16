set logscale x 2
set logscale y 2
set xlabel "Number of the descriptors"
set ylabel "Time in microseconds"
set title "One Active Connection (i7-12800H)"
set terminal pngcairo size 800,600
set output 'bench_12800H.png'
plot \
    "netlib_12800H_epoll.txt" with lines title "netlib epoll",\
    "netlib_12800H_poll.txt" with lines title "netlib poll",\
    "netlib_12800H_select.txt" with lines title "netlib select",\
    "libevent_12800H_epoll.txt" with lines title "libevent epoll",\
    "libevent_12800H_poll.txt" with lines title "libevent poll",\
    "libevent_12800H_select.txt" with lines title "libevent select"

