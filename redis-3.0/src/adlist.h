
#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#endif

#ifndef __ADLIST_H__
#define __ADLIST_H__
/*redis中自己的链表实现*/

/*
链表节点struct定义,双向链表
*/
typedef struct listNode {
    struct listNode *prev;   //前向指针
    struct listNode *next;   //后向指针
    void *value;             //存储数据的指针
} listNode;

/*
链表遍历时使用
*/
typedef struct listIter {
    listNode *next;
    int direction;
} listIter;

/*
链表的struct定义
*/
typedef struct list {
    listNode *head;                     //头结点
    listNode *tail;                     //尾节点
    void *(*dup)(void *ptr);            //节点值复制函数,一个函数指针
    void (*free)(void *ptr);            //节点值释放函数,函数指针
    int (*match)(void *ptr, void *key); //节点值对比函数
    PORT_ULONG len;                     //节点数目
} list;

/* 链表长度 */
#define listLength(l) ((l)->len)
/*链表头结点*/
#define listFirst(l) ((l)->head)
/*连边为节点*/
#define listLast(l) ((l)->tail)
/*当前节点n的前驱节点*/
#define listPrevNode(n) ((n)->prev)
/*当前节点n的后继节点*/
#define listNextNode(n) ((n)->next)
/*当前节点n的值*/
#define listNodeValue(n) ((n)->value)
/*设置链表的复制函数指针*/
#define listSetDupMethod(l,m) ((l)->dup = (m))
/*设置链表的释放函数指针*/
#define listSetFreeMethod(l,m) ((l)->free = (m))
/*设置链表的比较函数指针*/
#define listSetMatchMethod(l,m) ((l)->match = (m))
/*获取链表的复制函数指针*/
#define listGetDupMethod(l) ((l)->dup)
/*获取链表的释放函数指针*/
#define listGetFree(l) ((l)->free)
/*获取链表的比较函数指针*/
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
