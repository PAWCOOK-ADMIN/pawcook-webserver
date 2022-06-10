#include "http_conn.h"

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/home/pawcook/webserver/resources";

int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    if(one_shot) 
    {
        // 防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);  
}

// 从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次连接 socket 可读时，EPOLLIN 事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;            // ET 边缘触发；    EPOLLRDHUP：对端套接字关闭
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 所有的客户数
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;

// 关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;
    
    // 端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

void http_conn::init()
{

    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;       
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_request_path, FILENAME_LEN);
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    if(m_read_idx >= READ_BUFFER_SIZE) {        // 如果读缓冲区已满
        return false;
    }

    int bytes_read = 0;

    while(true) {
        // 从 m_read_buf + m_read_idx 索引处开始保存数据，大小是 READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        
        if (bytes_read == -1) {         // 读取数据失败，可能的原因是被中断或者连接 socket 收到了 RST 
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // 套接字的读缓冲区中的数据读完
                break;
            }
            return false;   
        } 
        else if (bytes_read == 0) {    // 对方关闭了写端，服务端收到 FIN，读到了文件结尾
            return false;
        }
        m_read_idx += bytes_read;
    }

    return true;
}

// 解析读缓冲区中的一行，判断依据 \r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[ m_checked_idx ];
        if (temp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx)                      // 如果读缓冲区的数据末尾中只有一个 \r，则行数据不完整
                return LINE_OPEN;
            else if (m_read_buf[ m_checked_idx + 1 ] == '\n') {         // 如果解析到 \r\n, 则将它们替换为 \0
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            else                                                        // 如果读缓冲区中只有一个 \r，则行出错
                return LINE_BAD;
        } 
        else if(temp == '\n')  {
            if((m_checked_idx > 1) && (m_read_buf[ m_checked_idx - 1 ] == '\r')) {              // 这种情况会出现吗
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析HTTP请求行，获得请求方法，请求的资源, 以及 HTTP 版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");           // 在字符串中搜索 “ \t” 中的某个字符，返回首次出现的指针
    
    if (! m_url) 
        return BAD_REQUEST;

    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';                        // 置位空字符，字符串结束符

    char* method = text;
    if (strcasecmp(method, "GET") == 0)     // 忽略大小写比较
        m_method = GET;
    else
        return BAD_REQUEST;

    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk(m_url, " \t");              // 在字符串中搜索 “ \t” 中的某个字符，返回首次出现的指针
    if (!m_version) 
        return BAD_REQUEST;

    *m_version++ = '\0';
    if (strcasecmp(m_version, "HTTP/1.1") != 0) 
        return BAD_REQUEST;
    
    // 有的请求可能是这种样子：http://192.168.110.129:10000/index.html
    if (strncasecmp(m_url, "http://", 7) == 0) {   
        m_url += 7;
        m_url = strchr(m_url, '/');             // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
    }
    if (!m_url || m_url[0] != '/') 
        return BAD_REQUEST;

    m_check_state = CHECK_STATE_HEADER;        // 检查状态变成检查首部字段
    return NO_REQUEST;
}

// 解析HTTP请求的一个首部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {   
    // 遇到空行，表示首部字段解析完毕
    if(text[0] == '\0') {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } 
    // 处理Connection 头部字段  Connection: keep-alive
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    }
    // 处理Content-Length头部字段
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } 
    else if (strncasecmp(text, "Host:", 5) == 0) {
        // 处理Host头部字段
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } 
    else
        std::cout << "oop! unknow header " << text << std::endl;

    return NO_REQUEST;
}

