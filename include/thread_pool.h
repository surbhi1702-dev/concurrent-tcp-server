// thread_pool.h
// A simple fixed-size thread pool.
//
// Idea: create N worker threads once at startup. The main thread pushes
// jobs into a queue; workers sleep on a condition variable until a job is
// available, take it, run it and go back to waiting. This avoids paying
// the cost of creating and destroying a thread for every client.

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
public:
    // max_queue = how many jobs may wait before submit() blocks.
    // This gives back-pressure: under very heavy load the accept loop slows
    // down instead of the queue growing without limit.
    ThreadPool(int num_threads, size_t max_queue)
        : max_queue_(max_queue), stopping_(false) {
        for (int i = 0; i < num_threads; i++) {
            workers_.emplace_back(&ThreadPool::worker_loop, this);
        }
    }

    ~ThreadPool() { shutdown(); }

    // Not copyable (it owns threads).
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Adds a job. Blocks while the queue is full. Returns false if the
    // pool is shutting down.
    bool submit(std::function<void()> job) {
        std::unique_lock<std::mutex> lock(mtx_);
        not_full_.wait(lock, [this] { return stopping_ || jobs_.size() < max_queue_; });
        if (stopping_) return false;
        jobs_.push(std::move(job));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    size_t queue_size() {
        std::lock_guard<std::mutex> lock(mtx_);
        return jobs_.size();
    }

    int num_threads() const { return (int)workers_.size(); }

    // Lets the workers finish the jobs already queued, then joins them.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stopping_) return;
            stopping_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
        for (std::thread& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                // wait() with a predicate handles spurious wakeups for us
                not_empty_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (stopping_ && jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop();
            }
            not_full_.notify_one();
            job();  // run the job WITHOUT holding the lock
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex mtx_;
    std::condition_variable not_empty_;  // signalled when a job is added
    std::condition_variable not_full_;   // signalled when a job is removed
    size_t max_queue_;
    bool stopping_;
};

#endif
