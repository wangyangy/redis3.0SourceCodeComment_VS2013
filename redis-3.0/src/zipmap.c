
/* Memory layout of a zipmap, for the map "foo" => "bar", "hello" => "world":
 *
 * <zmlen><len>"foo"<len><free>"bar"<len>"hello"<len><free>"world"
 *
 * <zmlen> is 1 byte length that holds the current size of the zipmap.
 * When the zipmap length is greater than or equal to 254, this value
 * is not used and the zipmap needs to be traversed to find out the length.
 *
 * <len> is the length of the following string (key or value).
 * <len> lengths are encoded in a single value or in a 5 bytes value.
 * If the first byte value (as an unsigned 8 bit value) is between 0 and
 * 253, it's a single-byte length. If it is 254 then a four bytes unsigned
 * integer follows (in the host byte ordering). A value of 255 is used to
 * signal the end of the hash.
 *
 * <free> is the number of free unused bytes after the string, resulting
 * from modification of values associated to a key. For instance if "foo"
 * is set to "bar", and later "foo" will be set to "hi", it will have a
 * free byte to use if the value will enlarge again later, or even in
 * order to add a key/value pair if it fits.
 *
 * <free> is always an unsigned 8 bit number, because if after an
 * update operation there are more than a few free bytes, the zipmap will be
 * reallocated to make sure it is as small as possible.
 *
 * The most compact representation of the above two elements hash is actually:
 *
 * "\x02\x03foo\x03\x00bar\x05hello\x05\x00world\xff"
 *
 * Note that because keys and values are prefixed length "objects",
 * the lookup will take O(N) where N is the number of elements
 * in the zipmap and *not* the number of bytes needed to represent the zipmap.
 * This lowers the constant times considerably.
 */

#include <stdio.h>
#include <string.h>
#include "zmalloc.h"
#include "endianconv.h"
#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#include "Win32_Interop/Win32_FDAPI.h"
#endif

#define ZIPMAP_BIGLEN 254
#define ZIPMAP_END 255

/* The following defines the max value for the <free> field described in the
 * comments above, that is, the max number of trailing bytes in a value. */
#define ZIPMAP_VALUE_MAX_FREE 4

/* The following macro returns the number of bytes needed to encode the length
 * for the integer value _l, that is, 1 byte for lengths < ZIPMAP_BIGLEN and
 * 5 bytes for all the other lengths. */
/*返回编码_l需要的字节数, 要么1要么5, 如果_l<254则使用一个字节,否则使用5个字节*/
#define ZIPMAP_LEN_BYTES(_l) (((_l) < ZIPMAP_BIGLEN) ? 1 : sizeof(unsigned int)+1)

/* 创建一个空zipmap. */
unsigned char *zipmapNew(void) {
    unsigned char *zm = zmalloc(2);

    zm[0] = 0; /* Length */
    zm[1] = ZIPMAP_END;
    return zm;
}

/* 解码p,返回(key)长度 */
static unsigned int zipmapDecodeLength(unsigned char *p) {
    unsigned int len = *p;

    if (len < ZIPMAP_BIGLEN) return len;
    memcpy(&len,p+1,sizeof(unsigned int));
    memrev32ifbe(&len);
    return len;
}

/* 编码len(klen或vlen)并写入p中
 * 如果p为空直接返货编码len需要的字节数 */
static unsigned int zipmapEncodeLength(unsigned char *p, unsigned int len) {
    if (p == NULL) {
		//直接返回需要的字节数
        return ZIPMAP_LEN_BYTES(len);
    } else {
        if (len < ZIPMAP_BIGLEN) {
            p[0] = len;  //设置值
            return 1;    //返回字节数
        } else {
            p[0] = ZIPMAP_BIGLEN;  //设置成固定值ZIPMAP_BIGLEN,表示用后面的4个字节编码
            memcpy(p+1,&len,sizeof(len)); //设置值
            memrev32ifbe(p+1);
            return 1+sizeof(len);   //返回字节数
        }
    }
}

