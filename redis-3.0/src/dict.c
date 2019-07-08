/* 
	Hash Tables Implementation.
 */
#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#include "Win32_Interop/Win32_Time.h"
#include "Win32_Interop/win32fixes.h"
extern BOOL g_IsForkedProcess;
#endif

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#ifndef _WIN32
#include <sys/time.h>
#endif
#include <ctype.h>

#include "dict.h"
#include "zmalloc.h"
#include "redisassert.h"

/* Using dictEnableResize() / dictDisableResize() we make possible to
 * enable/disable resizing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio. */
static int dict_can_resize = 1;
static unsigned int dict_force_resize_ratio = 5;

/* -------------------------- private prototypes ---------------------------- */

static int _dictExpandIfNeeded(dict *ht);
static PORT_ULONG _dictNextPower(PORT_ULONG size);
static int _dictKeyIndex(dict *ht, const void *key);
static int _dictInit(dict *ht, dictType *type, void *privDataPtr);

/* -------------------------- hash functions -------------------------------- */

/* int��hash���� */
unsigned int dictIntHashFunction(unsigned int key)
{
    key += ~(key << 15);
    key ^=  (key >> 10);
    key +=  (key << 3);
    key ^=  (key >> 6);
    key += ~(key << 11);
    key ^=  (key >> 16);
    return key;
}

static uint32_t dict_hash_function_seed = 5381;

void dictSetHashFunctionSeed(uint32_t seed) {
    dict_hash_function_seed = seed;
}

uint32_t dictGetHashFunctionSeed(void) {
    return dict_hash_function_seed;
}

/* MurmurHash2hash�㷨 */
unsigned int dictGenHashFunction(const void *key, int len) {
    /* 'm' and 'r' are mixing constants generated offline.
     They're not really 'magic', they just happen to work well.  */
    uint32_t seed = dict_hash_function_seed;
    const uint32_t m = 0x5bd1e995;
    const int r = 24;

    /* Initialize the hash to a 'random' value */
    uint32_t h = seed ^ len;

    /* Mix 4 bytes at a time into the hash */
    const unsigned char *data = (const unsigned char *)key;

    while(len >= 4) {
        uint32_t k = *(uint32_t*)data;

        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        data += 4;
        len -= 4;
    }

    /* Handle the last few bytes of the input array  */
    switch(len) {
    case 3: h ^= data[2] << 16;
    case 2: h ^= data[1] << 8;
    case 1: h ^= data[0]; h *= m;
    };

    /* Do a few final mixes of the hash to ensure the last few
     * bytes are well-incorporated. */
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return (unsigned int)h;
}

/* djbhash�㷨 */
unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len) {
    unsigned int hash = (unsigned int)dict_hash_function_seed;

    while (len--)
        hash = ((hash << 5) + hash) + (tolower(*buf++)); /* hash * 33 + c */
    return hash;
}

/* ----------------------------- API implementation ------------------------- */

/* ����hash�� */
static void _dictReset(dictht *ht)
{
    ht->table = NULL;
    ht->size = 0;
    ht->sizemask = 0;
    ht->used = 0;
}

/* ����һ���ֵ� */
dict *dictCreate(dictType *type,
        void *privDataPtr)
{
	//����ռ�,sizeof(*d)Ϊ88���ֽ�
    dict *d = zmalloc(sizeof(*d));
	//��ʼ��hash��
    _dictInit(d,type,privDataPtr);
    return d;
}

/* ��ʼ���ֵ� */
int _dictInit(dict *d, dictType *type,
        void *privDataPtr)
{
	//����hash������
    _dictReset(&d->ht[0]);
    _dictReset(&d->ht[1]);
    d->type = type;
    d->privdata = privDataPtr;
    d->rehashidx = -1;
    d->iterators = 0;
    return DICT_OK;
}

