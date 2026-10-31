#include "thread_pool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ── test harness ─────────────────────────────────────────────────────────────

static int g_failed = 0;

static void check(bool cond, const char* msg) {
    if (!cond) throw std::runtime_error(msg);
}

template <typename F>
static void runTest(const char* name, F&& f) {
    try {
        f();
        std::cout << "[PASS] " << name << "\n";
    } catch (const std::exception& e) {
        std::cout << "[FAIL] " << name << ": " << e.what() << "\n";
        ++g_failed;
    } catch (...) {
        std::cout << "[FAIL] " << name << ": unknown exception\n";
        ++g_failed;
    }
}

static constexpr auto kTimeout = std::chrono::seconds(5);

template <typename T>
static void waitOrThrow(std::future<T>& fut, const char* msg) {
    if (fut.wait_for(kTimeout) != std::future_status::ready)
        throw std::runtime_error(msg);
}

// ── test 1 ────────────────────────────────────────────────────────────────────
static void test_constructor_thread_count() {
    const size_t N = 4;

    std::atomic<size_t> running{0};
    std::mutex mtx;
    std::condition_variable cv;
    bool release = false;

    auto allRunning = std::make_shared<std::promise<void>>();
    std::atomic<bool> flagSet{false};
    auto fut = allRunning->get_future();

    {
        ThreadPool pool(N);

        for (size_t i = 0; i < N; ++i) {
            pool.submit([&, allRunning] {
                size_t cur = running.fetch_add(1) + 1;
                if (cur == N && !flagSet.exchange(true))
                    allRunning->set_value();

                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&] { return release; });
            });
        }

        bool ok = (fut.wait_for(kTimeout) == std::future_status::ready);
        {
            std::unique_lock<std::mutex> lock(mtx);
            release = true;
        }
        cv.notify_all();

        check(ok, "not all threads started — pool may have fewer threads than requested");
    }
}

// ── test 2 ────────────────────────────────────────────────────────────────────
static void test_single_task() {
    auto p = std::make_shared<std::promise<int>>();
    auto fut = p->get_future();

    {
        ThreadPool pool(2);
        pool.submit([p] { p->set_value(42); });
        waitOrThrow(fut, "single task was not executed within timeout");
    }

    check(fut.get() == 42, "task produced unexpected value");
}

// ── test 3 ────────────────────────────────────────────────────────────────────
static void test_no_task_loss() {
    const int N = 100;
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4);
        for (int i = 0; i < N; ++i)
            pool.submit([&counter] { counter.fetch_add(1); });
    }
    check(counter.load() == N, "some tasks were lost");
}

// ── test 4 ────────────────────────────────────────────────────────────────────
static void test_large_task_count() {
    const int N = 10'000;
    std::atomic<int> counter{0};
    {
        ThreadPool pool(8);
        for (int i = 0; i < N; ++i)
            pool.submit([&counter] { counter.fetch_add(1); });
    }
    check(counter.load() == N, "not all tasks from large batch were executed");
}

// ── test 5 ────────────────────────────────────────────────────────────────────
static void test_parallel_execution() {
    const size_t N = 4;

    std::atomic<size_t> running{0};
    std::mutex mtx;
    std::condition_variable cvAll, cvDone;
    bool allStarted = false;
    bool releaseAll = false;

    std::vector<std::future<void>> futs;
    futs.reserve(N);

    {
        ThreadPool pool(N);

        for (size_t i = 0; i < N; ++i) {
            auto p = std::make_shared<std::promise<void>>();
            futs.push_back(p->get_future());

            pool.submit([&, p] {
                {
                    std::unique_lock<std::mutex> lock(mtx);
                    if (++running == N) {
                        allStarted = true;
                        cvAll.notify_one();
                    }
                    cvDone.wait(lock, [&] { return releaseAll; });
                }
                p->set_value();
            });
        }

        {
            std::unique_lock<std::mutex> lock(mtx);
            bool ok = cvAll.wait_for(lock, kTimeout, [&] { return allStarted; });
            releaseAll = true;
            check(ok, "tasks did not start in parallel — possible serialization");
        }
        cvDone.notify_all();

        for (auto& f : futs)
            f.get();
    }
}

