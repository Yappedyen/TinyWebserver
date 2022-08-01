/*
 * @Author: PengYan 
 * @Date: 2022-07-18 17:20:14 
 * @Last Modified by:   PengYan 
 * @Last Modified time: 2022-07-18 17:20:14 
 */
#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

// 线程同步机制封装类


// 封装信号量类
class sem
{
public:
    sem()
    {
        // 信号量初始化
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            throw std::exception();
        }
    }
    sem(int num)
    {
        // 信号量初始化为num，0 用在线程间
        // 这是对由sem指定的信号量进行初始化，设置好它的共享选项（linux 只支持为0，即表示它是当前进程的局部信号量），然后给它一个初始值VALUE。
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }
    // 销毁信号量。我们用完信号量后都它进行清理。归还占有的一切资源。
    ~sem()
    {
        // 销毁信号量
        sem_destroy(&m_sem);
    }
    // sem_wait() 减小(锁定)由sem指定的信号量的值.
    // 如果信号量的值比0大,那么进行减一的操作,函数立即返回.
    // 如果信号量当前为0值,那么调用就会一直阻塞直到或者是信号量变得可以进行减一的操作
    // 等待信号量。
    bool wait()
    {
        // 原子操作方式,作用是从信号量的值减去一个“1”，但它永远会先等待该信号量为一个非零值才开始做减法。
        return sem_wait(&m_sem) == 0;
    }
    // 释放信号量。信号量值加1。并通知其他等待线程。
    bool post()
    {
        // 以原子操作方式将信号量加一,信号量大于0时,唤醒调用sem_post的线程
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};

// 封装锁类
class locker
{
public:
    locker()
    {
        // 初始化互斥锁类
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        // 销毁互斥锁
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock()
    {
        // 以原子操作方式给互斥锁加锁
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock()
    {
        // 以原子操作方式给互斥锁解锁
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t *get()
    {
        // 得到互斥锁
        return &m_mutex;
    }

private:
    // 互斥量
    pthread_mutex_t m_mutex;
};

// 封装条件变量
class cond
{
public:
    cond()
    {
        // 条件变量初始化
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            //pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()
    {
        // 销毁条件变量
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        // 等待目标条件变量，传入的是加锁的互斥锁，函数内部先解锁等待唤醒后再加锁
        // 用于阻塞当前线程，等待别的线程使用pthread_cond_signal()或pthread_cond_broadcast来唤醒
        ret = pthread_cond_wait(&m_cond, m_mutex);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    // 超时等待条件变量
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()
    {
        // 解除一个某个条件变量上阻塞的线程的阻塞
        return pthread_cond_signal(&m_cond) == 0;
    }
    bool broadcast()
    {
        // 解除当前所有在某个条件变量上阻塞的线程的阻塞
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};
#endif
