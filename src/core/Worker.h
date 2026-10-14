#pragma once
// Small bounded background pools. No unbounded thread creation; queues are capped and
// keyed jobs replace stale queued work (e.g. a newer search replaces an older pending one).
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nb {

class WorkerPool {
public:
    enum class Apartment { STA, MTA };
    WorkerPool(const char* name, int threads, Apartment apt, size_t maxQueue = 256);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // Returns false if the queue is full or the pool is stopping.
    // A non-empty key replaces an already queued (not yet started) job with the same key.
    bool Submit(std::function<void()> job, const std::string& key = {});
    void Stop();

    // Shell work (icons, AppsFolder, file search) needs STA; WinRT/devices use MTA.
    static WorkerPool& Shell();
    static WorkerPool& Background();
    static void StopAll();

private:
    struct Job {
        std::string key;
        std::function<void()> fn;
    };
    void Run();
    const char* name_;
    Apartment apt_;
    size_t maxQueue_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> q_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

} // namespace nb