/* ��С�ֵ�d */
int dictResize(dict *d)
{
    int minimal;
	//���dict_can_resize==0�������ڽ���rehash,���ش����־
    if (!dict_can_resize || dictIsRehashing(d)) return DICT_ERR;
	//��ȡ���нڵ���Ŀ
    minimal = (int)d->ht[0].used;                                               WIN_PORT_FIX /* cast (int) */
	//minimal����С�ڳ�ʼ����С
    if (minimal < DICT_HT_INITIAL_SIZE)
        minimal = DICT_HT_INITIAL_SIZE;
	//ͨ��minimal�����ֵ�d��С
    return dictExpand(d, minimal);
}

/* �����򴴽�hash��,����size��ֵ����һ��realSize>=size,��realSize��2���ݴη�*/
int dictExpand(dict *d,PORT_ULONG size)
{
    dictht n; /* �µ�hash�� */
	//���һ����ӽ�size��2^n,��ֵ��realsize
    PORT_ULONG realsize = _dictNextPower(size);

    /* ����rehash��size������,���ش����־ */
    if (dictIsRehashing(d) || d->ht[0].used > size)
        return DICT_ERR;

    /* ���realsize��ԭ����sizeһ��,���ش����־ */
    if (realsize == d->ht[0].size) return DICT_ERR;

    /* ��ʼ���µ�hash���Ա */
    n.size = realsize;
    n.sizemask = realsize-1;
    n.table = zcalloc(realsize*sizeof(dictEntry*));
    n.used = (size_t) 0;                                                        WIN_PORT_FIX /* cast (size_t) */

    /* ���ht[0]hash��Ϊkong,���µ�hash��ֵ���� */
    if (d->ht[0].table == NULL) {
        d->ht[0] = n;
        return DICT_OK;
    }

    /* ht[1]��Ϊ��rehash�����ڵ� */
    d->ht[1] = n;
    d->rehashidx = 0;
    return DICT_OK;
}

/* n��rehash����.rehash����һ���Լ�������ɶ��Ƿֶ�ν��������,��Ҫ��hash���������ǧ������ */
int dictRehash(dict *d, int n) {

    WIN32_ONLY(if (g_IsForkedProcess) return 0;)

    int empty_visits = n*10; /* Max number of empty buckets to visit. */
    //������ڽ���rehash����,����0
	if (!dictIsRehashing(d)) return 0;
	//ht[0]�ϻ���û�ƶ��Ľڵ�,��û��ִ����n��
    while(n-- && d->ht[0].used != 0) {
        dictEntry *de, *nextde;
		//ȷ��rehashindexû��Խ��
        assert(d->ht[0].size > (PORT_ULONG)d->rehashidx);
		//Ͱ�п���û������,����ҲҪ��һ��
        while(d->ht[0].table[d->rehashidx] == NULL) {
            d->rehashidx++;
            if (--empty_visits == 0) return 1;
        }
        de = d->ht[0].table[d->rehashidx];
        /* Move all the keys in this bucket from the old to the new hash HT */
		/*��deͰλ�õ�����entry�ƶ����µ�λ��*/
        while(de) {
            unsigned int h;
			//������ǰ������,��Ϊhash��ͻ�ǲ�������ַ�������,���Ի�������
            nextde = de->next;
            /* ����hash��������µ�����λ��,������൱��ȡ�� */
            h = dictHashKey(d, de->key) & d->ht[1].sizemask;
			//��¼һ��,���û������
            de->next = d->ht[1].table[h];
			//���ô���Ӧ��λ��
            d->ht[1].table[h] = de;
            d->ht[0].used--;
            d->ht[1].used++;
            de = nextde;
        }
		//��ǰλ����Ϊ��
        d->ht[0].table[d->rehashidx] = NULL;
		//������һ
        d->rehashidx++;
    }

    /* ht[0]������Ѿ�û�нڵ��˾��ͷſռ� */
    if (d->ht[0].used == 0) {
        zfree(d->ht[0].table);
		//��rehash��ht[1]����ht[0]
        d->ht[0] = d->ht[1];
		//����ht[1]
        _dictReset(&d->ht[1]);
        d->rehashidx = -1;
        return 0;
    }

    /* More to rehash... */
    return 1;
}

PORT_LONGLONG timeInMilliseconds(void) {
#ifdef _WIN32
    return GetHighResRelativeTime(1000);
#else
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((PORT_LONGLONG)tv.tv_sec)*1000)+(tv.tv_usec/1000);
#endif
}

