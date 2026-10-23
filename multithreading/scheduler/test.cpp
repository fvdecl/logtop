#include "scheduler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <ctime>
#endif

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

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

static constexpr auto kTimeout = std::chrono::seconds(5);
static constexpr auto kFarFuture = std::chrono::hours(1);

// Ждёт future; бросает std::runtime_error при превышении timeout.
template <typename T>
static void waitOrThrow(std::future<T>& fut, const char* msg,
                        std::chrono::milliseconds timeout = kTimeout) {
    if (fut.wait_for(timeout) != std::future_status::ready)
        throw std::runtime_error(msg);
}

// Процессорное время всего процесса (user + kernel), а не настенное.
static std::chrono::milliseconds processCpuTime() {
#ifdef _WIN32
    FILETIME creation, exit, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    auto toTicks = [](const FILETIME& ft) {
        return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    // FILETIME считается в 100-наносекундных интервалах
    return std::chrono::milliseconds((toTicks(kernel) + toTicks(user)) / 10'000);
#else
    return std::chrono::milliseconds(std::clock() * 1000 / CLOCKS_PER_SEC);
#endif
}

// Блокирует рабочий поток, пока не будет вызван release().
// Позволяет накопить задачи в очереди до того, как они начнут выполняться.
struct Gate {
    std::mutex mtx;
    std::condition_variable cv;
    bool released = false;

    void wait() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return released; });
    }

    void release() {
        {
            std::unique_lock<std::mutex> lock(mtx);
            released = true;
        }
        cv.notify_all();
    }
};

// ── test 1 ────────────────────────────────────────────────────────────────────
// Одна задача с нулевой задержкой выполняется.
//
// Обнаруживает: потерю notify при добавлении; рабочий поток, который не
// забирает задачи из очереди.
static void test_single_task() {
    auto p = std::make_shared<std::promise<int>>();
    auto fut = p->get_future();

    Scheduler scheduler;
    scheduler.scheduleAfter(0ms, [p] { p->set_value(42); });

    waitOrThrow(fut, "task was not executed within timeout");
    check(fut.get() == 42, "task produced unexpected value");
}

// ── test 2 ────────────────────────────────────────────────────────────────────
// Задача не выполняется раньше заданной задержки и не задерживается сверх
// разумного допуска планировщика ОС.
//
// Обнаруживает: игнорирование задержки; ожидание по неверным часам.
static void test_not_before_delay() {
    const auto delay = 50ms;
    auto p = std::make_shared<std::promise<Clock::time_point>>();
    auto fut = p->get_future();

    Scheduler scheduler;
    const auto start = Clock::now();
    scheduler.scheduleAfter(delay, [p] { p->set_value(Clock::now()); });

    waitOrThrow(fut, "delayed task was not executed within timeout");
    const auto elapsed = fut.get() - start;
    check(elapsed >= delay, "task ran before its delay elapsed");
    check(elapsed < delay + 1s, "task ran far later than its delay");
}

// ── test 2a ───────────────────────────────────────────────────────────────────
// Каждая из нескольких задач выполняется не раньше своего срока.
//
// Обнаруживает: запуск задачи по сроку соседней; ожидание только первого срока.
static void test_deadlines_respected() {
    static constexpr size_t N = 5;
    std::mutex mtx;
    std::vector<Clock::duration> lateness;  // фактическое время минус срок
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();

    {
        Scheduler scheduler;
        for (int i = static_cast<int>(N); i >= 1; --i) {
            const auto delay = std::chrono::milliseconds(i * 25);
            const auto deadline = Clock::now() + delay;
            scheduler.scheduleAfter(delay, [&, done, deadline] {
                std::unique_lock<std::mutex> lock(mtx);
                lateness.push_back(Clock::now() - deadline);
                if (lateness.size() == N) done->set_value();
            });
        }
        waitOrThrow(fut, "not all tasks were executed within timeout");
    }

    for (const auto& late : lateness) {
        check(late >= Clock::duration::zero(), "task ran before its deadline");
        check(late < 1s, "task ran far later than its deadline");
    }
}

