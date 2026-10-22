#include "thread_pool.h"

ThreadPool::ThreadPool(size_t threadCount) {
    workers_.reserve(threadCount);
    try {
        for (size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back(&ThreadPool::workerLoop, this);
        }
    } catch (...) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        worker.join();
    }
}

void ThreadPool::submit(std::function<void()> task) {
    if (!task) {
        return;
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });

            if (tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        try {
            task();
        } catch (...) {
            // задача не должна ронять рабочий поток
        }
    }
}
