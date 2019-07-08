#ifdef _WIN32
#include "Win32_Interop/win32fixes.h"
#endif

#include "redis.h"
POSIX_ONLY(#include <pthread.h>)
#include <math.h>
#include <ctype.h>


#ifdef __CYGWIN__
#define strtold(a,b) ((PORT_LONGDOUBLE)strtod((a),(b)))
#endif

/*
	robj����redis.h�ж����һ���ṹ��redisObject
	typedef struct redisObject {
		unsigned type : 4;  //����,�����������ַ���,�б�,����,map,���򼯺�
		unsigned encoding : 4;  //���뷽ʽ,����Ϊ�ַ���,����Ϊ����,����Ϊzipmap��
		unsigned lru : REDIS_LRU_BITS; // LRUʱ�� 
		int refcount;  //���ü���
		void *ptr;    //ָ������ֵ,ָ��ʵ�ʱ���ֵ�Ľṹ
	} robj;
*/
/* ��������ĺ��� */
robj *createObject(int type, void *ptr) {
	//����ռ�
    robj *o = zmalloc(sizeof(*o));
    o->type = type;  //���ö�������
    o->encoding = REDIS_ENCODING_RAW;  //���ö������,Ĭ��Ϊ�ַ�������
    o->ptr = ptr;     //����ָ��
    o->refcount = 1;  //���ü���
	//LRU_CLOCK��һ���궨��
    o->lru = LRU_CLOCK();   //���󴴽�ʱ��
    return o;
}

/* �����򵥶�̬�ַ������� */
robj *createRawStringObject(char *ptr, size_t len) {
    return createObject(REDIS_STRING,sdsnewlen(ptr,len));
}

/* ����embstr������ַ�������,������ptrָ������� */
robj *createEmbeddedStringObject(char *ptr, size_t len) {
    //����ռ�,�������ݵĿռ�
	robj *o = zmalloc(sizeof(robj)+sizeof(struct sdshdr)+len+1);
    struct sdshdr *sh = (void*)(o+1);
	//�������Ը�ֵ
    o->type = REDIS_STRING;
    o->encoding = REDIS_ENCODING_EMBSTR;
    o->ptr = sh+1;
    o->refcount = 1;
    o->lru = LRU_CLOCK();

    sh->len = (unsigned int)len;                                                WIN_PORT_FIX /* cast (unsigned int) */
    sh->free = 0;
    if (ptr) {
        memcpy(sh->buf,ptr,len);//��������
        sh->buf[len] = '\0';
    } else {
        memset(sh->buf,0,len+1); //���ptrΪ��,��0���
    }
    return o;
}

/* �����ַ�������,����len�Ĵ�С������ͬ���͵��ַ��� */
#define REDIS_ENCODING_EMBSTR_SIZE_LIMIT 39
robj *createStringObject(char *ptr, size_t len) {
    if (len <= REDIS_ENCODING_EMBSTR_SIZE_LIMIT)
        return createEmbeddedStringObject(ptr,len);
    else
        return createRawStringObject(ptr,len);
}
/*���ݴ����longlong�����������ַ�������*/
robj *createStringObjectFromLongLong(PORT_LONGLONG value) {
    robj *o;
	//ֱ�Ӵӹ��������ȡ��
    if (value >= 0 && value < REDIS_SHARED_INTEGERS) {
        incrRefCount(shared.integers[value]);
        o = shared.integers[value];
    } else {
		//��������
        if (value >= PORT_LONG_MIN && value <= PORT_LONG_MAX) {
            o = createObject(REDIS_STRING, NULL);
            o->encoding = REDIS_ENCODING_INT;
            o->ptr = (void*)(value);                                            /* WIN_PORT_FIX: (long) cast removed */
        } else {
            o = createObject(REDIS_STRING,sdsfromlonglong(value));
        }
    }
    return o;
}

/*���ݴ����LongDouble�����������ַ�������*/
robj *createStringObjectFromLongDouble(PORT_LONGDOUBLE value, int humanfriendly) {
    char buf[256];
    int len;
	//�����������ֵ
    if (isinf(value)) {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        if (value > 0) {
            memcpy(buf,"inf",3);
            len = 3;
        } else {
            memcpy(buf,"-inf",4);
            len = 4;
        }
    } else if (humanfriendly) {
        /* We use 17 digits precision since with 128 bit floats that precision
         * after rounding is able to represent most small decimal numbers in a
         * way that is "non surprising" for the user (that is, most small
         * decimal numbers will be represented in a way that when converted
         * back into a string are exactly the same as what the user typed.) */
        len = snprintf(buf,sizeof(buf),"%.15Lf",value);                         WIN_PORT_FIX /* %.17 -> %.15 on Windows the magic number is 15 */
        /* Now remove trailing zeroes after the '.' */
        if (strchr(buf,'.') != NULL) {
            char *p = buf+len-1;
            while(*p == '0') {
                p--;
                len--;
            }
            if (*p == '.') len--;
        }
    } else {
        len = snprintf(buf,sizeof(buf),"%.17Lg", value);    /* TODO: verify if it needs to be changed to %.15 as well*/
    }
    return createStringObject(buf,len);
}

/* ���Ʋ���o����,��� */
robj *dupStringObject(robj *o) {
    robj *d;

    redisAssert(o->type == REDIS_STRING);
	//�������ʹ�������
    switch(o->encoding) {
    case REDIS_ENCODING_RAW:
        return createRawStringObject(o->ptr,sdslen(o->ptr));
    case REDIS_ENCODING_EMBSTR:
        return createEmbeddedStringObject(o->ptr,sdslen(o->ptr));
    case REDIS_ENCODING_INT:
        d = createObject(REDIS_STRING, NULL);
        d->encoding = REDIS_ENCODING_INT;
        d->ptr = o->ptr;
        return d;
    default:
        redisPanic("Wrong encoding.");
        break;
    }
}

/*�����б����*/
robj *createListObject(void) {
    list *l = listCreate();  //������list
    robj *o = createObject(REDIS_LIST,l);  //��������
    listSetFreeMethod(l,decrRefCountVoid);
    o->encoding = REDIS_ENCODING_LINKEDLIST;
    return o;
}

/*����ziplist����*/
robj *createZiplistObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(REDIS_LIST,zl);
    o->encoding = REDIS_ENCODING_ZIPLIST;
    return o;
}
/*����set����*/
robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType,NULL);  //����hash��
    robj *o = createObject(REDIS_SET,d);
    o->encoding = REDIS_ENCODING_HT;
    return o;
}
/*����intset����*/
robj *createIntsetObject(void) {
    intset *is = intsetNew();
    robj *o = createObject(REDIS_SET,is);
    o->encoding = REDIS_ENCODING_INTSET;
    return o;
}
/*����hash����*/
robj *createHashObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(REDIS_HASH, zl);
    o->encoding = REDIS_ENCODING_ZIPLIST;
    return o;
}
/*����zset����*/
robj *createZsetObject(void) {
    zset *zs = zmalloc(sizeof(*zs));
    robj *o;

    zs->dict = dictCreate(&zsetDictType,NULL);
    zs->zsl = zslCreate();
    o = createObject(REDIS_ZSET,zs);
    o->encoding = REDIS_ENCODING_SKIPLIST;
    return o;
}
/*����ZsetZiplist����*/
robj *createZsetZiplistObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(REDIS_ZSET,zl);
    o->encoding = REDIS_ENCODING_ZIPLIST;
    return o;
}
/*�ͷ�sdsstring����ռ�*/
void freeStringObject(robj *o) {
    if (o->encoding == REDIS_ENCODING_RAW) {
        sdsfree(o->ptr);
    }
}
/*�ͷ�list����ռ�*/
void freeListObject(robj *o) {
    switch (o->encoding) {
    case REDIS_ENCODING_LINKEDLIST:
        listRelease((list*) o->ptr);
        break;
    case REDIS_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        redisPanic("Unknown list encoding type");
    }
}
/*�ͷ�set����ռ�*/
void freeSetObject(robj *o) {
    switch (o->encoding) {
    case REDIS_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
    case REDIS_ENCODING_INTSET:
        zfree(o->ptr);
        break;
    default:
        redisPanic("Unknown set encoding type");
    }
}
/*�ͷ�zset����ռ�*/
void freeZsetObject(robj *o) {
    zset *zs;
    switch (o->encoding) {
    case REDIS_ENCODING_SKIPLIST:
        zs = o->ptr;
        dictRelease(zs->dict);
        zslFree(zs->zsl);
        zfree(zs);
        break;
    case REDIS_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        redisPanic("Unknown sorted set encoding");
    }
}
/*�ͷ�Hash����ռ�*/
void freeHashObject(robj *o) {
    switch (o->encoding) {
    case REDIS_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
    case REDIS_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        redisPanic("Unknown hash encoding type");
        break;
    }
}
/*�������ü���*/
void incrRefCount(robj *o) {
    o->refcount++;
}
/*���ü������ٺ���,�����ü���Ϊ1ʱҪ�ͷŶ�Ӧ�Ŀռ�*/
void decrRefCount(robj *o) {
    if (o->refcount <= 0) redisPanic("decrRefCount against refcount <= 0");
	//�����ü���Ϊ1ʱҪ�ͷŶ�Ӧ�Ŀռ�
    if (o->refcount == 1) {
        switch(o->type) {
        case REDIS_STRING: freeStringObject(o); break;
        case REDIS_LIST: freeListObject(o); break;
        case REDIS_SET: freeSetObject(o); break;
        case REDIS_ZSET: freeZsetObject(o); break;
        case REDIS_HASH: freeHashObject(o); break;
        default: redisPanic("Unknown object type"); break;
        }
        zfree(o);
    } else {
        o->refcount--;
    }
}

