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

//��������ʼ��һ���¼���ѯ��״̬�ṹ
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;
	//����ռ�
    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;
	//�����ļ��¼���;����¼���
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
    //�����¼���Ĵ�С
	eventLoop->setsize = setsize;
	//�������һ�ε�ִ��ʱ��
    eventLoop->lastTime = time(NULL);
	//��ʼ��ʱ���¼�
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
	//�¼�������
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;
    eventLoop->beforesleep = NULL;
	// ����һ��epollʵ�����浽eventLoop��events��
    if (aeApiCreate(eventLoop) == -1) goto err;
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
	//��ʼ���������¼�Ϊ��
    for (i = 0; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return eventLoop;  //�����¼���ѯ�ṹָ��

err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

/* ���ص�ǰʹ����Ĵ�С */
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
// �����¼���Ĵ�С�����setsizeС�������ļ��������򷵻�AE_ERR�����Ҳ������������򷵻�AE_OK���ҵ�����С
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
// ɾ���¼���ѯ��״̬�ṹ
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);
    zfree(eventLoop);
}
// �¼������عر�
void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}
// ����һ���ļ��¼�
// ���ü���fd���¼�����Ϊmask�����¼�����ʱ�������proc����
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData)
{
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }
	//fd��Ӧ���ļ��¼���ַ
    aeFileEvent *fe = &eventLoop->events[fd];
	//����fd��mask�����¼�
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;
	//�����¼�����
    fe->mask |= mask;
	//�����¼���Ӧ�Ĵ�����
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
	//���ÿͻ��˴��������
    fe->clientData = clientData;
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}
//��eventLoop���¼�����ɾ��һ��fd��mask�¼�
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
// ��ȡfd���¼�����
int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
	aeFileEvent *fe;
    if (fd >= eventLoop->setsize) return 0;
    fe = &eventLoop->events[fd];

    return fe->mask;
}
// ��ȡ��ǰʱ����ͺ��룬�����浽������
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
// ��milliseconds������ת��Ϊsec�����ms�����룬���浽������
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
// ����һ��ʱ���¼�
PORT_LONGLONG aeCreateTimeEvent(aeEventLoop *eventLoop, PORT_LONGLONG milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc)
{
	//���浱ǰʱ���¼�id��������һ��ʱ���¼���id
    PORT_LONGLONG id = eventLoop->timeEventNextId++;
    aeTimeEvent *te;
	//����ռ�
    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;// ����ʱ���¼���ID
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);
	// ����ʱ���¼�����ķ���
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
	// ����ͻ��˴��������
    te->clientData = clientData;
	// ����ǰ�¼�����Ϊͷ�ڵ�
    te->next = eventLoop->timeEventHead;
    eventLoop->timeEventHead = te;
    return id;
}
// ɾ��ָ��ID��ʱ���¼�������ɾ��
int aeDeleteTimeEvent(aeEventLoop *eventLoop, PORT_LONGLONG id)
{
    aeTimeEvent *te, *prev = NULL;
	// ʱ���¼�ͷ�ڵ��ַ
    te = eventLoop->timeEventHead;
	// �������нڵ�
    while(te) {
		// �ҵ���ɾ��ʱ���¼�
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
		te = te->next; // ָ����һ��ʱ���¼���ַ
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
// Ѱ�ҵ�һ���쵽ʱ��ʱ���¼�
// ������������õ�֪���ж���ʱ�����ѡ����¼�����Ϊ�����Ƴ��κ��¼���˯���С�
// ����¼�����û��ʱ�佫����NULL��
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop)
{
	//ʱ���¼�ͷ����ַ
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;
	//��������ʱ���¼�
    while(te) {
		// Ѱ�ҵ�һ���쵽ʱ��ʱ���¼������浽nearest��
        if (!nearest || te->when_sec < nearest->when_sec ||
                (te->when_sec == nearest->when_sec &&
                 te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

/* ִ��ʱ���¼� */
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
	// ���ﳢ�Է���ʱ����ҵ��������һ�δ����¼���ʱ��ȵ�ǰʱ�仹Ҫ��
	// �������һ�δ����¼���ʱ��
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while(te) {
            te->when_sec = 0;
            te = te->next;
        }
    }
	// ������һ��ʱ���¼������ʱ��Ϊ��ǰʱ��
    eventLoop->lastTime = now;

    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId-1;
	// ����ʱ���¼�����
    while(te) {
        PORT_LONG now_sec, now_ms;
        PORT_LONGLONG id;
		// ȷ�����ǲ������ڴ˵�������ʱ���¼�������ʱ���¼��� ��ע�⣬�˼��Ŀǰ��Ч������������ͷ�ڵ�����µļ�ʱ��������������Ǹ���ʵʩϸ�ڣ���ü����ܻ��ٴ����ã����ǽ��䱣����δ���ķ���
        if (te->id > maxId) {
            te = te->next;
            continue;
        }
        aeGetTime(&now_sec, &now_ms);
		// �ҵ��Ѿ���ʱ��ʱ���¼�
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

// ����ʱ��ʱ���¼��;������ļ��¼�
// ���flags = 0������ʲô��������ֱ�ӷ���
// ���flags������ AE_ALL_EVENTS ����ִ���������͵��¼�
// ���flags������ AE_FILE_EVENTS ����ִ���ļ��¼�
// ���flags������ AE_TIME_EVENTS ����ִ��ʱ���¼�
// ���flags������ AE_DONT_WAIT ����ô�����������¼���ֱ�ӷ��أ��������ȴ�
// ��������ִ�е��¼�����
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int processed = 0, numevents;

#ifdef _WIN32	
    if (ServiceStopIssued() == TRUE) {
        aeStop(eventLoop);
    }
#endif

    /* ���ʲô�¼���û��������ֱ�ӷ��� */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
	// ��ǰ��û��Ҫ������ļ��¼�������������ʱ���¼�����û�����ò�������ʶ
    if (eventLoop->maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        aeTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;
		// ���������ʱ���¼���û�����ò�������ʶ
        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            shortest = aeSearchNearestTimer(eventLoop);  //��ȡ�������ʱ���¼�
		//��ȡ�������絽ʱ��ʱ���¼�
		if (shortest) {
            PORT_LONG now_sec, now_ms;

            //��ȡ��ǰʱ��
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
		// û�л�ȡ�������絽ʱ��ʱ���¼���ʱ���¼�����Ϊ��
        } else {
			// ��������˲�������ʶ
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;// ��tvp��ʱ������Ϊ0���Ͳ�������
                tvp = &tv;
            } else {
				// ��������һ��ʱ���¼��ĵ���
                tvp = NULL; /* wait forever */
            }
        }
		// �ȴ��������ļ������������¼�����
		// ���tvpΪNULL���������ڴˣ�����ȴ�tvp����������ʱ�䣬�ͻ���ʱ���¼���ʱ
		// �����˾����ļ��¼��ĸ���
        numevents = aeApiPoll(eventLoop, tvp);
		// ���������ļ��¼���
        for (j = 0; j < numevents; j++) {
			//��ȡ�����ļ��¼���ַ
            aeFileEvent *fe = &eventLoop->events[eventLoop->fired[j].fd];
			// ��ȡ�����ļ��¼������ͣ��ļ�������
			int mask = eventLoop->fired[j].mask;
            int fd = eventLoop->fired[j].fd;
            int rfired = 0;

			// ������ļ��ɶ��¼�����
            if (fe->mask & mask & AE_READABLE) {
				// ���ö��¼���ʶ �� ���ö��¼�����������¼�
                rfired = 1;
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);
            }
			// ������ļ���д�¼�����
            if (fe->mask & mask & AE_WRITABLE) {
				// ��д�¼���ִ�з�����ͬ����ִ��д�¼��������ظ�ִ����ͬ�ķ���
                if (!rfired || fe->wfileProc != fe->rfileProc)
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
            }
            processed++;//ִ�е��¼�������1
        }
    }
	// ִ��ʱ���¼�
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    return processed; /* return the number of processed file/time events */
}

// �ȴ�milliseconds��ֱ��fd��mask�¼�����
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
//�¼���ѯ������
void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
	//һֱѭ�����������¼�
    while (!eventLoop->stop) {
        if (eventLoop->beforesleep != NULL)
            eventLoop->beforesleep(eventLoop);
		//������ʱ���¼��;����¼�
        aeProcessEvents(eventLoop, AE_ALL_EVENTS);
    }
}
// ��������ʹ�õ�IO��·���ÿ������
char *aeGetApiName(void) {
    return aeApiName();
}
// ���ô����¼�֮ǰ�ĺ���
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}