// 我们没有真正解析 HTTP 请求的消息体，只是判断它是否被完整的读入到读缓冲区中
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = nullptr;

    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
                || ((line_status = parse_line()) == LINE_OK)) {
        // 获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;           // 设置下一行的起始位置
        std::cout<<"got 1 http line: "<< text << std::endl;

        switch (m_check_state) {
            case CHECK_STATE_REQUESTLINE: {             // 如果当前正在解析请求行
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {                  // 如果当前正在解析首部字段
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                else if (ret == GET_REQUEST)
                    return do_request();
                break;
            }
            case CHECK_STATE_CONTENT: {                 // 如果当前正在解析报文主体
                ret = parse_content(text);
                if (ret == GET_REQUEST)
                    return do_request();
                else
                    line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 当得到一个完整、正确的 HTTP 请求时，我们就分析请求的目标文件的属性，如果目标文件存在、对所有用户可读，且不是目录，
// 则使用 mmap 将其映射到内存地址 m_file_address 处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    // "/home/nowcoder/webserver/resources"
    strcpy(m_request_path, doc_root);
    int len = strlen(doc_root);
    strncpy(m_request_path + len, m_url, FILENAME_LEN - len - 1);

    // 获取m_request_path文件的相关的状态信息，-1 失败，0 成功
    if (stat(m_request_path, &m_file_stat) == -1)
        return NO_RESOURCE;

    // 判断访问权限
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode)) 
        return BAD_REQUEST;

    // 以只读方式打开文件
    int fd = open(m_request_path, O_RDONLY);
    // 创建私有文件映射
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 解除内存映射
void http_conn::unmap() {
    if(m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 向客户端发送 HTTP 响应
bool http_conn::write() {
    int temp = 0;
    
    // 如果要发送的字节为 0，则本次响应结束，重置读写缓冲区。
    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN); 
        init();
        return true;
    }

    while(1) {
        // 集中写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1) {
            // 如果 TCP 写缓冲没有空间获取被中断，则等待下一轮 EPOLLOUT 事件，虽然在此期间，服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if(errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            else {
                unmap();
                return false;
            }
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len) {           // 部分写的情况1，如果集中写的第一个缓冲区已经发送完毕，第二个缓冲区发送了一部分
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else {                                              // 部分写的情况2，集中写的第一个缓冲区发送了一部分数据。
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        // 如果集中写的数据发送完毕
        if (bytes_to_send <= 0) {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);            // 重新向 epoll 注册连接 socket 上的可读事件

            if (m_linger) {                                 // 如果设置了保持连接，则重置读写缓冲区等
                init();
                return true;
            }
            else 
                return false;
        }
    }
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response(const char* format, ...) {
    if(m_write_idx >= WRITE_BUFFER_SIZE)            // 如果写缓冲区已满
        return false;

    va_list arg_list;       
    va_start(arg_list, format);                     // 确定可变参数的起始地址。
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);

    m_write_idx += len;
    va_end(arg_list);                               // 将指针清零
    return true;
}

// 为 HTTP 响应报文添加状态行
bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 为 HTTP 响应报文添加首部字段
bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
    return true;
}

// 为 HTTP 响应报文添加首部字段 Content-Length
bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}

// 为 HTTP 响应报文添加首部字段 Connection
bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

// 为 HTTP 响应报文添加空行
bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

// 为 HTTP 响应报文添加首部字段 Content-Type
bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 为 HTTP 响应报文添加报文主体
bool http_conn::add_content(const char* content) {
    return add_response("%s", content);
}

// 根据服务器处理 HTTP 请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            add_content(error_500_form);
            break;
        case BAD_REQUEST:
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            add_content(error_400_form);
            break;
        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            add_content(error_404_form);
            break;
        case FORBIDDEN_REQUEST:
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            add_content(error_403_form);
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
            
        default:
            return false;
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 由线程池中的工作线程调用，这是处理 HTTP 请求的入口函数
void http_conn::process() {
    // 解析 HTTP 请求
    HTTP_CODE read_ret = process_read();

    if (read_ret == NO_REQUEST) {                       // 如果本次请求无效，则重新把连接 socket 上的读事件加入到 epoll 中
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    
    // 生成响应
    bool write_ret = process_write(read_ret);           // 当请求解析成功后，则把连接 socket 上的写事件加入到 epoll 中
    if (!write_ret) {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}