/*
 * @Author: PengYan 
 * @Date: 2022-07-23 20:50:36 
 * @Last Modified by: PengYan
 * @Last Modified time: 2022-07-24 01:42:36
 * add getfiletype()  to render html
 */
#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>
using namespace std;


const unordered_map<string, string> http_conn::SUFFIX_TYPE = {
    { ".html",  "text/html" },
    { ".xml",   "text/xml" },
    { ".xhtml", "application/xhtml+xml" },
    { ".txt",   "text/plain" },
    { ".rtf",   "application/rtf" },
    { ".pdf",   "application/pdf" },
    { ".word",  "application/nsword" },
    { ".png",   "image/png" },
    { ".gif",   "image/gif" },
    { ".jpg",   "image/jpeg" },
    { ".jpeg",  "image/jpeg" },
    { ".au",    "audio/basic" },
    { ".mpeg",  "video/mpeg" },
    { ".mpg",   "video/mpeg" },
    { ".avi",   "video/x-msvideo" },
    { ".gz",    "application/x-gzip" },
    { ".tar",   "application/x-tar" },
    { ".css",   "text/css "},
    { ".js",    "text/javascript "},
};

const unordered_set<string> http_conn::DEFAULT_HTML{
            "/index", "/register", "/login",
             "/welcome", "/video", "/picture", };

const unordered_map<string, int> http_conn::DEFAULT_HTML_TAG {
            {"/register.html", 0}, {"/login.html", 1},  };

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";
// 互斥锁对象
locker m_lock;
// 数据库用户密码
map<string, string> users;

// 初始化数据库连接
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
void setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    // return old_option;
}

// 向epoll中添加需要监听的文件描述符，将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    // 创建节点结构体将监听连接句柄
    epoll_event event;
    event.data.fd = fd;
    // 触发模式为ET
    // 设置该句柄为边缘触发EPOLLET（数据没处理完后续不会再触发事件，水平触发是不管数据有没有触发都返回事件）
    // EPOLLIN 请求事件   对端断开连接触发的epoll事件包含EPOLLIN | EPOLLRDHUP
    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;
    // 如果是多线程在处理，一个SOCKET事件到来，数据开始解析，这时候这个SOCKET又来了同样一个这样
    // 的事件，而你的数据解析尚未完成，那么程序会自动调度另外一个线程或者进程来处理新的事件，
    // 这造成一个很严重的问题，不同的线程或者进程在处理同一个SOCKET的事件.
    // EPOLLONESHOT这种方法，可以在epoll上注册这个事件，注册这个事件后，如果在处理写成当前
    // 的SOCKET后不再重新注册相关事件，那么这个事件就不再响应了或者说触发了。
    // 要想重新注册事件则需要调用epoll_ctl重置文件描述符上的事件，这样前面的socket就不会出
    // 现竞态这样就可以通过手动的方式来保证同一SOCKET只能被一个线程处理，不会跨越多个线程。
    // 防止同一个通信被不同的线程处理
    if (one_shot)
        event.events |= EPOLLONESHOT;
    // 添加监听连接句柄作为初始节点进入红黑树结构中，该节点后续处理连接的句柄
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);
}

// 从epoll中删除文件描述符，从内核事件表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置socket上EPOLLONESHOT事件，确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}
// 所有的客户数
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;