/* ִ��һ��ʱ���rehash����,��ʱ�䳬��msʱ�ͽ���rehash���� */
int dictRehashMilliseconds(dict *d, int ms) {
    PORT_LONGLONG start = timeInMilliseconds();
    int rehashes = 0;

    while(dictRehash(d,100)) {
        rehashes += 100;
        if (timeInMilliseconds()-start > ms) break;
    }
    return rehashes;
}

/* ����rehash���� */
static void _dictRehashStep(dict *d) {
	//��������������Ϊ0���ܽ��е���rehash����
    if (d->iterators == 0) dictRehash(d,1);
}

/* ���һ��Ԫ�ص�hash���� */
int dictAdd(dict *d, void *key, void *val)
{
    dictEntry *entry = dictAddRaw(d,key);

    if (!entry) return DICT_ERR;
    dictSetVal(d, entry, val);
    return DICT_OK;
}

/* 
	����һ��dictEntry
 */
dictEntry *dictAddRaw(dict *d, void *key)
{
    int index;
    dictEntry *entry;
    dictht *ht;
	//������ڽ���rehash����,��ִ�е���rehash
    if (dictIsRehashing(d)) _dictRehashStep(d);

    /* ���key��Ӧ��Ԫ���Ƿ��Ѿ�����,�Ѿ���������_dictKeyIndex����-1,��ֱ�ӷ���null */
    if ((index = _dictKeyIndex(d, key)) == -1)
        return NULL;

    /* Allocate the memory and store the new entry */
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
	//����ռ�
    entry = zmalloc(sizeof(*entry));
	//�ҵ�Ͱλ��,ͷ�巨����Ԫ��
    entry->next = ht->table[index];
	//����ͷԪ��
    ht->table[index] = entry;
    ht->used++;

    /* ����key�ֶ� */
    dictSetKey(d, entry, key);
    return entry;
}

/* ���Ƿ�ʽ����Ԫ��,�����key��ӦԪ�ش����򸲸�,����ֱ�Ӳ��� */
int dictReplace(dict *d, void *key, void *val)
{
    dictEntry *entry, auxentry;

    /* ���Ԫ��key���ֵ���,�������ͻ����ӳɹ�,����1,�������ʧ��,����ִ�� */
    if (dictAdd(d, key, val) == DICT_OK)
        return 1;
    /* ���ֵ��в���dictEntry */
    entry = dictFind(d, key);
    auxentry = *entry;
    dictSetVal(d, entry, val);
    dictFreeVal(d, &auxentry);
    return 0;
}

/* ����entry */
dictEntry *dictReplaceRaw(dict *d, void *key) {
	//����entry
    dictEntry *entry = dictFind(d,key);
	//��������򷵻�,�����������֮���ڷ���
    return entry ? entry : dictAddRaw(d,key);
}

/* ���Ҳ�ɾ��Ԫ��,����nofree��ֵ�ж��Ƿ��ͷż�ֵ */
static int dictGenericDelete(dict *d, const void *key, int nofree)
{
    unsigned int h, idx;
    dictEntry *he, *prevHe;
    int table;

    if (d->ht[0].size == 0) return DICT_ERR; /* d->ht[0].table is NULL */
    if (dictIsRehashing(d)) _dictRehashStep(d);
	//����hashֵ
    h = dictHashKey(d, key);

    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        prevHe = NULL;
		//����Ͱλ�õ�����
        while(he) {
			//����ҵ�Ԫ����
            if (dictCompareKeys(d, key, he->key)) {
                /* ����ͷ���,����ָ��ָ��,ɾ����ǰԪ�� */
                if (prevHe)
                    prevHe->next = he->next;
				//˵������ͷ���,ֱ��ɾ��ͷ���
                else
                    d->ht[table].table[idx] = he->next;
                if (!nofree) {
					//�ͷż�ֵ
                    dictFreeKey(d, he);
                    dictFreeVal(d, he);
                }
				//�ͷŵ�ǰ�ڵ�
                zfree(he);
                d->ht[table].used--;
                return DICT_OK;
            }
            prevHe = he;
            he = he->next;
        }
        if (!dictIsRehashing(d)) break;
    }
    return DICT_ERR; /* not found */
}
/* ɾ��key���ͷż�ֵ�� */
int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0);
}
/* ɾ��key���ͷż�ֵ�� */
int dictDeleteNoFree(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,1);
}

