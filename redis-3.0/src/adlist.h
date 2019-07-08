
#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#endif

#ifndef __ADLIST_H__
#define __ADLIST_H__
/*redis���Լ�������ʵ��*/

/*
����ڵ�struct����,˫������
*/
typedef struct listNode {
    struct listNode *prev;   //ǰ��ָ��
    struct listNode *next;   //����ָ��
    void *value;             //�洢���ݵ�ָ��
} listNode;

/*
�������ʱʹ��
*/
typedef struct listIter {
    listNode *next;
    int direction;
} listIter;

/*
�����struct����
*/
typedef struct list {
    listNode *head;                     //ͷ���
    listNode *tail;                     //β�ڵ�
    void *(*dup)(void *ptr);            //�ڵ�ֵ���ƺ���,һ������ָ��
    void (*free)(void *ptr);            //�ڵ�ֵ�ͷź���,����ָ��
    int (*match)(void *ptr, void *key); //�ڵ�ֵ�ԱȺ���
    PORT_ULONG len;                     //�ڵ���Ŀ
} list;

/* ������ */
#define listLength(l) ((l)->len)
/*����ͷ���*/
#define listFirst(l) ((l)->head)
/*����Ϊ�ڵ�*/
#define listLast(l) ((l)->tail)
/*��ǰ�ڵ�n��ǰ���ڵ�*/
#define listPrevNode(n) ((n)->prev)
/*��ǰ�ڵ�n�ĺ�̽ڵ�*/
#define listNextNode(n) ((n)->next)
/*��ǰ�ڵ�n��ֵ*/
#define listNodeValue(n) ((n)->value)
/*��������ĸ��ƺ���ָ��*/
#define listSetDupMethod(l,m) ((l)->dup = (m))
/*����������ͷź���ָ��*/
#define listSetFreeMethod(l,m) ((l)->free = (m))
/*��������ıȽϺ���ָ��*/
#define listSetMatchMethod(l,m) ((l)->match = (m))
/*��ȡ����ĸ��ƺ���ָ��*/
#define listGetDupMethod(l) ((l)->dup)
/*��ȡ������ͷź���ָ��*/
#define listGetFree(l) ((l)->free)
/*��ȡ����ıȽϺ���ָ��*/
#define listGetMatchMethod(l) ((l)->match)

/* Prototypes */
list *listCreate(void);
void listRelease(list *list);
list *listAddNodeHead(list *list, void *value);
list *listAddNodeTail(list *list, void *value);
list *listInsertNode(list *list, listNode *old_node, void *value, int after);
void listDelNode(list *list, listNode *node);
listIter *listGetIterator(list *list, int direction);
listNode *listNext(listIter *iter);
void listReleaseIterator(listIter *iter);
list *listDup(list *orig);
listNode *listSearchKey(list *list, void *key);
listNode *listIndex(list *list, PORT_LONG index);
void listRewind(list *list, listIter *li);
void listRewindTail(list *list, listIter *li);
void listRotate(list *list);

/* Directions for iterators */
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __ADLIST_H__ */
