#include "core/Worker.h"

#include <windows.h>
#include <objbase.h>

#include "core/Log.h"

namespace nb {

WorkerPool::WorkerPool(const char* name, int threads, Apartment apt, size_t maxQueue)
    : name_(name), apt_(apt), maxQueue_(maxQueue) {
    for (int i = 0; i < threads; ++i) threads_.emplace_back([this] { Run(); });
}

WorkerPool::~WorkerPool() { Stop(); }

bool WorkerPool::Submit(std::function<void()> job, const std::string& key) {
    {
        std::lock_guard lk(mu_);
        if (stop_) return false;
        if (!key.empty()) {
            for (auto& j : q_)
                if (j.key == key) {
                    j.fn = std::move(job);
                    return true;
                }
        }
        if (q_.size() >= maxQueue_) {
            LOGW("worker %s queue full, job dropped", name_);
            return false;
        }
        q_.push_back({key, std::move(job)});
    }
    cv_.notify_one();
    return true;
}

void WorkerPool::Stop() {
    {
        std::lock_guard lk(mu_);
        if (stop_ && threads_.empty()) return;
        stop_ = true;
        q_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

void WorkerPool::Run() {
    HRESULT hr = CoInitializeEx(nullptr, apt_ == Apartment::STA ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Job job;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
            if (stop_) break;
            job = std::move(q_.front());
            q_.pop_front();
        }
        try {
            if (job.fn) job.fn();
        } catch (...) {
            LOGE("worker %s: job threw", name_);
        }
    }
    if (SUCCEEDED(hr)) CoUninitialize();
}

namespace {
WorkerPool* g_shell = nullptr;
WorkerPool* g_bg = nullptr;
} // namespace

WorkerPool& WorkerPool::Shell() {
    if (!g_shell) g_shell = new WorkerPool("shell", 1, Apartment::STA);
    return *g_shell;
}

WorkerPool& WorkerPool::Background() {
    if (!g_bg) g_bg = new WorkerPool("bg", 2, Apartment::MTA);
    return *g_bg;
}

void WorkerPool::StopAll() {
    if (g_shell) g_shell->Stop();
    if (g_bg) g_bg->Stop();
}

} // namespace nb