/*���ü������ٺ���*/
void decrRefCountVoid(void *o) {
    decrRefCount(o);
}

//���ö������ü���
robj *resetRefCount(robj *obj) {
    obj->refcount = 0;
    return obj;
}
/*���ͼ�⺯��*/
int checkType(redisClient *c, robj *o, int type) {
    if (o->type != type) {
        addReply(c,shared.wrongtypeerr);
        return 1;
    }
    return 0;
}

int isObjectRepresentableAsLongLong(robj *o, PORT_LONGLONG *llval) {
    redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);
    if (o->encoding == REDIS_ENCODING_INT) {
        if (llval) *llval = (PORT_LONGLONG) o->ptr;
        return REDIS_OK;
    } else {
        return string2ll(o->ptr,sdslen(o->ptr),llval) ? REDIS_OK : REDIS_ERR;
    }
}

//���ַ���������н�ʡ�ռ�ı���ת��
robj *tryObjectEncoding(robj *o) {
    PORT_LONG value;
    sds s = o->ptr;
    size_t len;


    redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);

   
    if (!sdsEncodedObject(o)) return o;

    //���ü�������1���ַ��������ܽ��б���ת��
     if (o->refcount > 1) return o;

	//����Ϊ����С��21�������ַ������ַ�������
    len = sdslen(s);
    if (len <= 21 && string2l(s,len,&value)) {
        /* This object is encodable as a long. Try to use a shared object.
         * Note that we avoid using shared integers when maxmemory is used
         * because every object needs to have a private LRU field for the LRU
         * algorithm to work well. */
        if ((server.maxmemory == 0 ||
             (server.maxmemory_policy != REDIS_MAXMEMORY_VOLATILE_LRU &&
              server.maxmemory_policy != REDIS_MAXMEMORY_ALLKEYS_LRU)) &&
            value >= 0 &&
            value < REDIS_SHARED_INTEGERS)
		/*���ڴ���Բ���lru��û�п�ʹ���ڴ�����ֽ���
		0~OBJ_SHARED_INTEGERSʹ�ù��������ַ��������ı����*/
        {
            decrRefCount(o);
            incrRefCount(shared.integers[value]);
            return shared.integers[value];
		//����ת����OBJ_ENCODING_INT
        } else {
            if (o->encoding == REDIS_ENCODING_RAW) sdsfree(o->ptr);
            o->encoding = REDIS_ENCODING_INT;
            o->ptr = (void*) value;
            return o;
        }
    }

	//�ַ�������С��OBJ_ENCODING_EMBSTR_SIZE_LIMIT������ת����OBJ_ENCODING_EMBSTR
    if (len <= REDIS_ENCODING_EMBSTR_SIZE_LIMIT) {
        robj *emb;

        if (o->encoding == REDIS_ENCODING_EMBSTR) return o;
        emb = createEmbeddedStringObject(s,sdslen(s));
        decrRefCount(o);
        return emb;
    }

	//����ΪOBJ_ENCODING_RAW��sds�Ŀ��ô�С����len/10���ͷŵ����ÿռ�
    if (o->encoding == REDIS_ENCODING_RAW &&
        sdsavail(s) > len/10)
    {
        o->ptr = sdsRemoveFreeSpace(o->ptr);
    }

    /* Return the original object. */
    return o;
}

