#include "scheduler.h"

#include <algorithm>

namespace {

// now + delay без переполнения: отрицательная задержка не откладывает задачу,
// слишком большая насыщается до time_point::max()
std::chrono::steady_clock::time_point deadlineAfter(std::chrono::milliseconds delay) {
    using Clock = std::chrono::steady_clock;

    const auto now = Clock::now();
    if (delay <= std::chrono::milliseconds::zero()) {
        return now;
    }
    const auto maxDelay =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
    if (delay >= maxDelay) {
        return Clock::time_point::max();
    }
    return now + delay;
}

}  // namespace

Scheduler::Scheduler()
    : worker_(&Scheduler::workerLoop, this) {
}

Scheduler::~Scheduler() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_one();
    worker_.join();
    // невыполненные задачи уничтожаются вместе с tasks_ без запуска
}

void Scheduler::scheduleAfter(std::chrono::milliseconds delay,
                              std::function<void()> task) {
    if (!task) {
        return;
    }
    const auto time = deadlineAfter(delay);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        tasks_.push_back(Entry{ScheduledTask{time, std::move(task)}, nextSeq_++});
        std::push_heap(tasks_.begin(), tasks_.end(), Later{});
    }
    // рабочий поток пересчитает срок ожидания, если новая задача раньше текущей
    cv_.notify_one();
}

void Scheduler::workerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        if (stopping_) {
            return;
        }
        if (tasks_.empty()) {
            cv_.wait(lock);
            continue;
        }

        // копия, а не ссылка: пока ждём, вектор может быть перераспределён
        const auto deadline = tasks_.front().scheduled.time;
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            // wait_until(max) может переполниться при пересчёте в другие часы
            cv_.wait(lock);
            continue;
        }
        if (std::chrono::steady_clock::now() < deadline) {
            cv_.wait_until(lock, deadline);
            continue;
        }

        {
            std::pop_heap(tasks_.begin(), tasks_.end(), Later{});
            std::function<void()> task = std::move(tasks_.back().scheduled.task);
            tasks_.pop_back();

            lock.unlock();
            try {
                task();
            } catch (...) {
                // задача не должна ронять рабочий поток
            }
            // task (и её захваты) уничтожается здесь, до повторного захвата мьютекса
        }
        lock.lock();
    }
}
