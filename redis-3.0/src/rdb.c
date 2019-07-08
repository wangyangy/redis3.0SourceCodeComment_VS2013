/*
RDB�־û��ǰѵ�ǰ������������ʱ�����գ�point-in-time snapshot�����浽Ӳ�̵Ĺ��̣������������ⶪʧ��
1.1 RDB��������
	RDB�������Ʒ�Ϊ�ֶ��������Զ�������
	�ֶ��������������
		SAVE��������ǰRedis��������ֱ��RDB�������Ϊֹ��
		BGSAVE��Redis����ִ��fork()����������һ���ӽ��̣��ں�̨���RDB�־û��Ĺ��̡���������
	�Զ����������ã�
		c	
		save 900 1 //��������900��֮�ڣ������ݿ�ִ��������1���޸�
		save 300 10 //��������300��֮�ڣ������ݿ�ִ��������10�޸�
		save 60 1000 //��������60��֮�ڣ������ݿ�ִ��������1000�޸�
		// �����������������е�����һ�������Զ�����BGSAVE����,����ʹ������CONFIG SET ��������
*/

#ifdef _WIN32
#include "Win32_Interop/win32_types.h"
#endif

#include "redis.h"
#include "lzf.h"    /* LZF compression library */
#include "zipmap.h"
#include "endianconv.h"

#include <math.h>
#include <sys/types.h>
#ifdef _WIN32
#include <stdio.h>
#include "Win32_Interop/Win32_QFork.h"
#else
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#endif
#include <sys/stat.h>
/*������Ϊlen������pд��rdb��,����д�ĳ���*/
static int rdbWriteRaw(rio *rdb, void *p, size_t len) {
    if (rdb && rioWrite(rdb,p,len) == 0)
        return -1;
    return (int)len;                                                            WIN_PORT_FIX /* cast (int) */
}
/*������Ϊ1��type�ַ�д��rdb��*/
int rdbSaveType(rio *rdb, unsigned char type) {
    return rdbWriteRaw(rdb,&type,1);
}

/* ��rdb������1�ֽڵ����ݱ�����type��,������type */
int rdbLoadType(rio *rdb) {
    unsigned char type;
    if (rioRead(rdb,&type,1) == 0) return -1;
    return type;
}
// ��rio����һ��ʱ�䣬��λΪ�룬����Ϊ4�ֽ�
time_t rdbLoadTime(rio *rdb) {
    int32_t t32;
    if (rioRead(rdb,&t32,4) == 0) return -1;
    return (time_t)t32;
}
// дһ��longlong���͵�ʱ�䣬��λΪ����
int rdbSaveMillisecondTime(rio *rdb, PORT_LONGLONG t) {
    int64_t t64 = (int64_t) t;
    return rdbWriteRaw(rdb,&t64,8);
}
// ��rio�ж���һ������ʱ�䷵��
PORT_LONGLONG rdbLoadMillisecondTime(rio *rdb) {
    int64_t t64;
    if (rioRead(rdb,&t64,8) == 0) return -1;
    return (PORT_LONGLONG)t64;
}

/* Saves an encoded length. The first two bits in the first byte are used to
 * hold the encoding type. See the REDIS_RDB_* definitions for more information
 * on the types of encoding. */
// ��һ��������ĳ���д�뵽rio�У����ر��������len��Ҫ���ֽ���
int rdbSaveLen(rio *rdb, uint32_t len) {
    unsigned char buf[2];
    size_t nwritten;

    if (len < (1<<6)) {
        /* Save a 6 bit len */
        buf[0] = (len&0xFF)|(REDIS_RDB_6BITLEN<<6);
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        nwritten = 1;
    } else if (len < (1<<14)) {
        /* Save a 14 bit len */
        buf[0] = ((len>>8)&0xFF)|(REDIS_RDB_14BITLEN<<6);
        buf[1] = len&0xFF;
        if (rdbWriteRaw(rdb,buf,2) == -1) return -1;
        nwritten = 2;
    } else {
        /* Save a 32 bit len */
        buf[0] = (REDIS_RDB_32BITLEN<<6);
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        len = htonl(len);
        if (rdbWriteRaw(rdb,&len,4) == -1) return -1;
        nwritten = 1+4;
    }
    return (int)nwritten;                                                       WIN_PORT_FIX /* cast (int) */
}

/* Load an encoded length. The "isencoded" argument is set to 1 if the length
 * is not actually a length but an "encoding type". See the REDIS_RDB_ENC_*
 * definitions in rdb.h for more information. */
// ����һ����rio������lenֵ�������lenֵ�������������Ǳ�������ֵ����ô��isencoded����Ϊ1
uint32_t rdbLoadLen(rio *rdb, int *isencoded) {
    unsigned char buf[2];
    uint32_t len;
    int type;
	//Ĭ��Ϊû�б���
    if (isencoded) *isencoded = 0;
	//��rio�е�ֵ��ȡ��buf��
    if (rioRead(rdb,buf,1) == 0) return REDIS_RDB_LENERR;
	//ȡ�������λ
    type = (buf[0]&0xC0)>>6;
	//һ���������ֵ,���ؽ����ֵ,���ñ����־
    if (type == REDIS_RDB_ENCVAL) {
        /* Read a 6 bit encoding type. */
        if (isencoded) *isencoded = 1;
        return buf[0]&0x3F;
    } else if (type == REDIS_RDB_6BITLEN) {
        /* Read a 6 bit len. */
        return buf[0]&0x3F;
    } else if (type == REDIS_RDB_14BITLEN) {
        /* Read a 14 bit len. */
        if (rioRead(rdb,buf+1,1) == 0) return REDIS_RDB_LENERR;
        return ((buf[0]&0x3F)<<8)|buf[1];
    } else {
        /* Read a 32 bit len. */
        if (rioRead(rdb,&len,4) == 0) return REDIS_RDB_LENERR;
        return ntohl(len);
    }
}

/* Encodes the "value" argument as integer when it fits in the supported ranges
 * for encoded types. If the function successfully encodes the integer, the
 * representation is stored in the buffer pointer to by "enc" and the string
 * length is returned. Otherwise 0 is returned. */
