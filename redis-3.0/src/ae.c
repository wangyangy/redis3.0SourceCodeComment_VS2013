#include <stdio.h>
#include <sys/types.h>
#ifdef _WIN32
  #include <sys/types.h> 
  #include <sys/timeb.h>
  #include "../../src/Win32_Interop/Win32_FDAPI.h"
  #include "../../src/Win32_Interop/Win32_Service.h"
#else
  #include <sys/time.h>
  #include <unistd.h>
  #include <poll.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

//#include "ae.h"
#include "ae.h"
#include "zmalloc.h"
#include "config.h"

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
#ifdef _WIN32
#include "ae_wsiocp.c"
#else
#ifdef HAVE_EVPORT
#include "ae_evport.c"
#else
    #ifdef HAVE_EPOLL
    #include "ae_epoll.c"
    #else
        #ifdef HAVE_KQUEUE
        #include "ae_kqueue.c"
        #else
        #include "ae_select.c"
        #endif
    #endif
#endif
#endif

//创建并初始化一个事件轮询的状态结构
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;
	//分配空间
    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;
	//分配文件事件表和就绪事件表
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
    //设置事件表的大小
	eventLoop->setsize = setsize;
	//设置最近一次的执行时间
    eventLoop->lastTime = time(NULL);
	//初始化时间事件
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
	//事件处理开启
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;
    eventLoop->beforesleep = NULL;
	// 创建一个epoll实例保存到eventLoop的events中
    if (aeApiCreate(eventLoop) == -1) goto err;
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
	//初始化监听的事件为空
    for (i = 0; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return eventLoop;  //返回事件轮询结构指针

err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

/* 返回当前使劲表的大小 */
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

/* Resize the maximum set size of the event loop.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise AE_OK is returned and the operation is successful. */
// 调整事件表的大小，如果setsize小于最大的文件描述符则返回AE_ERR，并且不做调整，否则返回AE_OK，且调整大小
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    int i;

    if (setsize == eventLoop->setsize) return AE_OK;
    if (eventLoop->maxfd >= setsize) return AE_ERR;
    if (aeApiResize(eventLoop,setsize) == -1) return AE_ERR;

    eventLoop->events = zrealloc(eventLoop->events,sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zrealloc(eventLoop->fired,sizeof(aeFiredEvent)*setsize);
    eventLoop->setsize = setsize;

    /* Make sure that if we created new slots, they are initialized with
     * an AE_NONE mask. */
    for (i = eventLoop->maxfd+1; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return AE_OK;
}
// 删除事件轮询的状态结构
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);
    zfree(eventLoop);
}
// 事件处理开关关闭
void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}
// 创建一个文件事件
// 设置监听fd的事件类型为mask，让事件发生时，则调用proc函数
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData)
{
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }
	//fd对应的文件事件地址
    aeFileEvent *fe = &eventLoop->events[fd];
	//监听fd的mask类型事件
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;
	//设置事件类型
    fe->mask |= mask;
	//设置事件对应的处理函数
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
	//设置客户端传入的数据
    fe->clientData = clientData;
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}
//从eventLoop的事件表中删除一个fd的mask事件
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask)
{
	aeFileEvent *fe;
    if (fd >= eventLoop->setsize) return;
    fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) return;

    aeApiDelEvent(eventLoop, fd, mask);
    fe->mask = fe->mask & (~mask);
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
        int j;

        for (j = eventLoop->maxfd-1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }
}
// 获取fd的事件类型
int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
	aeFileEvent *fe;
    if (fd >= eventLoop->setsize) return 0;
    fe = &eventLoop->events[fd];

    return fe->mask;
}
// 获取当前时间秒和毫秒，并保存到参数中
static void aeGetTime(PORT_LONG *seconds, PORT_LONG *milliseconds)
{
#ifdef _WIN32
    struct _timeb tb;

    memset(&tb, 0, sizeof(struct _timeb));
    _ftime_s(&tb);
    (*seconds) = tb.time;
    (*milliseconds) = tb.millitm;
#else
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
#endif
}
// 将milliseconds个毫秒转换为sec个秒和ms个毫秒，保存到参数中
static void aeAddMillisecondsToNow(PORT_LONGLONG milliseconds, PORT_LONG *sec, PORT_LONG *ms) {
    PORT_LONG cur_sec, cur_ms, when_sec, when_ms;

    aeGetTime(&cur_sec, &cur_ms);
    when_sec = (PORT_LONG) (cur_sec + milliseconds/1000);
    when_ms = cur_ms + milliseconds%1000;
    if (when_ms >= 1000) {
        when_sec ++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}
// 创建一个时间事件
PORT_LONGLONG aeCreateTimeEvent(aeEventLoop *eventLoop, PORT_LONGLONG milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc)
{
	//保存当前时间事件id并更新下一个时间事件的id
    PORT_LONGLONG id = eventLoop->timeEventNextId++;
    aeTimeEvent *te;
	//分配空间
    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;// 设置时间事件的ID
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);
	// 设置时间事件处理的方法
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
	// 保存客户端传入的数据
    te->clientData = clientData;
	// 将当前事件设置为头节点
    te->next = eventLoop->timeEventHead;
    eventLoop->timeEventHead = te;
    return id;
}
// 删除指定ID的时间事件，惰性删除
int aeDeleteTimeEvent(aeEventLoop *eventLoop, PORT_LONGLONG id)
{
    aeTimeEvent *te, *prev = NULL;
	// 时间事件头节点地址
    te = eventLoop->timeEventHead;
	// 遍历所有节点
    while(te) {
		// 找到则删除时间事件
        if (te->id == id) {
            if (prev == NULL)
                eventLoop->timeEventHead = te->next;
            else
                prev->next = te->next;
            if (te->finalizerProc)
                te->finalizerProc(eventLoop, te->clientData);
            zfree(te);
            return AE_OK;
        }
        prev = te;
		te = te->next; // 指向下一个时间事件地址
    }
    return AE_ERR; /* NO event with the specified ID found */
}

