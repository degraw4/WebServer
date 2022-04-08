#ifndef __CONNECTION_POOL__
#define __CONNECTION_POOL__

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;

class SQLConnectionPool{
public:
	MYSQL *GetConnection();				 //获取数据库连接
	bool ReleaseConnection(MYSQL *conn); //释放连接
	int GetFreeConn();					 //获取连接
	void DestroyPool();					 //销毁所有连接

	//单例模式
	static SQLConnectionPool *getInstance();

	void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn); 

private:
	SQLConnectionPool();
	~SQLConnectionPool();

	int m_MaxConn;  //最大连接数
	int m_CurConn;  //当前已使用的连接数
	int m_FreeConn; //当前空闲的连接数
	locker lock;
	list<MYSQL *> connList; //连接池 list
	sem reserve;

public:
	string m_url;			 //主机地址
	string m_Port;		 //数据库端口号
	string m_User;		 //登陆数据库用户名
	string m_PassWord;	 //登陆数据库密码
	string m_DatabaseName; //使用数据库名
};

// 在用到mysql conn的时候，构造一个临时变量connRAII来管理conn资源
// connRAII的构造函数会申请conn，虚构函数会释放conn，从而保证RAII变量离开作用域会自动析构，从而自动释放变量
// 此处对于conn的申请与释放即位在conn list中pop与push
class connectionRAII{

public:
	connectionRAII(MYSQL **con, SQLConnectionPool *connPool);
	~connectionRAII();
	
private:
	MYSQL *conRAII;
	SQLConnectionPool *poolRAII;
};

#endif
