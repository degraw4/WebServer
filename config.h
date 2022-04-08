#ifndef __CONFIG_H__
#define __CONFIG_H__
#include "webserver.h"

using namespace std;

class Config{
public:
    Config();
    ~Config(){};

    void parse_arg(int argc, char*argv[]);

    // 端口
    int Port;
    // 日志写入方式
    int LogWrite;
    // 触发组合模式
    int TrigMode;
    // listenfd触发模式
    int ListenTrigmode;
    // connfd触发模式
    int ConnTrigmode;
    // 数据库连接池数量
    int SQLConnNum;
    // 线程池内线程数量
    int ThreadNum;
};

Config::Config(){
    // 端口，默认9999
    Port = 9999;
    // 日志写入方式，默认异步
    LogWrite = 1;
    // 触发组合模式，默认listenfd LT + connfd LT
    TrigMode = 0;
    // listenfd触发模式，默认LT
    ListenTrigmode = 0;
    // connfd触发模式，默认LT
    ConnTrigmode = 0;
    // 数据库连接数量，默认8
    SQLConnNum = 8;
    // 线程池内线程数量，默认8
    ThreadNum = 8;
}

void Config::parse_arg(int argc, char*argv[]){
    int opt;
    const char *str = "p:l:m:s:t:a:";
    while ((opt = getopt(argc, argv, str)) != -1){
        switch (opt){
        case 'p':{
            Port = atoi(optarg);
            break;
        }
        case 'l':{
            LogWrite = atoi(optarg);
            break;
        }
        case 'm':{
            TrigMode = atoi(optarg);
            break;
        }
        case 's':{
            SQLConnNum = atoi(optarg);
            break;
        }
        case 't':{
            ThreadNum = atoi(optarg);
            break;
        }
        default:
            break;
        }
    }
}

#endif