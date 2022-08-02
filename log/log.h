#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"
#include "assert.h"
using namespace std;

class Log
{
public:
    // 之前的单例懒汉模式是有线程安全问题的，需要做双重检查+互斥锁
    //C++11以后,使用静态局部变量懒汉模式不用加锁就是线程安全的
    // 只适合c++11以上标准
    // 首先，静态局部变量通过静态方法获得，且只有当该静态方法第一次执行
    // 的时候才会初始化，且后续再次调用该方法也不会再次初始化，
    // 这是静态局部变量的特性。
    // 而在c++11以上标准中，静态局部变量初始化是线程安全的。
    // 当其中一个线程初始化instance的时候，会阻塞其它线程的初始化行为。

    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }
    // delete拷贝构造和赋值函数，防止拷贝赋值
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;
    Log(Log &&) = delete;
    Log & operator=(const Log&&) = delete;

    static void *flush_log_thread(void *args)
    {
        return Log::get_instance()->async_write_log();
    }
    //可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    void write_log(int level, const char *format, ...);

    void flush(void);

private:
    Log();
    virtual ~Log();
    void *async_write_log()
    {
        string single_log;
        //从阻塞队列中取出一个日志string，写入文件
        while (m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }

private:
    char dir_name[128]; //路径名
    char log_name[128]; //log文件名
    int m_split_lines;  //日志最大行数
    int m_log_buf_size; //日志缓冲区大小
    long long m_count;  //日志行数记录
    int m_today;        //因为按天分类,记录当前时间是那一天
    FILE *m_fp;         //打开log的文件指针
    char *m_buf;
    block_queue<string> *m_log_queue; //阻塞队列
    bool m_is_async;                  //是否同步标志位
    locker m_mutex;
    int m_close_log; // 日志写入方式
};

#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}

#endif
