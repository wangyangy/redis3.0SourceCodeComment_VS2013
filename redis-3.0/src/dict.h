/* 
	Hash Tables Implementation.
 */

#include <stdint.h>
#ifdef _WIN32
#include "Win32_Interop/win32_types.h"
#endif

#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0
#define DICT_ERR 1

/* Unused arguments generate annoying warnings... */
#define DICT_NOTUSED(V) ((void) V)

/* hash���м�ֵ��(key-value)��struct���� */
typedef struct dictEntry {
    void *key;           //��
	//ֵ,�����в�ͬ������
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    struct dictEntry *next; //��һ��Ԫ��,���hash��ͻ
} dictEntry;

typedef struct dictType {
    unsigned int (*hashFunction)(const void *key);      //hash����ָ��
    void *(*keyDup)(void *privdata, const void *key);   //����key�ĺ���ָ��
    void *(*valDup)(void *privdata, const void *obj);   //����ֵ�ĺ���ָ��
    int (*keyCompare)(void *privdata, const void *key1, const void *key2);   //�Ƚϼ��ĺ���ָ��
    void (*keyDestructor)(void *privdata, void *key);   //���ټ��ĺ���ָ��
    void (*valDestructor)(void *privdata, void *obj);   //����ֵ�ĺ���ָ��
} dictType;

/* hash�� */
typedef struct dictht {
    dictEntry **table;   //Ͱ�ṹ,���һ��������׵�ַ,��������д��dictEntry
    PORT_ULONG size;     //hash��Ĵ�С
    PORT_ULONG sizemask; //���ڽ�hashֵӳ�䵽tableλ�õ�����,��СΪsize-1
    PORT_ULONG used;     //��¼hash�����нڵ�
} dictht;

typedef struct dict {
    dictType *type;       //�洢��һЩ����ָ��
    void *privdata;       //˽��������
    dictht ht[2];         //����hash��
    PORT_LONG rehashidx; /* rehash�ı��,rehash==-1��ʾû�н���rehash,��¼��rehash�Ľ��� */
    int iterators; /* ���ڵ����ĵ��������� */
} dict;

/* ������ */
typedef struct dictIterator {
    dict *d;
    PORT_LONG index;
    int table, safe;
    dictEntry *entry, *nextEntry;
    /* unsafe iterator fingerprint for misuse detection. */
    PORT_LONGLONG fingerprint;
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);

/* ÿ��hash��ĳ�ʼ��С 4 */
#define DICT_HT_INITIAL_SIZE     4

/* ------------------------------- Macros ------------------------------------*/
/*val���ٺ�������,����ú�������val*/
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

/* ֵ���ƺ�������,����ú�������ֵ���Ѹ��Ƶ�ֵ����entry,����ֱ�Ӹ�ֵ */
#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        entry->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        entry->v.val = (_val_); \
} while(0)


#define dictSetSignedIntegerVal(entry, _val_) \
    do { entry->v.s64 = _val_; } while(0)

#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { entry->v.u64 = _val_; } while(0)

#define dictSetDoubleVal(entry, _val_) \
    do { entry->v.d = _val_; } while(0)

/*key���ٺ�������,����ú�������key*/
#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)
/*key���ƺ�������,����ú�������key����ֵ����entry*/
#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        entry->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        entry->key = (_key_); \
} while(0)
/*key�ȽϺ�������,����ú����Ƚ�key*/
#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

#define dictHashKey(d, key) (d)->type->hashFunction(key)
#define dictGetKey(he) ((he)->key)
/*��ȡԪ�ص�ֵ*/
#define dictGetVal(he) ((he)->v.val)
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
#define dictGetDoubleVal(he) ((he)->v.d)
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)
/*����hash���ܵ�Ԫ������*/
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
/*����Ƿ����ڽ���rehash����*/
#define dictIsRehashing(d) ((d)->rehashidx != -1)

/* API */
dict *dictCreate(dictType *type, void *privDataPtr);
int dictExpand(dict *d, PORT_ULONG size);
int dictAdd(dict *d, void *key, void *val);
dictEntry *dictAddRaw(dict *d, void *key);
int dictReplace(dict *d, void *key, void *val);
dictEntry *dictReplaceRaw(dict *d, void *key);
int dictDelete(dict *d, const void *key);
int dictDeleteNoFree(dict *d, const void *key);
void dictRelease(dict *d);
dictEntry * dictFind(dict *d, const void *key);
void *dictFetchValue(dict *d, const void *key);
int dictResize(dict *d);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);
void dictPrintStats(dict *d);
unsigned int dictGenHashFunction(const void *key, int len);
unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len);
void dictEmpty(dict *d, void(callback)(void*));
void dictEnableResize(void);
void dictDisableResize(void);
int dictRehash(dict *d, int n);
int dictRehashMilliseconds(dict *d, int ms);
void dictSetHashFunctionSeed(unsigned int initval);
unsigned int dictGetHashFunctionSeed(void);
PORT_ULONG dictScan(dict *d, PORT_ULONG v, dictScanFunction *fn, void *privdata);

/* Hash table types */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#endif /* __DICT_H */
