#include "config.h"

int main(int argc, char *argv[]){
    // MySQL
    string MySQLUser = "debian-sys-maint";
    string MySQLPasswd = "AyoPBTUoOY857Tog";
    string DatabaseName = "webdb";

    // 命令行解析
    Config config;
    config.parse_arg(argc, argv);

    // 初始化server
    WebServer server;
    server.init(config.Port, MySQLUser, MySQLPasswd, DatabaseName, config.LogWrite, 
                config.TrigMode,  config.SQLConnNum,  config.ThreadNum);
    
    // 初始化日志模块
    server.setLog();

    // 触发模式
    server.setTrigMode();

    // 数据库
    server.setSQLPool();

    // 线程池
    server.setThreadPool();

    // 监听
    server.eventListen();

    // 运行
    server.eventLoop();

    return 0;
}