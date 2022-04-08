#ifndef __BLOCK_QUEUE_H__
#define __BLOCK_QUEUE_H__

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"
using namespace std;

template <class T>
class block_queue{
public:
    block_queue(int max_size = 1000){
        if (max_size <= 0){
            exit(-1);
        }

        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }

    void clear(){
        m_mutex.lock();
        m_size = 0;
        m_front = -1;
        m_back = -1;
        m_mutex.unlock();
    }

    ~block_queue(){
        m_mutex.lock();
        if (m_array != NULL)
            delete [] m_array;

        m_mutex.unlock();
    }
    
    bool full() {
        m_mutex.lock();
        if (m_size >= m_max_size){
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }
    
    bool empty() {
        m_mutex.lock();
        if (0 == m_size){
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }
    
    bool front(T &value) {
        m_mutex.lock();
        if (0 == m_size){
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }
    
    bool back(T &value) {
        m_mutex.lock();
        if (0 == m_size){
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;
    }

    int size() {
        int tmp = 0;
        m_mutex.lock();
        tmp = m_size;
        m_mutex.unlock();
        return tmp;
    }

    int max_size(){
        int tmp = 0;
        m_mutex.lock();
        tmp = m_max_size;
        m_mutex.unlock();
        return tmp;
    }

    // 往队列添加元素，需要将所有使用队列的线程先唤醒
    // 当有元素push进队列,相当于生产者生产了一个元素
    // 若当前没有线程等待条件变量,则唤醒无意义
    // broadcast相当于通知消费者队列中有元素，不管是新加的还是满了
    bool push(const T &item){
        m_mutex.lock();
        if (m_size >= m_max_size){
            m_cond.broadcast();
            m_mutex.unlock();
            return false;
        }

        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;

        m_size++;

        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }

    //pop时,如果当前队列没有元素,将会等待条件变量
    bool pop(T &item){
        m_mutex.lock();
        while (m_size <= 0){
            // pthread_cond_wait成功返回0，否则表示函数失败
            /*
            pthread_cond_wait内部机制：
            wait表示内部函数内部等待条件变量cond，两种情况：
                条件变量满足：（一开始其实没有可能，因为while进入wait前已经lock了，若满足，说明有人也lock并改变了条件，显然违反互斥）
                条件变量不满足：unlock，线程挂起等待signal，等待成功则尝试lock，成功后函数返回0
            由于本函数是阻塞式函数，因此if和while理论上效果一样，但是while可以在退出是多一道判断，更加安全，防止多个消费者等待
            */
            if (!m_cond.wait(m_mutex.get())){
                m_mutex.unlock();
                return false;
            }
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

    // 增加了超时处理
    // 为何超时使用if?
    bool pop(T &item, int ms_timeout){
        struct timespec t = {0, 0};
        struct timeval now = {0, 0};
        gettimeofday(&now, NULL);
        m_mutex.lock();
        if (m_size <= 0){
            t.tv_sec = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000;
            if (!m_cond.timewait(m_mutex.get(), t)){
                m_mutex.unlock();
                return false;
            }
        }

        if (m_size <= 0){
            m_mutex.unlock();
            return false;
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

private:
    locker m_mutex;
    cond m_cond;

    T *m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

#endif
