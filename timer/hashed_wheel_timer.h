#ifndef __HASHED_WHEEL_TIMER__
#define __HASHED_WHEEL_TIMER__

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
#include <vector>
#include <unordered_set>
#include <time.h>

#include "../log/log.h"

using namespace std;

class ClientData{
public:
    sockaddr_in address;
    int socketFd;
    int index;
};

class HashedWheelTimer{
public:
    HashedWheelTimer(){};
    ~HashedWheelTimer(){};

    void init(int timeslot, int timeout);
    void addTimer(ClientData* user);
    void delTimer(ClientData* user);
    void adjustTimer(ClientData* user);
    void tick();

    vector<unordered_set<ClientData*>> wheel;  // 时间轮
    int TIMESLOT;                              // 最小时间单位
    int TIMEOUT;                               // 超时时间
    int size;                                  // 时间轮大小
    int ticker;                                // 时间轮指针

};

class Utils{
public:
    Utils(){};
    ~Utils(){};

    void init(int timeslot, int timeout);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

    static int *pipeFd;
    static int epollFd;
    HashedWheelTimer hashedWheelTimer;
    int TIMESLOT;
    int TIMEOUT;
};

void callback(ClientData *user);

# endif