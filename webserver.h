#ifndef __WEBSERVER_H__
#define __WEBSERVER_H__

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./thread/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD = 65536;           // 最大文件描述符
const int MAX_EVENT_NUMBER = 10000; // 最大事件数
const int TIMESLOT = 5;             // 最小超时单位

class WebServer{
public:
    WebServer();
    ~WebServer();

    void init(int _port, string _user, string _password, string _databaseName,
              int _logWrite , int _trigMode, int _SQLNum, int _threadNum);
    void setThreadPool();
    void setSQLPool();
    void setLog();
    void setTrigMode();
    
    void eventListen();
    void eventLoop();

    void registerNewConnection(int connfd, struct sockaddr_in client_address);
    void adjustTimer(util_timer *timer);
    void delateTimer(util_timer *timer, int sockfd);
    bool dealNewConnection();
    bool dealSignal(bool& timeout, bool& stop_server);
    void dealRead(int sockfd);
    void dealWrite(int sockfd);

public:
    int port;
    char *rootPath;
    int logWrite;

    int pipeFd[2];
    int epollFd;
    HttpConn *httpConns;

    // 数据库
    SQLConnectionPool *sqlConnectionPool;
    string user;         
    string password;     
    string databaseName; 
    int SQLNum;

    // 线程池
    ThreadPool<HttpConn> *threadPool;
    int threadNum;

    // epoll_event
    epoll_event events[MAX_EVENT_NUMBER];

    int listenFd;
    int trigMode;
    int listenTrigmode;
    int connTrigmode;

    // 定时器
    client_data *users_timer;
    Utils utils;
};

#endif