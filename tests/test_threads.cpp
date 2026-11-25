#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <coroio/all.hpp>
#include <coroio/loop_thread.hpp>
#include <coroio/mpsc_queue.hpp>

#include "testlib.h"

using namespace NNet;

namespace {

struct TItem : TMpscQueueNode<TItem> {
    TItem(int producer, int seq, std::atomic<int>& live)
        : Producer(producer)
        , Seq(seq)
        , Live(live)
    {
        Live.fetch_add(1);
    }

    ~TItem() {
        Live.fetch_sub(1);
    }

    int Producer;
    int Seq;
    std::atomic<int>& Live;
};

// Polls instead of blocking, so a lost wakeup fails the test and does not hang it
template<typename TPredicate>
bool WaitFor(TPredicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

} // namespace

void test_mpsc_queue_single_producer_order() {
    std::atomic<int> live = 0;
    TMpscQueue<TItem> queue;
    ASSERT_TRUE(queue.Pop() == nullptr);

    int pushed = 0;
    int popped = 0;
    // Pops fall both inside a taken batch and at its end
    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 1 + round % 7; ++i) {
            queue.Push(std::make_unique<TItem>(0, pushed++, live));
        }
        for (int i = 0; i < 1 + round % 5; ++i) {
            auto item = queue.Pop();
            if (!item) {
                ASSERT_EQ(popped, pushed);
                break;
            }
            ASSERT_EQ(item->Seq, popped++);
            ASSERT_TRUE(item->Next == nullptr);
        }
    }
    while (auto item = queue.Pop()) {
        ASSERT_EQ(item->Seq, popped++);
    }
    ASSERT_EQ(popped, pushed);
    ASSERT_EQ(live.load(), 0);
}

// Push returns true exactly when the consumer has taken everything pushed before
void test_mpsc_queue_reports_empty() {
    std::atomic<int> live = 0;
    TMpscQueue<TItem> queue;
    ASSERT_TRUE(queue.Push(std::make_unique<TItem>(0, 0, live)));
    ASSERT_FALSE(queue.Push(std::make_unique<TItem>(0, 1, live)));
    // The consumer takes both at the first Pop, so the shared part is empty again
    ASSERT_EQ(queue.Pop()->Seq, 0);
    ASSERT_TRUE(queue.Push(std::make_unique<TItem>(0, 2, live)));
    ASSERT_FALSE(queue.Push(std::make_unique<TItem>(0, 3, live)));
    for (int seq = 1; seq < 4; ++seq) {
        ASSERT_EQ(queue.Pop()->Seq, seq);
    }
    ASSERT_TRUE(queue.Pop() == nullptr);
    ASSERT_TRUE(queue.Push(std::make_unique<TItem>(0, 4, live)));
}

void test_mpsc_queue_deletes_unpopped() {
    std::atomic<int> live = 0;
    {
        TMpscQueue<TItem> queue;
        for (int i = 0; i < 10; ++i) {
            queue.Push(std::make_unique<TItem>(0, i, live));
        }
        // Nine items stay on the consumer side, five more on the shared side
        ASSERT_EQ(queue.Pop()->Seq, 0);
        for (int i = 10; i < 15; ++i) {
            queue.Push(std::make_unique<TItem>(0, i, live));
        }
        ASSERT_EQ(live.load(), 14);
    }
    ASSERT_EQ(live.load(), 0);
}

void test_mpsc_queue_many_producers() {
    constexpr int producerCount = 8;
    constexpr int perProducer = 20000;
    std::atomic<int> live = 0;
    TMpscQueue<TItem> queue;
    std::atomic<bool> start = false;
    std::vector<std::thread> producers;
    for (int p = 0; p < producerCount; ++p) {
        producers.emplace_back([&, p] {
            while (!start.load()) {
                std::this_thread::yield();
            }
            for (int i = 0; i < perProducer; ++i) {
                queue.Push(std::make_unique<TItem>(p, i, live));
            }
        });
    }

    std::vector<int> next(producerCount, 0);
    int total = 0;
    bool ordered = true;
    start.store(true);
    const bool complete = WaitFor([&] {
        while (auto item = queue.Pop()) {
            ordered = ordered && item->Seq == next[item->Producer];
            next[item->Producer] = item->Seq + 1;
            ++total;
        }
        return total == producerCount * perProducer;
    });
    for (auto& producer : producers) {
        producer.join();
    }
    ASSERT_TRUE(complete);
    ASSERT_TRUE(ordered);
    ASSERT_TRUE(queue.Pop() == nullptr);
    ASSERT_EQ(live.load(), 0);
}

