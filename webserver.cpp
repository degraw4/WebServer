#include "webserver.h"

WebServer::WebServer(){
    // 最多建立max_fd个HttpConn类连接对象
    httpConns = new HttpConn[MAX_FD];

    // 获取root文件夹路径
    char pwd[200];
    getcwd(pwd, 200);
    rootPath = (char *)malloc(strlen(pwd) + 6);
    strcpy(rootPath, pwd);
    strcat(rootPath, "/root");

    // 定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer(){
    close(epollFd);
    close(listenFd);
    close(pipeFd[1]);
    close(pipeFd[0]);
    delete[] httpConns;
    delete[] users_timer;
    delete threadPool;
    // 数据库连接池不必delete，因为单例模式的static会自动析构
}

void WebServer::init(int _port, string _user, string _password, string _databaseName,
                     int _logWrite , int _trigMode, int _SQLNum, int _threadNum){
    port = _port;
    user = _user;
    password = _password;
    databaseName = _databaseName;
    SQLNum = _SQLNum;
    threadNum = _threadNum;
    logWrite = _logWrite;
    trigMode = _trigMode;
}

void WebServer::setTrigMode(){
    // LT + LT
    if (0 == trigMode){
        listenTrigmode = 0;
        connTrigmode = 0;
    }
    // LT + ET
    else if (1 == trigMode){
        listenTrigmode = 0;
        connTrigmode = 1;
    }
    // ET + LT
    else if (2 == trigMode){
        listenTrigmode = 1;
        connTrigmode = 0;
    }
    // ET + ET
    else if (3 == trigMode){
        listenTrigmode = 1;
        connTrigmode = 1;
    }
}

// 下面的初始化都是单例模式，先get instance，再init
// get instance只是简单的构造一个static的实例，内部会调用构造函数做最基本的操作（如默认构造函数）
// init负责后面的操作
void WebServer::setLog(){
    // 异步写日志
    if (1 == logWrite)
        Log::getInstance()->init("./ServerLog", 2000, 800000, 800);
    // 同步写日志
    else
        Log::getInstance()->init("./ServerLog", 2000, 800000, 0);
}

void WebServer::setSQLPool(){
    // 初始化数据库连接池
    // 连接池都是连接好的，属于长连接模型
    sqlConnectionPool = SQLConnectionPool::getInstance();
    sqlConnectionPool->init("localhost", user, password, databaseName, 3306, SQLNum);

    // static map，所有http conn类初始化一次即可
    // users结构将mysql中的user表及其对应的passwd存到users结构内部的map中
    httpConns->initMySQLUser(sqlConnectionPool);
}

void WebServer::setThreadPool(){
    //线程池
    threadPool = new ThreadPool<HttpConn>(sqlConnectionPool, threadNum);
}

// 绑定listenfd，注册epoll，创建管道，设置信号处理函数，设置alarm
void WebServer::eventListen(){
    listenFd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listenFd >= 0);
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    // REUSEADDR
    int flag = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    int ret  = bind(listenFd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenFd, 5);
    assert(ret >= 0);

    // !!!!!
    utils.init(TIMESLOT);

    // epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    // epoll_create参数在高版本linux下，只要>0，效果都一样
    epollFd = epoll_create(5);
    assert(epollFd != -1);

    // 将listenfd注册到epoll并设置非阻塞
    utils.addfd(epollFd, listenFd, false, listenTrigmode);
    HttpConn::epollFd = epollFd;
    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipeFd);
    assert(ret != -1);
    utils.setnonblocking(pipeFd[1]);
    // pipi[0]对epollfd注册读事件
    // 对主线程，0读1写
    utils.addfd(epollFd, pipeFd[0], false, 0);

    utils.addsig(SIGPIPE, SIG_IGN);
    // 捕获下面两个信号，处理函数为主线程向pipe[1]发送对应的信号
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    // 最开始的计时，后续循环计时
    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = pipeFd;
    Utils::u_epollfd = epollFd;
}