// ��longlong���͵�value�����һ���������룬������Ա��룬��������ֵ������enc�У����ر������ֽ���
int rdbEncodeInteger(PORT_LONGLONG value, unsigned char *enc) {
    if (value >= -(1<<7) && value <= (1<<7)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT8;
        enc[1] = value&0xFF;
        return 2;
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;
    } else if (value >= -((PORT_LONGLONG)1<<31) && value <= ((PORT_LONGLONG)1<<31)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}

/* Loads an integer-encoded object with the specified encoding type "enctype".
 * If the "encode" argument is set the function may return an integer-encoded
 * string object, otherwise it always returns a raw string object. */
// ��rio�е�����ֵ���ݲ�ͬ�ı����������������flags������һ����ͬ���͵�ֵ������
robj *rdbLoadIntegerObject(rio *rdb, int enctype, int encode) {
    unsigned char enc[4];
    PORT_LONGLONG val;
	// ���ݲ�ͬ�������������ͣ���rio�ж�������ֵ��enc��
    if (enctype == REDIS_RDB_ENC_INT8) {
        if (rioRead(rdb,enc,1) == 0) return NULL;
        val = (signed char)enc[0];
    } else if (enctype == REDIS_RDB_ENC_INT16) {
        uint16_t v;
        if (rioRead(rdb,enc,2) == 0) return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == REDIS_RDB_ENC_INT32) {
        uint32_t v;
        if (rioRead(rdb,enc,4) == 0) return NULL;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        val = (int32_t)v;
    } else {
        val = 0; /* anti-warning */
        redisPanic("Unknown RDB integer encoding type");
    }
	// ����Ǳ����������ֵ����ת��Ϊ�ַ������󣬷���
    if (encode)
        return createStringObjectFromLongLong(val);
    else
        return createObject(REDIS_STRING,sdsfromlonglong(val));
}

/* String objects in the form "2391" "-100" without any space and with a
 * range of values that can fit in an 8, 16 or 32 bit signed value can be
 * encoded as integers to save space */
// ��һЩ�����ֵ��ַ�������ת��Ϊ���Ա�����������Խ�ʡ�ڴ�
int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    PORT_LONGLONG value;
    char *endptr, buf[32];

    /* Check if it's possible to encode this value as a number */
    value = strtoll(s, &endptr, 10);
    if (endptr[0] != '\0') return 0;
    ll2string(buf,32,value);

    /* If the number converted back into a string is not identical
     * then it's not possible to encode the string as integer */
    if (strlen(buf) != len || memcmp(buf,s,len)) return 0;

    return rdbEncodeInteger(value,enc);
}
// ����һ��LZFѹ�������ַ�������Ϣд��rio������д����ֽ���
int rdbSaveLzfStringObject(rio *rdb, unsigned char *s, size_t len) {
    size_t comprlen, outlen;
    unsigned char byte;
    int n, nwritten = 0;
    void *out;

    /* We require at least four bytes compression for this to be worth it */
    if (len <= 4) return 0;
    outlen = len-4;
    if ((out = zmalloc(outlen+1)) == NULL) return 0;
    comprlen = lzf_compress(s, (unsigned int)len, out, (unsigned int)outlen);   WIN_PORT_FIX /* cast (unsigned int) (unsigned int) */
    if (comprlen == 0) {
        zfree(out);
        return 0;
    }
    /* Data compressed! Let's save it on disk */
    byte = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_LZF;
    if ((n = rdbWriteRaw(rdb,&byte,1)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbSaveLen(rdb,(uint32_t)comprlen)) == -1) goto writeerr;          WIN_PORT_FIX /* cast (uint32_t) */
    nwritten += n;

    if ((n = rdbSaveLen(rdb,(uint32_t)len)) == -1) goto writeerr;               WIN_PORT_FIX /* cast (uint32_t) */
    nwritten += n;

    if ((n = rdbWriteRaw(rdb,out,comprlen)) == -1) goto writeerr;
    nwritten += n;

    zfree(out);
    return nwritten;

writeerr:
    zfree(out);
    return -1;
}
// ��rio�ж���һ��ѹ�������ַ����������ѹ�����ع����ɵ��ַ�������
robj *rdbLoadLzfStringObject(rio *rdb) {
    unsigned int len, clen;
    unsigned char *c = NULL;
    sds val = NULL;

    if ((clen = rdbLoadLen(rdb,NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((len = rdbLoadLen(rdb,NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((c = zmalloc(clen)) == NULL) goto err;
    if ((val = sdsnewlen(NULL,len)) == NULL) goto err;
    if (rioRead(rdb,c,clen) == 0) goto err;
    if (lzf_decompress(c,clen,val,len) == 0) goto err;
    zfree(c);
    return createObject(REDIS_STRING,val);
err:
    zfree(c);
    sdsfree(val);
    return NULL;
}

/* Save a string object as [len][data] on disk. If the object is a string
 * representation of an integer value we try to save it in a special form */
// ��һ��ԭ�����ַ���ֵд�뵽rio
int rdbSaveRawString(rio *rdb, unsigned char *s, size_t len) {
    int enclen;
    int n, nwritten = 0;

    /* Try integer encoding */
    if (len <= 11) {
        unsigned char buf[5];
        if ((enclen = rdbTryIntegerEncoding((char*)s,len,buf)) > 0) {
            if (rdbWriteRaw(rdb,buf,enclen) == -1) return -1;
            return enclen;
        }
    }

    /* Try LZF compression - under 20 bytes it's unable to compress even
     * aaaaaaaaaaaaaaaaaa so skip it */
    if (server.rdb_compression && len > 20) {
        n = rdbSaveLzfStringObject(rdb,s,len);
        if (n == -1) return -1;
        if (n > 0) return n;
        /* Return value of 0 means data can't be compressed, save the old way */
    }

    /* Store verbatim */
    if ((n = rdbSaveLen(rdb,(uint32_t)len)) == -1) return -1;                   WIN_PORT_FIX /* cast (uint32_t) */
    nwritten += n;
    if (len > 0) {
        if (rdbWriteRaw(rdb,s,len) == -1) return -1;
        nwritten += (int)len;                                                   WIN_PORT_FIX /* cast (int) */
    }
    return nwritten;
}

/* Save a PORT_LONGLONG value as either an encoded string or a string. */
// �� longlong���͵�valueת��Ϊ�ַ������󣬲��ҽ��б��룬Ȼ��д��rio��
int rdbSaveLongLongAsStringObject(rio *rdb, PORT_LONGLONG value) {
    unsigned char buf[32];
    int n, nwritten = 0;
    int enclen = rdbEncodeInteger(value,buf);
    if (enclen > 0) {
        return rdbWriteRaw(rdb,buf,enclen);
    } else {
        /* Encode as string */
        enclen = ll2string((char*)buf,32,value);
        redisAssert(enclen < 32);
        if ((n = rdbSaveLen(rdb,enclen)) == -1) return -1;
        nwritten += n;
        if ((n = rdbWriteRaw(rdb,buf,enclen)) == -1) return -1;
        nwritten += n;
    }
    return nwritten;
}

/* Like rdbSaveStringObjectRaw() but handle encoded objects */
// ���ַ�������objд��rio��
int rdbSaveStringObject(rio *rdb, robj *obj) {
    /* Avoid to decode the object, then encode it again, if the
     * object is already integer encoded. */
    if (obj->encoding == REDIS_ENCODING_INT) {
        return rdbSaveLongLongAsStringObject(rdb, (PORT_LONG) obj->ptr);
    } else {
        redisAssertWithInfo(NULL,obj,sdsEncodedObject(obj));
        return rdbSaveRawString(rdb,obj->ptr,sdslen(obj->ptr));
    }
}
// ����flags������rio����һ���ַ���������б���
robj *rdbGenericLoadStringObject(rio *rdb, int encode) {
    int isencoded;
    uint32_t len;
    robj *o;
	// ��rio�ж���һ���ַ������󣬱������ͱ�����isencoded�У�������ֽ�Ϊlen
    len = rdbLoadLen(rdb,&isencoded);
	// ��������Ķ��󱻱���(isencoded������Ϊ1)������ݲ�ͬ�ĳ���ֵlenӳ�䵽��ͬ����������
    if (isencoded) {
        switch(len) {
        case REDIS_RDB_ENC_INT8:
        case REDIS_RDB_ENC_INT16:
        case REDIS_RDB_ENC_INT32:
            return rdbLoadIntegerObject(rdb,len,encode);
        case REDIS_RDB_ENC_LZF:
            return rdbLoadLzfStringObject(rdb);
        default:
            redisPanic("Unknown RDB encoding type");
        }
    }
	// ���lenֵ�����򷵻�NULL
    if (len == REDIS_RDB_LENERR) return NULL;
    o = encode ? createStringObject(NULL,len) :
                 createRawStringObject(NULL,len);
    if (len && rioRead(rdb,o->ptr,len) == 0) {
        decrRefCount(o);
        return NULL;
    }
    return o;
}
// ��rio�ж���һ���ַ�������Ķ���
robj *rdbLoadStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,0);
}
// ��rio�ж���һ���ַ�������Ķ��󣬶���ʹ�ò�ͬ���͵ı���
robj *rdbLoadEncodedStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,1);
}

/* Save a double value. Doubles are saved as strings prefixed by an unsigned
 * 8 bit integer specifying the length of the representation.
 * This 8 bit integer has special values in order to specify the following
 * conditions:
 * 253: not a number
 * 254: + inf
 * 255: - inf
 */
// д��һ��double���͵��ַ���ֵ���ַ���ǰ��һ��8λ�����޷�������������ʾ�������ĳ���
// ��λ�����е�ֵ��ʾһЩ���������
// 253����ʾ��������
// 254����ʾ������
// 255����ʾ������
int rdbSaveDoubleValue(rio *rdb, double val) {
    unsigned char buf[128];
    int len;

    if (isnan(val)) {
        buf[0] = 253;
        len = 1;
    } else if (!isfinite(val)) {
        len = 1;
        buf[0] = (val < 0) ? 255 : 254;
    } else {
#if (DBL_MANT_DIG >= 52) && (LLONG_MAX == 0x7fffffffffffffffLL)
        /* Check if the float is in a safe range to be casted into a
         * PORT_LONGLONG. We are assuming that PORT_LONGLONG is 64 bit here.
         * Also we are assuming that there are no implementations around where
         * double has precision < 52 bit.
         *
         * Under this assumptions we test if a double is inside an interval
         * where casting to PORT_LONGLONG is safe. Then using two castings we
         * make sure the decimal part is zero. If all this is true we use
         * integer printing function that is much faster. */
        double min = -4503599627370495; /* (2^52)-1 */
        double max = 4503599627370496; /* -(2^52) */
        if (val > min && val < max && val == ((double)((PORT_LONGLONG)val)))
            ll2string((char*)buf+1,sizeof(buf)-1,(PORT_LONGLONG)val);
        else
#endif
            snprintf((char*)buf+1,sizeof(buf)-1,"%.17g",val);
        buf[0] = (unsigned char)strlen((char*)buf+1);                           WIN_PORT_FIX /* cast (unsigned char) */
        len = buf[0]+1;
    }
    return rdbWriteRaw(rdb,buf,len);
}

/* �����ַ�����ʾ��doubleֵ */
int rdbLoadDoubleValue(rio *rdb, double *val) {
    char buf[256];
    unsigned char len;
#ifdef _WIN32
    double scannedVal = 0;
    int assigned = 0;
    memset(buf, 0, sizeof(buf));
#endif

    if (rioRead(rdb,&len,1) == 0) return -1;
    switch(len) {
    case 255: *val = R_NegInf; return 0;
    case 254: *val = R_PosInf; return 0;
    case 253: *val = R_Nan; return 0;
    default:
        if (rioRead(rdb,buf,len) == 0) return -1;
        buf[len] = '\0';
#ifdef _WIN32
        assigned = sscanf_s(buf, "%lg", &scannedVal);
        if( assigned != 0 ) {
            (*val) = scannedVal;
            return 0;
        } else {
            return -1;
        }
#else
        sscanf(buf, "%lg", val);
        return 0;
#endif
    }
}

// ������o������д��rio��
int rdbSaveObjectType(rio *rdb, robj *o) {
    switch (o->type) {
    case REDIS_STRING:
        return rdbSaveType(rdb,REDIS_RDB_TYPE_STRING);
    case REDIS_LIST:
        if (o->encoding == REDIS_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,REDIS_RDB_TYPE_LIST_ZIPLIST);
        else if (o->encoding == REDIS_ENCODING_LINKEDLIST)
            return rdbSaveType(rdb,REDIS_RDB_TYPE_LIST);
        else
            redisPanic("Unknown list encoding");
    case REDIS_SET:
        if (o->encoding == REDIS_ENCODING_INTSET)
            return rdbSaveType(rdb,REDIS_RDB_TYPE_SET_INTSET);
        else if (o->encoding == REDIS_ENCODING_HT)
            return rdbSaveType(rdb,REDIS_RDB_TYPE_SET);
        else
            redisPanic("Unknown set encoding");
    case REDIS_ZSET:
        if (o->encoding == REDIS_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,REDIS_RDB_TYPE_ZSET_ZIPLIST);
        else if (o->encoding == REDIS_ENCODING_SKIPLIST)
            return rdbSaveType(rdb,REDIS_RDB_TYPE_ZSET);
        else
            redisPanic("Unknown sorted set encoding");
    case REDIS_HASH:
        if (o->encoding == REDIS_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,REDIS_RDB_TYPE_HASH_ZIPLIST);
        else if (o->encoding == REDIS_ENCODING_HT)
            return rdbSaveType(rdb,REDIS_RDB_TYPE_HASH);
        else
            redisPanic("Unknown hash encoding");
    default:
        redisPanic("Unknown object type");
    }
    return -1; /* avoid warning */
}

// ��rio�ж���һ�����Ͳ�����
int rdbLoadObjectType(rio *rdb) {
    int type;
    if ((type = rdbLoadType(rdb)) == -1) return -1;
    if (!rdbIsObjectType(type)) return -1;
    return type;
}

// ��һ������д��rio�У���������-1���ɹ�����д���ֽ���
int rdbSaveObject(rio *rdb, robj *o) {
    int n, nwritten = 0;

    if (o->type == REDIS_STRING) {
        /* Save a string value */
        if ((n = rdbSaveStringObject(rdb,o)) == -1) return -1;
        nwritten += n;
    } else if (o->type == REDIS_LIST) {
        /* Save a list value */
        if (o->encoding == REDIS_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
            list *list = o->ptr;
            listIter li;
            listNode *ln;

            if ((n = rdbSaveLen(rdb,(uint32_t)listLength(list))) == -1) return -1;  WIN_PORT_FIX /* cast (uint32_t) */
            nwritten += n;

            listRewind(list,&li);
            while((ln = listNext(&li))) {
                robj *eleobj = listNodeValue(ln);
                if ((n = rdbSaveStringObject(rdb,eleobj)) == -1) return -1;
                nwritten += n;
            }
        } else {
            redisPanic("Unknown list encoding");
        }
    } else if (o->type == REDIS_SET) {
        /* Save a set value */
        if (o->encoding == REDIS_ENCODING_HT) {
            dict *set = o->ptr;
            dictIterator *di = dictGetIterator(set);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,(uint32_t)dictSize(set))) == -1) return -1; WIN_PORT_FIX /* cast (uint32_t) */
            nwritten += n;

            while((de = dictNext(di)) != NULL) {
                robj *eleobj = dictGetKey(de);
                if ((n = rdbSaveStringObject(rdb,eleobj)) == -1) return -1;
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else if (o->encoding == REDIS_ENCODING_INTSET) {
            size_t l = intsetBlobLen((intset*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else {
            redisPanic("Unknown set encoding");
        }
    } else if (o->type == REDIS_ZSET) {
        /* Save a sorted set value */
        if (o->encoding == REDIS_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else if (o->encoding == REDIS_ENCODING_SKIPLIST) {
            zset *zs = o->ptr;
            dictIterator *di = dictGetIterator(zs->dict);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,(uint32_t)dictSize(zs->dict))) == -1) return -1;    WIN_PORT_FIX /* cast (uint32_t) */
            nwritten += n;

            while((de = dictNext(di)) != NULL) {
                robj *eleobj = dictGetKey(de);
                double *score = dictGetVal(de);

                if ((n = rdbSaveStringObject(rdb,eleobj)) == -1) return -1;
                nwritten += n;
                if ((n = rdbSaveDoubleValue(rdb,*score)) == -1) return -1;
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else {
            redisPanic("Unknown sorted set encoding");
        }
    } else if (o->type == REDIS_HASH) {
        /* Save a hash value */
        if (o->encoding == REDIS_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;

        } else if (o->encoding == REDIS_ENCODING_HT) {
            dictIterator *di = dictGetIterator(o->ptr);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,(uint32_t)dictSize((dict*)o->ptr))) == -1) return -1;   WIN_PORT_FIX /* cast (uint32_t) */
            nwritten += n;

            while((de = dictNext(di)) != NULL) {
                robj *key = dictGetKey(de);
                robj *val = dictGetVal(de);

                if ((n = rdbSaveStringObject(rdb,key)) == -1) return -1;
                nwritten += n;
                if ((n = rdbSaveStringObject(rdb,val)) == -1) return -1;
                nwritten += n;
            }
            dictReleaseIterator(di);

        } else {
            redisPanic("Unknown hash encoding");
        }

    } else {
        redisPanic("Unknown object type");
    }
    return nwritten;
}

/* Return the length the object will have on disk if saved with
 * the rdbSaveObject() function. Currently we use a trick to get
 * this length with very little changes to the code. In the future
 * we could switch to a faster solution. */
off_t rdbSavedObjectLen(robj *o) {
    int len = rdbSaveObject(NULL,o);
    redisAssertWithInfo(NULL,o,len != -1);
    return (off_t)len;                                                          WIN_PORT_FIX /* cast (off_t) */
}

/* Save a key-value pair, with expire time, type, key, value.
 * On error -1 is returned.
 * On success if the key was actually saved 1 is returned, otherwise 0
 * is returned (the key was already expired). */
// ��һ��������ֵ���󣬹���ʱ�䣬������д�뵽rio�У���������-1���ɹ�����1�������ڷ���0
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val,
                        PORT_LONGLONG expiretime, PORT_LONGLONG now)
{
    /* Save the expire time */
    if (expiretime != -1) {
        /* If this key is already expired skip it */
        if (expiretime < now) return 0;
        if (rdbSaveType(rdb,REDIS_RDB_OPCODE_EXPIRETIME_MS) == -1) return -1;
        if (rdbSaveMillisecondTime(rdb,expiretime) == -1) return -1;
    }

    /* Save type, key, value */
    if (rdbSaveObjectType(rdb,val) == -1) return -1;
    if (rdbSaveStringObject(rdb,key) == -1) return -1;
    if (rdbSaveObject(rdb,val) == -1) return -1;
    return 1;
}

