#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#include "Win32_Interop/win32_types.h"
#endif

#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/* 创建list并返回指针 */
list *listCreate(void)
{
    struct list *list;
	//申请空间
    if ((list = zmalloc(sizeof(*list))) == NULL)
        return NULL;
	//初始化头尾节点为NULL
    list->head = list->tail = NULL;
    list->len = 0;
    list->dup = NULL;
    list->free = NULL;
    list->match = NULL;
    return list;
}

/* 释放整个list */
void listRelease(list *list)
{
    PORT_ULONG len;
    listNode *current, *next;
	//获取头结点
    current = list->head;
	//节点个数
    len = list->len;
    while(len--) {
        next = current->next;
		//释放当前节点存储的内容
        if (list->free) list->free(current->value);
		//释放该节点
        zfree(current);
        current = next;
    }
	//释放list结构
    zfree(list);
}

/* 头插法添加节点,返回list指针 */
list *listAddNodeHead(list *list, void *value)
{
    listNode *node;
	//申请一个节点的空间
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
	//设置节点的值
    node->value = value;
	//如果链表没有节点
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    //链表已经有节点
    } else {
		//设置前驱为空
        node->prev = NULL;
		//后继为头结点
        node->next = list->head;
		//当前头结点的前驱为node
        list->head->prev = node;
		//更新头结点
        list->head = node;
    }
	//长度加一
    list->len++;
	//返回链表
    return list;
}

/* 尾插法添加节点 */
list *listAddNodeTail(list *list, void *value)
{
    listNode *node;
	//申请一个节点的空间
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
	//设置节点的值
    node->value = value;
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
		//设置新节点的前驱为尾节点
        node->prev = list->tail;
		//设置新节点的后继为NULL
        node->next = NULL;
		//设置taild的后继节点为新节点node
        list->tail->next = node;
		//更新后继节点
        list->tail = node;
    }
	//长度加一
    list->len++;
	//返回链表指针
    return list;
}

/*在链表中的节点old_node的前面或者后面插入节点,如果after!=0从后面插入,否则从前面插入*/
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
    listNode *node;
	//申请一个节点的空间
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
	//设置节点的值
    node->value = value;
	//后面插入
    if (after) {
        node->prev = old_node;
        node->next = old_node->next;
		//判断是否需要更新尾节点
        if (list->tail == old_node) {
            list->tail = node;
		}
	//前面插入
    } else {
        node->next = old_node;
        node->prev = old_node->prev;
		//判断是否需要更新头结点
        if (list->head == old_node) {
            list->head = node;
        }
    }
	//更新指针指向
    if (node->prev != NULL) {
        node->prev->next = node;
    }
    if (node->next != NULL) {
        node->next->prev = node;
    }
	//长度加一
    list->len++;
    return list;
}

/* 删除链表节点 */
void listDelNode(list *list, listNode *node)
{
	//删除节点的前驱不为空
    if (node->prev)
        node->prev->next = node->next;
	//否则是删除头节点
    else
        list->head = node->next;
	//删除节点的后继不为空
    if (node->next)
        node->next->prev = node->prev;
	//否则是删除尾节点
    else
        list->tail = node->prev;
	//释放节点的存储内容
    if (list->free) list->free(node->value);
	//释放节点
    zfree(node);
	//长度减一
    list->len--;
}

/* 返回链表迭代器 */
listIter *listGetIterator(list *list, int direction)
{
    listIter *iter;

    if ((iter = zmalloc(sizeof(*iter))) == NULL) return NULL;
    if (direction == AL_START_HEAD)
        iter->next = list->head;
    else
        iter->next = list->tail;
    iter->direction = direction;
    return iter;
}

/* 释放链表迭代器 */
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

/* 迭代器指针重置为头节点 */
void listRewind(list *list, listIter *li) {
    li->next = list->head;
    li->direction = AL_START_HEAD;
}

/* 迭代器指针重置为尾节点 */
void listRewindTail(list *list, listIter *li) {
    li->next = list->tail;
    li->direction = AL_START_TAIL;
}

/* 
	返回迭代器的下一个元素
 * */
listNode *listNext(listIter *iter)
{
    listNode *current = iter->next;

    if (current != NULL) {
        if (iter->direction == AL_START_HEAD)
            iter->next = current->next;
        else
            iter->next = current->prev;
    }
    return current;
}

/* 复制链表(深拷贝) */
list *listDup(list *orig)
{
    list *copy;
    listIter *iter;
    listNode *node;

    if ((copy = listCreate()) == NULL)
        return NULL;
	//函数指针复值
    copy->dup = orig->dup;
    copy->free = orig->free;
    copy->match = orig->match;
    iter = listGetIterator(orig, AL_START_HEAD);
    while((node = listNext(iter)) != NULL) {
        void *value;

        if (copy->dup) {
            value = copy->dup(node->value);
            if (value == NULL) {
                listRelease(copy);
                listReleaseIterator(iter);
                return NULL;
            }
        } else
            value = node->value;
        if (listAddNodeTail(copy, value) == NULL) {
            listRelease(copy);
            listReleaseIterator(iter);
            return NULL;
        }
    }
    listReleaseIterator(iter);
    return copy;
}

/* 搜索list中匹配key的元素 */
listNode *listSearchKey(list *list, void *key)
{
    listIter *iter;
    listNode *node;

    iter = listGetIterator(list, AL_START_HEAD);
    while((node = listNext(iter)) != NULL) {
        if (list->match) {
            if (list->match(node->value, key)) {
                listReleaseIterator(iter);
                return node;
            }
        } else {
            if (key == node->value) {
                listReleaseIterator(iter);
                return node;
            }
        }
    }
    listReleaseIterator(iter);
    return NULL;
}

/* 返回list中指定位置index的元素 */
listNode *listIndex(list *list, PORT_LONG index) {
    listNode *n;

    if (index < 0) {
        index = (-index)-1;
        n = list->tail;
        while(index-- && n) n = n->prev;
    } else {
        n = list->head;
        while(index-- && n) n = n->next;
    }
    return n;
}

/* Rotate the list removing the tail node and inserting it to the head. */
void listRotate(list *list) {
    listNode *tail = list->tail;

    if (listLength(list) <= 1) return;

    /* 更新tail为当前tail->ore */
    list->tail = tail->prev;
    list->tail->next = NULL;
    /* 将原tail插入链表头 */
    list->head->prev = tail;
    tail->prev = NULL;
    tail->next = list->head;
	//更新head
    list->head = tail;
}
