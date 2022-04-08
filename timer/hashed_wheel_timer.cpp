#include "hashed_wheel_timer.h"
#include "../http/http_conn.h"

void HashedWheelTimer::init(int timeslot, int timeout){
    TIMESLOT = timeslot;
    TIMEOUT = timeout;
    size = 1 + TIMEOUT / TIMESLOT;
    ticker = 0;
    wheel = vector<unordered_set<ClientData*>>(size, unordered_set<ClientData*>());
}

void HashedWheelTimer::addTimer(ClientData* user){
    if(!user){
        return;
    }
    int position = (ticker + TIMEOUT / TIMESLOT) % size;
    user->index = position;
    wheel[position].insert(user);
}

void HashedWheelTimer::delTimer(ClientData* user){
    if(!user){
        return;
    }
    int oldPosition = user->index;
    wheel[oldPosition].erase(user);
}

void HashedWheelTimer::adjustTimer(ClientData* user){
    if(!user){
        return;
    }
    delTimer(user);
    addTimer(user);
}

void HashedWheelTimer::tick(){
    auto tmpWheel = wheel[ticker];
    for(auto it = tmpWheel.begin(); it != tmpWheel.end(); it++){
        ClientData* user = *it;
        if(!user){
            return;
        }
        callback(user);
        delTimer(user);
    }
    ticker++;
    ticker %= size;
}

void Utils::init(int timeslot, int timeout){
    TIMESLOT = timeslot;
    TIMEOUT = timeout;
    hashedWheelTimer.init(timeslot, timeout);
}

// 对文件描述符设置非阻塞
int Utils::setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
// et数据很多时只触发一次，因此要循环读取，但是读取过程中如果有新的事件到达，可能触发其他线程来处理这个fd
// ONESHOT原理是，每次触发事件之后，将事件注册从epollfd上清除，下次需要用epoll_ctl的EPOLL_CTL_MOD手动加上
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode){
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置为非阻塞
    setnonblocking(fd);
}

//信号处理函数
void Utils::sig_handler(int sig){
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipeFd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart){
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler(){
    hashedWheelTimer.tick();
    alarm(TIMESLOT);
}

void Utils::show_error(int connfd, const char *info){
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::pipeFd = 0;
int Utils::epollFd = 0;

// 只需要epoll 删除，fd关闭，删除timer（已做）
// http conn数组和client data数组都是可以复用的
void callback(ClientData *user)
{
    epoll_ctl(Utils::epollFd, EPOLL_CTL_DEL, user->socketFd, 0);
    assert(user);
    close(user->socketFd);
    HttpConn::userCount--;
}

