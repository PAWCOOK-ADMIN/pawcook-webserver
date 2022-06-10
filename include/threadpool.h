#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <exception>
#include <list>
#include <pthread.h>
#include "locker.h"
#include <iostream>


// 线程池类，定义成模板是为了代码的复用，T 代表任务
template<typename T>
class threadpool
{
private:
    // 线程的数量
    int m_thread_number;

    // 线程池中的线程 id 数组，大小为 m_thread_number
    pthread_t *m_threads;

    // 任务队列中请求的最大数量
    int m_max_requests;

    // 任务队列，存放任务
    std::list<T*> m_workqueue;

    // 互斥锁，使得多个线程能够互斥地对任务队列进行访问
    locker m_queuelocker;

    // 信号量，同步线程对任务队列的访问（比如，什么时候可以访问队列，什么时候该等待）
    sem m_queuesem;

    // 是否结束线程
    bool m_stop;

private:
    // 线程的主函数
    static void *worker(void *arg);     // 之所以要定义为 static，是因为 c++ 的要求
    void run();                         // 线程工作的逻辑单元

public:
    threadpool(int thread_number=8, int max_requests=10000);
    
    ~threadpool();

    // 添加任务到任务队列中
    bool append(T *request);
};

// 构造函数
template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests):
    m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(nullptr) {

        if (thread_number <=0 || max_requests <=0) 
            throw std::exception();
        
        // 申请创建线程 id 数组
        m_threads = new pthread_t[m_thread_number];
        if(!m_threads){
            throw std::exception();
        }
        
        // 创建 thread_number 个线程，并设置为脱离状态
        for (int i=0; i<thread_number; ++i) {
            std::cout << "create the " << i << "th thread" << std::endl;
            if (pthread_create(m_threads+i, nullptr, worker, this) != 0) {
                delete []m_threads;
                throw std::exception();
            }

            if (pthread_detach(m_threads[i])) {
                delete []m_threads;
                throw std::exception();
            }
        }
}


template<typename T>
threadpool<T>::~threadpool(){
    delete []m_threads;
    m_stop = true;
}

template<typename T>
bool threadpool<T>::append(T *request) {
    // 需要对队列上锁，因为该队列被所有线程共享
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests) {     // 如果请求队列已满
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(request);             // 添加请求到请求队列中
    m_queuelocker.unlock();
    m_queuesem.post();                         // 信号量增 1
    return true;
}

template<typename T>
void *threadpool<T>::worker(void *arg) {
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run() {
    while (!m_stop) {
        m_queuesem.wait();             // 等待信号量，也即等待请求队列中存在请求
        m_queuelocker.lock();           // 如果请求到来，则获取请求队列的一个请求
        
        if(m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }

        T *request = m_workqueue.front();       // 从请求队列中取出一个请求
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if (!request)
            continue;
        
        request->process();           // 调用请求中的处理函数，处理请求
    }
}



#endif