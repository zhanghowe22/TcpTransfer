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
  public:
    explicit ThreadPool(size_t thread_num) : m_stop(false), is_stopped(false)
    {
        // 检查线程数是否合法（至少1个线程）
        if (thread_num == 0) {
            throw std::invalid_argument("线程池线程数不能为0");
        }
        // 创建num_threads个工作线程，每个线程执行worker_loop
        for (size_t i = 0; i < thread_num; ++i) {
            workers.emplace_back([this]() {
                this->worker_loop(); // 线程执行的核心循环（单独抽成成员函数，代码更清晰）
            });
        }
    }

    // 析构函数：自动触发优雅停止（防止忘记调用stop()导致资源泄露）
    ~ThreadPool()
    {
        stop(); // 析构时自动停止，保证资源回收
    }

    // 禁用拷贝构造和赋值（线程池不能拷贝）
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 提交任务到队列（支持任意参数的函数）
    template <typename F, typename... Args>
    void submit(F&& f, Args&&... args)
    {
        // 包装任务：将函数f和参数args...绑定成无参可调用对象
        auto task = std::bind(std::forward<F>(f),         // 完美转发函数f
                              std::forward<Args>(args)... // 完美转发参数args...
        );

        // 加锁保护任务队列，确保线程安全
        std::lock_guard<std::mutex> lock(mtx);

        // 检查线程池是否已停止，停止则拒绝新任务
        if (m_stop) {
            throw std::runtime_error("线程池已停止，无法提交新任务");
        }

        // 将包装后的任务加入队列
        tasks.emplace(std::move(task));

        // 唤醒一个空闲线程处理新任务
        cv.notify_one();
    }

    void stop()
    {
        // 1. 加锁，设置stop标志（线程安全）
        std::unique_lock<std::mutex> lock(mtx);
        if (is_stopped) { // 已经停止过，直接返回
            return;
        }
        m_stop = true;   // 标记“开始停止，拒绝新任务”
        lock.unlock(); // 提前解锁：避免唤醒后线程拿不到锁，导致死锁

        // 2. 唤醒所有阻塞的工作线程
        cv.notify_all(); // 为什么用notify_all？因为所有线程可能都在等任务，需要全部唤醒让它们检查stop标志

        // 3. 等待所有工作线程执行完任务并退出（回收线程资源）
        for (std::thread& worker : workers) {
            if (worker.joinable()) { // 确保线程是可join的（避免重复join）
                worker.join();       // 阻塞主线程，直到该工作线程结束
            }
        }

        // 4. 标记“完全停止”，避免重复调用
        is_stopped = true;
        std::lock_guard<std::mutex> lock_clean(mtx);
        tasks = std::queue<std::function<void()>>(); // 清空队列
    }

  private:
    // 工作线程的主循环：不断从队列取任务执行
    void worker_loop()
    {
        while (true) {                  // 只要线程池不停止，就一直循环
            std::function<void()> task; // 存储从队列取出的任务

            // 1. 加锁，检查队列和停止状态
            std::unique_lock<std::mutex> lock(mtx);

            // 2. 条件变量等待：队列空且未停止 → 阻塞；否则唤醒
            cv.wait(lock, [this]() { return this->m_stop || !this->tasks.empty(); });

            // 3. 退出条件：已停止 + 队列空（所有任务处理完）
            if (this->m_stop && this->tasks.empty()) {
                return; // 线程退出，结束循环
            }

            // 4. 取任务（此时队列非空，可安全弹出）
            task = std::move(this->tasks.front());
            this->tasks.pop();
            lock.unlock(); // 释放锁，避免执行任务时占用锁

            // 5. 执行任务（捕获异常，避免单个任务崩溃导致线程退出）
            try {
                task();
            } catch (const std::exception& e) {
                // 任务执行异常，打印日志（实际项目中可替换为日志系统）
                fprintf(stderr, "线程池任务执行异常：%s\n", e.what());
            } catch (...) {
                fprintf(stderr, "线程池任务执行未知异常\n");
            }
        }
    }

  private:
    std::vector<std::thread>          workers;    // 工作线程数组
    std::queue<std::function<void()>> tasks;      // 任务队列（存储待执行的函数）
    std::mutex                        mtx;        // 保护任务队列的互斥锁
    std::condition_variable           cv;         // 条件变量（通知线程任务到来）
    bool                              m_stop;       // 停止标志：true=触发停止（不再接受新任务）
    bool                              is_stopped; // 完全停止标志：true=所有线程已回收、任务已处理完
};