/* Produces a dump of the database in RDB format sending it to the specified
 * Redis I/O channel. On success REDIS_OK is returned, otherwise REDIS_ERR
 * is returned and part of the output, or all the output, can be
 * missing because of I/O errors.
 *
 * When the function returns REDIS_ERR and if 'error' is not NULL, the
 * integer pointed by 'error' is set to the value of errno just after the I/O
 * error. */
// ��һ��RDB��ʽ�ļ�����д�뵽rio�У��ɹ�����C_OK������C_ERR��һ���ֻ����еĳ�����Ϣ
// ����������C_ERR������error����NULL����ôerror������Ϊһ��������errno
int rdbSaveRio(rio *rdb, int *error) {
    dictIterator *di = NULL;
    dictEntry *de;
    char magic[10];
    int j;
    PORT_LONGLONG now = mstime();
    uint64_t cksum;
	//������У���
    if (server.rdb_checksum)
        rdb->update_cksum = rioGenericUpdateChecksum;//����У��ͺ���
	//��redis�汾��Ϣ���浽magic��,д��rio��
    snprintf(magic,sizeof(magic),"REDIS%04d",REDIS_RDB_VERSION);
    if (rdbWriteRaw(rdb,magic,9) == -1) goto werr;
	//�����������ڵ����ݿ�
    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;   //��ǰ���ݿ�ָ��
        dict *d = db->dict;          //��ǰ���ݿ��ֵ���ֵ�
		//�����յ����ݿ�
        if (dictSize(d) == 0) continue;
		//�����ֵ����͵ĵ�����
        di = dictGetSafeIterator(d);
        if (!di) return REDIS_ERR;

        //д�����ݿ�ѡ���ʾ��,REDIS_RDB_OPCODE_SELECTDBΪ254
        if (rdbSaveType(rdb,REDIS_RDB_OPCODE_SELECTDB) == -1) goto werr;
		//д�����ݿ�id,ռ��һ���ֽ�
        if (rdbSaveLen(rdb,j) == -1) goto werr;

        //�������ݿ����м�ֵ��
        while((de = dictNext(di)) != NULL) {
            sds keystr = dictGetKey(de);   //��
            robj key, *o = dictGetVal(de); //ֵ
            PORT_LONGLONG expire;
			//��ջ�д���һ����ֵ�Զ��󲢳�ʼ��
            initStaticStringObject(key,keystr);
            expire = getExpire(db,&key);  //���ù���ʱ��
			// �����ļ�����ֵ���󣬹���ʱ��д��rio��
            if (rdbSaveKeyValuePair(rdb,&key,o,expire,now) == -1) goto werr;
        }
        dictReleaseIterator(di);  //�ͷŵ�����
    }
    di = NULL; /* So that we don't release it again on error. */

	// д��һ��EOF�룬RDB_OPCODE_EOF = 255
    if (rdbSaveType(rdb,REDIS_RDB_OPCODE_EOF) == -1) goto werr;

	// CRC64����ͣ���У��ͼ���Ϊ0��û�п���ʱ��������rdb�ļ�ʱ������
    cksum = rdb->cksum;
    memrev64ifbe(&cksum);
    if (rioWrite(rdb,&cksum,8) == 0) goto werr;
    return REDIS_OK;