//��REDIS_ENCODING_INT�����ַ���ת��REDIS_ENCODING_EMBSTR����REDIS_ENCODING_RAW
robj *getDecodedObject(robj *o) {
    robj *dec;

    if (sdsEncodedObject(o)) {
        incrRefCount(o);
        return o;
    }
    if (o->type == REDIS_STRING && o->encoding == REDIS_ENCODING_INT) {
        char buf[32];

        ll2string(buf,32,(PORT_LONG)o->ptr);
        dec = createStringObject(buf,strlen(buf));
        return dec;
    } else {
        redisPanic("Unknown encoding type");
    }
}

/* Compare two string objects via strcmp() or strcoll() depending on flags.
 * Note that the objects may be integer-encoded. In such a case we
 * use ll2string() to get a string representation of the numbers on the stack
 * and compare the strings, it's much faster than calling getDecodedObject().
 *
 * Important note: when REDIS_COMPARE_BINARY is used a binary-safe comparison
 * is used. */

#define REDIS_COMPARE_BINARY (1<<0)
#define REDIS_COMPARE_COLL (1<<1)

/*�Ƚ��ַ�������*/
int compareStringObjectsWithFlags(robj *a, robj *b, int flags) {
    redisAssertWithInfo(NULL,a,a->type == REDIS_STRING && b->type == REDIS_STRING);
    char bufa[128], bufb[128], *astr, *bstr;
    size_t alen, blen, minlen;
	//ֱ�ӽ��бȽ�
    if (a == b) return 0;
	//���a���ַ�������
    if (sdsEncodedObject(a)) {
        astr = a->ptr;
        alen = sdslen(astr);
	//��������������
    } else {
        alen = ll2string(bufa,sizeof(bufa),(PORT_LONG) a->ptr);//����ת��Ϊ�ַ���
        astr = bufa;
    }
	//���b���ַ�������
    if (sdsEncodedObject(b)) {
        bstr = b->ptr;
        blen = sdslen(bstr);
	//��������������
    } else {
        blen = ll2string(bufb,sizeof(bufb),(PORT_LONG) b->ptr);
        bstr = bufb;
    }
	//�Ƚ�
    if (flags & REDIS_COMPARE_COLL) {
        return strcoll(astr,bstr);
    } else {
        int cmp;

        minlen = (alen < blen) ? alen : blen;
        cmp = memcmp(astr,bstr,minlen);
        if (cmp == 0) return (int)(alen-blen);                                  WIN_PORT_FIX /* cast (int) */
        return cmp;
    }
}