// ── test 3 ────────────────────────────────────────────────────────────────────
// Задачи, добавленные вразнобой, выполняются в порядке срока.
//
// Обнаруживает: max-кучу вместо min-кучи; выполнение в порядке добавления.
static void test_time_order() {
    std::mutex mtx;
    std::vector<int> order;
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();

    {
        Scheduler scheduler;
        auto record = [&, done](int id) {
            return [&, done, id] {
                std::unique_lock<std::mutex> lock(mtx);
                order.push_back(id);
                if (order.size() == 3) done->set_value();
            };
        };
        scheduler.scheduleAfter(60ms, record(3));
        scheduler.scheduleAfter(20ms, record(1));
        scheduler.scheduleAfter(40ms, record(2));

        waitOrThrow(fut, "not all tasks were executed within timeout");
    }

    check(order == std::vector<int>({1, 2, 3}), "tasks ran out of time order");
}

// ── test 4 ────────────────────────────────────────────────────────────────────
// Задачи с одинаковой задержкой выполняются в порядке добавления.
//
// Метод: первая задача блокирует рабочий поток, пока остальные копятся в
// очереди. После release все они просрочены и выбираются из кучи.
//
// Обнаруживает: нестабильный порядок кучи при равных сроках.
static void test_fifo_for_equal_delay() {
    static constexpr size_t N = 50;
    Gate gate;
    std::vector<size_t> order;
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();

    {
        Scheduler scheduler;
        scheduler.scheduleAfter(0ms, [&gate] { gate.wait(); });
        for (size_t i = 0; i < N; ++i) {
            scheduler.scheduleAfter(0ms, [&order, done, i] {
                order.push_back(i);  // задачи выполняются в одном потоке
                if (order.size() == N) done->set_value();
            });
        }
        gate.release();

        waitOrThrow(fut, "not all tasks were executed within timeout");
    }

    for (size_t i = 0; i < N; ++i)
        check(order[i] == i, "tasks with equal delay ran out of submission order");
}

// ── test 5 ────────────────────────────────────────────────────────────────────
// Задача с ранним сроком, добавленная во время ожидания поздней, выполняется
// без ожидания срока поздней.
//
// Обнаруживает: рабочий поток, который не пересчитывает время ожидания
// после добавления более ранней задачи.
static void test_earlier_task_preempts_wait() {
    auto p = std::make_shared<std::promise<void>>();
    auto fut = p->get_future();

    Scheduler scheduler;
    scheduler.scheduleAfter(std::chrono::duration_cast<std::chrono::milliseconds>(kFarFuture),
                            [] {});
    std::this_thread::sleep_for(20ms);  // даём рабочему потоку уйти в ожидание
    scheduler.scheduleAfter(10ms, [p] { p->set_value(); });

    waitOrThrow(fut, "earlier task waited for the later deadline", 1000ms);
}

// ── test 5a ───────────────────────────────────────────────────────────────────
// Рабочий поток не расходует процессор, пока ждёт пустую очередь, далёкий
// срок или «бесконечный» срок.
//
// Метод: сравниваем процессорное время процесса за 300 мс ожидания. Активное
// ожидание одного потока съело бы ~300 мс; допуск — 100 мс.
//
// Обнаруживает: busy-wait; цикл опроса времени; переполнение wait_until(max).
static void test_no_busy_wait() {
    const auto idle = 300ms;
    const auto budget = 100ms;

    Scheduler empty;
    Scheduler farFuture;
    farFuture.scheduleAfter(std::chrono::duration_cast<std::chrono::milliseconds>(kFarFuture),
                            [] {});
    Scheduler never;
    never.scheduleAfter(std::chrono::milliseconds::max(), [] {});

    const auto cpuBefore = processCpuTime();
    std::this_thread::sleep_for(idle);
    const auto cpuUsed = processCpuTime() - cpuBefore;

    check(cpuUsed < budget, "waiting worker thread consumed CPU time (busy-wait)");
}