//д�����
werr:
    if (error) *error = errno;   //���������
    if (di) dictReleaseIterator(di);  //�ͷŵ�����
    return REDIS_ERR;
}

/* This is just a wrapper to rdbSaveRio() that additionally adds a prefix
 * and a suffix to the generated RDB dump. The prefix is:
 *
 * $EOF:<40 bytes unguessable hex string>\r\n
 *
 * While the suffix is the 40 bytes hex string we announced in the prefix.
 * This way processes receiving the payload can understand when it ends
 * without doing any processing of the content. */
// ��rdbSaveRio()�����ϣ���������һ��ǰ׺��һ����׺����ʽ���£�
// $EOF:<40 bytes unguessable hex string>\r\n
int rdbSaveRioWithEOFMark(rio *rdb, int *error) {
    char eofmark[REDIS_EOF_MARK_SIZE];

    getRandomHexChars(eofmark,REDIS_EOF_MARK_SIZE);
    if (error) *error = 0;
    if (rioWrite(rdb,"$EOF:",5) == 0) goto werr;
    if (rioWrite(rdb,eofmark,REDIS_EOF_MARK_SIZE) == 0) goto werr;
    if (rioWrite(rdb,"\r\n",2) == 0) goto werr;
    if (rdbSaveRio(rdb,error) == REDIS_ERR) goto werr;
    if (rioWrite(rdb,eofmark,REDIS_EOF_MARK_SIZE) == 0) goto werr;
    return REDIS_OK;

werr: /* Write error. */
    /* Set 'error' only if not already set by rdbSaveRio() call. */
    if (error && *error == 0) *error = errno;
    return REDIS_ERR;
}

/* Save the DB on disk. Return REDIS_ERR on error, REDIS_OK on success. */
// �����ݿⱣ���ڴ����ϣ�����C_OK�ɹ������򷵻�C_ERR
int rdbSave(char *filename) {
    char tmpfile[256];
    FILE *fp;
    rio rdb;
    int error;

    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
	//������ʱ�ļ�,д�ķ�ʽ��
    fp = fopen(tmpfile,IF_WIN32("wb","w"));
	//���ļ�ʧ��,д����־
    if (!fp) {
        redisLog(REDIS_WARNING, "Failed opening .rdb for saving: %s",
            strerror(errno));
        return REDIS_ERR;
    }
	//��ʼ��һ��rio����,rio������һ���ļ�����IO
    rioInitWithFile(&rdb,fp);
	//�����ݿ�����д��rio��
    if (rdbSaveRio(&rdb,&error) == REDIS_ERR) {
        errno = error;
        goto werr;
    }

    /* ˢ�»�����ȷ���������ݶ�д������� */
    if (fflush(fp) == EOF) goto werr;
	//��fpָ����ļ�ͬ����������
    if (fsync(fileno(fp)) == -1) goto werr;
	//�ر��ļ�
    if (fclose(fp) == EOF) goto werr; 

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
	//ԭ���Ըı�rdb�ļ�������
    if (rename(tmpfile,filename) == -1) {
		// �ı�����ʧ�ܣ����õ�ǰĿ¼·����������־��Ϣ��ɾ����ʱ�ļ�
        redisLog(REDIS_WARNING,"Error moving temp DB file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        return REDIS_ERR;
    }
    redisLog(REDIS_NOTICE,"DB saved on disk");  //д��־
    server.dirty = 0;   //�������
    server.lastsave = time(NULL);  //������һ��SAVE������ʱ�� 
    server.lastbgsave_status = REDIS_OK;  //����SAVE״̬
    return REDIS_OK;
// rdbSaveRio()������д��������д��־���ر��ļ���ɾ����ʱ�ļ�������C_ERR
werr:
    redisLog(REDIS_WARNING,"Write error saving DB on disk: %s", strerror(errno));
    fclose(fp);
    unlink(tmpfile);
    return REDIS_ERR;
}
// ��̨����RDB�־û�BGSAVE����
int rdbSaveBackground(char *filename) {
    pid_t childpid;
    PORT_LONGLONG start;
	//��ǰû��ִ��RDB����,���򷵻�error
    if (server.rdb_child_pid != -1) return REDIS_ERR;
	//�������
    server.dirty_before_bgsave = server.dirty;
	//���һ��ִ��BGSAVE��ʱ��
    server.lastbgsave_try = time(NULL);
	//fork������ʼ��ʱ��
    start = ustime();
#ifdef _WIN32
	//�����ӽ���,��linux�еĲ�ͬ,����windows�д�����,���幦����һ����
    childpid = BeginForkOperation_Rdb(filename, &server, sizeof(server), dictGetHashFunctionSeed());
#else
	//��������linux�н��е�fork����
    if ((childpid = fork()) == 0) {
        int retval;

        /* �رռ�Ͳ�׽��� */
        closeListeningSockets(0);
		//���ý��̱���,����ʶ��
        redisSetProcTitle("redis-rdb-bgsave");
		//ִ�б������,�����ݿ�д��filename�ļ���
        retval = rdbSave(filename);
        if (retval == REDIS_OK) {
			// �õ��ӽ��̽��̵���˽������ҳ���С�������RDB��ͬʱ����������д������ݣ���ô�ӽ��̾ͻ´��һ���ݸ����̵��ڴ棬�����Ǻ͸����̹���һ���ڴ档
            size_t private_dirty = zmalloc_get_private_dirty();
			// ���ӽ��̷��������д��־
            if (private_dirty) {
                redisLog(REDIS_NOTICE,
                    "RDB: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }
        }
		// �ӽ����˳��������źŸ������̣�����0��ʾBGSAVE�ɹ���1��ʾʧ��
        exitFromChild((retval == REDIS_OK) ? 0 : 1);
    } else {
#endif
        /* ������ִ�еĴ��� */
		//����forkִ�е�ʱ��
        server.stat_fork_time = ustime()-start;
		//����fork����,GB/ÿ��
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
		//���forkִ��ʱ���������õ���ֵ,��Ҫ������뵽һ���ֵ���,�봫���'fork'����
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);
		// ���fork����
        if (childpid == -1) {
            server.lastbgsave_status = REDIS_ERR; //����BGSAVE����
			//������־
            redisLog(REDIS_WARNING,"Can't save in background: fork: %s",
                strerror(errno));
            return REDIS_ERR;
        }
		//������־
        redisLog(REDIS_NOTICE,"Background saving started by pid %d",childpid);
        server.rdb_save_time_start = time(NULL);//����BGSAVE��ʼ��ʱ��
		server.rdb_child_pid = childpid; //���ø���ִ��BGSAVE�������ӽ���id
        server.rdb_child_type = REDIS_RDB_CHILD_TYPE_DISK;//����BGSAVE�����ͣ���������д��
		//�رչ�ϣ����resize����Ϊresize�����л��и��ƿ�������
        updateDictResizePolicy();
        return REDIS_OK;
#ifndef _WIN32
    }
#endif
    return REDIS_OK; /* unreached */
}
// ɾ����ʱ�ļ�����BGSAVEִ�б��ж�ʱʹ��
void rdbRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,sizeof(tmpfile),"temp-%d.rdb", (int) childpid);
    unlink(tmpfile);
}

