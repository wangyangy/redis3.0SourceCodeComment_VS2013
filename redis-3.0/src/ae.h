#ifndef __AE_H__
#define __AE_H__

#define AE_OK 0
#define AE_ERR -1

#define AE_NONE 0
#define AE_READABLE 1
#define AE_WRITABLE 2

#define AE_FILE_EVENTS 1
#define AE_TIME_EVENTS 2
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT 4

#define AE_NOMORE -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

struct aeEventLoop;

/* Types and data structures */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, PORT_LONGLONG id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* 文件事件结构 */
typedef struct aeFileEvent {
    int mask; /* 文件事件类型 */
    aeFileProc *rfileProc;  //可读处理函数,函数指针
    aeFileProc *wfileProc;  //可写处理函数,函数指针
    void *clientData;       //客户端传入的数据
} aeFileEvent;

/* 时间事件结构 */
typedef struct aeTimeEvent {
    PORT_LONGLONG id; /* 时间事件id */
    PORT_LONG when_sec; /* 事件到达的秒数 */
    PORT_LONG when_ms; /* 事件到达的毫秒数 */
    aeTimeProc *timeProc;   //事件处理函数
    aeEventFinalizerProc *finalizerProc; //事件终结函数
    void *clientData;      //客户端传入的数据
    struct aeTimeEvent *next;    //指向下一个事件
} aeTimeEvent;

/* A fired event */
typedef struct aeFiredEvent {
    int fd;       //就绪事件文件描述符
    int mask;     //就绪事件类型
} aeFiredEvent;

/* 事件状态结构 */
typedef struct aeEventLoop {
    int maxfd;   /* 当前注册的最大的文件描述符 */
    int setsize; /* 文件描述符监听集合的大小 */
    PORT_LONGLONG timeEventNextId;   //下一个事件的id
    time_t lastTime;     /* 最后一次执行事件的时间 */
    aeFileEvent *events; /* 注册事件 */
    aeFiredEvent *fired; /* 就绪事件 */
    aeTimeEvent *timeEventHead;   //事件头结点指针
    int stop;        //事件处理开关
    void *apidata; /* 多路复用库的时间状态数据 */
    aeBeforeSleepProc *beforesleep;  //执行事件之前的处理函数
} aeEventLoop;

/* Prototypes */
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
PORT_LONGLONG aeCreateTimeEvent(aeEventLoop *eventLoop, PORT_LONGLONG milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, PORT_LONGLONG id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, PORT_LONGLONG milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);

#endif