// ── test 6 ────────────────────────────────────────────────────────────────────
// Одновременный scheduleAfter из нескольких потоков не теряет задачи.
//
// Обнаруживает: гонку при вставке в кучу; повреждение кучи.
static void test_concurrent_schedule() {
    static constexpr int THREADS = 8;
    static constexpr int PER_THREAD = 1000;
    static constexpr int TOTAL = THREADS * PER_THREAD;

    std::atomic<int> counter{0};
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();

    Scheduler scheduler;
    std::vector<std::thread> producers;
    for (int t = 0; t < THREADS; ++t) {
        producers.emplace_back([&scheduler, &counter, done, t] {
            for (int i = 0; i < PER_THREAD; ++i) {
                scheduler.scheduleAfter(std::chrono::milliseconds((t + i) % 5), [&counter, done] {
                    if (counter.fetch_add(1) + 1 == TOTAL) done->set_value();
                });
            }
        });
    }
    for (auto& producer : producers)
        producer.join();

    waitOrThrow(fut, "some concurrently scheduled tasks were lost");
    check(counter.load() == TOTAL, "unexpected number of executed tasks");
}

// ── test 6a ───────────────────────────────────────────────────────────────────
// Длительная задача не блокирует scheduleAfter: мьютекс не удерживается во
// время выполнения пользовательского кода.
//
// Обнаруживает: вызов задачи под мьютексом планировщика.
static void test_long_task_does_not_block_schedule() {
    static constexpr int N = 100;
    Gate gate;
    std::atomic<int> counter{0};
    auto started = std::make_shared<std::promise<void>>();
    auto startedFut = started->get_future();
    auto done = std::make_shared<std::promise<void>>();
    auto doneFut = done->get_future();

    Scheduler scheduler;
    scheduler.scheduleAfter(0ms, [&gate, started] {
        started->set_value();
        gate.wait();
    });
    waitOrThrow(startedFut, "long task was not started within timeout");

    // scheduleAfter выполняется в отдельном потоке, чтобы при дефекте тест
    // упал по таймауту, а не завис навсегда
    auto submitted = std::async(std::launch::async, [&] {
        for (int i = 0; i < N; ++i) {
            scheduler.scheduleAfter(0ms, [&counter, done] {
                if (counter.fetch_add(1) + 1 == N) done->set_value();
            });
        }
    });
    const bool ok = submitted.wait_for(1s) == std::future_status::ready;
    gate.release();
    submitted.get();
    check(ok, "scheduleAfter blocked while a long task was running");

    waitOrThrow(doneFut, "tasks submitted during a long task were not executed");
}

// ── test 7 ────────────────────────────────────────────────────────────────────
// Scheduler с пустой очередью создаётся и уничтожается без зависаний, в том
// числе сразу после создания и после выполнения всех задач.
//
// Обнаруживает: потерянное уведомление при остановке; зависание деструктора
// на cv_.wait.
static void test_destroy_empty() {
    const auto start = Clock::now();
    for (int i = 0; i < 200; ++i) {
        Scheduler scheduler;
    }
    {
        auto p = std::make_shared<std::promise<void>>();
        auto fut = p->get_future();
        Scheduler scheduler;
        scheduler.scheduleAfter(0ms, [p] { p->set_value(); });
        waitOrThrow(fut, "task was not executed within timeout");
    }
    check(Clock::now() - start < kTimeout, "destroying an empty scheduler took too long");
}

// ── test 7a ───────────────────────────────────────────────────────────────────
// Деструктор не ждёт отложенные задачи и не выполняет их.
//
// Обнаруживает: деструктор, ждущий срока последней задачи; выполнение
// отложенных задач при остановке.
static void test_destructor_discards_pending() {
    std::atomic<bool> executed{false};
    const auto start = Clock::now();
    {
        Scheduler scheduler;
        scheduler.scheduleAfter(std::chrono::duration_cast<std::chrono::milliseconds>(kFarFuture),
                                [&executed] { executed.store(true); });
    }
    check(Clock::now() - start < kTimeout, "destructor waited for a pending task");
    check(!executed.load(), "pending task was executed during shutdown");
}

// ── test 8 ────────────────────────────────────────────────────────────────────
// После возврата из деструктора отложенная задача не выполняется.
//
// Обнаруживает: detach рабочего потока; выполнение задачи после разрушения
// объекта (use-after-free захватов).
static void test_no_execution_after_destruction() {
    std::atomic<bool> executed{false};
    {
        Scheduler scheduler;
        scheduler.scheduleAfter(30ms, [&executed] { executed.store(true); });
    }
    std::this_thread::sleep_for(100ms);
    check(!executed.load(), "task ran after the scheduler was destroyed");
}