#ifndef _WIN32

namespace {

// The consumer side of the wakeup protocol from the TMpscQueue description
template<typename TPoller, typename TFn>
TFuture<void> Consume(TPoller& poller, INotifier& notifier, TMpscQueue<TItem>& queue, TFn fn) {
    while (true) {
        co_await notifier.Wait();
        int batch = 0;
        while (auto item = queue.Pop()) {
            fn(*item);
            // Producers may push as fast as this pops: let the loop poll, so that it sees
            // a stop request. Yield() would be resumed in the same timer pass.
            if (++batch % 256 == 0) {
                co_await poller.Sleep(std::chrono::nanoseconds(1));
            }
        }
    }
}

void Produce(INotifier& notifier, TMpscQueue<TItem>& queue, std::unique_ptr<TItem> item) {
    if (queue.Push(std::move(item))) {
        notifier.Notify();
    }
}

} // namespace

template<typename TPoller>
void test_loop_thread_runs_coroutines() {
    TLoopThread<TPoller> io;
    std::atomic<bool> done = false;
    std::thread::id resumedOn;
    auto task = [](TPoller& poller, std::thread::id& resumedOn, std::atomic<bool>& done) -> TFuture<void> {
        co_await poller.Sleep(std::chrono::milliseconds(1));
        resumedOn = std::this_thread::get_id();
        done.store(true);
    }(io.Poller(), resumedOn, done);
    ASSERT_FALSE(task.done());

    io.Start();
    ASSERT_TRUE(WaitFor([&] { return done.load(); }));
    io.Stop();
    io.Join();
    ASSERT_TRUE(task.done());
    ASSERT_TRUE(resumedOn != std::this_thread::get_id());
    ASSERT_TRUE(resumedOn != std::thread::id());
}

template<typename TPoller>
void test_loop_thread_stop() {
    {
        // Never started
        TLoopThread<TPoller> io;
        io.Stop();
        io.Join();
    }
    {
        // A request made before Start() is kept
        TLoopThread<TPoller> io;
        io.Stop();
        io.Start();
        io.Join();
    }
    {
        // Repeated requests, also after the loop has exited
        TLoopThread<TPoller> io;
        io.Start();
        io.Stop();
        io.Stop();
        io.Join();
        io.Stop();
        io.Join();
    }
    {
        // From another thread, while the loop sleeps in the poller
        TLoopThread<TPoller> io;
        io.Start();
        std::thread stopper([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            io.Stop();
        });
        io.Join();
        stopper.join();
    }
    {
        // The destructor stops a running loop
        TLoopThread<TPoller> io;
        io.Start();
    }
}

// Each producer pushes one item and waits until the consumer has seen it: the consumer
// goes to sleep between the rounds, and a lost wakeup stops the test at the deadline.
template<typename TPoller>
void test_mpsc_queue_wakes_consumer() {
    constexpr int producerCount = 3;
    constexpr int rounds = 2000;
    std::atomic<int> live = 0;
    TLoopThread<TPoller> io;
    TNotifier notifier(io.Poller());
    TMpscQueue<TItem> queue;
    std::array<std::atomic<int>, producerCount> seen = {};
    std::atomic<bool> ordered = true;
    auto consumer = Consume(io.Poller(), notifier, queue, [&](TItem& item) {
        if (item.Seq != seen[item.Producer].load()) {
            ordered.store(false);
        }
        seen[item.Producer].store(item.Seq + 1);
    });
    io.Start();

    std::atomic<int> completed = 0;
    std::vector<std::thread> producers;
    for (int p = 0; p < producerCount; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < rounds; ++i) {
                // Long enough for the consumer to block in the poller
                if (i % 100 == p) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                Produce(notifier, queue, std::make_unique<TItem>(p, i, live));
                if (!WaitFor([&] { return seen[p].load() == i + 1; })) {
                    return;
                }
            }
            completed.fetch_add(1);
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    io.Stop();
    io.Join();
    ASSERT_EQ(completed.load(), producerCount);
    ASSERT_TRUE(ordered.load());
    ASSERT_EQ(live.load(), 0);
}

