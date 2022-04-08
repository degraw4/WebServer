#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

SQLConnectionPool::SQLConnectionPool(){
	m_CurConn = 0;
	m_FreeConn = 0;
}

SQLConnectionPool *SQLConnectionPool::getInstance()
{
	static SQLConnectionPool connPool;
	return &connPool;
}

// 初始化
// "localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log
void SQLConnectionPool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn){
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;

	for (int i = 0; i < MaxConn; i++){
		// 使用了mysql_init内部申请的内存
		MYSQL *con = NULL;
		con = mysql_init(con);

		if (con == NULL){
			LOG_ERROR("MySQL Init Error");
			exit(1);
		}
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL){
			LOG_ERROR("MySQL Connect Error");
			exit(1);
		}
		connList.push_back(con);
		++m_FreeConn;
	}

	// 信号量的值使用max连接数初始化
	reserve = sem(m_FreeConn);

	m_MaxConn = m_FreeConn;
}


// 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *SQLConnectionPool::GetConnection(){
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;

	// 对于连接池的操作涉及信号量和mutex，wait会阻塞等待
	// 先信号量，后mutex
	reserve.wait();
	lock.lock();

	con = connList.front();
	connList.pop_front();

	--m_FreeConn;
	++m_CurConn;

	lock.unlock();
	return con;
}

// 释放当前使用的连接
bool SQLConnectionPool::ReleaseConnection(MYSQL *con){
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con);
	++m_FreeConn;
	--m_CurConn;

	lock.unlock();

	reserve.post();
	return true;
}

// 销毁数据库连接池
void SQLConnectionPool::DestroyPool(){

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear();
	}

	lock.unlock();
}

// 当前空闲的连接数
int SQLConnectionPool::GetFreeConn(){
	return this->m_FreeConn;
}

SQLConnectionPool::~SQLConnectionPool(){
	DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, SQLConnectionPool *connPool){
	*SQL = connPool->GetConnection();
	
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}