/* Load a Redis object of the specified type from the specified file.
 * On success a newly allocated object is returned, otherwise NULL. */
// ��rio�ж���һ��rdbtype���͵Ķ��󣬳ɹ������¶����ַ�����򷵻�NULL
robj *rdbLoadObject(int rdbtype, rio *rdb) {
    robj *o, *ele, *dec;
    size_t len;
    unsigned int i;

    if (rdbtype == REDIS_RDB_TYPE_STRING) {
        /* Read string value */
        if ((o = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
        o = tryObjectEncoding(o);
    } else if (rdbtype == REDIS_RDB_TYPE_LIST) {
        /* Read list value */
        if ((len = rdbLoadLen(rdb,NULL)) == REDIS_RDB_LENERR) return NULL;

        /* Use a real list when there are too many entries */
        if (len > server.list_max_ziplist_entries) {
            o = createListObject();
        } else {
            o = createZiplistObject();
        }

        /* Load every single element of the list */
        while(len--) {
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;

            /* If we are using a ziplist and the value is too big, convert
             * the object to a real list. */
            if (o->encoding == REDIS_ENCODING_ZIPLIST &&
                sdsEncodedObject(ele) &&
                sdslen(ele->ptr) > server.list_max_ziplist_value)
                    listTypeConvert(o,REDIS_ENCODING_LINKEDLIST);

            if (o->encoding == REDIS_ENCODING_ZIPLIST) {
                dec = getDecodedObject(ele);
                o->ptr = ziplistPush(o->ptr,dec->ptr,(unsigned int)sdslen(dec->ptr),REDIS_TAIL);    WIN_PORT_FIX /* cast (unsigned int) */
                decrRefCount(dec);
                decrRefCount(ele);
            } else {
                ele = tryObjectEncoding(ele);
                listAddNodeTail(o->ptr,ele);
            }
        }
    } else if (rdbtype == REDIS_RDB_TYPE_SET) {
        /* Read list/set value */
        if ((len = rdbLoadLen(rdb,NULL)) == REDIS_RDB_LENERR) return NULL;

        /* Use a regular set when there are too many entries. */
        if (len > server.set_max_intset_entries) {
            o = createSetObject();
            /* It's faster to expand the dict to the right size asap in order
             * to avoid rehashing */
            if (len > DICT_HT_INITIAL_SIZE)
                dictExpand(o->ptr,len);
        } else {
            o = createIntsetObject();
        }

        /* Load every single element of the list/set */
        for (i = 0; i < len; i++) {
            PORT_LONGLONG llval;
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
            ele = tryObjectEncoding(ele);

            if (o->encoding == REDIS_ENCODING_INTSET) {
                /* Fetch integer value from element */
                if (isObjectRepresentableAsLongLong(ele,&llval) == REDIS_OK) {
                    o->ptr = intsetAdd(o->ptr,llval,NULL);
                } else {
                    setTypeConvert(o,REDIS_ENCODING_HT);
                    dictExpand(o->ptr,len);
                }
            }

            /* This will also be called when the set was just converted
             * to a regular hash table encoded set */
            if (o->encoding == REDIS_ENCODING_HT) {
                dictAdd((dict*)o->ptr,ele,NULL);
            } else {
                decrRefCount(ele);
            }
        }
    } else if (rdbtype == REDIS_RDB_TYPE_ZSET) {
        /* Read list/set value */
        size_t zsetlen;
        size_t maxelelen = 0;
        zset *zs;

        if ((zsetlen = rdbLoadLen(rdb,NULL)) == REDIS_RDB_LENERR) return NULL;
        o = createZsetObject();
        zs = o->ptr;

        /* Load every single element of the list/set */
        while(zsetlen--) {
            robj *ele;
            double score;
            zskiplistNode *znode;

            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
            ele = tryObjectEncoding(ele);
            if (rdbLoadDoubleValue(rdb,&score) == -1) return NULL;

            /* Don't care about integer-encoded strings. */
            if (sdsEncodedObject(ele) && sdslen(ele->ptr) > maxelelen)
                maxelelen = sdslen(ele->ptr);

            znode = zslInsert(zs->zsl,score,ele);
            dictAdd(zs->dict,ele,&znode->score);
            incrRefCount(ele); /* added to skiplist */
        }

        /* Convert *after* loading, since sorted sets are not stored ordered. */
        if (zsetLength(o) <= server.zset_max_ziplist_entries &&
            maxelelen <= server.zset_max_ziplist_value)
                zsetConvert(o,REDIS_ENCODING_ZIPLIST);
    } else if (rdbtype == REDIS_RDB_TYPE_HASH) {
        size_t len;
        int ret;

        len = rdbLoadLen(rdb, NULL);
        if (len == REDIS_RDB_LENERR) return NULL;

        o = createHashObject();

        /* Too many entries? Use a hash table. */
        if (len > server.hash_max_ziplist_entries)
            hashTypeConvert(o, REDIS_ENCODING_HT);

        /* Load every field and value into the ziplist */
        while (o->encoding == REDIS_ENCODING_ZIPLIST && len > 0) {
            robj *field, *value;

            len--;
            /* Load raw strings */
            field = rdbLoadStringObject(rdb);
            if (field == NULL) return NULL;
            redisAssert(sdsEncodedObject(field));
            value = rdbLoadStringObject(rdb);
            if (value == NULL) return NULL;
            redisAssert(sdsEncodedObject(value));

            /* Add pair to ziplist */
            o->ptr = ziplistPush(o->ptr, field->ptr, (unsigned int)sdslen(field->ptr), ZIPLIST_TAIL);   WIN_PORT_FIX /* cast (unsigned int) */
            o->ptr = ziplistPush(o->ptr, value->ptr, (unsigned int)sdslen(value->ptr), ZIPLIST_TAIL);   WIN_PORT_FIX /* cast (unsigned int) */
            /* Convert to hash table if size threshold is exceeded */
            if (sdslen(field->ptr) > server.hash_max_ziplist_value ||
                sdslen(value->ptr) > server.hash_max_ziplist_value)
            {
                decrRefCount(field);
                decrRefCount(value);
                hashTypeConvert(o, REDIS_ENCODING_HT);
                break;
            }
            decrRefCount(field);
            decrRefCount(value);
        }

        /* Load remaining fields and values into the hash table */
        while (o->encoding == REDIS_ENCODING_HT && len > 0) {
            robj *field, *value;

            len--;
            /* Load encoded strings */
            field = rdbLoadEncodedStringObject(rdb);
            if (field == NULL) return NULL;
            value = rdbLoadEncodedStringObject(rdb);
            if (value == NULL) return NULL;

            field = tryObjectEncoding(field);
            value = tryObjectEncoding(value);

            /* Add pair to hash table */
            ret = dictAdd((dict*)o->ptr, field, value);
            redisAssert(ret == DICT_OK);
        }

        /* All pairs should be read by now */
        redisAssert(len == 0);

    } else if (rdbtype == REDIS_RDB_TYPE_HASH_ZIPMAP  ||
               rdbtype == REDIS_RDB_TYPE_LIST_ZIPLIST ||
               rdbtype == REDIS_RDB_TYPE_SET_INTSET   ||
               rdbtype == REDIS_RDB_TYPE_ZSET_ZIPLIST ||
               rdbtype == REDIS_RDB_TYPE_HASH_ZIPLIST)
    {
        robj *aux = rdbLoadStringObject(rdb);

        if (aux == NULL) return NULL;
        o = createObject(REDIS_STRING,NULL); /* string is just placeholder */
        o->ptr = zmalloc(sdslen(aux->ptr));
        memcpy(o->ptr,aux->ptr,sdslen(aux->ptr));
        decrRefCount(aux);

        /* Fix the object encoding, and make sure to convert the encoded
         * data type into the base type if accordingly to the current
         * configuration there are too many elements in the encoded data
         * type. Note that we only check the length and not max element
         * size as this is an O(N) scan. Eventually everything will get
         * converted. */
        switch(rdbtype) {
            case REDIS_RDB_TYPE_HASH_ZIPMAP:
                /* Convert to ziplist encoded hash. This must be deprecated
                 * when loading dumps created by Redis 2.4 gets deprecated. */
                {
                    unsigned char *zl = ziplistNew();
                    unsigned char *zi = zipmapRewind(o->ptr);
                    unsigned char *fstr, *vstr;
                    unsigned int flen, vlen;
                    unsigned int maxlen = 0;

                    while ((zi = zipmapNext(zi, &fstr, &flen, &vstr, &vlen)) != NULL) {
                        if (flen > maxlen) maxlen = flen;
                        if (vlen > maxlen) maxlen = vlen;
                        zl = ziplistPush(zl, fstr, flen, ZIPLIST_TAIL);
                        zl = ziplistPush(zl, vstr, vlen, ZIPLIST_TAIL);
                    }

                    zfree(o->ptr);
                    o->ptr = zl;
                    o->type = REDIS_HASH;
                    o->encoding = REDIS_ENCODING_ZIPLIST;

                    if (hashTypeLength(o) > server.hash_max_ziplist_entries ||
                        maxlen > server.hash_max_ziplist_value)
                    {
                        hashTypeConvert(o, REDIS_ENCODING_HT);
                    }
                }
                break;
            case REDIS_RDB_TYPE_LIST_ZIPLIST:
                o->type = REDIS_LIST;
                o->encoding = REDIS_ENCODING_ZIPLIST;
                if (ziplistLen(o->ptr) > server.list_max_ziplist_entries)
                    listTypeConvert(o,REDIS_ENCODING_LINKEDLIST);
                break;
            case REDIS_RDB_TYPE_SET_INTSET:
                o->type = REDIS_SET;
                o->encoding = REDIS_ENCODING_INTSET;
                if (intsetLen(o->ptr) > server.set_max_intset_entries)
                    setTypeConvert(o,REDIS_ENCODING_HT);
                break;
            case REDIS_RDB_TYPE_ZSET_ZIPLIST:
                o->type = REDIS_ZSET;
                o->encoding = REDIS_ENCODING_ZIPLIST;
                if (zsetLength(o) > server.zset_max_ziplist_entries)
                    zsetConvert(o,REDIS_ENCODING_SKIPLIST);
                break;
            case REDIS_RDB_TYPE_HASH_ZIPLIST:
                o->type = REDIS_HASH;
                o->encoding = REDIS_ENCODING_ZIPLIST;
                if (hashTypeLength(o) > server.hash_max_ziplist_entries)
                    hashTypeConvert(o, REDIS_ENCODING_HT);
                break;
            default:
                redisPanic("Unknown encoding");
                break;
        }
    } else {
        redisPanic("Unknown object type");
    }
    return o;
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats. */
// ���������״̬��Ϣ
void startLoading(FILE *fp) {
    struct IF_WIN32(_stat64,stat) sb;                                           // TODO: verify for 32-bit

    /* Load the DB */
    server.loading = 1;
    server.loading_start_time = time(NULL);
    server.loading_loaded_bytes = 0;
    if (fstat(fileno(fp), &sb) == -1) {
        server.loading_total_bytes = 0;
    } else {
        server.loading_total_bytes = sb.st_size;
    }
}

/* Refresh the loading progress info */
// ��������ʱserver��״̬��Ϣ
void loadingProgress(off_t pos) {
    server.loading_loaded_bytes = pos;
    if (server.stat_peak_memory < zmalloc_used_memory())
        server.stat_peak_memory = zmalloc_used_memory();
}

/* Loading finished */
// �������
void stopLoading(void) {
    server.loading = 0;
}

/* Track loading progress in order to serve client's from time to time
   and if needed calculate rdb checksum  */
// �����������Ϣ���Ա�client���в�ѯ����rdb��ѯ��ʱҲ��Ҫ
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len) {
    if (server.rdb_checksum)
        rioGenericUpdateChecksum(r, buf, len);
    if (server.loading_process_events_interval_bytes &&
        (r->processed_bytes + len)/server.loading_process_events_interval_bytes > r->processed_bytes/server.loading_process_events_interval_bytes)
    {
        /* The DB can take some non trivial amount of time to load. Update
         * our cached time since it is used to create and update the last
         * interaction time with clients and for other important things. */
        updateCachedTime();
        if (server.masterhost && server.repl_state == REDIS_REPL_TRANSFER)
            replicationSendNewlineToMaster();
        loadingProgress((off_t)r->processed_bytes);                             WIN_PORT_FIX /* cast (off_t) */
        processEventsWhileBlocked();
    }
}
// ��ָ����RDB�ļ��������ݿ���
int rdbLoad(char *filename) {
    uint32_t dbid;
    int type, rdbver;
    redisDb *db = server.db+0;
    char buf[1024];
    PORT_LONGLONG expiretime, now = mstime();   //��ȡ��ǰload������ʱ��
    FILE *fp;
    rio rdb;
	//ֻ�����ļ�
    if ((fp = fopen(filename,"r")) == NULL) return REDIS_ERR;
	//��ʼ��һ���ļ�������
    rioInitWithFile(&rdb,fp);
	//���ü���У��͵ĺ���
    rdb.update_cksum = rdbLoadProgressCallback;
	//����������д������ֽ���,2M
    rdb.max_processing_chunk = server.loading_process_events_interval_bytes;
	// ����9���ֽڵ�buf��buf�б�����Redis�汾"redis0007"
	if (rioRead(&rdb,buf,9) == 0) goto eoferr;
    buf[9] = '\0';
	// �������İ汾�ű�ʶ
    if (memcmp(buf,"REDIS",5) != 0) {
        fclose(fp);
        redisLog(REDIS_WARNING,"Wrong signature trying to load DB from file");
        errno = EINVAL;
        return REDIS_ERR;
    }
	// ת�����������汾��С
    rdbver = atoi(buf+5);
    if (rdbver < 1 || rdbver > REDIS_RDB_VERSION) {
        fclose(fp);
        redisLog(REDIS_WARNING,"Can't handle RDB format version %d",rdbver);
        errno = EINVAL;
        return REDIS_ERR;
    }
	//��������ʱserver��״̬��Ϣ
    startLoading(fp);
	// ��ʼ��ȡRDB�ļ������ݿ���
    while(1) {
        robj *key, *val;
        expiretime = -1;

		// ���ȶ�������
        if ((type = rdbLoadType(&rdb)) == -1) goto eoferr;
		// �����������
		// ��������Ƕ�������ʱ�䵥λΪ��
        if (type == REDIS_RDB_OPCODE_EXPIRETIME) {
			//��rio�ж�ȡ����ʱ��
            if ((expiretime = rdbLoadTime(&rdb)) == -1) goto eoferr;
			// �ӹ���ʱ������һ����ֵ�Ե�����
            if ((type = rdbLoadType(&rdb)) == -1) goto eoferr;
            /* the EXPIRETIME opcode specifies time in seconds, so convert
             * into milliseconds. */
            expiretime *= 1000;//ת���ɺ���
		//��ȡ�Ĺ���ʱ��Ϊ����
        } else if (type == REDIS_RDB_OPCODE_EXPIRETIME_MS) {
            /* Milliseconds precision expire times introduced with RDB
             * version 3. */
            if ((expiretime = rdbLoadMillisecondTime(&rdb)) == -1) goto eoferr;
            /* We read the time so we need to read the object type again. */
            if ((type = rdbLoadType(&rdb)) == -1) goto eoferr;
        }
		// �������EOF����ֱ������ѭ��
        if (type == REDIS_RDB_OPCODE_EOF)
            break;

		// ���������л����ݿ����
        if (type == REDIS_RDB_OPCODE_SELECTDB) {
			// ��ȡ��һ�����ȣ�����������ݿ��ID
            if ((dbid = rdbLoadLen(&rdb,NULL)) == REDIS_RDB_LENERR)
                goto eoferr;
			// ��������ID�Ƿ�Ϸ�
            if (dbid >= (unsigned)server.dbnum) {
                redisLog(REDIS_WARNING,"FATAL: Data file was created with a Redis server configured to handle more than %d databases. Exiting\n", server.dbnum);
                exit(1);
            }
			db = server.db + dbid; // �л����ݿ�
            continue;// ��������ѭ�����ڶ�һ��type
        }
		// ����һ��key����
        if ((key = rdbLoadStringObject(&rdb)) == NULL) goto eoferr;
		// ����һ��val����
        if ((val = rdbLoadObject(type,&rdb)) == NULL) goto eoferr;
		// �����ǰ�������Ǵӽڵ㣬�Ҹü������˹���ʱ�䣬�Ѿ�����
        if (server.masterhost == NULL && expiretime != -1 && expiretime < now) {
            decrRefCount(key);   //�ͷż�ֵ��
            decrRefCount(val);
            continue;
        }
		// ��û�й��ڵļ�ֵ�����ӵ����ݿ��ֵ���ֵ���
        dbAdd(db,key,val);

		// �����Ҫ�����ù���ʱ��
        if (expiretime != -1) setExpire(db,key,expiretime);

        decrRefCount(key);//�ͷ���ʱ����
    }
	// ��ʱ�Ѿ��������������ݿ�ļ�ֵ�ԣ�������EOF������EOF����RDB�ļ��Ľ�������Ҫ����У���
	// ��RDB�汾����5ʱ���ҿ�����У��͵Ĺ��ܣ���ô����У���
	if (rdbver >= 5 && server.rdb_checksum) {
        uint64_t cksum, expected = rdb.cksum;
		//����8�ֽڵ�У���Ȼ��Ƚ�
        if (rioRead(&rdb,&cksum,8) == 0) goto eoferr;
        memrev64ifbe(&cksum);
        if (cksum == 0) {
            redisLog(REDIS_WARNING,"RDB file was saved with checksum disabled: no check performed.");
        } else if (cksum != expected) {
            redisLog(REDIS_WARNING,"Wrong RDB checksum. Aborting now.");
            exit(1);
        }
    }

    fclose(fp);  //�ر�rdb�ļ�
    stopLoading();  //������������״̬
    return REDIS_OK;
//�����˳�
eoferr: /* unexpected end of file is handled here with a fatal exit */
    redisLog(REDIS_WARNING,"Short read or OOM loading DB. Unrecoverable error, aborting now.");
    exit(1);
    return REDIS_ERR; /* Just to avoid warning */
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of actual BGSAVEs. */
// �����ӽ��̽���BGSAVE���ʱ��Ҫ���͵�ʵ���ź�
// BGSAVE��������д����̵�
// exitcode���ӽ����˳�ʱ���˳��룬�ɹ��˳�Ϊ0
// bysignal �ӽ��̽��ܵ��ź�
void backgroundSaveDoneHandlerDisk(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        redisLog(REDIS_NOTICE,
            "Background saving terminated with success");
        server.dirty = server.dirty - server.dirty_before_bgsave;
        server.lastsave = time(NULL);
        server.lastbgsave_status = REDIS_OK;
    } else if (!bysignal && exitcode != 0) {
        redisLog(REDIS_WARNING, "Background saving error");
        server.lastbgsave_status = REDIS_ERR;
    } else {
        mstime_t latency;

        redisLog(REDIS_WARNING,
            "Background saving terminated by signal %d", bysignal);
        latencyStartMonitor(latency);
        rdbRemoveTempFile(server.rdb_child_pid);
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("rdb-unlink-temp-file",latency);
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * tirggering an error conditon. */
        if (bysignal != SIGUSR1)
            server.lastbgsave_status = REDIS_ERR;
    }
    server.rdb_child_pid = -1;
    server.rdb_child_type = REDIS_RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_last = time(NULL)-server.rdb_save_time_start;
    server.rdb_save_time_start = -1;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) */
    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? REDIS_OK : REDIS_ERR, REDIS_RDB_CHILD_TYPE_DISK);
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of RDB -> Salves socket transfers for
 * diskless replication. */
// �����ӽ��̽���BGSAVE���ʱ��Ҫ���͵�ʵ���ź�
// BGSAVE��������д��ӽڵ��socket��
// exitcode���ӽ����˳�ʱ���˳��룬�ɹ��˳�Ϊ0
// bysignal �ӽ��̽��ܵ��ź�
void backgroundSaveDoneHandlerSocket(int exitcode, int bysignal) {
    uint64_t *ok_slaves;

    if (!bysignal && exitcode == 0) {
        redisLog(REDIS_NOTICE,
            "Background RDB transfer terminated with success");
    } else if (!bysignal && exitcode != 0) {
        redisLog(REDIS_WARNING, "Background transfer error");
    } else {
        redisLog(REDIS_WARNING,
            "Background transfer terminated by signal %d", bysignal);
    }
    server.rdb_child_pid = -1;
    server.rdb_child_type = REDIS_RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_start = -1;

    /* If the child returns an OK exit code, read the set of slave client
     * IDs and the associated status code. We'll terminate all the slaves
     * in error state.
     *
     * If the process returned an error, consider the list of slaves that
     * can continue to be emtpy, so that it's just a special case of the
     * normal code path. */
    ok_slaves = zmalloc(sizeof(uint64_t)); /* Make space for the count. */
    ok_slaves[0] = 0;
    if (!bysignal && exitcode == 0) {
        int readlen = sizeof(uint64_t);

        if (read(server.rdb_pipe_read_result_from_child, ok_slaves, readlen) ==
                 readlen)
        {
            readlen = (int)(ok_slaves[0]*sizeof(uint64_t)*2);                   WIN_PORT_FIX /* cast (int) */

            /* Make space for enough elements as specified by the first
             * uint64_t element in the array. */
            ok_slaves = zrealloc(ok_slaves,sizeof(uint64_t)+readlen);
            if (readlen &&
                read(server.rdb_pipe_read_result_from_child, ok_slaves+1,
                     readlen) != readlen)
            {
                ok_slaves[0] = 0;
            }
        }
    }

    close(server.rdb_pipe_read_result_from_child);
    close(server.rdb_pipe_write_result_to_parent);

    /* We can continue the replication process with all the slaves that
     * correctly received the full payload. Others are terminated. */
    listNode *ln;
    listIter li;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;

        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) {
            uint64_t j;
            int errorcode = 0;

            /* Search for the slave ID in the reply. In order for a slave to
             * continue the replication process, we need to find it in the list,
             * and it must have an error code set to 0 (which means success). */
            for (j = 0; j < ok_slaves[0]; j++) {
                if (slave->id == ok_slaves[2*j+1]) {
                    errorcode = (int)(ok_slaves[2*j+2]);                        WIN_PORT_FIX /* cast (int) */
                    break; /* Found in slaves list. */
                }
            }
            if (j == ok_slaves[0] || errorcode != 0) {
                redisLog(REDIS_WARNING,
                "Closing slave %s: child->slave RDB transfer failed: %s",
                    replicationGetSlaveName(slave),
                    (errorcode == 0) ? "RDB transfer child aborted"
                                     : strerror(errorcode));
                freeClient(slave);
            } else {
                redisLog(REDIS_WARNING,
                "Slave %s correctly received the streamed RDB file.",
                    replicationGetSlaveName(slave));
                /* Restore the socket as non-blocking. */
                anetNonBlock(NULL,slave->fd);
                anetSendTimeout(NULL,slave->fd,0);
            }
        }
    }
    zfree(ok_slaves);

    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? REDIS_OK : REDIS_ERR, REDIS_RDB_CHILD_TYPE_SOCKET);
}

