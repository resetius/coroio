# netlib
This is a simple network library which uses C++ coroutines. 
The library supports the following backends: ```poll```, ```select```, ```epoll```, ```uring```, ```kqueue```.
For ```uring``` the [liburing library](https://github.com/axboe/liburing) is used. 

## Benchmark

The benchmark methodology was taken from the [libevent library](https://libevent.org).

There are two benchmarks. The first one measures how long it takes to serve one active connection and exposes scalability issues of traditional interfaces like select or poll. The second benchmark measures how long it takes to serve one hundred active connections that chain writes to new connections until thousand writes and reads have happened. It exercises the event loop several times.

Performance comparison using different event notification mechansims in Libevent and netlib as follows.

* CPU i7-12800H
* Ubuntu 23.04
* clang 16
* libevent master 4c993a0e7bcd47b8a56514fb2958203f39f1d906 (Tue Apr 11 04:44:37 2023 +0000)

<img src="/bench/bench_12800H.png?raw=true" width="400"/><img src="/bench/bench_12800H_100.png?raw=true" width="400"/>


* CPU i5-11400F
* Ubuntu 23.04, WSL2, kernel 6.1.21.1-microsoft-standard-WSL2+

<img src="/bench/bench_11400F.png?raw=true" width="400"/><img src="/bench/bench_11400F_100.png?raw=true" width="400"/>

* CPU Apple M1
* MacBook Air M1 16G
* MacOS 12.6.3

<img src="/bench/bench_M1.png?raw=true" width="400"/><img src="/bench/bench_M1_100.png?raw=true" width="400"/>