// 用connetfd和address注册http_conn，注册client data计时器
void WebServer::registerNewConnection(int connFd, struct sockaddr_in clientAddress){
    // 将connectfd以oneshot模式加入epoll fd的读模式
    httpConns[connFd].init(connFd, clientAddress, rootPath, connTrigmode, user, password, databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connFd].address = clientAddress;
    users_timer[connFd].sockfd = connFd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connFd];
    // call back函数作用是从epoll删除fd，close fd，http连接--
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connFd].timer = timer;
    // 将计时器插入链表
    utils.m_timer_lst.add_timer(timer);
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjustTimer(util_timer *timer){
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

// 删除某个连接：删除timer，调用call back
void WebServer::delateTimer(util_timer *timer, int socketFd){
    timer->cb_func(&users_timer[socketFd]);
    if (timer){
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[socketFd].sockfd);
}

// 接收新链接
bool WebServer::dealNewConnection(){
    struct sockaddr_in clientAddress;
    socklen_t clientAddrlength = sizeof(clientAddress);
    // level triger
    if (0 == listenTrigmode){
        int connFd = accept(listenFd, (struct sockaddr *)&clientAddress, &clientAddrlength);
        if (connFd < 0){
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (HttpConn::userCount >= MAX_FD){
            utils.show_error(connFd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        registerNewConnection(connFd, clientAddress);
    }
    // edge triger
    /*
    ET模式下只有某个socket从unreadable变为readable或从unwritable变为writable时，epoll_wait才会返回该socket
    ET模式下，正确的读写方式为:读：只要可读，就一直读，直到返回0，或者 errno = EAGAIN
    写:只要可写，就一直写，直到数据发送完，或者 errno = EAGAIN
    */
    else{
        while (1){
            int connFd = accept(listenFd, (struct sockaddr *)&clientAddress, &clientAddrlength);
            if (connFd < 0){
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (HttpConn::userCount >= MAX_FD){
                utils.show_error(connFd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            registerNewConnection(connFd, clientAddress);
        }
        return false;
    }
    return true;
}

// 主线程从pipe 0读取信号并处理
bool WebServer::dealSignal(bool &timeout, bool &stop)
{
    int sig;
    char signals[1024];
    int ret = recv(pipeFd[0], signals, sizeof(signals), 0);

    if (ret == -1  || ret == 0){
        return false;
    }
    
    for (int i = 0; i < ret; ++i){
        switch (signals[i]){
            case SIGALRM:{
                timeout = true;
                break;
            }
            case SIGTERM:{
                stop = true;
                break;
            }
        }
    }

    return true;
}

void WebServer::dealRead(int socketFd){
    util_timer *timer = users_timer[socketFd].timer;
    if (httpConns[socketFd].readOnce()){
        LOG_INFO("deal with the client(%s)", inet_ntoa(httpConns[socketFd].getAddress()->sin_addr));

        // 若监测到读事件，将该事件放入请求队列
        // proactor模式下，主线程和工作线程异步处理，主线程只向工作队列中读请求
        // 工作线程读取请求，解析，然后有写回就write
        threadPool->append(httpConns + socketFd);
        if (timer){
            adjustTimer(timer);
        }
    }
    else{
        delateTimer(timer, socketFd);
    }
}

void WebServer::dealWrite(int socketFd){
    util_timer *timer = users_timer[socketFd].timer;
    //proactor
    if (httpConns[socketFd].write()){
        LOG_INFO("send data to the client(%s)", inet_ntoa(httpConns[socketFd].getAddress()->sin_addr));
        if (timer){
            adjustTimer(timer);
        }
    }
    else{
        delateTimer(timer, socketFd);
    }
}

void WebServer::eventLoop(){
    bool timeout = false;
    bool stop = false;

    while (!stop){
        int number = epoll_wait(epollFd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR){
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++){
            int socketFd = events[i].data.fd;

            //处理新到的客户连接
            if (socketFd == listenFd){
                // 新链接注册http coon和client data，注册epoll注册timer
                bool flag = dealNewConnection();
                if (false == flag)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[socketFd].timer;
                delateTimer(timer, socketFd);
            }
            //处理信号
            else if ((socketFd == pipeFd[0]) && (events[i].events & EPOLLIN)){
                bool flag = dealSignal(timeout, stop);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN){
                dealRead(socketFd);
            }
            else if (events[i].events & EPOLLOUT){
                dealWrite(socketFd);
            }
        }
        if (timeout){
            utils.timer_handler();
            LOG_INFO("%s", "timer tick");
            timeout = false;
        }
    }
}