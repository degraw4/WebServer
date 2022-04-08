WebServer
===============
Linux下C++轻量级Web服务器，助力初学者快速实践网络编程，搭建属于自己的服务器.

* 使用 **线程池 + 非阻塞socket + epoll(ET和LT均实现) + 事件处理** 的并发模型
* 使用**状态机**解析HTTP请求报文，支持解析**GET和POST**请求
* 访问服务器数据库实现web端用户**注册、登录**功能
* 实现**同步/异步日志系统**，记录服务器运行状态
* 经Webbench压力测试可以实现**上万的并发连接**数据交换

快速运行
------------
* 服务器测试环境
	* Ubuntu版本16.04
	* MySQL版本5.7.29
* 浏览器测试环境
	* Chrome

* 测试前确认已安装MySQL数据库

    ```C++
    // 建立yourdb库
    create database yourdb;

    // 创建user表
    USE yourdb;
    CREATE TABLE user(
        username char(50) NULL,
        passwd char(50) NULL
    )ENGINE=InnoDB;

    // 添加数据
    INSERT INTO user(username, passwd) VALUES('name', 'passwd');
    ```

* 修改main.cpp中的数据库初始化信息

    ```C++
    //数据库登录名,密码,库名
    string user = "root";
    string passwd = "root";
    string databasename = "yourdb";
    ```

* build

    ```C++
    make
    ```

* 启动server

    ```C++
    ./server
    ```

* 浏览器端

    ```C++
    ip:port
    ```

个性化运行
------

```C++
./server [-p Port] [-l Log] [-m TrigMode] [-s SQLnum] [-t threadNum]
```

温馨提示: 以上参数不是非必须，不用全部使用

* -p，自定义端口号
	* 默认9999
* -l，选择日志写入方式，默认异步写入
	* 0，同步写入
	* 1，异步写入
* -m，listenfd和connfd的模式组合，默认使用LT + LT
	* 0，表示使用LT + LT
	* 1，表示使用LT + ET
    * 2，表示使用ET + LT
    * 3，表示使用ET + ET
* -s，数据库连接数量
	* 默认为8
* -t，线程数量
	* 默认为8

测试示例命令与含义

```C++
./server -p 9007 -l 1 -m 0 -s 10 -t 10
```

- [x] 端口9007
- [x] 异步写入日志
- [x] 使用LT + LT组合
- [x] 数据库连接池内有10条连接
- [x] 线程池内有10条线程