// ── test 8a ───────────────────────────────────────────────────────────────────
// Многократное уничтожение планировщика, когда часть задач уже выполнена,
// а часть ещё ждёт срока.
//
// Метод: задачи держат shared_ptr на объект-маркер. После уничтожения всех
// планировщиков use_count должен вернуться к 1: отброшенные задачи разрушены.
//
// Обнаруживает: утечку отброшенных задач; выполнение после деструктора;
// гонку остановки с рабочим потоком (при сборке с ASan — use-after-free).
static void test_destroy_with_pending_stress() {
    static constexpr int ROUNDS = 200;
    auto token = std::make_shared<int>(0);
    std::atomic<bool> destroyed{false};
    std::atomic<bool> ranAfterDestruction{false};

    for (int round = 0; round < ROUNDS; ++round) {
        destroyed.store(false);
        {
            Scheduler scheduler;
            for (int i = 0; i < 20; ++i) {
                scheduler.scheduleAfter(std::chrono::milliseconds(i % 4),
                                        [token, &destroyed, &ranAfterDestruction] {
                    if (destroyed.load()) ranAfterDestruction.store(true);
                });
            }
            if (round % 2 == 0)
                std::this_thread::sleep_for(1ms);
        }
        destroyed.store(true);
    }
    std::this_thread::sleep_for(20ms);

    check(!ranAfterDestruction.load(), "task ran after its scheduler was destroyed");
    check(token.use_count() == 1, "discarded tasks were not destroyed");
}

// ── test 9 ────────────────────────────────────────────────────────────────────
// Деструктор дожидается задачи, которая уже выполняется (join, не detach).
//
// Обнаруживает: деструктор, возвращающийся до завершения текущей задачи.
static void test_destructor_joins_running_task() {
    std::atomic<bool> completed{false};
    auto started = std::make_shared<std::promise<void>>();
    auto fut = started->get_future();
    {
        Scheduler scheduler;
        scheduler.scheduleAfter(0ms, [&completed, started] {
            started->set_value();
            std::this_thread::sleep_for(50ms);
            completed.store(true, std::memory_order_release);
        });
        waitOrThrow(fut, "task was not started within timeout");
    }
    check(completed.load(std::memory_order_acquire),
          "destructor returned before the running task completed");
}

// ── test 10 ───────────────────────────────────────────────────────────────────
// Исключение в задаче не останавливает рабочий поток.
//
// Обнаруживает: std::terminate из рабочего потока; поток, прекративший
// обработку после исключения.
static void test_exception_in_task() {
    auto p = std::make_shared<std::promise<void>>();
    auto fut = p->get_future();

    Scheduler scheduler;
    scheduler.scheduleAfter(0ms, [] { throw std::runtime_error("intentional exception"); });
    scheduler.scheduleAfter(10ms, [p] { p->set_value(); });

    waitOrThrow(fut, "task after an exception was not executed");
}

// ── test 11 ───────────────────────────────────────────────────────────────────
// Пустая std::function игнорируется и не ломает планировщик.
//
// Обнаруживает: std::bad_function_call в рабочем потоке.
static void test_empty_function() {
    auto p = std::make_shared<std::promise<void>>();
    auto fut = p->get_future();

    Scheduler scheduler;
    scheduler.scheduleAfter(0ms, std::function<void()>{});
    scheduler.scheduleAfter(0ms, [p] { p->set_value(); });

    waitOrThrow(fut, "scheduler stopped working after empty-function submit");
}

// ── test 12 ───────────────────────────────────────────────────────────────────
// Отрицательная задержка не откладывает задачу и не обгоняется задачами с
// бо́льшим сроком.
//
// Обнаруживает: переполнение/некорректную обработку отрицательной задержки.
static void test_negative_delay() {
    auto p = std::make_shared<std::promise<void>>();
    auto fut = p->get_future();

    Scheduler scheduler;
    scheduler.scheduleAfter(std::chrono::duration_cast<std::chrono::milliseconds>(kFarFuture),
                            [] {});
    scheduler.scheduleAfter(-1000ms, [p] { p->set_value(); });

    waitOrThrow(fut, "task with negative delay was not executed promptly");
}

