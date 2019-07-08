#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#include "Win32_Interop/win32_types.h"
#endif

#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/* ����list������ָ�� */
list *listCreate(void)
{
    struct list *list;
	//����ռ�
    if ((list = zmalloc(sizeof(*list))) == NULL)
        return NULL;
	//��ʼ��ͷβ�ڵ�ΪNULL
    list->head = list->tail = NULL;
    list->len = 0;
    list->dup = NULL;
    list->free = NULL;
    list->match = NULL;
    return list;
}

/* �ͷ�����list */
void listRelease(list *list)
{
    PORT_ULONG len;
    listNode *current, *next;
	//��ȡͷ���
    current = list->head;
	//�ڵ����
    len = list->len;
    while(len--) {
        next = current->next;
		//�ͷŵ�ǰ�ڵ�洢������
        if (list->free) list->free(current->value);
		//�ͷŸýڵ�
        zfree(current);
        current = next;
    }
	//�ͷ�list�ṹ
    zfree(list);
}

/* ͷ�巨��ӽڵ�,����listָ�� */
list *listAddNodeHead(list *list, void *value)
{
    listNode *node;
	//����һ���ڵ�Ŀռ�
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
	//���ýڵ��ֵ
    node->value = value;
	//�������û�нڵ�
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    //�����Ѿ��нڵ�
    } else {
		//����ǰ��Ϊ��
        node->prev = NULL;
		//���Ϊͷ���
        node->next = list->head;
		//��ǰͷ����ǰ��Ϊnode
        list->head->prev = node;
		//����ͷ���
        list->head = node;
    }
	//���ȼ�һ
    list->len++;
	//��������
    return list;
}

/* β�巨��ӽڵ� */
list *listAddNodeTail(list *list, void *value)
{
    listNode *node;
	//����һ���ڵ�Ŀռ�
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
	//���ýڵ��ֵ
    node->value = value;
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
		//�����½ڵ��ǰ��Ϊβ�ڵ�
        node->prev = list->tail;
		//�����½ڵ�ĺ��ΪNULL
        node->next = NULL;
		//����taild�ĺ�̽ڵ�Ϊ�½ڵ�node
        list->tail->next = node;
		//���º�̽ڵ�
        list->tail = node;
    }
	//���ȼ�һ
    list->len++;
	//��������ָ��
    return list;
}

/*�������еĽڵ�old_node��ǰ����ߺ������ڵ�,���after!=0�Ӻ������,�����ǰ�����*/
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
    listNode *node;
	//����һ���ڵ�Ŀռ�
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
	//���ýڵ��ֵ
    node->value = value;
	//�������
    if (after) {
        node->prev = old_node;
        node->next = old_node->next;
		//�ж��Ƿ���Ҫ����β�ڵ�
        if (list->tail == old_node) {
            list->tail = node;
		}
	//ǰ�����
    } else {
        node->next = old_node;
        node->prev = old_node->prev;
		//�ж��Ƿ���Ҫ����ͷ���
        if (list->head == old_node) {
            list->head = node;
        }
    }
	//����ָ��ָ��
    if (node->prev != NULL) {
        node->prev->next = node;
    }
    if (node->next != NULL) {
        node->next->prev = node;
    }
	//���ȼ�һ
    list->len++;
    return list;
}

/* ɾ������ڵ� */
void listDelNode(list *list, listNode *node)
{
	//ɾ���ڵ��ǰ����Ϊ��
    if (node->prev)
        node->prev->next = node->next;
	//������ɾ��ͷ�ڵ�
    else
        list->head = node->next;
	//ɾ���ڵ�ĺ�̲�Ϊ��
    if (node->next)
        node->next->prev = node->prev;
	//������ɾ��β�ڵ�
    else
        list->tail = node->prev;
	//�ͷŽڵ�Ĵ洢����
    if (list->free) list->free(node->value);
	//�ͷŽڵ�
    zfree(node);
	//���ȼ�һ
    list->len--;
}

/* ������������� */
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

/* �ͷ���������� */
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

/* ������ָ������Ϊͷ�ڵ� */
void listRewind(list *list, listIter *li) {
    li->next = list->head;
    li->direction = AL_START_HEAD;
}

/* ������ָ������Ϊβ�ڵ� */
void listRewindTail(list *list, listIter *li) {
    li->next = list->tail;
    li->direction = AL_START_TAIL;
}

/* 
	���ص���������һ��Ԫ��
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

/* ��������(���) */
list *listDup(list *orig)
{
    list *copy;
    listIter *iter;
    listNode *node;

    if ((copy = listCreate()) == NULL)
        return NULL;
	//����ָ�븴ֵ
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

/* ����list��ƥ��key��Ԫ�� */
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

/* ����list��ָ��λ��index��Ԫ�� */
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

    /* ����tailΪ��ǰtail->ore */
    list->tail = tail->prev;
    list->tail->next = NULL;
    /* ��ԭtail��������ͷ */
    list->head->prev = tail;
    tail->prev = NULL;
    tail->next = list->head;
	//����head
    list->head = tail;
}
