#include "lst_timer.h"
#include "http_conn.h"
#include <climits>

/////////////////////////////////sort_timer_lst//////////////////////////////////
// 初始化定时器链表
sort_timer_lst::sort_timer_lst(){
    head = new util_timer;
    tail = new util_timer;

    head->next = tail;
    tail->prev = head;
    tail->expire = LONG_MAX;            // 哨兵
}

// 销毁定时器链表
sort_timer_lst::~sort_timer_lst(){
    util_timer *tmp = head;

    while (tmp) {           // 删除其中的每个定时器
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

// 增加一个定时器到链表中
void sort_timer_lst::add_timer(util_timer *timer){
    if (!timer)                     // 如果形参为空
        return;

    // 将定时器插入合适位置（升序）
    util_timer *tmp = head->next;
    while (tmp) {
        if (timer->expire < tmp->expire){
            timer->next = tmp;
            timer->prev = tmp->prev;
            tmp->prev->next = timer;
            tmp->prev = timer;
            return;
        }
    }
}

// 调整定时器的时间，只考虑超时时间延长的情况
void sort_timer_lst::adjust_timer(util_timer *timer) {

    if (!timer || timer == head || timer == tail)             // 如果形参为空、头结点或尾节点
        return;

    // 如果调整后的超时时间仍然小于其后面的定时器的超时时间则不用调整
    util_timer *tmp = timer->next;                  
    if (timer->expire < tmp->expire)
        return;

    // 把定时器摘出来，重新插入定时器链表
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    add_timer(timer);
}

// 从定时器链表中删除某个定时器
void sort_timer_lst::del_timer(util_timer *timer){
    if (!timer)                     // 如果形参为空
        return;

    if ((timer == head) || (timer == tail)) {        // 如果要删除的是头结点或者为尾节点，则说明删除整个定时器链表
        util_timer *tmp = head;

        while (tmp) {               // 删除其中的每个定时器
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
        
        head = NULL;
        tail = NULL;
        return;
    }

    // 删除指定定时器
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

// SIGALARM 信号每次被触发时，都在其信号处理函数中调用一次 tick
// 该函数每次执行，都清楚一次到期的客户端连接
void sort_timer_lst::tick() {
    if (head->next == tail)             // 如果没有客户端连接
        return;
    
    time_t cur = time(nullptr);         // 获取当前时间     
    util_timer *tmp = head->next;
    while (tmp != tail){
        if (cur < tmp->expire)          // 定时器没有超时，直接结束，因为升序，故后面也不用判断
            break;

        tmp->cb_func(tmp->user_data);   // 如果超时，则调用超时处理函数 cb_func
        
        del_timer(tmp);         // 从链表中删除超时了的定时器
    }
}

////////////////////////////Utils/////////////////////////////////////

void Utils::init(int timeslot){
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode){
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数
void Utils::sig_handler(int sig){
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 设置信号处理函数 
// restart 表示是否自动重启由信号处理器程序中断的系统调用，比如 read
void Utils::addsig(int sig, void(handler)(int), bool restart){
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;                    // 信号处理函数     

    if (restart)
        sa.sa_flags |= SA_RESTART;

    sigfillset(&sa.sa_mask);                    // 信号掩码
    assert(sigaction(sig, &sa, nullptr) != -1);    
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler() {
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info){
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
void cb_func(client_data *user_data){
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);           // 从 epoll 实例中删除监听的文件描述符
    assert(user_data);
    close(user_data->sockfd);                         // 关闭客户端连接
    http_conn::m_user_count--;
}