/* �Ƚ��ַ������� */
int compareStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_BINARY);
}

/* �Ƚ��ַ������� */
int collateStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_COLL);
}

/* �Ƚ������ַ������� */
int equalStringObjects(robj *a, robj *b) {
    if (a->encoding == REDIS_ENCODING_INT &&
        b->encoding == REDIS_ENCODING_INT){
        /* If both strings are integer encoded just check if the stored
         * long is the same. */
        return a->ptr == b->ptr;
    } else {
        return compareStringObjects(a,b) == 0;
    }
}

/*��ȡ���󳤶�*/
size_t stringObjectLen(robj *o) {
    redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);
    if (sdsEncodedObject(o)) {
        return sdslen(o->ptr);
    } else {
        char buf[32];

        return ll2string(buf,32,(PORT_LONG)o->ptr);
    }
}
/*��ȡ�����ֵ,����target*/
int getDoubleFromObject(robj *o, double *target) {
    double value;
    char *eptr;

    if (o == NULL) {
        value = 0;
    } else {
        redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);
        if (sdsEncodedObject(o)) {
            errno = 0;
            value = strtod(o->ptr, &eptr);
            if (isspace(((char*)o->ptr)[0]) ||
                eptr[0] != '\0' ||
                (errno == ERANGE &&
                    (value == HUGE_VAL || value == -HUGE_VAL || value == 0)) ||
                errno == EINVAL ||
                isnan(value))
                return REDIS_ERR;
        } else if (o->encoding == REDIS_ENCODING_INT) {
            value = (PORT_LONG)o->ptr;
        } else {
            redisPanic("Unknown string encoding");
        }
    }
    *target = value;
    return REDIS_OK;
}
/*��ȡ�����ֵ,��ȡʧ����ظ�error*/
int getDoubleFromObjectOrReply(redisClient *c, robj *o, double *target, const char *msg) {
    double value;
    if (getDoubleFromObject(o, &value) != REDIS_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not a valid float");
        }
        return REDIS_ERR;
    }
    *target = value;
    return REDIS_OK;
}
/*��ȡ�����ֵ*/
int getLongDoubleFromObject(robj *o, PORT_LONGDOUBLE *target) {
    PORT_LONGDOUBLE value;
    char *eptr;

    if (o == NULL) {
        value = 0;
    } else {
        redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);
        if (sdsEncodedObject(o)) {
            errno = 0;
            value = IF_WIN32(wstrtod,strtold)(o->ptr,&eptr);                    // TODO: verify for 32-bit
            if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' ||
                errno == ERANGE || isnan(value))
                return REDIS_ERR;
        } else if (o->encoding == REDIS_ENCODING_INT) {
            value = (PORT_LONG)o->ptr;
        } else {
            redisPanic("Unknown string encoding");
        }
    }
    *target = value;
    return REDIS_OK;
}
/*��ȡ�����ֵ,��ȡʧ����ظ�error*/
int getLongDoubleFromObjectOrReply(redisClient *c, robj *o, PORT_LONGDOUBLE *target, const char *msg) {
    PORT_LONGDOUBLE value;
    if (getLongDoubleFromObject(o, &value) != REDIS_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not a valid float");
        }
        return REDIS_ERR;
    }
    *target = value;
    return REDIS_OK;
}
/*��ȡ�����ֵ*/
int getLongLongFromObject(robj *o, PORT_LONGLONG *target) {
    PORT_LONGLONG value;
    char *eptr;

    if (o == NULL) {
        value = 0;
    } else {
        redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);
        if (sdsEncodedObject(o)) {
            errno = 0;
            value = strtoll(o->ptr, &eptr, 10);
            if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' ||
                errno == ERANGE)
                return REDIS_ERR;
        } else if (o->encoding == REDIS_ENCODING_INT) {
            value = (PORT_LONG)o->ptr;
        } else {
            redisPanic("Unknown string encoding");
        }
    }
    if (target) *target = value;
    return REDIS_OK;
}
/*��ȡ�����ֵ,��ȡʧ����ظ�error*/
int getLongLongFromObjectOrReply(redisClient *c, robj *o, PORT_LONGLONG *target, const char *msg) {
    PORT_LONGLONG value;
    if (getLongLongFromObject(o, &value) != REDIS_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not an integer or out of range");
        }
        return REDIS_ERR;
    }
    *target = value;
    return REDIS_OK;
}
/*��ȡ�����ֵ,��ȡʧ����ظ�error*/
int getLongFromObjectOrReply(redisClient *c, robj *o, PORT_LONG *target, const char *msg) {
    PORT_LONGLONG value;

    if (getLongLongFromObjectOrReply(c, o, &value, msg) != REDIS_OK) return REDIS_ERR;
    if (value < PORT_LONG_MIN || value > PORT_LONG_MAX) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is out of range");
        }
        return REDIS_ERR;
    }
    *target = (PORT_LONG)value;                                                 WIN_PORT_FIX /* cast (PORT_LONG) */
    return REDIS_OK;
}
/*��ȡ��������*/
char *strEncoding(int encoding) {
    switch(encoding) {
    case REDIS_ENCODING_RAW: return "raw";
    case REDIS_ENCODING_INT: return "int";
    case REDIS_ENCODING_HT: return "hashtable";
    case REDIS_ENCODING_LINKEDLIST: return "linkedlist";
    case REDIS_ENCODING_ZIPLIST: return "ziplist";
    case REDIS_ENCODING_INTSET: return "intset";
    case REDIS_ENCODING_SKIPLIST: return "skiplist";
    case REDIS_ENCODING_EMBSTR: return "embstr";
    default: return "unknown";
    }
}

