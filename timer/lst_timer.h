#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

class util_timer;  // 前向声明

struct client_data
{
    sockaddr_in address;    // 客户端socket地址
    int sockfd;             // socket文件描述符
    util_timer *timer;      //定时器
};

// 定时器类
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;      // 任务超时时间，这里使用绝对时间
    
    void (* cb_func)(client_data *);    // 任务回调函数，回调函数处理的客户数据，由定时器的执行者传递给回调函数
    client_data *user_data;     // 客户数据
    util_timer *prev;       // 链表前一个定时器
    util_timer *next;       // 链表后一个定时器
};

// 定时器链表，是一个升序、双向链表，且带有头结点和尾节点
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer);
    void del_timer(util_timer *timer);
    void tick();

private:
    // 一个重载的辅助函数，它被公有的 add_timer 函数和 adjust_timer 函数调用
    // 该函数表示将目标定时器 timer 添加到节点 lst_head 之后的部分链表中
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;   // 头结点
    util_timer *tail;   // 尾节点
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    void setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数，添加信号捕捉
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;
};

void cb_func(client_data *user_data);

#endif