/* When a background RDB saving/transfer terminates, call the right handler. */
// ��BGSAVE ���RDB�ļ���Ҫô���͸��ӽڵ㣬Ҫô���浽���̣�������ȷ�Ĵ���
void backgroundSaveDoneHandler(int exitcode, int bysignal) {
    switch(server.rdb_child_type) {
    case REDIS_RDB_CHILD_TYPE_DISK:
        backgroundSaveDoneHandlerDisk(exitcode,bysignal);
        break;
    case REDIS_RDB_CHILD_TYPE_SOCKET:
        backgroundSaveDoneHandlerSocket(exitcode,bysignal);
        break;
    default:
        redisPanic("Unknown RDB child type.");
        break;
    }
}

/* Spawn an RDB child that writes the RDB to the sockets of the slaves
 * that are currently in REDIS_REPL_WAIT_BGSAVE_START state. */
// forkһ���ӽ��̽�rdbд��״̬Ϊ�ȴ�BGSAVE��ʼ�Ĵӽڵ��socket��
int rdbSaveToSlavesSockets(void) {
    int *fds;
    uint64_t *clientids;
    int numfds;
    listNode *ln;
    listIter li;
    pid_t childpid;
    PORT_LONGLONG start;
    int pipefds[2];

    if (server.rdb_child_pid != -1) return REDIS_ERR;

    /* Before to fork, create a pipe that will be used in order to
     * send back to the parent the IDs of the slaves that successfully
     * received all the writes. */
    if (pipe(pipefds) == -1) return REDIS_ERR;
    server.rdb_pipe_read_result_from_child = pipefds[0];
    server.rdb_pipe_write_result_to_parent = pipefds[1];

    /* Collect the file descriptors of the slaves we want to transfer
     * the RDB to, which are i WAIT_BGSAVE_START state. */
    fds = zmalloc(sizeof(int)*listLength(server.slaves));
    /* We also allocate an array of corresponding client IDs. This will
     * be useful for the child process in order to build the report
     * (sent via unix pipe) that will be sent to the parent. */
    clientids = zmalloc(sizeof(uint64_t)*listLength(server.slaves));
    numfds = 0;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;

        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) {
            clientids[numfds] = slave->id;
            fds[numfds++] = slave->fd;
            replicationSetupSlaveForFullResync(slave,getPsyncInitialOffset());
            /* Put the socket in non-blocking mode to simplify RDB transfer.
             * We'll restore it when the children returns (since duped socket
             * will share the O_NONBLOCK attribute with the parent). */
            anetBlock(NULL,slave->fd);
            anetSendTimeout(NULL,slave->fd,server.repl_timeout*1000);
        }
    }

    /* Create the child process. */
    start = ustime();

