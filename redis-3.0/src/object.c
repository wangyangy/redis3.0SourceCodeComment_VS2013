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
	robj是在redis.h中定义的一个结构体redisObject
	typedef struct redisObject {
		unsigned type : 4;  //类型,对象类型有字符串,列表,集合,map,有序集合
		unsigned encoding : 4;  //编码方式,编码为字符串,编码为正数,编码为zipmap等
		unsigned lru : REDIS_LRU_BITS; // LRU时间 
		int refcount;  //引用计数
		void *ptr;    //指向对象的值,指向实际保存值的结构
	} robj;
*/
/* 创建对象的函数 */
robj *createObject(int type, void *ptr) {
	//申请空间
    robj *o = zmalloc(sizeof(*o));
    o->type = type;  //设置对象类型
    o->encoding = REDIS_ENCODING_RAW;  //设置对象编码,默认为字符串对象
    o->ptr = ptr;     //数据指针
    o->refcount = 1;  //引用计数
	//LRU_CLOCK是一个宏定义
    o->lru = LRU_CLOCK();   //对象创建时间
    return o;
}

/* 创建简单动态字符串对象 */
robj *createRawStringObject(char *ptr, size_t len) {
    return createObject(REDIS_STRING,sdsnewlen(ptr,len));
}

/* 创建embstr编码的字符串对象,数据是ptr指针的内容 */
robj *createEmbeddedStringObject(char *ptr, size_t len) {
    //申请空间,包括数据的空间
	robj *o = zmalloc(sizeof(robj)+sizeof(struct sdshdr)+len+1);
    struct sdshdr *sh = (void*)(o+1);
	//对象属性赋值
    o->type = REDIS_STRING;
    o->encoding = REDIS_ENCODING_EMBSTR;
    o->ptr = sh+1;
    o->refcount = 1;
    o->lru = LRU_CLOCK();

    sh->len = (unsigned int)len;                                                WIN_PORT_FIX /* cast (unsigned int) */
    sh->free = 0;
    if (ptr) {
        memcpy(sh->buf,ptr,len);//复制数据
        sh->buf[len] = '\0';
    } else {
        memset(sh->buf,0,len+1); //如果ptr为空,用0填充
    }
    return o;
}