// The loop stops while the producers keep pushing. What the consumer got is a prefix
// of each producer's sequence, and the queue deletes the rest.
template<typename TPoller>
void test_mpsc_queue_stop_under_load() {
    constexpr int producerCount = 4;
    for (int iteration = 0; iteration < 30; ++iteration) {
        std::atomic<int> live = 0;
        std::atomic<bool> stopProducers = false;
        std::atomic<int> consumed = 0;
        std::vector<int> next(producerCount, 0);
        bool ordered = true;
        std::vector<std::thread> producers;
        {
            TLoopThread<TPoller> io;
            TNotifier notifier(io.Poller());
            TMpscQueue<TItem> queue;
            // next and ordered belong to the loop thread until Join()
            auto consumer = Consume(io.Poller(), notifier, queue, [&](TItem& item) {
                ordered = ordered && item.Seq == next[item.Producer];
                next[item.Producer] = item.Seq + 1;
                consumed.fetch_add(1);
            });
            io.Start();
            for (int p = 0; p < producerCount; ++p) {
                producers.emplace_back([&, p] {
                    for (int i = 0; !stopProducers.load(); ++i) {
                        Produce(notifier, queue, std::make_unique<TItem>(p, i, live));
                        if (i % 64 == 0) {
                            std::this_thread::yield();
                        }
                    }
                });
            }
            const bool running = WaitFor([&] { return consumed.load() > 1000 * (1 + iteration % 3); });
            io.Stop();
            io.Join();
            // The producers outlive the loop, but not the queue and the notifier.
            // Nobody pops this item any more.
            Produce(notifier, queue, std::make_unique<TItem>(0, -1, live));
            stopProducers.store(true);
            for (auto& producer : producers) {
                producer.join();
            }
            ASSERT_TRUE(running);
            ASSERT_TRUE(ordered);
            ASSERT_TRUE(live.load() > 0);
        }
        ASSERT_EQ(live.load(), 0);
    }
}

#endif // _WIN32

#define TEST_SUITE_NAME "test_threads"

#define my_unit_test(f, a) RegisterTest(#f, #a, f<a>)
#define my_unit_test2(f, a, b) my_unit_test(f, a); my_unit_test(f, b)
#define my_unit_test3(f, a, b, c) my_unit_test2(f, a, b); my_unit_test(f, c)
#define my_unit_test4(f, a, b, c, d) my_unit_test3(f, a, b, c); my_unit_test(f, d)

#ifdef __linux__
#ifdef HAVE_URING
#define my_unit_poller(f) my_unit_test4(f, TSelect, TPoll, TEPoll, TUring)
#else
#define my_unit_poller(f) my_unit_test3(f, TSelect, TPoll, TEPoll)
#endif
#elif defined(__APPLE__) || defined(__FreeBSD__)
#define my_unit_poller(f) my_unit_test3(f, TSelect, TPoll, TKqueue)
#else
#define my_unit_poller(f) my_unit_test2(f, TSelect, TPoll)
#endif

int main(int argc, char* argv[]) {
    TInitializer init;

    testing::InitGoogleTest(&argc, argv);

    ADD_TEST(unit_test, test_mpsc_queue_single_producer_order);
    ADD_TEST(unit_test, test_mpsc_queue_reports_empty);
    ADD_TEST(unit_test, test_mpsc_queue_deletes_unpopped);
    ADD_TEST(unit_test, test_mpsc_queue_many_producers);
#ifndef _WIN32
    ADD_TEST(my_unit_poller, test_loop_thread_runs_coroutines);
    ADD_TEST(my_unit_poller, test_loop_thread_stop);
    ADD_TEST(my_unit_poller, test_mpsc_queue_wakes_consumer);
    ADD_TEST(my_unit_poller, test_mpsc_queue_stop_under_load);
#endif

    return RUN_ALL_TESTS();
}
