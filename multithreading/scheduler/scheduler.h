#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

struct ScheduledTask {
    std::chrono::steady_clock::time_point time;
    std::function<void()> task;

    bool operator<(const ScheduledTask& other) const {
        return time < other.time;
    }
};

class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    void scheduleAfter(std::chrono::milliseconds delay,
                       std::function<void()> task);

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

private:
    // seq сохраняет порядок добавления для задач с одинаковым временем
    struct Entry {
        ScheduledTask scheduled;
        std::uint64_t seq;
    };

    // Компаратор кучи: на вершине самая ранняя задача, при равенстве — добавленная первой
    struct Later {
        bool operator()(const Entry& a, const Entry& b) const {
            if (b.scheduled < a.scheduled) {
                return true;
            }
            if (a.scheduled < b.scheduled) {
                return false;
            }
            return a.seq > b.seq;
        }
    };

    void workerLoop();

    std::vector<Entry> tasks_;  // min-куча по (time, seq)
    std::mutex mutex_;
    std::condition_variable cv_;
    std::uint64_t nextSeq_ = 0;
    bool stopping_ = false;
    std::thread worker_;  // объявлен последним: стартует после инициализации остальных полей
};