//关闭连接，
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        // 从epoll中移除监听的文件描述符
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        // 关闭一个连接，客户总量减一
        m_user_count--;
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;
    // 添加到epoll对象中，新的客户连接置为EPOLLONESHOT事件
    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    // 总用户数+1
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    // 网站的根目录
    doc_root = root;
    // 触发模式
    m_TRIGMode = TRIGMode;
    // 日志写入方式
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    // 初始化状态为解析请求首行
    m_check_state = CHECK_STATE_REQUESTLINE;
    // 默认不保持连接  Connection:keep-alive保持连接
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
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    method_ = path_ = version_ = body_ = "";
    state_ = CHECK_STATE_REQUESTLINE;
    header_.clear();
    post_.clear();

    // 分配缓冲区，填充\0
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
// 解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        // \r 回车  \r\n回车换行
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                // 行数据不完整
                return LINE_OPEN;
            // 换行符
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                // 将 \r\n 替换为\0\0
                // 更新为字符串的结束符
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                // 读取了完整的行
                return LINE_OK;
            }
            return LINE_BAD;
        }
        // 因为可能每次读取的不是完整的数据，所以要判断
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}
// 非阻塞的读
//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    // 读取到的字节
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        // 从m_read_buf[m_read_idx]索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {   
        // 需要不断读取把数据读取完
        while (true)
        {
            // // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    // 没有数据
                    break;
                return false;
            }
            // 没有数据可读
            else if (bytes_read == 0)
            {
                // 对方关闭连接
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // error
    // 用正则来处理
    string temp;
    temp = text;
    if(!ParseRequestLine_(temp)){
        return BAD_REQUEST;
    }
    ParsePath_();
    // // 主状态机检查状态变成检查请求头
    // m_check_state = CHECK_STATE_HEADER;
    // return NO_REQUEST;


    // GET / HTTP/1.1\0
    // 判断第二个参数中的字符哪个在text中最先出现
    // 检索字符串 str1 中第一个匹配字符串 str2 中字符的字符，不包含空结束字符,并返回该字符位置
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    // \0/ HTTP/1.1\0
    // m_url 是地址，修改指向的值
    *m_url++ = '\0';
    // m_url = / HTTP/1.1\0
    // text = GET\0/ HTTP/1.1\0
    char *method = text;
    // 获得请求方法
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    
    // 看从str第一个开始，前面的字符有几个在accept中;
    // 或者检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标
    // 这里应该为0,m_url就为 /
    m_url += strspn(m_url, " \t");
    // 检索字符串 str1 中第一个在字符串 str2 中出现的字符下标。  
    //  HTTP/1.1\0
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    // \0HTTP/1.1\0
    *m_version++ = '\0';
    // HTTP/1.1\0
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    // 如果有http / https 去除
    // http://192.168.1.1:10000/index.html
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        // m_url = 192.168.1.1:10000/index.html
        m_url += 7;
        // 找到第一个出现/的字符位置
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    // 判断url是否正确
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示index界面
    if (strlen(m_url) == 1)
        strcat(m_url, "/index.html");
    // 主状态机检查状态变成检查请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}
// 解析请求行
bool http_conn::ParseRequestLine_(const string& line) {
    // 正则表达式
    // ()标记一个子表达式的开始和结束位置
    // * 匹配前面的子表达式零次或多次
    // ^ 表示字符串的开始，匹配输入字符串开始的位置
    // $ 表示字符串的结尾，匹配输入字符串结尾的位置
    // []中使用^来表示集合的补集，匹配不在指定的范围内的任何字符
    regex patten("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$");
    // 对匹配结果进行存储
    smatch subMatch;
    if(regex_match(line, subMatch, patten)) {   
        method_ = subMatch[1];
        path_ = subMatch[2];
        version_ = subMatch[3];
        state_ = CHECK_STATE_HEADER;
        return true;
    }
    LOG_ERROR("RequestLine Error");
    return false;
}

// regex解析请求头
void http_conn::ParseHeader_(const string& line) {
    regex patten("^([^:]*): ?(.*)$");
    smatch subMatch;
    if(regex_match(line, subMatch, patten)) {
        header_[subMatch[1]] = subMatch[2];
    }
    else {
        state_ = CHECK_STATE_CONTENT;
    }
}

// 解析请求体
void http_conn::ParseBody_(const string& line) {
    body_ = line;
    ParsePost_();
    // state_ = FINISH;
    LOG_DEBUG("Body:%s, len:%d", line.c_str(), line.size());
}

void http_conn::ParsePath_() {
    if(path_ == "/") {
        path_ = "/index.html"; 
    }
    else {
        for(auto &item: DEFAULT_HTML) {
            if(item == path_) {
                path_ += ".html";
                break;
            }
        }
    }
}
// 转换十六进制为十进制
int http_conn::ConverHex(char ch) {
    if(ch >= 'A' && ch <= 'F') return ch -'A' + 10;
    if(ch >= 'a' && ch <= 'f') return ch -'a' + 10;
    return ch;
}
// 处理POST请求
void http_conn::ParsePost_() {
    if(method_ == "POST" && header_["Content-Type"] == "application/x-www-form-urlencoded") {
        ParseFromUrlencoded_();
        if(DEFAULT_HTML_TAG.count(path_)) {
            int tag = DEFAULT_HTML_TAG.find(path_)->second;
            LOG_DEBUG("Tag:%d", tag);
            if(tag == 0 || tag == 1) {
                bool isLogin = (tag == 1);
                if(UserVerify(post_["user"], post_["password"], isLogin)) {
                    path_ = "/welcome.html";
                    strcpy(m_url, "/welcome.html");
                } 
                else {
                    path_ = "/error.html";
                    strcpy(m_url, "/error.html");
                }
            }
        }
    }   
}
// 处理请求体
void http_conn::ParseFromUrlencoded_() {
    if(body_.size() == 0) { return; }

    string key, value;
    int num = 0;
    int n = body_.size();
    int i = 0, j = 0;

    for(; i < n; i++) {
        char ch = body_[i];
        switch (ch) {
        case '=':
            key = body_.substr(j, i - j);
            j = i + 1;
            break;
        case '+':
            body_[i] = ' ';
            break;
        case '%':
            num = ConverHex(body_[i + 1]) * 16 + ConverHex(body_[i + 2]);
            body_[i + 2] = num % 10 + '0';
            body_[i + 1] = num / 10 + '0';
            i += 2;
            break;
        case '&':
            value = body_.substr(j, i - j);
            j = i + 1;
            post_[key] = value;
            LOG_DEBUG("%s = %s", key.c_str(), value.c_str());
            break;
        default:
            break;
        }
    }
    assert(j <= i);
    if(post_.count(key) == 0 && j < i) {
        value = body_.substr(j, i - j);
        post_[key] = value;
    }
}

bool http_conn::UserVerify(const string &name, const string &pwd, bool isLogin) {
    if(name == "" || pwd == "") { return false; }
    // flag 标记用户名被使用(false),未被使用true;
    bool flag = false;
    LOG_INFO("Verify name:%s pwd:%s", name.c_str(), pwd.c_str());
    // if(!isLogin) { flag = true; }
    // 登录行为
    if(isLogin){
        if(users.find(name) != users.end()){
            if(users[name] == pwd){
                LOG_DEBUG( "UserVerify success!!");
                flag = true;
            }
            else{
                flag = false;
                LOG_DEBUG("pwd error!");
            }
        }else{
            LOG_DEBUG("user not found!");
            flag = false;
        }
    }
    // 注册行为
    else{
        // 用户名被使用
        if(users.find(name) != users.end()){
            LOG_DEBUG("user used!");
            flag = false;
        }else{
            /* 注册行为 且 用户名未被使用*/
            LOG_DEBUG("regirster!");
            char order[256] = { 0 };
            bzero(order, 256);
            snprintf(order, 256,"INSERT INTO user(username, passwd) VALUES('%s','%s')", name.c_str(), pwd.c_str());
            LOG_DEBUG( "%s", order);
            m_lock.lock();
            // 插入数据库失败
            if(mysql_query(mysql, order)) { 
                LOG_DEBUG( "Insert error!");
                flag = false; 
                strcpy(m_url, "/registerError.html");
            }else{
                users.insert(pair<string, string>(name, pwd));
                LOG_DEBUG( "Insert Success!");
                flag = true;
                // 注册成功，m_url变为登录地址
                strcpy(m_url, "/login.html");
            }
            m_lock.unlock();
        }
    }
    // 用户名找到
    if(users.find(name) != users.end()){
        if(isLogin){
            if(users[name] == pwd){
                LOG_DEBUG( "UserVerify success!!");
                flag = true;
            }
            else{
                flag = false;
                LOG_DEBUG("pwd error!");
            }
        }else{
            flag = false;
            LOG_DEBUG("user used!");
        }
    }else{
        if(isLogin){
            LOG_DEBUG("user not found!");
            flag = true;
        }else{
            /* 注册行为 且 用户名未被使用*/
            LOG_DEBUG("regirster!");
            char order[256] = { 0 };
            bzero(order, 256);
            snprintf(order, 256,"INSERT INTO user(username, passwd) VALUES('%s','%s')", name.c_str(), pwd.c_str());
            LOG_DEBUG( "%s", order);
            m_lock.lock();
            // 插入数据库失败
            if(mysql_query(mysql, order)) { 
                LOG_DEBUG( "Insert error!");
                flag = false; 
                strcpy(m_url, "/registerError.html");
            }else{
                users.insert(pair<string, string>(name, pwd));
                LOG_DEBUG( "Insert Success!");
                flag = true;
                // 注册成功，m_url变为登录地址
                strcpy(m_url, "/login.html");
            }
            m_lock.unlock();
        }
    }

    


    if (users.find(name) != users.end() && users[name] == pwd){
        LOG_DEBUG( "UserVerify success!!");
        return true;
    }
    return false;
    // MYSQL* sql;
    // SqlConnRAII(&sql,  SqlConnPool::Instance());
    // assert(sql);
    
    // bool flag = false;
    // unsigned int j = 0;
    // char order[256] = { 0 };
    // MYSQL_FIELD *fields = nullptr;
    // MYSQL_RES *res = nullptr;
    
    // if(!isLogin) { flag = true; }
    // /* 查询用户及密码 */
    // snprintf(order, 256, "SELECT username, password FROM user WHERE username='%s' LIMIT 1", name.c_str());
    // LOG_DEBUG("%s", order);

    // if(mysql_query(sql, order)) { 
    //     mysql_free_result(res);
    //     return false; 
    // }
    // res = mysql_store_result(sql);
    // j = mysql_num_fields(res);
    // fields = mysql_fetch_fields(res);

    // while(MYSQL_ROW row = mysql_fetch_row(res)) {
    //     LOG_DEBUG("MYSQL ROW: %s %s", row[0], row[1]);
    //     string password(row[1]);
    //     /* 注册行为 且 用户名未被使用*/
    //     if(isLogin) {
    //         if(pwd == password) { flag = true; }
    //         else {
    //             flag = false;
    //             LOG_DEBUG("pwd error!");
    //         }
    //     } 
    //     else { 
    //         flag = false; 
    //         LOG_DEBUG("user used!");
    //     }
    // }
    // mysql_free_result(res);

    // /* 注册行为 且 用户名未被使用*/
    // if(!isLogin && flag == true) {
    //     LOG_DEBUG("regirster!");
    //     bzero(order, 256);
    //     snprintf(order, 256,"INSERT INTO user(username, password) VALUES('%s','%s')", name.c_str(), pwd.c_str());
    //     LOG_DEBUG( "%s", order);
    //     if(mysql_query(sql, order)) { 
    //         LOG_DEBUG( "Insert error!");
    //         flag = false; 
    //     }
    //     flag = true;
    // }
    // SqlConnPool::Instance()->FreeConn(sql);
    // LOG_DEBUG( "UserVerify success!!");
    // return flag;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // C++方式来处理
    string te;
    te = text;
    ParseHeader_(te);
    // return NO_REQUEST;

    // 遇到空行，表示头部字段解析完毕
    if (text[0] == '\0')
    {
        // 如果HTTP请求有消息体，一般有请求消息体是POST类型，则还需要读
        // 需要继续读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        // 处理Connection头部字段 Connection: keep-alive
        text += 11;
        text += strspn(text, " \t");
        // 是否保持长连接
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        // 处理Host头部字段
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        // LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    // C++
    string tem;
    tem = text;
    ParseBody_(tem);
    // return GET_REQUEST;
    // 请求体是否完整
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        // POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
    
}

// 主状态机， 解析请求
http_conn::HTTP_CODE http_conn::process_read()
{
    // 行的读取状态为读取一个完整的行
    LINE_STATUS line_status = LINE_OK;
    // 请求不完整
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    // 循环读取完整的行
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        // 解析到了一行完整的数据,或者解析到了请求体，也是完整的数据
        // 获取一行数据，这里获取的是行的起始地址，text就是一行字符串的起始地址，因为已经将\r\n替换为\0\0(字符串结束符)
        text = get_line();
        // 更新每一行的起始位置
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
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

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{

    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    // 找到第一个出现字符的位置
    const char *p = strrchr(m_url, '/');

    //处理cgi；应该就是post
    // 2是login；3是register
    /*
    if (m_method == POST && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        // char *m_url_real = (char *)malloc(sizeof(char) * 200);
        // strcpy(m_url_real, "/");
        // strcat(m_url_real, m_url + 2);
        // strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        // free(m_url_real);

        //将用户名和密码提取出来
        //user=123&password=123
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
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(string(name)) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    // 注册成功，m_url变为登录地址
                    strcpy(m_url, "/login.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(string(name)) != users.end() && users[name] == string(password))
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/error.html");
        }
    }
    */

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    // 获取m_real_file文件的相关的状态信息， -1失败， 0成功
    if (stat(m_real_file, &m_file_stat) < 0 || S_ISDIR(m_file_stat.st_mode))
        return NO_RESOURCE;

    // 判断访问权限
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;
    // 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    // 创建内存映射
    // mmap将一个文件或者其它对象映射进内存,addr:映射开始地址(0表示系统决定);len:映射区的长度;prot:期望的内存保护标志;
    // flags:指定映射对象的类型，映射选项和映射页是否可共享;fd:有效的文件描述符;offset:被映射对象内容的起点
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
// 对内存映射区执行munmap操作
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
// 非阻塞的写
// 写HTTP响应
bool http_conn::write()
{
    int temp = 0;
    // 将要发送的字节（m_write_idx) 写缓冲区中待发送的字节数
    if (bytes_to_send == 0)
    {
        // 将要发送的字节为0，这一次响应结束
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }
        // 已经发送的字节 + temp
        bytes_have_send += temp;
        // 将要发送的字节 - temp
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }
        // 没有数据要发送了
        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
// 往写缓冲中写入待发送的数据
bool http_conn::add_response(const char *format, ...)
{
    // 写索引超出缓冲区大小，返回
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    // 将格式化数据从可变参数列表写入大小缓冲区,返回写入的字符数
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    // 写入大小不能超过缓冲区剩余大小
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    // 更新写缓冲中要发送的字节数
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
// 添加响应行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
// 添加响应头
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_content_type() && add_linger() &&
           add_blank_line();
}
// 添加响应体长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

string http_conn::GetFileType_() {
    /* 判断文件类型 */
    string path_;
    // path_ = m_url;
    path_ = m_real_file;
    string::size_type idx = path_.find_last_of('.');
    if(idx == string::npos) {
        return "text/plain";
    }
    string suffix = path_.substr(idx);
    if(SUFFIX_TYPE.count(suffix) == 1) {
        return SUFFIX_TYPE.find(suffix)->second;
    }
    return "text/plain";
}
// 添加响应体类型
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", GetFileType_().c_str());
}
// 长连接
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
// 添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
// 添加响应体
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
        // 服务器内部错误500
        case INTERNAL_ERROR:
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }
        // 请求语法错误
        case BAD_REQUEST:
        {
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if (!add_content(error_400_form))
                return false;
            break;
        }
        // 请求无资源
        case NO_RESOURCE:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }
        // 客户对资源没有访问权限
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }
        // 文件请求，请求成功
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            if (m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else
            {
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


// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process()
{
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    // 生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