/* ���������ֵ�,����hash�����ͷ�,����dict�ṹ���ò��ͷ� */
int _dictClear(dict *d, dictht *ht, void(callback)(void *)) {
    PORT_ULONG i;

    /* Free all the elements */
    for (i = 0; i < ht->size && ht->used > 0; i++) {
        dictEntry *he, *nextHe;

        if (callback && (i & 65535) == 0) callback(d->privdata);

        if ((he = ht->table[i]) == NULL) continue;
        while(he) {
            nextHe = he->next;
            dictFreeKey(d, he);
            dictFreeVal(d, he);
            zfree(he);
            ht->used--;
            he = nextHe;
        }
    }
    /* Free the table and the allocated cache structure */
    zfree(ht->table);
    /* Re-initialize the table */
    _dictReset(ht);
    return DICT_OK; /* never fails */
}

/* ���������ֵ�,���пռ䶼�ͷ�,����dict�ṹ */
void dictRelease(dict *d)
{
    _dictClear(d,&d->ht[0],NULL);
    _dictClear(d,&d->ht[1],NULL);
    zfree(d);
}

/*�����ֵ����Ƿ����key,������ڷ�����Ӧ��dictEntry,���򷵻�NULL*/
dictEntry *dictFind(dict *d, const void *key)
{
    dictEntry *he;
    unsigned int h, idx, table;

    if (d->ht[0].size == 0) return NULL; /* We don't have a table at all */
    if (dictIsRehashing(d)) _dictRehashStep(d);
	//����hashֵ
    h = dictHashKey(d, key);
    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        while(he) {
            if (dictCompareKeys(d, key, he->key))
                return he;
            he = he->next;
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

/*��ȡԪ�ص�ֵ*/
void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;
	//���ҽڵ�
    he = dictFind(d,key);
    return he ? dictGetVal(he) : NULL;
}

/* ָ�ƺ���,�����жϵ������ڵ����ڼ��Ƿ��б�Ķ��ֵ�����˽�ֹ�Ĳ��� */
PORT_LONGLONG dictFingerprint(dict *d) {
    PORT_LONGLONG integers[6], hash = 0;
    int j;

    integers[0] = (PORT_LONG) d->ht[0].table;
    integers[1] = d->ht[0].size;
    integers[2] = d->ht[0].used;
    integers[3] = (PORT_LONG) d->ht[1].table;
    integers[4] = d->ht[1].size;
    integers[5] = d->ht[1].used;

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

/*��ȡ�ֵ������*/
dictIterator *dictGetIterator(dict *d)
{
    dictIterator *iter = zmalloc(sizeof(*iter));

    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = 0;
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}
/*�����ֵ����͵ĵ�����*/
dictIterator *dictGetSafeIterator(dict *d) {
    dictIterator *i = dictGetIterator(d);

    i->safe = 1;
    return i;
}

/*��ȡ�ֵ����һ��Ԫ��*/
dictEntry *dictNext(dictIterator *iter)
{
    while (1) {
        if (iter->entry == NULL) {
            dictht *ht = &iter->d->ht[iter->table];
            if (iter->index == -1 && iter->table == 0) {
                if (iter->safe)
                    iter->d->iterators++;
                else
                    iter->fingerprint = dictFingerprint(iter->d);
            }
            iter->index++;
            if (iter->index >= (PORT_LONG) ht->size) {
                if (dictIsRehashing(iter->d) && iter->table == 0) {
                    iter->table++;
                    iter->index = 0;
                    ht = &iter->d->ht[1];
                } else {
                    break;
                }
            }
            iter->entry = ht->table[iter->index];
        } else {
            iter->entry = iter->nextEntry;
        }
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }
    return NULL;
}
/*�ͷŵ�����*/
void dictReleaseIterator(dictIterator *iter)
{
    if (!(iter->index == -1 && iter->table == 0)) {
        if (iter->safe)
            iter->d->iterators--;
        else
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
    zfree(iter);
}

/*��ȡһ�������key */
dictEntry *dictGetRandomKey(dict *d)
{
    dictEntry *he, *orighe;
    unsigned int h;
    int listlen, listele;

    if (dictSize(d) == 0) return NULL;
    if (dictIsRehashing(d)) _dictRehashStep(d);
    if (dictIsRehashing(d)) {
        do {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            h = (unsigned int) (d->rehashidx + (random() % (d->ht[0].size +     WIN_PORT_FIX /* cast (unsigned int) */
                                            d->ht[1].size -
                                            d->rehashidx)));
            he = (h >= d->ht[0].size) ? d->ht[1].table[h - d->ht[0].size] :
                                      d->ht[0].table[h];
        } while(he == NULL);
    } else {
        do {
            h = random() & d->ht[0].sizemask;
            he = d->ht[0].table[h];
        } while(he == NULL);
    }

    /* Now we found a non empty bucket, but it is a linked
     * list and we need to get a random element from the list.
     * The only sane way to do so is counting the elements and
     * select a random index. */
    listlen = 0;
    orighe = he;
    while(he) {
        he = he->next;
        listlen++;
    }
    listele = random() % listlen;
    he = orighe;
    while(listele--) he = he->next;
    return he;
}

/* ����һЩ�����key
 * This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 */
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned int j; /* internal hash table id, 0 or 1. */
    unsigned int tables; /* 1 or 2 tables? */
    unsigned int stored = 0, maxsizemask;
    unsigned int maxsteps;

    if (dictSize(d) < count) count = (unsigned int)dictSize(d);                 WIN_PORT_FIX /* cast (unsigned int) */
    maxsteps = count*10;

    /* Try to do a rehashing work proportional to 'count'. */
    for (j = 0; j < count; j++) {
        if (dictIsRehashing(d))
            _dictRehashStep(d);
        else
            break;
    }

    tables = dictIsRehashing(d) ? 2 : 1;
    maxsizemask = (unsigned int) d->ht[0].sizemask;                             WIN_PORT_FIX /* cast (unsigned int) */
    if (tables > 1 && maxsizemask < d->ht[1].sizemask)
        maxsizemask = (unsigned int) d->ht[1].sizemask;                         WIN_PORT_FIX /* cast (unsigned int) */

    /* Pick a random point inside the larger table. */
    unsigned int i = random() & maxsizemask;
    unsigned int emptylen = 0; /* Continuous empty entries so far. */
    while(stored < count && maxsteps--) {
        for (j = 0; j < tables; j++) {
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            if (tables == 2 && j == 0 && i < (unsigned int) d->rehashidx) {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                if (i >= d->ht[1].size) i = (unsigned int) d->rehashidx;        WIN_PORT_FIX /* cast (unsigned int) */
                continue;
            }
            if (i >= d->ht[j].size) continue; /* Out of range for this table. */
            dictEntry *he = d->ht[j].table[i];

            /* Count contiguous empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            if (he == NULL) {
                emptylen++;
                if (emptylen >= 5 && emptylen > count) {
                    i = random() & maxsizemask;
                    emptylen = 0;
                }
            } else {
                emptylen = 0;
                while (he) {
                    /* Collect all the elements of the buckets found non
                     * empty while iterating. */
                    *des = he;
                    des++;
                    he = he->next;
                    stored++;
                    if (stored == count) return stored;
                }
            }
        }
        i = (i+1) & maxsizemask;
    }
    return stored;
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static PORT_ULONG rev(PORT_ULONG v) {
    PORT_ULONG s = 8 * sizeof(v); // bit size; must be power of 2
    PORT_ULONG mask = ~0;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/* �ú������ڵ����ֵ��Ԫ��
   dictScan() is used to iterate over the elements of a dictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the dictionary entry
 * 'de' as second argument.
 *
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 */
PORT_ULONG dictScan(dict *d,
                       PORT_ULONG v,
                       dictScanFunction *fn,
                       void *privdata)
{
    dictht *t0, *t1;
    const dictEntry *de;
    PORT_ULONG m0, m1;

    if (dictSize(d) == 0) return 0;

    if (!dictIsRehashing(d)) {
        t0 = &(d->ht[0]);
        m0 = (PORT_ULONG)t0->sizemask;                                          WIN_PORT_FIX /* cast (PORT_ULONG) */

        /* Emit entries at cursor */
        de = t0->table[v & m0];
        while (de) {
            fn(privdata, de);
            de = de->next;
        }

    } else {
        t0 = &d->ht[0];
        t1 = &d->ht[1];

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (t0->size > t1->size) {
            t0 = &d->ht[1];
            t1 = &d->ht[0];
        }

        m0 = (PORT_ULONG)t0->sizemask;                                          WIN_PORT_FIX /* cast (PORT_ULONG) */
        m1 = (PORT_ULONG)t1->sizemask;                                          WIN_PORT_FIX /* cast (PORT_ULONG) */

        /* Emit entries at cursor */
        de = t0->table[v & m0];
        while (de) {
            fn(privdata, de);
            de = de->next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        do {
            /* Emit entries at cursor */
            de = t1->table[v & m1];
            while (de) {
                fn(privdata, de);
                de = de->next;
            }

            /* Increment bits not covered by the smaller mask */
            v = (((v | m0) + 1) & ~m0) | (v & m0);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    /* Set unmasked bits so incrementing the reversed cursor
     * operates on the masked bits of the smaller table */
    v |= ~m0;

    /* Increment the reverse cursor */
    v = rev(v);
    v++;
    v = rev(v);

    return v;
}

/* ------------------------- private functions ------------------------------ */

/* ����ֵ���Ҫ���� */
static int _dictExpandIfNeeded(dict *d)
{
    /* ����rehash��ֱ�ӷ��� */
    if (dictIsRehashing(d)) return DICT_OK;

    /* ���hash��Ϊ��,��ʼ����СΪ4 */
    if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    /* ���Ͱλ���Ѿ�ȫ��ʹ��,����Ԫ��ռ�еı���������ֵ,����չ�ֵ� */
    if (d->ht[0].used >= d->ht[0].size &&
        (dict_can_resize ||
         d->ht[0].used/d->ht[0].size > dict_force_resize_ratio))
    {
        return dictExpand(d, d->ht[0].used*2);
    }
    return DICT_OK;
}

/* ����2���ݴη�����hash��Ĵ�С,��С����size���,���Ǵ��ڵ���size */
static PORT_ULONG _dictNextPower(PORT_ULONG size)
{
    PORT_ULONG i = DICT_HT_INITIAL_SIZE;

    if (size >= PORT_LONG_MAX) return PORT_LONG_MAX;
    while(1) {
        if (i >= size)
            return i;
        i *= 2;
    }
}

/* ���key���ڵ�Ͱλ�õ��������Ƿ��Ѿ�����key,�����򷵻�-1,���򷵻�hashֵ */
static int _dictKeyIndex(dict *d, const void *key)
{
    unsigned int h, idx, table;
    dictEntry *he;

    /* ����Ƿ���Ҫ����,����ʧ��ֱ�ӷ��� */
    if (_dictExpandIfNeeded(d) == DICT_ERR)
        return -1;
    /* ����hashֵ*/
    h = dictHashKey(d, key);
	
    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;
		//������ǰhͰλ�õ�����Ԫ��
        he = d->ht[table].table[idx];
        while(he) {
			//����Ѿ������򷵻�-1
            if (dictCompareKeys(d, key, he->key))
                return -1;
            he = he->next;
        }
        if (!dictIsRehashing(d)) break;
    }
	//��������λ��
    return idx;
}

void dictEmpty(dict *d, void(callback)(void*)) {
    _dictClear(d,&d->ht[0],callback);
    _dictClear(d,&d->ht[1],callback);
    d->rehashidx = -1;
    d->iterators = 0;
}

void dictEnableResize(void) {
    dict_can_resize = 1;
}

void dictDisableResize(void) {
    dict_can_resize = 0;
}

#if 0

/* The following is code that we don't use for Redis currently, but that is part
of the library. */

/* ----------------------- Debugging ------------------------*/

#define DICT_STATS_VECTLEN 50
static void _dictPrintStatsHt(dictht *ht) {
    PORT_ULONG i, slots = 0, chainlen, maxchainlen = 0;
    PORT_ULONG totchainlen = 0;
    PORT_ULONG clvector[DICT_STATS_VECTLEN];

    if (ht->used == 0) {
        printf("No stats available for empty dictionaries\n");
        return;
    }

    for (i = 0; i < DICT_STATS_VECTLEN; i++) clvector[i] = 0;
    for (i = 0; i < ht->size; i++) {
        dictEntry *he;

        if (ht->table[i] == NULL) {
            clvector[0]++;
            continue;
        }
        slots++;
        /* For each hash entry on this slot... */
        chainlen = 0;
        he = ht->table[i];
        while(he) {
            chainlen++;
            he = he->next;
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN-1)]++;
        if (chainlen > maxchainlen) maxchainlen = chainlen;
        totchainlen += chainlen;
    }
    printf("Hash table stats:\n");
    printf(" table size: %ld\n", ht->size);
    printf(" number of elements: %ld\n", ht->used);
    printf(" different slots: %ld\n", slots);
    printf(" max chain length: %ld\n", maxchainlen);
    printf(" avg chain length (counted): %.02f\n", (float)totchainlen/slots);
    printf(" avg chain length (computed): %.02f\n", (float)ht->used/slots);
    printf(" Chain length distribution:\n");
    for (i = 0; i < DICT_STATS_VECTLEN-1; i++) {
        if (clvector[i] == 0) continue;
        printf("   %s%ld: %ld (%.02f%%)\n",(i == DICT_STATS_VECTLEN-1)?">= ":"", i, clvector[i], ((float)clvector[i]/ht->size)*100);
    }
}

void dictPrintStats(dict *d) {
    _dictPrintStatsHt(&d->ht[0]);
    if (dictIsRehashing(d)) {
        printf("-- Rehashing into ht[1]:\n");
        _dictPrintStatsHt(&d->ht[1]);
    }
}

/* ----------------------- StringCopy Hash Table Type ------------------------*/

static unsigned int _dictStringCopyHTHashFunction(const void *key)
{
    return dictGenHashFunction(key, strlen(key));
}

static void *_dictStringDup(void *privdata, const void *key)
{
    int len = strlen(key);
    char *copy = zmalloc(len+1);
    DICT_NOTUSED(privdata);

    memcpy(copy, key, len);
    copy[len] = '\0';
    return copy;
}

static int _dictStringCopyHTKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    DICT_NOTUSED(privdata);

    return strcmp(key1, key2) == 0;
}

static void _dictStringDestructor(void *privdata, void *key)
{
    DICT_NOTUSED(privdata);

    zfree(key);
}

dictType dictTypeHeapStringCopyKey = {
    _dictStringCopyHTHashFunction, /* hash function */
    _dictStringDup,                /* key dup */
    NULL,                          /* val dup */
    _dictStringCopyHTKeyCompare,   /* key compare */
    _dictStringDestructor,         /* key destructor */
    NULL                           /* val destructor */
};

/* This is like StringCopy but does not auto-duplicate the key.
 * It's used for intepreter's shared strings. */
dictType dictTypeHeapStrings = {
    _dictStringCopyHTHashFunction, /* hash function */
    NULL,                          /* key dup */
    NULL,                          /* val dup */
    _dictStringCopyHTKeyCompare,   /* key compare */
    _dictStringDestructor,         /* key destructor */
    NULL                           /* val destructor */
};

/* This is like StringCopy but also automatically handle dynamic
 * allocated C strings as values. */
dictType dictTypeHeapStringCopyKeyValue = {
    _dictStringCopyHTHashFunction, /* hash function */
    _dictStringDup,                /* key dup */
    _dictStringDup,                /* val dup */
    _dictStringCopyHTKeyCompare,   /* key compare */
    _dictStringDestructor,         /* key destructor */
    _dictStringDestructor,         /* val destructor */
};
#endif