/* ���ƶ������ʱ�� */
PORT_ULONGLONG estimateObjectIdleTime(robj *o) {
    PORT_ULONGLONG lruclock = LRU_CLOCK();
    if (lruclock >= o->lru) {
        return (lruclock - o->lru) * REDIS_LRU_CLOCK_RESOLUTION;
    } else {
        return (lruclock + (REDIS_LRU_CLOCK_MAX - o->lru)) *
                    REDIS_LRU_CLOCK_RESOLUTION;
    }
}

/* This is a helper function for the OBJECT command. We need to lookup keys
 * without any modification of LRU or other parameters. */
robj *objectCommandLookup(redisClient *c, robj *key) {
    dictEntry *de;

    if ((de = dictFind(c->db->dict,key->ptr)) == NULL) return NULL;
    return (robj*) dictGetVal(de);
}

robj *objectCommandLookupOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = objectCommandLookup(c,key);

    if (!o) addReply(c, reply);
    return o;
}

/* Object command allows to inspect the internals of an Redis Object.
 * Usage: OBJECT <refcount|encoding|idletime> <key> */
void objectCommand(redisClient *c) {
    robj *o;

    if (!strcasecmp(c->argv[1]->ptr,"refcount") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.nullbulk))
                == NULL) return;
        addReplyLongLong(c,o->refcount);
    } else if (!strcasecmp(c->argv[1]->ptr,"encoding") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.nullbulk))
                == NULL) return;
        addReplyBulkCString(c,strEncoding(o->encoding));
    } else if (!strcasecmp(c->argv[1]->ptr,"idletime") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.nullbulk))
                == NULL) return;
        addReplyLongLong(c,estimateObjectIdleTime(o)/1000);
    } else {
        addReplyError(c,"Syntax error. Try OBJECT (refcount|encoding|idletime)");
    }
}

