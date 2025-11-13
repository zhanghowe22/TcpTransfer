// 线程池类
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// 线程池类：管理多个工作线程，处理提交的任务
class ThreadPool
{
  private:
    std::vector<std::thread>          workers; // 工作线程数组
    std::queue<std::function<void()>> tasks;   // 任务队列（存储待执行的函数）
    std::mutex                        mtx;     // 保护任务队列的互斥锁
    std::condition_variable           cv;      // 条件变量（通知线程任务到来）
    std::atomic<bool>                 stop;    // 原子变量：是否停止线程池（线程安全）

    // 工作线程的主循环：不断从队列取任务执行
    void worker_loop()
    {
        while (!stop) {                 // 只要线程池不停止，就一直循环
            std::function<void()> task; // 存储从队列取出的任务

            // 加锁取任务
            {
                // unique_lock可手动释放
                std::unique_lock<std::mutex> lock(mtx);
                // 等待条件：队列不为空 或 线程池停止
                cv.wait(lock, [this] { return !tasks.empty() || stop; });

                // 如果线程池已停止且队列空，退出循环
                if (stop && tasks.empty()) {
                    return;
                }

                // 取出任务（移动语义，避免拷贝）
                task = std::move(tasks.front());
                tasks.pop();
            }

            // 退出 {} 代码块（unique_lock 出作用域，自动调用析构函数释放锁）
            // 执行任务（解锁后执行，避免任务执行时占用锁）
            task();
        }
    }

  public:
    explicit ThreadPool(size_t num_threads = 5) : stop(false)
    {
        // 创建num_threads个工作线程，每个线程执行worker_loop
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back(&ThreadPool::worker_loop, this);
        }
    }

    ~ThreadPool()
    {
        stop = true;     // 原子操作：通知所有线程准备停止
        cv.notify_all(); // 唤醒所有等待的线程
        // 等待所有工作线程结束
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    // 禁用拷贝构造和赋值（线程池不能拷贝）
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 提交任务到队列（支持任意参数的函数）
    template <typename F, typename... Args>
    void submit(F&& f, Args&&... args)
    {
        // 包装任务为无参函数（用std::bind绑定参数）
        auto task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

        // 加锁将任务放入队列
        {
            std::lock_guard<std::mutex> lock(mtx);
            // 如果线程池已停止，不接受新任务
            if (stop) {
                throw std::runtime_error("线程池已停止，无法提交任务");
            }
            tasks.emplace(std::move(task));
        }

        // 通知一个等待的线程有新任务
        cv.notify_one();
    }
};