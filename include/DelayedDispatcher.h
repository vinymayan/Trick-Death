#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace Utils {
    class DelayedDispatcher {
    public:
        using Task = std::move_only_function<void()>;
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        static DelayedDispatcher& Get() {
            static DelayedDispatcher instance;
            return instance;
        }

        template <class Rep, class Period>
        void PostDelayed(std::chrono::duration<Rep, Period> delay, Task&& task) {
            const auto executeAt = Clock::now() + delay;
            {
                std::scoped_lock lock(mutex_);
                queue_.emplace(executeAt, std::move(task));
            }
            condition_.notify_one();
        }

        void Stop() {
            worker_.request_stop();
            condition_.notify_all();
        }

    private:
        struct ScheduledTask {
            TimePoint time;
            mutable Task task;

            bool operator>(const ScheduledTask& other) const {
                return time > other.time;
            }
        };

        DelayedDispatcher() :
            worker_([this](std::stop_token stopToken) { RunLoop(stopToken); }) {}

        ~DelayedDispatcher() {
            Stop();
        }

        void RunLoop(std::stop_token stopToken) {
            while (!stopToken.stop_requested()) {
                Task task;
                {
                    std::unique_lock lock(mutex_);
                    condition_.wait(lock, stopToken, [this] { return !queue_.empty(); });
                    if (stopToken.stop_requested()) {
                        return;
                    }

                    const auto executeAt = queue_.top().time;
                    if (executeAt > Clock::now()) {
                        condition_.wait_until(lock, stopToken, executeAt, [this, executeAt] {
                            return !queue_.empty() && queue_.top().time < executeAt;
                        });
                        continue;
                    }

                    task = std::move(queue_.top().task);
                    queue_.pop();
                }

                if (task) {
                    task();
                }
            }
        }

        std::priority_queue<ScheduledTask, std::vector<ScheduledTask>, std::greater<>> queue_;
        std::mutex mutex_;
        std::condition_variable_any condition_;
        std::jthread worker_;
    };
}