/* Search for a matching key, returning a pointer to the entry inside the
 * zipmap. Returns NULL if the key is not found.
  搜索匹配的key,返回元素的指针,如果没有返回空*/
static unsigned char *zipmapLookupRaw(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned int *totlen) {
    unsigned char *p = zm+1, *k = NULL;
    unsigned int l,llen;
	//不到结尾
    while(*p != ZIPMAP_END) {
        unsigned char free;

        /* 得到key_len */
        l = zipmapDecodeLength(p);
		//得到key_len占用的字节数
        llen = zipmapEncodeLength(NULL,l);
		//比较
        if (key != NULL && k == NULL && l == klen && !memcmp(p+llen,key,l)) {
            /* totlen==null表示不需要得到整个zipmap占用的字节数 */
            if (totlen != NULL) {
                k = p;
            } else {
                return p;
            }
        }
		//后移
        p += llen+l;
        /* Skip the value as well */
        l = zipmapDecodeLength(p);
        p += zipmapEncodeLength(NULL,l);
        free = p[0];
        p += l+1+free; /* +1 to skip the free byte */
    }
	//totlen记录了zipmap占用的字节数
    if (totlen != NULL) *totlen = (unsigned int)(p-zm)+1;
    return k;
}

/*返回一个节点需要的字节数*/
static unsigned int zipmapRequiredLength(unsigned int klen, unsigned int vlen) { WIN_PORT_FIX /* long -> int */
    unsigned int l;
    l = klen+vlen+3;
    if (klen >= ZIPMAP_BIGLEN) l += 4;
    if (vlen >= ZIPMAP_BIGLEN) l += 4;
    return l;
}

/* 返回一个键占用的总空间,编码长度占用空间和数据占用空间(encoded length + payload) */
static unsigned int zipmapRawKeyLength(unsigned char *p) {
    unsigned int l = zipmapDecodeLength(p);
    return zipmapEncodeLength(NULL,l) + l;
}

/* 返回一个值占用的总空间,编码长度空间和数据空间
 * (encoded length + single byte free count + payload) */
static unsigned int zipmapRawValueLength(unsigned char *p) {
    unsigned int l = zipmapDecodeLength(p);
    unsigned int used;

    used = zipmapEncodeLength(NULL,l);
    used += p[used] + 1 + l;
    return used;
}

/* 返回一个键值对占用的空间(entry = key + associated value + trailing
 * free space if any). */
static unsigned int zipmapRawEntryLength(unsigned char *p) {
    unsigned int l = zipmapRawKeyLength(p); //键的总空间
    return l + zipmapRawValueLength(p+l);   //加上值的总空间
}

/*重新调整zipmap的空间*/
static inline unsigned char *zipmapResize(unsigned char *zm, unsigned int len) {
    zm = zrealloc(zm, len);
    zm[len-1] = ZIPMAP_END;
    return zm;
}

/* Set key to value, creating the key if it does not already exist.
 * If 'update' is not NULL, *update is set to 1 if the key was
 * already preset, otherwise to 0. */
