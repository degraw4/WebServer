#ifndef __THREADPOOL_H__
#define __THREADPOOL_H__

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../mysql/sql_connection_pool.h"

template <typename T>
class ThreadPool{
public:
    /*threadNumber是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    ThreadPool(SQLConnectionPool *connPool, int threadNumber = 8, int max_request = 10000);
    ~ThreadPool();
    bool append(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int threadNumber;        //线程池中的线程数
    int maxRequests;         //请求队列中允许的最大请求数
    pthread_t *threads;       //描述线程池的数组，其大小为m_threadNumber
    std::list<T *> workQueue; //请求队列
    locker queueLocker;       //保护请求队列的互斥锁
    sem queueStat;            //是否有任务需要处理
    SQLConnectionPool *connPool;  //数据库
};

template <typename T>
ThreadPool<T>::ThreadPool(SQLConnectionPool *conn_pool, int thread_number, int max_requests) 
                         :threadNumber(thread_number), maxRequests(max_requests), threads(NULL), connPool(conn_pool){
    if (threadNumber <= 0 || max_requests <= 0)
        throw std::exception();
    threads = new pthread_t[threadNumber];
    if (!threads)
        throw std::exception();
    for (int i = 0; i < threadNumber; ++i){
        // pthread_create第三个参数需要是static函数，因为静态没有this指针参数
        if (pthread_create(threads + i, NULL, worker, this) != 0){
            delete[] threads;
            throw std::exception();
        }
        if (pthread_detach(threads[i])){
            delete[] threads;
            throw std::exception();
        }
    }
}

// 析构仅仅delete pid数组
// 因为线程设置为detatch，运行结束后会自动释放所有资源
template <typename T>
ThreadPool<T>::~ThreadPool(){
    delete[] threads;
}

template <typename T>
bool ThreadPool<T>::append(T *request){
    queueLocker.lock();
    if (workQueue.size() >= maxRequests){
        queueLocker.unlock();
        return false;
    }
    workQueue.push_back(request);
    queueLocker.unlock();
    queueStat.post();
    return true;
}

template <typename T>
void *ThreadPool<T>::worker(void *arg){
    ThreadPool *pool = (ThreadPool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void ThreadPool<T>::run(){
    while (true){
        queueStat.wait();
        queueLocker.lock();
        if (workQueue.empty()){
            queueLocker.unlock();
            continue;
        }
        T *request = workQueue.front();
        workQueue.pop_front();
        queueLocker.unlock();
        if (!request)
            continue;

        connectionRAII mysqlcon(&request->MySQL, connPool);
        request->process();
    }
}

#endif
