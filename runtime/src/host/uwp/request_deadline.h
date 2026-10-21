// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
namespace uwp {
// Wake cancellation even when a transport never completes. Joining only waits for
// the callback, never for the request deadline after a successful request.
class RequestDeadline {
    std::mutex mutex_;
    std::condition_variable wake_;
    bool done_ = false;
    std::thread worker_;

  public:
    RequestDeadline(std::chrono::milliseconds timeout, std::function<void()> cancel)
        : worker_([this, timeout, cancel = std::move(cancel)] {
              std::unique_lock lock(mutex_);
              if (!wake_.wait_for(lock, timeout, [this] { return done_; })) {
                  lock.unlock();
                  try {
                      cancel();
                  } catch (...) {
                  }
              }
          }) {}
    ~RequestDeadline() {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        wake_.notify_one();
        worker_.join();
    }
};
// A session owns cancellation independently of the App and worker lifetimes.
template <class Transfer> struct RequestGroup {
    std::mutex mutex;
    std::condition_variable wake;
    bool closed = false;
    std::vector<std::weak_ptr<Transfer>> requests;
    std::shared_ptr<Transfer> start() {
        std::unique_lock lock(mutex);
        for (;;) {
            std::erase_if(requests, [](auto &r) { return r.expired(); });
            if (closed || requests.size() < 4)
                break;
            wake.wait_for(lock, std::chrono::milliseconds(10));
        }
        auto request = std::make_shared<Transfer>();
        if (closed)
            request->cancel();
        std::erase_if(requests, [](auto &r) { return r.expired(); });
        requests.push_back(request);
        return request;
    }
    bool is_closed() {
        std::lock_guard lock(mutex);
        return closed;
    }
    void cancel() {
        std::lock_guard lock(mutex);
        closed = true;
        wake.notify_all();
        for (auto &r : requests)
            if (auto request = r.lock())
                request->cancel();
    }
};
} // namespace uwp
