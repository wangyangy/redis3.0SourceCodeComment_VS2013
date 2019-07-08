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

/* �ļ��¼��ṹ */
typedef struct aeFileEvent {
    int mask; /* �ļ��¼����� */
    aeFileProc *rfileProc;  //�ɶ�������,����ָ��
    aeFileProc *wfileProc;  //��д������,����ָ��
    void *clientData;       //�ͻ��˴��������
} aeFileEvent;

/* ʱ���¼��ṹ */
typedef struct aeTimeEvent {
    PORT_LONGLONG id; /* ʱ���¼�id */
    PORT_LONG when_sec; /* �¼���������� */
    PORT_LONG when_ms; /* �¼�����ĺ����� */
    aeTimeProc *timeProc;   //�¼�������
    aeEventFinalizerProc *finalizerProc; //�¼��սắ��
    void *clientData;      //�ͻ��˴��������
    struct aeTimeEvent *next;    //ָ����һ���¼�
} aeTimeEvent;

/* A fired event */
typedef struct aeFiredEvent {
    int fd;       //�����¼��ļ�������
    int mask;     //�����¼�����
} aeFiredEvent;

/* �¼�״̬�ṹ */
typedef struct aeEventLoop {
    int maxfd;   /* ��ǰע��������ļ������� */
    int setsize; /* �ļ��������������ϵĴ�С */
    PORT_LONGLONG timeEventNextId;   //��һ���¼���id
    time_t lastTime;     /* ���һ��ִ���¼���ʱ�� */
    aeFileEvent *events; /* ע���¼� */
    aeFiredEvent *fired; /* �����¼� */
    aeTimeEvent *timeEventHead;   //�¼�ͷ���ָ��
    int stop;        //�¼�������
    void *apidata; /* ��·���ÿ��ʱ��״̬���� */
    aeBeforeSleepProc *beforesleep;  //ִ���¼�֮ǰ�Ĵ�����
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