// ── test 13 ───────────────────────────────────────────────────────────────────
// Предельные задержки не приводят к переполнению времени.
//
// Обнаруживает: переполнение now + delay, превращающее «никогда» в «сейчас».
static void test_extreme_delays() {
    std::atomic<bool> executed{false};
    auto p = std::make_shared<std::promise<void>>();
    auto fut = p->get_future();
    {
        Scheduler scheduler;
        scheduler.scheduleAfter(std::chrono::milliseconds::max(),
                                [&executed] { executed.store(true); });
        scheduler.scheduleAfter(std::chrono::milliseconds::min(), [p] { p->set_value(); });
        waitOrThrow(fut, "task with minimal delay was not executed");
        std::this_thread::sleep_for(20ms);
    }
    check(!executed.load(), "task with maximal delay was executed");
}

// ── test 14 ───────────────────────────────────────────────────────────────────
// Задача может планировать новую задачу.
//
// Обнаруживает: выполнение задачи под мьютексом планировщика (deadlock).
static void test_schedule_from_task() {
    auto p = std::make_shared<std::promise<void>>();
    auto fut = p->get_future();

    Scheduler scheduler;
    scheduler.scheduleAfter(0ms, [&scheduler, p] {
        scheduler.scheduleAfter(10ms, [p] { p->set_value(); });
    });

    waitOrThrow(fut, "task scheduled from another task was not executed");
}

// ── test 15 ───────────────────────────────────────────────────────────────────
// Все задачи выполняются в одном отдельном потоке, не параллельно.
//
// Обнаруживает: выполнение в вызывающем потоке; несколько рабочих потоков.
static void test_single_worker_thread() {
    static constexpr size_t N = 20;
    std::mutex mtx;
    std::vector<std::thread::id> ids;
    std::atomic<int> active{0};
    std::atomic<int> maxActive{0};
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();

    {
        Scheduler scheduler;
        for (size_t i = 0; i < N; ++i) {
            scheduler.scheduleAfter(std::chrono::milliseconds(i % 3), [&, done] {
                int cur = active.fetch_add(1) + 1;
                int expected = maxActive.load();
                while (cur > expected && !maxActive.compare_exchange_weak(expected, cur))
                    ;
                std::this_thread::yield();
                active.fetch_sub(1);

                std::unique_lock<std::mutex> lock(mtx);
                ids.push_back(std::this_thread::get_id());
                if (ids.size() == N) done->set_value();
            });
        }
        waitOrThrow(fut, "not all tasks were executed within timeout");
    }

    check(maxActive.load() == 1, "tasks ran concurrently");
    for (const auto& id : ids) {
        check(id == ids.front(), "tasks ran in different threads");
        check(id != std::this_thread::get_id(), "task ran in the calling thread");
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    runTest("single_task",                    test_single_task);
    runTest("not_before_delay",               test_not_before_delay);
    runTest("deadlines_respected",            test_deadlines_respected);
    runTest("time_order",                     test_time_order);
    runTest("fifo_for_equal_delay",           test_fifo_for_equal_delay);
    runTest("earlier_task_preempts_wait",     test_earlier_task_preempts_wait);
    runTest("no_busy_wait",                   test_no_busy_wait);
    runTest("concurrent_schedule",            test_concurrent_schedule);
    runTest("long_task_does_not_block",       test_long_task_does_not_block_schedule);
    runTest("destroy_empty",                  test_destroy_empty);
    runTest("destructor_discards_pending",    test_destructor_discards_pending);
    runTest("no_execution_after_destruction", test_no_execution_after_destruction);
    runTest("destroy_with_pending_stress",    test_destroy_with_pending_stress);
    runTest("destructor_joins_running_task",  test_destructor_joins_running_task);
    runTest("exception_in_task",              test_exception_in_task);
    runTest("empty_function",                 test_empty_function);
    runTest("negative_delay",                 test_negative_delay);
    runTest("extreme_delays",                 test_extreme_delays);
    runTest("schedule_from_task",             test_schedule_from_task);
    runTest("single_worker_thread",           test_single_worker_thread);

    std::cout << "\n";
    if (g_failed == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    }
    std::cout << g_failed << " test(s) FAILED.\n";
    return 1;
}