/**/
unsigned char *zipmapSet(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned char *val, unsigned int vlen, int *update) {
    unsigned int zmlen, offset;
	//reqlen一个键值对需要的内存空间
    unsigned int freelen, reqlen = (unsigned int)zipmapRequiredLength(klen,vlen); /* WIN_PORT_FIX cast (unsigned int) */
    unsigned int empty, vempty;
    unsigned char *p;

    freelen = reqlen;
    if (update) *update = 0;
	//首先在zipmap中查找key
    p = zipmapLookupRaw(zm,key,klen,&zmlen);
	//zipmap中不存在key
    if (p == NULL) {
        /* 扩容 */
        zm = zipmapResize(zm, zmlen+reqlen);
        p = zm+zmlen-1;
        zmlen = zmlen+reqlen;

        /* Increase zipmap length (this is an insert) */
        if (zm[0] < ZIPMAP_BIGLEN) zm[0]++;
	//zipmap中找到了key则更新其value
    } else {
        /* Key found. Is there enough space for the new value? */
        /* Compute the total length: */
        if (update) *update = 1;
        freelen = zipmapRawEntryLength(p);
		//如果空闲空间不足
        if (freelen < reqlen) {
            /* Store the offset of this key within the current zipmap, so
             * it can be resized. Then, move the tail backwards so this
             * pair fits at the current position. */
            offset = (unsigned int)(p-zm);
			//扩容
            zm = zipmapResize(zm, zmlen-freelen+reqlen);
            p = zm+offset;

            /* 将内容复制到指定位置 */
            memmove(p+reqlen, p+freelen, zmlen-(offset+freelen+1));
            zmlen = zmlen-freelen+reqlen;
            freelen = reqlen;
        }
    }

    /*将当前key-value后面的内容向后移动,预留空间 */
    empty = freelen-reqlen;
    if (empty >= ZIPMAP_VALUE_MAX_FREE) {
        /* First, move the tail <empty> bytes to the front, then resize
         * the zipmap to be <empty> bytes smaller. */
        offset = (unsigned int)(p-zm);
        memmove(p+reqlen, p+freelen, zmlen-(offset+freelen+1));
        zmlen -= empty;
        zm = zipmapResize(zm, zmlen);
        p = zm+offset;
        vempty = 0;
    } else {
        vempty = empty;
    }

	/*向zipmap中写入key-value*/
    p += zipmapEncodeLength(p,klen); //编码并写入keylen
    memcpy(p,key,klen); //写入key
    p += klen;
    /* Value: */
    p += zipmapEncodeLength(p,vlen);  //编码并写入valuelen
    *p++ = vempty;     
    memcpy(p,val,vlen);    //写入value
    return zm;
}

/* 删除指定的key */
unsigned char *zipmapDel(unsigned char *zm, unsigned char *key, unsigned int klen, int *deleted) {
    unsigned int zmlen, freelen;
	//搜索key
    unsigned char *p = zipmapLookupRaw(zm,key,klen,&zmlen);
	//如果找到该对象
    if (p) {
		//计算该对象的空间
        freelen = zipmapRawEntryLength(p);
		//移动,覆盖删除的元素
        memmove(p, p+freelen, zmlen-((p-zm)+freelen+1));
		//重新分配空间
        zm = zipmapResize(zm, zmlen-freelen);

        /* Decrease zipmap length */
        if (zm[0] < ZIPMAP_BIGLEN) zm[0]--;

        if (deleted) *deleted = 1;
    } else {
        if (deleted) *deleted = 0;
    }
    return zm;
}

/* 迭代之前调用,重置游标.Call before iterating through elements via zipmapNext() */
unsigned char *zipmapRewind(unsigned char *zm) {
    return zm+1;
}

/* 迭代的时候使用该函数
 * 获得首地址
 * unsigned char *i = zipmapRewind(my_zipmap);
 * 向后迭代
 * while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
 *     printf("%d bytes key at $p\n", klen, key);
 *     printf("%d bytes value at $p\n", vlen, value);
 * }
 */
unsigned char *zipmapNext(unsigned char *zm, unsigned char **key, unsigned int *klen, unsigned char **value, unsigned int *vlen) {
    //没有元素直接返回
	if (zm[0] == ZIPMAP_END) return NULL;
	//获取key
    if (key) {
        *key = zm;
        *klen = zipmapDecodeLength(zm);
        *key += ZIPMAP_LEN_BYTES(*klen);
    }
	//根据编码长度向后移动
    zm += zipmapRawKeyLength(zm);
	//获取值
    if (value) {
        *value = zm+1;
        *vlen = zipmapDecodeLength(zm);
        *value += ZIPMAP_LEN_BYTES(*vlen);
    }
	//向后移动整个键值对大小的内存
    zm += zipmapRawValueLength(zm);
    return zm;
}

/* 搜索key,如果找到返回1,没有找到返回0,将找到的键值对存在在参数中 */
int zipmapGet(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned char **value, unsigned int *vlen) {
    unsigned char *p;
	//搜索key
    if ((p = zipmapLookupRaw(zm,key,klen,NULL)) == NULL) return 0;
    p += zipmapRawKeyLength(p);
	//获取value
    *vlen = zipmapDecodeLength(p);
    *value = p + ZIPMAP_LEN_BYTES(*vlen) + 1;
    return 1;
}

