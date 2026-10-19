// Minimal fork-join thread pool (no dependencies). The calling thread participates, so a pool created
// with 0 workers simply runs the loop serially.
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class ThreadPool {
public:
    explicit ThreadPool(int workers) {
        for (int i = 0; i < workers; i++) threads_.emplace_back([this, i] { loop(i + 1); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Total participants (workers + the calling thread). `f(item, participantIndex)` with index < participants().
    int participants() const { return (int)threads_.size() + 1; }

    template <class F>
    void parallelFor(int count, F&& f) {
        if (count <= 0) return;
        if (threads_.empty() || count == 1) {
            for (int i = 0; i < count; i++) f(i, 0);
            return;
        }
        std::function<void(int, int)> fn = std::ref(f);
        {
            std::lock_guard<std::mutex> lk(m_);
            job_ = &fn;
            count_ = count;
            next_.store(0, std::memory_order_relaxed);
            busy_ = (int)threads_.size();
            generation_++;
        }
        cv_.notify_all();
        run(0);  // the caller works too
        std::unique_lock<std::mutex> lk(m_);
        done_.wait(lk, [this] { return busy_ == 0; });
        job_ = nullptr;
    }

private:
    void run(int participant) {
        const std::function<void(int, int)>& fn = *job_;
        for (;;) {
            int i = next_.fetch_add(1, std::memory_order_relaxed);
            if (i >= count_) break;
            fn(i, participant);
        }
    }
    void loop(int participant) {
        unsigned seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return quit_ || generation_ != seen; });
                if (quit_) return;
                seen = generation_;
            }
            run(participant);
            {
                std::lock_guard<std::mutex> lk(m_);
                if (--busy_ == 0) done_.notify_one();
            }
        }
    }

    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable cv_, done_;
    const std::function<void(int, int)>* job_ = nullptr;
    std::atomic<int> next_{0};
    int count_ = 0, busy_ = 0;
    unsigned generation_ = 0;
    bool quit_ = false;
};

// Shared pool used by the simulation and the mesher.
inline ThreadPool& globalPool() {
    static ThreadPool pool([] {
        unsigned hw = std::thread::hardware_concurrency();
        int w = (int)hw - 2;  // leave a core for the main thread's driver work
        if (const char* e = std::getenv("VS_THREADS")) w = std::atoi(e) - 1;  // override: total thread count
        return std::clamp(w, 0, 15);
    }());
    return pool;
}
