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

/* hash表中键值对(key-value)的struct定义 */
typedef struct dictEntry {
    void *key;           //键
	//值,可以有不同的类型
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    struct dictEntry *next; //下一个元素,解决hash冲突
} dictEntry;

typedef struct dictType {
    unsigned int (*hashFunction)(const void *key);      //hash函数指针
    void *(*keyDup)(void *privdata, const void *key);   //复制key的函数指针
    void *(*valDup)(void *privdata, const void *obj);   //复制值的函数指针
    int (*keyCompare)(void *privdata, const void *key1, const void *key2);   //比较键的函数指针
    void (*keyDestructor)(void *privdata, void *key);   //销毁键的函数指针
    void (*valDestructor)(void *privdata, void *obj);   //销毁值的函数指针
} dictType;

/* hash表 */
typedef struct dictht {
    dictEntry **table;   //桶结构,存放一个数组的首地址,这个数组中存放dictEntry
    PORT_ULONG size;     //hash表的大小
    PORT_ULONG sizemask; //用于将hash值映射到table位置的索引,大小为size-1
    PORT_ULONG used;     //记录hash表已有节点
} dictht;

typedef struct dict {
    dictType *type;       //存储着一些函数指针
    void *privdata;       //私有数据域
    dictht ht[2];         //两张hash表
    PORT_LONG rehashidx; /* rehash的标记,rehash==-1表示没有进行rehash,记录了rehash的进度 */
    int iterators; /* 正在迭代的迭代器数量 */
} dict;

/* 迭代器 */
typedef struct dictIterator {
    dict *d;
    PORT_LONG index;
    int table, safe;
    dictEntry *entry, *nextEntry;
    /* unsafe iterator fingerprint for misuse detection. */
    PORT_LONGLONG fingerprint;
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);

/* 每个hash表的初始大小 4 */
#define DICT_HT_INITIAL_SIZE     4

/* ------------------------------- Macros ------------------------------------*/
/*val销毁函数存在,则调用函数销毁val*/
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

/* 值复制函数存在,则调用函数复制值并把复制的值赋给entry,否则直接赋值 */
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

/*key销毁函数存在,则掉用函数销毁key*/
#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)
/*key复制函数存在,则调用函数复制key并将值赋给entry*/
#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        entry->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        entry->key = (_key_); \
} while(0)
/*key比较函数存在,则调用函数比较key*/
#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

#define dictHashKey(d, key) (d)->type->hashFunction(key)
#define dictGetKey(he) ((he)->key)
/*获取元素的值*/
#define dictGetVal(he) ((he)->v.val)
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
#define dictGetDoubleVal(he) ((he)->v.d)
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)
/*两个hash表总的元素数量*/
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
/*检测是否正在进行rehash操作*/
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