/* 判断key是否存在 */
int zipmapExists(unsigned char *zm, unsigned char *key, unsigned int klen) {
    return zipmapLookupRaw(zm,key,klen,NULL) != NULL;
}

/* 返回zipmap的长度 */
unsigned int zipmapLen(unsigned char *zm) {
    unsigned int len = 0;
	//如果元素数目小于254则直接返回zm[0],zm[0]存储着元素数目
    if (zm[0] < ZIPMAP_BIGLEN) {
        len = zm[0];
	//如果元素数目大于254则需要迭代zipmap来获取元素数量
    } else {
        unsigned char *p = zipmapRewind(zm);
        while((p = zipmapNext(p,NULL,NULL,NULL,NULL)) != NULL) len++;

        /* Re-store length if small enough */
        if (len < ZIPMAP_BIGLEN) zm[0] = len;
    }
    return len;
}

/* 返回zipmap的内存大小 */
size_t zipmapBlobLen(unsigned char *zm) {
    unsigned int totlen;
    zipmapLookupRaw(zm,NULL,0,&totlen);
    return totlen;
}

#ifdef ZIPMAP_TEST_MAIN
void zipmapRepr(unsigned char *p) {
    unsigned int l;

    printf("{status %u}",*p++);
    while(1) {
        if (p[0] == ZIPMAP_END) {
            printf("{end}");
            break;
        } else {
            unsigned char e;

            l = zipmapDecodeLength(p);
            printf("{key %u}",l);
            p += zipmapEncodeLength(NULL,l);
            if (l != 0 && fwrite(p,l,1,stdout) == 0) perror("fwrite");
            p += l;

            l = zipmapDecodeLength(p);
            printf("{value %u}",l);
            p += zipmapEncodeLength(NULL,l);
            e = *p++;
            if (l != 0 && fwrite(p,l,1,stdout) == 0) perror("fwrite");
            p += l+e;
            if (e) {
                printf("[");
                while(e--) printf(".");
                printf("]");
            }
        }
    }
    printf("\n");
}

int main(void) {
    unsigned char *zm;

    zm = zipmapNew();

    zm = zipmapSet(zm,(unsigned char*) "name",4, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "surname",7, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "age",3, (unsigned char*) "foo",3,NULL);
    zipmapRepr(zm);

    zm = zipmapSet(zm,(unsigned char*) "hello",5, (unsigned char*) "world!",6,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "bar",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "!",1,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "12345",5,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "new",3, (unsigned char*) "xx",2,NULL);
    zm = zipmapSet(zm,(unsigned char*) "noval",5, (unsigned char*) "",0,NULL);
    zipmapRepr(zm);
    zm = zipmapDel(zm,(unsigned char*) "new",3,NULL);
    zipmapRepr(zm);

    printf("\nLook up large key:\n");
    {
        unsigned char buf[512];
        unsigned char *value;
        unsigned int vlen, i;
        for (i = 0; i < 512; i++) buf[i] = 'a';

        zm = zipmapSet(zm,buf,512,(unsigned char*) "long",4,NULL);
        if (zipmapGet(zm,buf,512,&value,&vlen)) {
            printf("  <long key> is associated to the %d bytes value: %.*s\n",
                vlen, vlen, value);
        }
    }

    printf("\nPerform a direct lookup:\n");
    {
        unsigned char *value;
        unsigned int vlen;

        if (zipmapGet(zm,(unsigned char*) "foo",3,&value,&vlen)) {
            printf("  foo is associated to the %d bytes value: %.*s\n",
                vlen, vlen, value);
        }
    }
    printf("\nIterate through elements:\n");
    {
        unsigned char *i = zipmapRewind(zm);
        unsigned char *key, *value;
        unsigned int klen, vlen;

        while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
            printf("  %d:%.*s => %d:%.*s\n", klen, klen, key, vlen, vlen, value);
        }
    }
    return 0;
}
#endif
