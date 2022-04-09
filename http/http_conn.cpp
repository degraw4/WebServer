#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>

// 定义http状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker lock;
int HttpConn::userCount = 0;
int HttpConn::epollFd = -1;
map<string, string> HttpConn::users;

void HttpConn::initMySQLUser(SQLConnectionPool *connPool){
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user")){
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result)){
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 对文件描述符设置非阻塞
int setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollFd, int fd, bool one_shot, int TRIGMode){
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从内核时间表删除描述符
void removefd(int epollFd, int fd){
    epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 将事件重置为EPOLLONESHOT
void modfd(int epollFd, int fd, int ev, int TRIGMode){
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollFd, EPOLL_CTL_MOD, fd, &event);
}

// 关闭连接
void HttpConn::closeConn(bool real_close){
    if (real_close && (socketFd != -1)){
        removefd(epollFd, socketFd);
        socketFd = -1;
        userCount--;
    }
}

// 初始化连接,外部调用初始化套接字地址
void HttpConn::init(int sockfd, const sockaddr_in &addr, char *root, int connTrigmode,
                     string user, string password, string databaseName){
    socketFd = sockfd;
    address = addr;

    addfd(epollFd, sockfd, true, m_TRIGMode);
    userCount++;

    // 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = connTrigmode;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, password.c_str());
    strcpy(sql_name, databaseName.c_str());

    init();
}

// 初始化新接受的连接
// check_state默认为分析请求行状态
void HttpConn::init(){
    MySQL = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    m_request_and_header = 0;
    cgi = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机，用于分析出一行内容
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
HttpConn::LINE_STATUS HttpConn::parse_line(){
    char temp;

    // upload
    if(m_content_length > THRESHOLD 
    && m_checked_idx == m_request_and_header && m_check_state != 0){
        return LINE_BAD;
    }

    for (; m_checked_idx < m_read_idx; ++m_checked_idx){
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r'){
            if ((m_checked_idx + 1) == m_read_idx){
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_idx + 1] == '\n'){
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n'){
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r'){
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 循环读取客户数据，直到无数据可读或对方关闭连接
// 非阻塞ET工作模式下，需要一次性将数据读完
bool HttpConn::readOnce()
{
    if (m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }
    int bytes_read = 0;

    // LT
    if (0 == m_TRIGMode){
        bytes_read = recv(socketFd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0){
            return false;
        }
        return true;
    }
    // ET
    else{
        while (true){
            bytes_read = recv(socketFd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1){
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0){
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

// 解析http请求行，获得请求方法，目标url及http版本号
HttpConn::HTTP_CODE HttpConn::parse_request_line(char *text){
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析http请求的header
HttpConn::HTTP_CODE HttpConn::parse_headers(char *text){
    // header已空且m_content_length不为0，说明后面还有content
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            m_request_and_header = m_checked_idx;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 判断http请求是否被完整读入
HttpConn::HTTP_CODE HttpConn::parse_content(char *text){
    if (m_read_idx >= (m_content_length + m_request_and_header)){
        // 注册 or 登录
        if(m_content_length <= THRESHOLD){
            text[m_content_length] = '\0';
            // POST请求中最后为输入的用户名和密码
            m_string = text;
            return GET_REQUEST;
        }
        // 上传
        else{
            text[m_content_length] = '\0';
            return GET_REQUEST;
        }
    }
    // 没有完整读入
    else{
        return NO_REQUEST;
    }
}

// 对读到http conn的报文内容进行解析
HttpConn::HTTP_CODE HttpConn::process_read(){
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        // get_line用于将指针向后偏移，指向未处理的字符
        // m_read_buf + m_start_line
        // m_start_line是行在buffer中的起始位置，将该位置后面的数据赋给text
        // 此时从状态机已提前将一行的末尾字符\r\n变为\0\0，所以text可以直接取出完整的行进行解析
        text = get_line();
        // 已经解析的字符个数 = m_read_buf读取的位置m_checked_idx
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            // 只解析头部，http code仍为NO_REQUEST
            // cgi存储来请求为GET or POST
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                // 没有请求内容，则生成响应报文
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

// 将请求对应的响应html内容读入m_file_address中
HttpConn::HTTP_CODE HttpConn::do_request(){
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    const char *p = strrchr(m_url, '/');

    // 处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        // 根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码提取出来
        // user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            // 如果是注册，先检测数据库中是否有重名的
            // 没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                lock.lock();
                int res = mysql_query(MySQL, sql_insert);
                users.insert(pair<string, string>(name, password));
                lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        // 如果是登录，直接判断
        // 若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    // 注册
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 登录
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 浏览图片
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 浏览视频
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // about
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 上传文件
    else if (*(p + 1) == '8')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/upload.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    // upload
    else if (*(p + 1) == '4'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/upload_over.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);

        m_string = new char[m_content_length];
        memcpy(m_string, m_read_buf + m_checked_idx, m_content_length);

        auto c = strchr(m_string, '\n');
        int head_len = strchr(m_string, '\n') - m_string;
        int tail_len = head_len + 4;
        int name_start = strstr(m_string, "filename=") - m_string + 10;
        int name_end = strchr(m_string + name_start, '\"') - m_string;
        char *file_name = new char[name_end - name_start];
        memcpy(file_name, m_string + name_start, name_end - name_start);

        int file_start = strstr(m_string + name_end + 5, "\r\n") - m_string + 4;
        int file_end = m_content_length - tail_len - 1;

        char *file_path = new char[strlen(file_name) + strlen("./root/file/") + 1];
        strcpy(file_path, "./root/file/");
        strcat(file_path, file_name);
        FILE *fp = fopen(file_path, "w+");
        fwrite(m_string + file_start, 1, file_end - file_start, fp);
        fclose(fp);
        delete [] file_name;
        delete [] file_path;
        delete [] m_string;
    }
    // 下载文件
    else if (*(p + 1) == '9')
    {
        system("tree -H '.' -L 3 -I '*.ico|*.html' ./root/ --noreport --charset utf-8 -o ./root/download.html");
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/download.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else{
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    // fd为对应打开的html文件
    int fd = open(m_real_file, O_RDONLY);
    // mmap
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

void HttpConn::unmap(){
    if (m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool HttpConn::write(){
    int temp = 0;

    if (bytes_to_send == 0){
        modfd(epollFd, socketFd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1){
        temp = writev(socketFd, m_iv, m_iv_count);

        if (temp < 0){
            if (errno == EAGAIN){
                modfd(epollFd, socketFd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len){
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else{
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0){
            unmap();
            modfd(epollFd, socketFd, EPOLLIN, m_TRIGMode);

            if (m_linger){
                init();
                return true;
            }
            else{
                return false;
            }
        }
    }
}

bool HttpConn::add_response(const char *format, ...){
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}

bool HttpConn::add_status_line(int status, const char *title){
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool HttpConn::add_headers(int content_len){
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

bool HttpConn::add_content_length(int content_len){
    return add_response("Content-Length:%d\r\n", content_len);
}

bool HttpConn::add_content_type(){
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool HttpConn::add_linger(){
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool HttpConn::add_blank_line(){
    return add_response("%s", "\r\n");
}

bool HttpConn::add_content(const char *content){
    return add_response("%s", content);
}

bool HttpConn::process_write(HTTP_CODE ret){
    switch (ret){
        case INTERNAL_ERROR:{
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }
        case BAD_REQUEST:{
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }
        case FORBIDDEN_REQUEST:{
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }
        case FILE_REQUEST:{
            add_status_line(200, ok_200_title);
            if (m_file_stat.st_size != 0){
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else{
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
        default:
            return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void HttpConn::process(){
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST){
        modfd(epollFd, socketFd, EPOLLIN, m_TRIGMode);
        return;
    }
    // 调用process_write完成报文响应，不是发送文件，而是发送响应的html
    bool write_ret = process_write(read_ret);
    if (!write_ret){
        closeConn();
    }
    // 子线程调用process_write完成响应报文，随后注册epollout事件
    // 主线程检测写事件，并调用HttpConn::write函数将响应报文发送给浏览器端。
    modfd(epollFd, socketFd, EPOLLOUT, m_TRIGMode);
}
