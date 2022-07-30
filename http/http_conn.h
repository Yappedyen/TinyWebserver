#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#pragma once
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>
#include<unordered_map>
#include<unordered_set>
#include<regex>
#include<string>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

using namespace std;
class http_conn
{
public:
    static const int FILENAME_LEN = 200;        // 文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048;   // 读缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;  // 写缓冲区的大小
    // HTTP请求方法，但我们只支持GET
    enum METHOD
    {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH};
    /* 解析客户端请求时，主状态机状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体*/
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    /*服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST: 请求不完整，需要继续读取客户数据
        GET_REQUEST: 表示获得了一个完整的客户请求
        BAD_REQUEST: 表示客户请求语法错误
        NO_RESOURCE: 表示服务器没有资源
        FORBIDDEN_REQUEST: 表示客户对资源没有足够的访问权限
        FILE_REQUEST: 文件请求，获取文件成功
        INTERNAL_ERROR: 表示服务器内部错误
        CLOSED_CONNECTION: 表示客户端已经关闭连接*/
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    // 初始化新接收的连接
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    // 关闭连接
    void close_conn(bool real_close = true);
    // 处理客户端的请求
    void process();
    // 非阻塞的读
    bool read_once();
    // 非阻塞的写
    bool write();
    // 获取客户端地址
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    // 初始化数据库连接
    void initmysql_result(connection_pool *connPool);
    // 定时器标记
    int timer_flag;
    // 
    int improv;


private:
    // 初始化连接其余的信息
    void init();
    
    // 解析HTTP请求
    HTTP_CODE process_read();
    // 填充HTTP应答
    bool process_write(HTTP_CODE ret);
    // 下面这一组函数被process_read调用以分析HTTP请求

    // 解析请求首行
    HTTP_CODE parse_request_line(char *text);
    // 解析请求头
    HTTP_CODE parse_headers(char *text);
    // 解析请求体
    HTTP_CODE parse_content(char *text);
    // 对请求进行响应
    HTTP_CODE do_request();
    // 读取一行
    char *get_line() { return m_read_buf + m_start_line; };
    // 解析请求行
    LINE_STATUS parse_line();
    // 这一组函数被process_write调用以填充HTTP应答
    // 对内存映射区执行munmap操作
    void unmap();
    // 往写缓冲中写入待发送的数据
    bool add_response(const char *format, ...);
    // 添加响应体
    bool add_content(const char *content);
    // 添加状态行
    bool add_status_line(int status, const char *title);
    // 添加响应头
    bool add_headers(int content_length);
    // 添加响应体类型
    bool add_content_type();
    // 添加响应体长度
    bool add_content_length(int content_length);
    // 添加HTTP响应是否保持连接
    bool add_linger();
    // 添加空行
    bool add_blank_line();

    string GetFileType_();
    // 响应体类型
    static const std::unordered_map<std::string, std::string> SUFFIX_TYPE;

public:
    // 所有的socket上的事件都被注册到同一个epoll对象中
    static int m_epollfd;
    // 统计用户的数量
    static int m_user_count;
    // 数据库连接对象
    MYSQL *mysql;           
    int m_state;  //读为0, 写为1

private:
    // 该HTTP连接的socket
    int m_sockfd;
    // 通信的socket地址
    sockaddr_in m_address;
    // 读缓冲
    char m_read_buf[READ_BUFFER_SIZE];
    // 标识读缓冲区中以及读入的客户端数据的最后一个字节的下一个位置
    int m_read_idx;
    // 当前正在分析的字符在读缓冲区的位置
    int m_checked_idx;
    // 当前正在解析的行的起始位置
    int m_start_line;
    // 写缓冲
    char m_write_buf[WRITE_BUFFER_SIZE];
    // 写缓冲区中待发送的字节数
    int m_write_idx;
    // 主状态机当前所处的状态
    CHECK_STATE m_check_state;
    // 请求方法
    METHOD m_method;
    // 客户请求的文件的完整路径，其内容等于doc_root + m_url, doc_root为网站根目录
    char m_real_file[FILENAME_LEN];
    // 请求目标文件的文件名
    char *m_url;
    // 协议版本，只支持HTTP1.1
    char *m_version;
    // 主机名
    char *m_host;
    // HTTP请求的消息体的长度
    int m_content_length;
    // HTTP请求是否要保持连接
    bool m_linger;
    // 客户请求的目标文件被mmap到内存中的起始位置
    char *m_file_address;
    // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct stat m_file_stat;
    // 我们将采用writev来执行写操作，所以定义下面两个成员。
    // i/o 向量
    struct iovec m_iv[2];
    // m_iv_count表示被写内存块的数量
    int m_iv_count;
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    // 将要发送的数据的字节数
    int bytes_to_send;
    // 已经发送的字节数
    int bytes_have_send;
    // 网站根目录
    char *doc_root;
    // 用户映射
    map<string, string> m_users;
    // 连接的触发模式
    int m_TRIGMode;
    // 日志写入方式
    int m_close_log;
    // 数据库用户名
    char sql_user[100];
    // 数据库密码
    char sql_passwd[100];
    // 数据库名
    char sql_name[100];

private:
    bool ParseRequestLine_(const std::string& line);
    void ParseHeader_(const std::string& line);
    void ParseBody_(const std::string& line);

    void ParsePath_();
    void ParsePost_();
    void ParseFromUrlencoded_();

    bool UserVerify(const std::string& name, const std::string& pwd, bool isLogin);

    CHECK_STATE state_;
    std::string method_, path_, version_, body_;
    std::unordered_map<std::string, std::string> header_;
    std::unordered_map<std::string, std::string> post_;

    static const std::unordered_set<std::string> DEFAULT_HTML;
    static const std::unordered_map<std::string, int> DEFAULT_HTML_TAG;
    static int ConverHex(char ch);
};

#endif
