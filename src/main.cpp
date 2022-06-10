#include <iostream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65535                // webserve 能接受的最大连接个数
#define MAX_EVENT_NUMBER 10000      // epoll 能监听的最大文件描述符的数目

// 添加文件描述符到 epoll 中，定义在 http_conn
extern void addfd(int epollfd, int fd, bool one_shot);
// 从 epoll 中删除描述符
extern void removefd(int epollfd, int fd);
// 修改文件描述符
extern void modfd(int epollfd, int fd, int ev);

// 注册信号处理函数
void addsig(int sig, void (handler) (int)) {
    struct sigaction sa;
    
    memset(&sa, 0, sizeof(sa));  
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);                // 设置临时阻塞信号集
    sigaction(sig, &sa, nullptr);           // 设置信号的处理函数
}


// 参数用于指定端口号
int main(int argc, char *argv[]) {

    // 判断参数
    if(argc <= 1) {     // 参数不足，未传入端口号
        std::cout << "按照如下格式运行：" << basename(argv[0]) << " port_number" << std::endl;
        exit(-1);
    }

    // 获取端口号
    int port = std::stoi(argv[1], nullptr, 10);

    // 对 SIGPIE 信号进行处理
    addsig(SIGPIPE, SIG_IGN);               // 向一个没有读端的管道写数据时会产生该信号

    // 创建线程池
    threadpool<http_conn> *pool = nullptr;          // 任务对象是一个 http 连接
    try {
        pool = new threadpool<http_conn>;
    }catch(...) {
        exit(-1);
    }

    // 创建一个数组保存所有的客户端信息
    http_conn *users = new http_conn[MAX_FD];

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);

    // 设置端口复用，必须在绑定之前（作用，允许多个套接字绑定在同一个端口上）
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // 绑定 socket 地址
    struct sockaddr_in address;
    address.sin_family = AF_INET;           // 地址族
    address.sin_addr.s_addr = INADDR_ANY;   // IP 地址
    address.sin_port = htons(port);         // 将主机字节序转换为网络字节序
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));

    // 监听
    listen(listenfd, 5);            // 设置未决连接的个数为 5

    // 创建 epoll 事件数组和 epoll 实例
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);

    // 将监听的文件描述符添加到 epoll 实例中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    // web 服务器一直循环
    while (true){              
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);            // 检测 epoll 实例中是否有就绪事件
        if (num<0 && errno != EINTR) {
            std::cout << "epoll failure" << std::endl;
            break;
        }

        // 循环遍历事件数组
        for (int i=0; i<num; i++) {
            int sockfd = events[i].data.fd;

            if (sockfd == listenfd) {           // 说明有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);              // 接受来自客户端的连接请求

                // 目前连接数已达到最大，则关闭连接请求，表示服务器正忙
                if (http_conn::m_user_count >= MAX_FD) {
                    close(connfd);
                    continue;
                }

                // 将新的客户的数据初始化，放入 users 数组中
                users[connfd].init(connfd, client_address);
            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {        // EPOLLHUP：挂断       EPOLLRDHUP：对端套接字关闭      EPOLLERR：有错误发生
                // 异常断开或错误，则断开连接
                users[sockfd].close_conn();
            }
            else if (events[i].events & EPOLLIN) {  // 读事件
                if (users[sockfd].read())                   // 将所有数据读出      
                    pool->append(users + sockfd);           // 把任务（即 http 请求）追加到线程池中
                else 
                    users[sockfd].close_conn();
            }
            else if (events[i].events & EPOLLOUT) {  // 写事件
                if(!users[sockfd].write())                 // 一次性写，但是失败（成功但并未请求保持连接）
                    users[sockfd].close_conn();
            }
        }
    }
    
    close(epollfd);
    close(listenfd);
    delete []users;
    delete pool;

    return 0;
}