/* 创建字符串对象,根据len的大小创建不同类型的字符串 */
#define REDIS_ENCODING_EMBSTR_SIZE_LIMIT 39
robj *createStringObject(char *ptr, size_t len) {
    if (len <= REDIS_ENCODING_EMBSTR_SIZE_LIMIT)
        return createEmbeddedStringObject(ptr,len);
    else
        return createRawStringObject(ptr,len);
}
/*根据传入的longlong型整数创建字符串对象*/
robj *createStringObjectFromLongLong(PORT_LONGLONG value) {
    robj *o;
	//直接从共享对象中取出
    if (value >= 0 && value < REDIS_SHARED_INTEGERS) {
        incrRefCount(shared.integers[value]);
        o = shared.integers[value];
    } else {
		//创建对象
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

/*根据传入的LongDouble型整数创建字符串对象*/
robj *createStringObjectFromLongDouble(PORT_LONGDOUBLE value, int humanfriendly) {
    char buf[256];
    int len;
	//如果不是正常值
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

/* 复制参数o对象,深拷贝 */
robj *dupStringObject(robj *o) {
    robj *d;

    redisAssert(o->type == REDIS_STRING);
	//根据类型创建对象
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

/*创建列表对象*/
robj *createListObject(void) {
    list *l = listCreate();  //创建空list
    robj *o = createObject(REDIS_LIST,l);  //创建对象
    listSetFreeMethod(l,decrRefCountVoid);
    o->encoding = REDIS_ENCODING_LINKEDLIST;
    return o;
}

/*创建ziplist对象*/
robj *createZiplistObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(REDIS_LIST,zl);
    o->encoding = REDIS_ENCODING_ZIPLIST;
    return o;
}
/*创建set对象*/
robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType,NULL);  //创建hash表
    robj *o = createObject(REDIS_SET,d);
    o->encoding = REDIS_ENCODING_HT;
    return o;
}
/*创建intset对象*/
robj *createIntsetObject(void) {
    intset *is = intsetNew();
    robj *o = createObject(REDIS_SET,is);
    o->encoding = REDIS_ENCODING_INTSET;
    return o;
}
/*创建hash对象*/
robj *createHashObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(REDIS_HASH, zl);
    o->encoding = REDIS_ENCODING_ZIPLIST;
    return o;
}
/*创建zset对象*/
robj *createZsetObject(void) {
    zset *zs = zmalloc(sizeof(*zs));
    robj *o;

    zs->dict = dictCreate(&zsetDictType,NULL);
    zs->zsl = zslCreate();
    o = createObject(REDIS_ZSET,zs);
    o->encoding = REDIS_ENCODING_SKIPLIST;
    return o;
}
/*创建ZsetZiplist对象*/
robj *createZsetZiplistObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(REDIS_ZSET,zl);
    o->encoding = REDIS_ENCODING_ZIPLIST;
    return o;
}
/*释放sdsstring对象空间*/
void freeStringObject(robj *o) {
    if (o->encoding == REDIS_ENCODING_RAW) {
        sdsfree(o->ptr);
    }
}
/*释放list对象空间*/
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
/*释放set对象空间*/
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
/*释放zset对象空间*/
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
/*释放Hash对象空间*/
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
/*增加引用计数*/
void incrRefCount(robj *o) {
    o->refcount++;
}
/*引用计数减少函数,当引用计数为1时要释放对应的空间*/
void decrRefCount(robj *o) {
    if (o->refcount <= 0) redisPanic("decrRefCount against refcount <= 0");
	//当引用计数为1时要释放对应的空间
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

/*引用计数减少函数*/
void decrRefCountVoid(void *o) {
    decrRefCount(o);
}

//重置对象引用计数
robj *resetRefCount(robj *obj) {
    obj->refcount = 0;
    return obj;
}
/*类型检测函数*/
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

//对字符串对象进行节省空间的编码转换
robj *tryObjectEncoding(robj *o) {
    PORT_LONG value;
    sds s = o->ptr;
    size_t len;


    redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);

   
    if (!sdsEncodedObject(o)) return o;

    //引用计数大于1的字符串对象不能进行编码转换
     if (o->refcount > 1) return o;

	//内容为长度小于21的数字字符串的字符串对象
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
		/*当内存策略不是lru或没有可使用内存和数字介于
		0~OBJ_SHARED_INTEGERS使用共享整数字符串，不改变编码*/
        {
            decrRefCount(o);
            incrRefCount(shared.integers[value]);
            return shared.integers[value];
		//编码转换成OBJ_ENCODING_INT
        } else {
            if (o->encoding == REDIS_ENCODING_RAW) sdsfree(o->ptr);
            o->encoding = REDIS_ENCODING_INT;
            o->ptr = (void*) value;
            return o;
        }
    }

	//字符串长度小于OBJ_ENCODING_EMBSTR_SIZE_LIMIT，编码转换成OBJ_ENCODING_EMBSTR
    if (len <= REDIS_ENCODING_EMBSTR_SIZE_LIMIT) {
        robj *emb;

        if (o->encoding == REDIS_ENCODING_EMBSTR) return o;
        emb = createEmbeddedStringObject(s,sdslen(s));
        decrRefCount(o);
        return emb;
    }

	//编码为OBJ_ENCODING_RAW，sds的可用大小大于len/10，释放掉可用空间
    if (o->encoding == REDIS_ENCODING_RAW &&
        sdsavail(s) > len/10)
    {
        o->ptr = sdsRemoveFreeSpace(o->ptr);
    }

    /* Return the original object. */
    return o;
}

//将REDIS_ENCODING_INT编码字符串转成REDIS_ENCODING_EMBSTR或者REDIS_ENCODING_RAW
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

/*比较字符串对象*/
int compareStringObjectsWithFlags(robj *a, robj *b, int flags) {
    redisAssertWithInfo(NULL,a,a->type == REDIS_STRING && b->type == REDIS_STRING);
    char bufa[128], bufb[128], *astr, *bstr;
    size_t alen, blen, minlen;
	//直接进行比较
    if (a == b) return 0;
	//如果a是字符串编码
    if (sdsEncodedObject(a)) {
        astr = a->ptr;
        alen = sdslen(astr);
	//否则是整数编码
    } else {
        alen = ll2string(bufa,sizeof(bufa),(PORT_LONG) a->ptr);//整数转换为字符串
        astr = bufa;
    }
	//如果b是字符串编码
    if (sdsEncodedObject(b)) {
        bstr = b->ptr;
        blen = sdslen(bstr);
	//否则是整数编码
    } else {
        blen = ll2string(bufb,sizeof(bufb),(PORT_LONG) b->ptr);
        bstr = bufb;
    }
	//比较
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

/* 比较字符串对象 */
int compareStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_BINARY);
}

/* 比较字符串对象 */
int collateStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_COLL);
}

/* 比较两个字符串对象 */
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

/*获取对象长度*/
size_t stringObjectLen(robj *o) {
    redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);
    if (sdsEncodedObject(o)) {
        return sdslen(o->ptr);
    } else {
        char buf[32];

        return ll2string(buf,32,(PORT_LONG)o->ptr);
    }
}
/*获取对象的值,赋给target*/
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
/*获取对象的值,获取失败则回复error*/
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
/*获取对象的值*/
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
/*获取对象的值,获取失败则回复error*/
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
/*获取对象的值*/
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
/*获取对象的值,获取失败则回复error*/
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
/*获取对象的值,获取失败则回复error*/
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
/*获取编码类型*/
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

/* 估计对象空闲时间 */
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