/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
// 寻找第一个快到时的时间事件
// 这个操作是有用的知道有多少时间可以选择该事件设置为不用推迟任何事件的睡眠中。
// 如果事件链表没有时间将返回NULL。
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop)
{
	//时间事件头结点地址
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;
	//遍历所有时间事件
    while(te) {
		// 寻找第一个快到时的时间事件，保存到nearest中
        if (!nearest || te->when_sec < nearest->when_sec ||
                (te->when_sec == nearest->when_sec &&
                 te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

/* 执行时间事件 */
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    PORT_LONGLONG maxId;
    time_t now = time(NULL);

    /* If the system clock is moved to the future, and then set back to the
     * right value, time events may be delayed in a random way. Often this
     * means that scheduled operations will not be performed soon enough.
     *
     * Here we try to detect system clock skews, and force all the time
     * events to be processed ASAP when this happens: the idea is that
     * processing events earlier is less dangerous than delaying them
     * indefinitely, and practice suggests it is. */
	// 这里尝试发现时间混乱的情况，上一次处理事件的时间比当前时间还要大
	// 重置最近一次处理事件的时间
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while(te) {
            te->when_sec = 0;
            te = te->next;
        }
    }
	// 设置上一次时间事件处理的时间为当前时间
    eventLoop->lastTime = now;

    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId-1;
	// 遍历时间事件链表
    while(te) {
        PORT_LONG now_sec, now_ms;
        PORT_LONGLONG id;
		// 确保我们不处理在此迭代中由时间事件创建的时间事件。 请注意，此检查目前无效：我们总是在头节点添加新的计时器，但是如果我们更改实施细节，则该检查可能会再次有用：我们将其保留在未来的防御
        if (te->id > maxId) {
            te = te->next;
            continue;
        }
        aeGetTime(&now_sec, &now_ms);
		// 找到已经到时的时间事件
        if (now_sec > te->when_sec ||
            (now_sec == te->when_sec && now_ms >= te->when_ms))
        {
            int retval;

            id = te->id;
            retval = te->timeProc(eventLoop, id, te->clientData);
            processed++;
            /* After an event is processed our time event list may
             * no longer be the same, so we restart from head.
             * Still we make sure to don't process events registered
             * by event handlers itself in order to don't loop forever.
             * To do so we saved the max ID we want to handle.
             *
             * FUTURE OPTIMIZATIONS:
             * Note that this is NOT great algorithmically. Redis uses
             * a single time event so it's not a problem but the right
             * way to do this is to add the new elements on head, and
             * to flag deleted elements in a special way for later
             * deletion (putting references to the nodes to delete into
             * another linked list). */
            if (retval != AE_NOMORE) {
                aeAddMillisecondsToNow(retval,&te->when_sec,&te->when_ms);
            } else {
                aeDeleteTimeEvent(eventLoop, id);
            }
            te = eventLoop->timeEventHead;
        } else {
            te = te->next;
        }
    }
    return processed;
}

// 处理到时的时间事件和就绪的文件事件
// 如果flags = 0，函数什么都不做，直接返回
// 如果flags设置了 AE_ALL_EVENTS ，则执行所有类型的事件
// 如果flags设置了 AE_FILE_EVENTS ，则执行文件事件
// 如果flags设置了 AE_TIME_EVENTS ，则执行时间事件
// 如果flags设置了 AE_DONT_WAIT ，那么函数处理完事件后直接返回，不阻塞等待
// 函数返回执行的事件个数
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int processed = 0, numevents;

#ifdef _WIN32	
    if (ServiceStopIssued() == TRUE) {
        aeStop(eventLoop);
    }
#endif

    /* 如果什么事件都没有设置则直接返回 */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
	// 当前还没有要处理的文件事件，或者设置了时间事件但是没有设置不阻塞标识
    if (eventLoop->maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        aeTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;
		// 如果设置了时间事件而没有设置不阻塞标识
        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            shortest = aeSearchNearestTimer(eventLoop);  //获取最近到的时间事件
		//获取到了最早到时的时间事件
		if (shortest) {
            PORT_LONG now_sec, now_ms;

            //获取当前时间
            aeGetTime(&now_sec, &now_ms);
            tvp = &tv;
            tvp->tv_sec = (int)(shortest->when_sec - now_sec);                  WIN_PORT_FIX /* cast (int) */
            if (shortest->when_ms < now_ms) {
                tvp->tv_usec = (int)((shortest->when_ms+1000) - now_ms)*1000;   WIN_PORT_FIX /* cast (int) */
                tvp->tv_sec --;
            } else {
                tvp->tv_usec = (int)(shortest->when_ms - now_ms)*1000;          WIN_PORT_FIX /* cast (int) */
            }
            if (tvp->tv_sec < 0) tvp->tv_sec = 0;
            if (tvp->tv_usec < 0) tvp->tv_usec = 0;
		// 没有获取到了最早到时的时间事件，时间事件链表为空
        } else {
			// 如果设置了不阻塞标识
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;// 将tvp的时间设置为0，就不会阻塞
                tvp = &tv;
            } else {
				// 阻塞到第一个时间事件的到来
                tvp = NULL; /* wait forever */
            }
        }
		// 等待所监听文件描述符上有事件发生
		// 如果tvp为NULL，则阻塞在此，否则等待tvp设置阻塞的时间，就会有时间事件到时
		// 返回了就绪文件事件的个数
        numevents = aeApiPoll(eventLoop, tvp);
		// 遍历就绪文件事件表
        for (j = 0; j < numevents; j++) {
			//获取就绪文件事件地址
            aeFileEvent *fe = &eventLoop->events[eventLoop->fired[j].fd];
			// 获取就绪文件事件的类型，文件描述符
			int mask = eventLoop->fired[j].mask;
            int fd = eventLoop->fired[j].fd;
            int rfired = 0;

			// 如果是文件可读事件发生
            if (fe->mask & mask & AE_READABLE) {
				// 设置读事件标识 且 调用读事件方法处理读事件
                rfired = 1;
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);
            }
			// 如果是文件可写事件发生
            if (fe->mask & mask & AE_WRITABLE) {
				// 读写事件的执行发法不同，则执行写事件，避免重复执行相同的方法
                if (!rfired || fe->wfileProc != fe->rfileProc)
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
            }
            processed++;//执行的事件次数加1
        }
    }
	// 执行时间事件
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    return processed; /* return the number of processed file/time events */
}

// 等待milliseconds，直到fd的mask事件发生
int aeWait(int fd, int mask, PORT_LONGLONG milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, (int)milliseconds))== 1) {                      WIN_PORT_FIX /* cast (int) */
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
	if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}
//事件轮询主函数
void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
	//一直循环监听处理事件
    while (!eventLoop->stop) {
        if (eventLoop->beforesleep != NULL)
            eventLoop->beforesleep(eventLoop);
		//处理到的时间事件和就绪事件
        aeProcessEvents(eventLoop, AE_ALL_EVENTS);
    }
}
// 返回正在使用的IO多路复用库的名字
char *aeGetApiName(void) {
    return aeApiName();
}
// 设置处理事件之前的函数
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}