// ── test 6 ────────────────────────────────────────────────────────────────────
static void test_max_concurrency() {
    const size_t POOL  = 3;
    const int    TASKS = 30;

    std::atomic<int> active{0};
    std::atomic<int> maxActive{0};

    {
        ThreadPool pool(POOL);
        for (int i = 0; i < TASKS; ++i) {
            pool.submit([&] {
                int cur = active.fetch_add(1) + 1;
                int expected = maxActive.load();
                while (cur > expected &&
                       !maxActive.compare_exchange_weak(expected, cur))
                    ;
                std::this_thread::yield();
                active.fetch_sub(1);
            });
        }
    }

    check(maxActive.load() <= static_cast<int>(POOL),
          "more tasks ran concurrently than the pool size allows");
}

// ── test 7 ────────────────────────────────────────────────────────────────────
static void test_sequential_submit() {
    const int N = 20;
    std::atomic<int> counter{0};
    ThreadPool pool(2);

    for (int i = 0; i < N; ++i) {
        auto p = std::make_shared<std::promise<void>>();
        auto fut = p->get_future();

        pool.submit([&counter, p] {
            counter.fetch_add(1);
            p->set_value();
        });

        waitOrThrow(fut, "sequentially submitted task was not executed within timeout");
    }

    check(counter.load() == N, "not all sequentially submitted tasks ran");
}

// ── test 8 ────────────────────────────────────────────────────────────────────
static void test_destructor_joins() {
    std::atomic<bool> taskCompleted{false};
    {
        ThreadPool pool(2);
        pool.submit([&taskCompleted] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            taskCompleted.store(true, std::memory_order_release);
        });
    }
    check(taskCompleted.load(std::memory_order_acquire),
          "destructor returned before the running task completed — likely detach instead of join");
}

// ── test 9 ────────────────────────────────────────────────────────────────────
static void test_queued_tasks_drained_on_shutdown() {
    const int N = 500;
    std::atomic<int> counter{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < N; ++i)
            pool.submit([&counter] { counter.fetch_add(1); });
    }
    check(counter.load() == N, "queued tasks were discarded during shutdown");
}

// ── test 10 ───────────────────────────────────────────────────────────────────
static void test_exception_in_task() {
    const int AFTER = 50;
    std::atomic<int> counter{0};

    auto exThrown = std::make_shared<std::promise<void>>();
    auto exFut = exThrown->get_future();

    {
        ThreadPool pool(2);

        pool.submit([exThrown] {
            exThrown->set_value();
            throw std::runtime_error("intentional exception");
        });
        waitOrThrow(exFut, "throwing task was not executed");

        for (int i = 0; i < AFTER; ++i)
            pool.submit([&counter] { counter.fetch_add(1); });
    }

    check(counter.load() == AFTER,
          "tasks after an exception were not executed — worker thread may have crashed");
}

// ── test 11 ───────────────────────────────────────────────────────────────────
static void test_zero_threads() {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(0);
        pool.submit([&counter] { counter.fetch_add(1); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(counter.load() == 0,
          "task ran in a zero-thread pool — unexpected execution");
}

// ── test 12 ───────────────────────────────────────────────────────────────────
static void test_empty_function() {
    auto p = std::make_shared<std::promise<void>>();
    auto fut = p->get_future();

    ThreadPool pool(2);
    pool.submit(std::function<void()>{});
    pool.submit([p] { p->set_value(); });

    waitOrThrow(fut, "pool stopped working after empty-function submit");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    runTest("constructor_thread_count",          test_constructor_thread_count);
    runTest("single_task",                       test_single_task);
    runTest("no_task_loss",                      test_no_task_loss);
    runTest("large_task_count",                  test_large_task_count);
    runTest("parallel_execution",                test_parallel_execution);
    runTest("max_concurrency",                   test_max_concurrency);
    runTest("sequential_submit",                 test_sequential_submit);
    runTest("destructor_joins",                  test_destructor_joins);
    runTest("queued_tasks_drained_on_shutdown",  test_queued_tasks_drained_on_shutdown);
    runTest("exception_in_task",                 test_exception_in_task);
    runTest("zero_threads",                      test_zero_threads);
    runTest("empty_function",                    test_empty_function);

    std::cout << "\n";
    if (g_failed == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    }
    std::cout << g_failed << " test(s) FAILED.\n";
    return 1;
}