#ifdef _WIN32
    childpid = BeginForkOperation_Socket(fds, numfds, clientids, pipefds[1], &server, sizeof(server), dictGetHashFunctionSeed());
#else
    if ((childpid = fork()) == 0) {
        /* Child */
        int retval;
        rio slave_sockets;

        rioInitWithFdset(&slave_sockets,fds,numfds);
        zfree(fds);

        closeListeningSockets(0);
        redisSetProcTitle("redis-rdb-to-slaves");

        retval = rdbSaveRioWithEOFMark(&slave_sockets,NULL);
        if (retval == REDIS_OK && rioFlush(&slave_sockets) == 0)
            retval = REDIS_ERR;

        if (retval == REDIS_OK) {
            size_t private_dirty = zmalloc_get_private_dirty();

            if (private_dirty) {
                redisLog(REDIS_NOTICE,
                    "RDB: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }

            /* If we are returning OK, at least one slave was served
             * with the RDB file as expected, so we need to send a report
             * to the parent via the pipe. The format of the message is:
             *
             * <len> <slave[0].id> <slave[0].error> ...
             *
             * len, slave IDs, and slave errors, are all uint64_t integers,
             * so basically the reply is composed of 64 bits for the len field
             * plus 2 additional 64 bit integers for each entry, for a total
             * of 'len' entries.
             *
             * The 'id' represents the slave's client ID, so that the master
             * can match the report with a specific slave, and 'error' is
             * set to 0 if the replication process terminated with a success
             * or the error code if an error occurred. */
            void *msg = zmalloc(sizeof(uint64_t)*(1+2*numfds));
            uint64_t *len = msg;
            uint64_t *ids = len+1;
            int j, msglen;

            *len = numfds;
            for (j = 0; j < numfds; j++) {
                *ids++ = clientids[j];
                *ids++ = slave_sockets.io.fdset.state[j];
            }

            /* Write the message to the parent. If we have no good slaves or
             * we are unable to transfer the message to the parent, we exit
             * with an error so that the parent will abort the replication
             * process with all the childre that were waiting. */
            msglen = sizeof(uint64_t)*(1+2*numfds);
            if (*len == 0 ||
                write(server.rdb_pipe_write_result_to_parent,msg,msglen)
                != msglen)
            {
                retval = REDIS_ERR;
            }
            zfree(msg);
        }
        zfree(clientids);
        exitFromChild((retval == REDIS_OK) ? 0 : 1);
    } else {
#endif
        /* Parent */
        server.stat_fork_time = ustime()-start;
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);
        if (childpid == -1) {
            redisLog(REDIS_WARNING,"Can't save in background: fork: %s",
                strerror(errno));

            /* Undo the state change. The caller will perform cleanup on
             * all the slaves in BGSAVE_START state, but an early call to
             * replicationSetupSlaveForFullResync() turned it into BGSAVE_END */
            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                redisClient *slave = ln->value;
                int j;

                for (j = 0; j < numfds; j++) {
                    if (slave->id == clientids[j]) {
                        slave->replstate = REDIS_REPL_WAIT_BGSAVE_START;
                        break;
                    }
                }
            }
            close(pipefds[0]);
            close(pipefds[1]);
        } else {
            redisLog(REDIS_NOTICE,"Background RDB transfer started by pid %d",
                childpid);
            server.rdb_save_time_start = time(NULL);
            server.rdb_child_pid = childpid;
            server.rdb_child_type = REDIS_RDB_CHILD_TYPE_SOCKET;
            updateDictResizePolicy();
        }
        zfree(clientids);
        zfree(fds);
        return (childpid == -1) ? REDIS_ERR : REDIS_OK;
#ifndef _WIN32
    }
#endif
    return REDIS_OK; /* unreached */
}
// SAVE ����ʵ��
void saveCommand(redisClient *c) {
    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");
        return;
    }
    if (rdbSave(server.rdb_filename) == REDIS_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}
// BGSAVE ����ʵ��
void bgsaveCommand(redisClient *c) {
    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");
    } else if (server.aof_child_pid != -1) {
        addReplyError(c,"Can't BGSAVE while AOF log rewriting is in progress");
    } else if (rdbSaveBackground(server.rdb_filename) == REDIS_OK) {
        addReplyStatus(c,"Background saving started");
    } else {
        addReply(c,shared.err);
    }
}