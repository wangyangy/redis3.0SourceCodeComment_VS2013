/* 
* ----------------------------------------------------------------------------
*
* ZIPLIST OVERALL LAYOUT:
* The general layout of the ziplist is as follows:
* <zlbytes><zltail><zllen><entry><entry><zlend>
*
* <zlbytes> is an unsigned integer to hold the number of bytes that the
* ziplist occupies. This value needs to be stored to be able to resize the
* entire structure without the need to traverse it first.
*
* <zltail> is the offset to the last entry in the list. This allows a pop
* operation on the far side of the list without the need for full traversal.
*
* <zllen> is the number of entries.When this value is larger than 2**16-2,
* we need to traverse the entire list to know how many items it holds.
*
* <zlend> is a single byte special value, equal to 255, which indicates the
* end of the list.
*
* ZIPLIST ENTRIES:
* Every entry in the ziplist is prefixed by a header that contains two pieces
* of information. First, the length of the previous entry is stored to be
* able to traverse the list from back to front. Second, the encoding with an
* optional string length of the entry itself is stored.
*
* The length of the previous entry is encoded in the following way:
* If this length is smaller than 254 bytes, it will only consume a single
* byte that takes the length as value. When the length is greater than or
* equal to 254, it will consume 5 bytes. The first byte is set to 254 to
* indicate a larger value is following. The remaining 4 bytes take the
* length of the previous entry as value.
*
* The other header field of the entry itself depends on the contents of the
* entry. When the entry is a string, the first 2 bits of this header will hold
* the type of encoding used to store the length of the string, followed by the
* actual length of the string. When the entry is an integer the first 2 bits
* are both set to 1. The following 2 bits are used to specify what kind of
* integer will be stored after this header. An overview of the different
* types and encodings is as follows:
*
* |00pppppp| - 1 byte
*      String value with length less than or equal to 63 bytes (6 bits).
* |01pppppp|qqqqqqqq| - 2 bytes
*      String value with length less than or equal to 16383 bytes (14 bits).
* |10______|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt| - 5 bytes
*      String value with length greater than or equal to 16384 bytes.
* |11000000| - 1 byte(表示数字占用后面2字节,即数字用后面2个字节来编码)
*      Integer encoded as int16_t (2 bytes).
* |11010000| - 1 byte(表示数字占用后面4字节)
*      Integer encoded as int32_t (4 bytes).
* |11100000| - 1 byte(表示数字占用后面8字节)
*      Integer encoded as int64_t (8 bytes).
* |11110000| - 1 byte(表示数字占用后面3字节)
*      Integer encoded as 24 bit signed (3 bytes).
* |11111110| - 1 byte(表示数字占用后面1字节)
*      Integer encoded as 8 bit signed (1 byte).
* |1111xxxx| - (with xxxx between 0000 and 1101) immediate 4 bit integer.
*      Unsigned integer from 0 to 12. The encoded value is actually from
*      1 to 13 because 0000 and 1111 can not be used, so 1 should be
*      subtracted from the encoded 4 bit value to obtain the right value.
* |11111111| - End of ziplist.
*
* All the integers are represented in little endian byte order.
*
* ----------------------------------------------------------------------------

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "zmalloc.h"
#include "util.h"
#include "ziplist.h"
#include "endianconv.h"
#include "redisassert.h"

#ifdef _WIN32
#include "Win32_Interop/Win32_FDAPI.h"
#endif

//结尾标识符
#define ZIP_END 255  
#define ZIP_BIGLEN 254

//0xc0:11000000
#define ZIP_STR_MASK 0xc0
//0x30:00110000
#define ZIP_INT_MASK 0x30
//00000000
#define ZIP_STR_06B (0 << 6)
//01000000
#define ZIP_STR_14B (1 << 6)
//10000000
#define ZIP_STR_32B (2 << 6)
//相当于加法:11000000
#define ZIP_INT_16B (0xc0 | 0<<4)
//11010000
#define ZIP_INT_32B (0xc0 | 1<<4)
//11100000
#define ZIP_INT_64B (0xc0 | 2<<4)
//11110000
#define ZIP_INT_24B (0xc0 | 3<<4)
//11111110
#define ZIP_INT_8B 0xfe
/* 4 bit integer immediate encoding */
#define ZIP_INT_IMM_MASK 0x0f
#define ZIP_INT_IMM_MIN 0xf1    /* 11110001 */
#define ZIP_INT_IMM_MAX 0xfd    /* 11111101 */
#define ZIP_INT_IMM_VAL(v) (v & ZIP_INT_IMM_MASK)

#define INT24_MAX 0x7fffff
#define INT24_MIN (-INT24_MAX - 1)

/* 判断是否是字符串编码 */
#define ZIP_IS_STR(enc) (((enc) & ZIP_STR_MASK) < ZIP_STR_MASK)

/*
zl是char*类型的, (*((uint32_t*)(zl))) 先对char *类型的zl进行强制类型转换成uint32_t *类型，
然后在用*运算符进行取内容运算，此时zl能访问的内存大小为4个字节.将zl定位到前4个字节用来存储整个压缩列表的内存数
*/
#define ZIPLIST_BYTES(zl)       (*((uint32_t*)(zl)))
//将zl定位到4字节到8字节的tail_offset成员，记录着压缩列表尾节点距离列表的起始地址的偏移字节量
#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t*)((zl)+sizeof(uint32_t))))
//将zl定位到8字节到10字节的length成员，记录着压缩列表的节点数量
#define ZIPLIST_LENGTH(zl)      (*((uint16_t*)((zl)+sizeof(uint32_t)*2)))
//压缩列表表头（以上三个属性）的大小10个字节
#define ZIPLIST_HEADER_SIZE     (sizeof(uint32_t)*2+sizeof(uint16_t))
//压缩列表首节点的地址
#define ZIPLIST_ENTRY_HEAD(zl)  ((zl)+ZIPLIST_HEADER_SIZE)
//压缩列表尾节点的地址
#define ZIPLIST_ENTRY_TAIL(zl)  ((zl)+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)))
//end成员的地址，一个字节。
#define ZIPLIST_ENTRY_END(zl)   ((zl)+intrev32ifbe(ZIPLIST_BYTES(zl))-1)

/* 增加ziplist节点数 */
#define ZIPLIST_INCR_LENGTH(zl,incr) { \
if (ZIPLIST_LENGTH(zl) < UINT16_MAX) \
	ZIPLIST_LENGTH(zl) = intrev16ifbe(intrev16ifbe(ZIPLIST_LENGTH(zl)) + incr); \
}

/*压缩列表节点结构,但不是这个结构存储数据*/
typedef struct zlentry {
	//prevrawlen前驱节点长度,prevrawlensize编码前前驱节点长度所需要的字节数
	unsigned int prevrawlensize, prevrawlen;
	//len当前节点值长度,lensize编码当前节点值长度len所需要的字节数
	unsigned int lensize, len;
	//当前节点header的大小=lensize+prevrawlensize
	unsigned int headersize;
	//当前节点的编码格式
	unsigned char encoding;
	//指向当前节点的指针
	unsigned char *p;
} zlentry;

/* 从指针ptr中取出编码的值保存到encoding中去 */
#define ZIP_ENTRY_ENCODING(ptr, encoding) do {					 \
	(encoding) = (ptr[0]);										 \
	if ((encoding) < ZIP_STR_MASK) (encoding) &= ZIP_STR_MASK;   \
} while (0)

/* 返回保存encoding编码的值所需的字节数量 */
static unsigned int zipIntSize(unsigned char encoding) {
	switch (encoding) {
	case ZIP_INT_8B:  return 1;
	case ZIP_INT_16B: return 2;
	case ZIP_INT_24B: return 3;
	case ZIP_INT_32B: return 4;
	case ZIP_INT_64B: return 8;
	default: return 0; /* 4 bit immediate */
	}
	assert(NULL);
	return 0;
}
/*
Ziplist 是为了尽可能节约内存而设计双端队列,
Ziplist 能存储strings和integer值，整型值被存储为实际的整型值而不是字符数组。
Ziplist 在头部和尾部的操作时间0（1），ziplist的操作都需要重新分配内存，所以
实际的复杂度和ziplist的使用内存有关。

* The general layout of the ziplist is as follows:
* <zlbytes>  <zltail>  <zllen> <entry> <entry> ......  <entry><zlend>
* |-----ziplist header--------|----------entry---------------|--end--|
* zlbytes: 4字节，是一个无符号整数，保存着 ziplist 使用的内存数量。
* zltail:  4字节，保存着到达列表中最后一个节点的偏移量。
*           这个偏移量使得对表尾的 pop 操作可以在无须遍历整个列表的情况下进行。
* zllen: 2字节，保存着列表中的节点数量。当 zllen 保存的值大于 2**16-2 时，
*        程序需要遍历整个列表才能知道列表实际包含了多少个节点。
* zlend: 1字节，值为 255 ，标识列表的末尾。
*
*/

/* 函数返回值是编码rawlen需要的字节数,即len,编码有好几种规则,大类是字符串和整数,字符串有3中编码方式分别对应不同的数据长度,
   整数有多种编码方式 */
static unsigned int zipEncodeLength(unsigned char *p, unsigned char encoding, unsigned int rawlen) {
	unsigned char len = 1, buf[5];
	//如果是字符串编码
	if (ZIP_IS_STR(encoding)) {
		/* 如果数据长度小于等于63,只需要1字节编码(前面的00是固定的) */
		if (rawlen <= 0x3f) {  //63
			//p为空则直接返回len
			if (!p) return len; //len=1
			buf[0] = ZIP_STR_06B | rawlen;//buf[0]就表示长度
		}
		//数据的长度大于63小于2^14,此时需要2字节编码(len=2)(前面的01是固定的)
		else if (rawlen <= 0x3fff) {  //16383
			len += 1;
			if (!p) return len;
			buf[0] = ZIP_STR_14B | ((rawlen >> 8) & 0x3f);
			buf[1] = rawlen & 0xff;
		}
		//数据长度大于2^14,此时需要5字节编码(len=5)
		else {
			len += 4;
			if (!p) return len;
			buf[0] = ZIP_STR_32B; //如果是用5个字节存储,则直接将第一个字节设置为10000000,表示一个entry的长度用后面四个字节表示
			buf[1] = (rawlen >> 24) & 0xff;
			buf[2] = (rawlen >> 16) & 0xff;
			buf[3] = (rawlen >> 8) & 0xff;
			buf[4] = rawlen & 0xff;
		}
	}
	//如果是整数编码,通过最前面的注释可以知道,整数编码长度是固定的就是1
	else {
		/* Implies integer encoding, so length is always 1. */
		if (!p) return len;
		buf[0] = encoding;  //直接设置整数的编码(固定的)
	}

	/* 将存储信息复制到p中 */
	memcpy(p, buf, len);
	return len;
}

/*  解码ptr指针,取出列表节点的相关信息,保存在encoding,lensize,len变量中,整型变量是固定的1
	所以lendize=1,len值根据编码规则进行位运算得到,这个函数和zipEncodeLength正好相反
*/
#define ZIP_DECODE_LENGTH(ptr, encoding, lensize, len) do {                    \
    ZIP_ENTRY_ENCODING((ptr), (encoding));									   \
    if ((encoding) < ZIP_STR_MASK) {                                           \
        if ((encoding) == ZIP_STR_06B) {                                       \
            (lensize) = 1;                                                     \
            (len) = (ptr)[0] & 0x3f;                                           \
        } else if ((encoding) == ZIP_STR_14B) {                                \
            (lensize) = 2;                                                     \
            (len) = (((ptr)[0] & 0x3f) << 8) | (ptr)[1];                       \
        } else if (encoding == ZIP_STR_32B) {                                  \
            (lensize) = 5;                                                     \
            (len) = ((ptr)[1] << 24) |                                         \
                    ((ptr)[2] << 16) |                                         \
                    ((ptr)[3] <<  8) |                                         \
                    ((ptr)[4]);                                                \
        } else {                                                               \
            assert(NULL);                                                      \
        }                                                                      \
    } else {                                                                   \
        (lensize) = 1;                                                         \
        (len) = zipIntSize(encoding);                                          \
    }                                                                          \
} while(0);


/* 编码前一个条目的长度并将其写入p.如果p为NULL，则返回编码此长度所需的字节数,要么1要么5。 */
static unsigned int zipPrevEncodeLength(unsigned char *p, unsigned int len) {
	//如果p为空,直接返回编码此长度需要的字节数
	if (p == NULL) {
		return (len < ZIP_BIGLEN) ? 1 : sizeof(len)+1;  //当len<ZIP_BIGLEN时只需要一个字节.sizeof(len)+1其实就是4+1=5
	}
	else {
		if (len < ZIP_BIGLEN) {
			p[0] = len;   //设置值
			return 1;     //返回编码长度
		}
		else {
			p[0] = ZIP_BIGLEN;         //第一个字节设置254,表示用后面4个字节编码,是有个标志
			memcpy(p + 1, &len, sizeof(len));  //设置值
			memrev32ifbe(p + 1);
			return 1 + sizeof(len);   //返回编码长度
		}
	}
}

/* Encode the length of the previous entry and write it to "p". This only
* uses the larger encoding (required in __ziplistCascadeUpdate). */
static void zipPrevEncodeLengthForceLarge(unsigned char *p, unsigned int len) {
	if (p == NULL) return;                //p为空直接返回
	p[0] = ZIP_BIGLEN;                    //第一个字节设置254,表示用后面4个字节编码,是有个标志
	memcpy(p + 1, &len, sizeof(len));     //设置值
	memrev32ifbe(p + 1);
}

/* 存储编码长度的字节只有两种:1字节或者5字节,并将值赋给prevlensize */
#define ZIP_DECODE_PREVLENSIZE(ptr, prevlensize) do {                          \
    if ((ptr)[0] < ZIP_BIGLEN) {                                               \
        (prevlensize) = 1;                                                     \
    } else {                                                                   \
        (prevlensize) = 5;                                                     \
    }                                                                          \
} while(0);

/* 根据prevlensize类型获得数据长度(prevlen) */
#define ZIP_DECODE_PREVLEN(ptr, prevlensize, prevlen) do {                     \
    ZIP_DECODE_PREVLENSIZE(ptr, prevlensize);                                  \
    if ((prevlensize) == 1) {                                                  \
        (prevlen) = (ptr)[0];                                                  \
    } else if ((prevlensize) == 5) {                                           \
        assert(sizeof((prevlensize)) == 4);                                    \
        memcpy(&(prevlen), ((char*)(ptr)) + 1, 4);                             \
        memrev32ifbe(&prevlen);                                                \
    }                                                                          \
} while(0);

/* 
 * 计算编码新的前置节点长度 len 所需的字节数，
 * 减去编码 p 原来的前置节点长度所需的字节数之差。
 */
static int zipPrevLenByteDiff(unsigned char *p, unsigned int len) {
	unsigned int prevlensize;
	//将p中编码长度的字节数赋给prevlensize
	ZIP_DECODE_PREVLENSIZE(p, prevlensize);
	//计算现在的长度减去原来已经有的长度
	return zipPrevEncodeLength(NULL, len) - prevlensize;
}

/* 返回指针p所指向的节点占用的字节数总和。 */
static unsigned int zipRawEntryLength(unsigned char *p) {
	unsigned int prevlensize, encoding, lensize, len;
	//记录前entry节点长度所用的字节数
	ZIP_DECODE_PREVLENSIZE(p, prevlensize);
	//得到当前节点存储信息(encoding和实际存储数据的字节数)
	ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
	return prevlensize + lensize + len;
}

/* 检测是否能将字符串存编码为整数类型,如果可以的话，将编码后的整数保存在指针v的值中，并将编码的方式保存在指针encoding的值中。 */
static int zipTryEncoding(unsigned char *entry, unsigned int entrylen, PORT_LONGLONG *v, unsigned char *encoding) {
	PORT_LONGLONG value;

	if (entrylen >= 32 || entrylen == 0) return 0;
	//如果字符串可以转化为整数
	if (string2ll((char*)entry, entrylen, &value)) {
		//下面就是根据数值大小进行编码
		//如果数值在[0,12]之间
		if (value >= 0 && value <= 12) {
			*encoding = (unsigned char)(ZIP_INT_IMM_MIN + value);                 WIN_PORT_FIX /* cast (unsigned char) */
		}
		else if (value >= INT8_MIN && value <= INT8_MAX) {
			*encoding = ZIP_INT_8B;
		}
		else if (value >= INT16_MIN && value <= INT16_MAX) {
			*encoding = ZIP_INT_16B;
		}
		else if (value >= INT24_MIN && value <= INT24_MAX) {
			*encoding = ZIP_INT_24B;
		}
		else if (value >= INT32_MIN && value <= INT32_MAX) {
			*encoding = ZIP_INT_32B;
		}
		else {
			*encoding = ZIP_INT_64B;
		}
		*v = value;
		return 1;
	}
	return 0;
}

/* 将一个整数根据编码类型存储到p中 */
static void zipSaveInteger(unsigned char *p, int64_t value, unsigned char encoding) {
	int16_t i16;
	int32_t i32;
	int64_t i64;
	if (encoding == ZIP_INT_8B) {
		((int8_t*)p)[0] = (int8_t)value;
	}
	else if (encoding == ZIP_INT_16B) {
		i16 = (int16_t)value;                                                   WIN_PORT_FIX /* cast (int16_t) */
			memcpy(p, &i16, sizeof(i16));
		memrev16ifbe(p);
	}
	else if (encoding == ZIP_INT_24B) {
		i32 = (int32_t)(value << 8);                                              WIN_PORT_FIX /* cast (int32_t) */
			memrev32ifbe(&i32);
		memcpy(p, ((uint8_t*)&i32) + 1, sizeof(i32)-sizeof(uint8_t));
	}
	else if (encoding == ZIP_INT_32B) {
		i32 = (int32_t)value;                                                   WIN_PORT_FIX /* cast (int32_t) */
			memcpy(p, &i32, sizeof(i32));
		memrev32ifbe(p);
	}
	else if (encoding == ZIP_INT_64B) {
		i64 = value;
		memcpy(p, &i64, sizeof(i64));
		memrev64ifbe(p);
	}
	else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
		/* Nothing to do, the value is stored in the encoding itself. */
	}
	else {
		assert(NULL);
	}
}

/* 根据编码类型从p中读取一个整数 */
static int64_t zipLoadInteger(unsigned char *p, unsigned char encoding) {
	int16_t i16;
	int32_t i32;
	int64_t i64, ret = 0;
	if (encoding == ZIP_INT_8B) {
		ret = ((int8_t*)p)[0];
	}
	else if (encoding == ZIP_INT_16B) {
		memcpy(&i16, p, sizeof(i16));
		memrev16ifbe(&i16);
		ret = i16;
	}
	else if (encoding == ZIP_INT_32B) {
		memcpy(&i32, p, sizeof(i32));
		memrev32ifbe(&i32);
		ret = i32;
	}
	else if (encoding == ZIP_INT_24B) {
		i32 = 0;
		memcpy(((uint8_t*)&i32) + 1, p, sizeof(i32)-sizeof(uint8_t));
		memrev32ifbe(&i32);
		ret = i32 >> 8;
	}
	else if (encoding == ZIP_INT_64B) {
		memcpy(&i64, p, sizeof(i64));
		memrev64ifbe(&i64);
		ret = i64;
	}
	else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
		ret = (encoding & ZIP_INT_IMM_MASK) - 1;
	}
	else {
		assert(NULL);
	}
	return ret;
}

/* 返回一个entry的详细信息(从char*中提取一个entry) */
static zlentry zipEntry(unsigned char *p) {
	zlentry e;

	ZIP_DECODE_PREVLEN(p, e.prevrawlensize, e.prevrawlen);
	ZIP_DECODE_LENGTH(p + e.prevrawlensize, e.encoding, e.lensize, e.len);
	e.headersize = e.prevrawlensize + e.lensize;
	e.p = p;
	return e;
}

/* 创建一个新的ziplist. */
unsigned char *ziplistNew(void) {
	unsigned int bytes = ZIPLIST_HEADER_SIZE + 1;  //ziplist头加上结束标识符的位数(11)
	unsigned char *zl = zmalloc(bytes);
	ZIPLIST_BYTES(zl) = intrev32ifbe(bytes);
	ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE);
	ZIPLIST_LENGTH(zl) = 0;
	zl[bytes - 1] = ZIP_END;
	return zl;
}

/* 重置ziplist的大小为len */
static unsigned char *ziplistResize(unsigned char *zl, unsigned int len) {
	zl = zrealloc(zl, len);
	ZIPLIST_BYTES(zl) = intrev32ifbe(len);
	zl[len - 1] = ZIP_END;
	return zl;
}

/* 
 * 当将一个新节点添加到某个节点之前的时候，
 * 如果原节点的header空间不足以保存新节点的长度，
 * 那么就需要对原节点的header空间进行扩展（从 1 字节扩展到 5 字节）。
 * 但是，当对原节点进行扩展之后，原节点的下一个节点的prevlen可能出现空间不足，
 * 这种情况在多个连续节点的长度都接近 ZIP_BIGLEN 时可能发生。
 * 这个函数就用于检查并修复后续节点的空间问题。
 * 注意，程序的检查是针对 p 的后续节点，而不是 p 所指向的节点。
 * 因为节点 p 在传入之前已经完成了所需的空间扩展工作
 */
static unsigned char *__ziplistCascadeUpdate(unsigned char *zl, unsigned char *p) {
	//curlen当前总的内存数
	size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), rawlen, rawlensize;
	size_t offset, noffset, extra;
	unsigned char *np;
	zlentry cur, next;
	//不是结尾
	while (p[0] != ZIP_END) {
		//从p中提取一个entry
		cur = zipEntry(p);
		//当前entry的总长度
		rawlen = cur.headersize + cur.len;
		//计算编码cur所需要的字节数
		rawlensize = zipPrevEncodeLength(NULL, (unsigned int)rawlen);            WIN_PORT_FIX /* cast (unsigned int) */

		/* 如果是结尾就终止 */
		if (p[rawlen] == ZIP_END) break;
		//下一个节点
		next = zipEntry(p + rawlen);

		/* 如果编码长度没有变化,则不用继续向后更新,直接终止 */
		if (next.prevrawlen == rawlen) break;
		//如果后面记录的之前的长度小于新的长度,就扩容
		if (next.prevrawlensize < rawlensize) {
			/* The "prevlen" field of "next" needs more bytes to hold
			* the raw length of "cur". */
			offset = p - zl;    //获取偏移量
			extra = rawlensize - next.prevrawlensize;   //获取应该增加的扩容量
			//重新调整大小
			zl = ziplistResize(zl, (unsigned int)(curlen + extra));                WIN_PORT_FIX /* cast (unsigned int) */
			p = zl + offset;  //调整位置

			/* Current pointer and offset for next element. */
			np = p + rawlen;
			noffset = np - zl;

			/* 更新 tail_offset成员*/
			if ((zl + intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))) != np) {
				ZIPLIST_TAIL_OFFSET(zl) =
					(uint32_t)intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + extra); WIN_PORT_FIX /* cast (uint32_t) */
			}

			/* 尾节点后移 */
			memmove(np + rawlensize,
				np + next.prevrawlensize,
				curlen - noffset - next.prevrawlensize - 1);
			//重新编码
			zipPrevEncodeLength(np, (unsigned int)rawlen);                       WIN_PORT_FIX /* cast (unsigned int) */

			/* Advance the cursor */
			p += rawlen;   //向后移动,继续更新
			curlen += extra;  //内存增加
		}
		//如果后面记录的之前的长度大于等于新的长度,就缩减容量
		else {
			if (next.prevrawlensize > rawlensize) {
				/* This would result in shrinking, which we want to avoid.
				* So, set "rawlen" in the available bytes. */
				zipPrevEncodeLengthForceLarge(p + rawlen, (unsigned int)rawlen);
			}
			//长度相等
			else {
				//重新编码一下,其实内容没有变化
				zipPrevEncodeLength(p + rawlen, (unsigned int)rawlen);
			}

			/* Stop here, as the raw length of "next" has not changed. */
			break;
		}
	}
	return zl;
}

/* 删除元素的函数,返回ziplist指针(删除的元素是第num个) */
static unsigned char *__ziplistDelete(unsigned char *zl, unsigned char *p, unsigned int num) {
	unsigned int i, totlen, deleted = 0;
	size_t offset;
	int nextdiff = 0;
	zlentry first, tail;
	//获取头结点
	first = zipEntry(p);
	//计算到num位置的指针
	for (i = 0; p[0] != ZIP_END && i < num; i++) {
		p += zipRawEntryLength(p);
		deleted++;
	}

	totlen = (unsigned int)(p - first.p);                                         WIN_PORT_FIX /* cast (unsigned int) */
	if (totlen > 0) {
		//没有到尾部
		if (p[0] != ZIP_END) {
			/* Storing `prevrawlen` in this entry may increase or decrease the
			* number of bytes required compare to the current `prevrawlen`.
			* There always is room to store this, because it was previously
			* stored by an entry that is now being deleted. */
			nextdiff = zipPrevLenByteDiff(p, first.prevrawlen);
			p -= nextdiff;
			zipPrevEncodeLength(p, first.prevrawlen);

			/* 更新tail_offset */
			ZIPLIST_TAIL_OFFSET(zl) =
				intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) - totlen);

			/* When the tail contains more than one entry, we need to take
			* "nextdiff" in account as well. Otherwise, a change in the
			* size of prevlen doesn't have an effect on the *tail* offset. */
			tail = zipEntry(p);
			if (p[tail.headersize + tail.len] != ZIP_END) {
				ZIPLIST_TAIL_OFFSET(zl) =
					intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + nextdiff);
			}

			/* 将尾节点移动到头部 */
			memmove(first.p, p,
				intrev32ifbe(ZIPLIST_BYTES(zl)) - (p - zl) - 1);
		}
		else {
			/* 删除整个尾节点,更新tail_offset,不需要调整内存 */
			ZIPLIST_TAIL_OFFSET(zl) =
				(unsigned int)intrev32ifbe((first.p - zl) - first.prevrawlen);      WIN_PORT_FIX /* cast (unsigned int) */
		}

		/* 调整大小更新长度 */
		offset = first.p - zl;
		zl = ziplistResize(zl, intrev32ifbe(ZIPLIST_BYTES(zl)) - totlen + nextdiff);
		ZIPLIST_INCR_LENGTH(zl, -deleted);
		p = zl + offset;

		/* 级联更新 */
		if (nextdiff != 0)
			zl = __ziplistCascadeUpdate(zl, p);
	}
	return zl;
}

/* 向指定位置插入一个条目,参数:压缩列表,插入位置,元素值,元素长度 */
static unsigned char *__ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
	//保存当前链表长度(内存数)
	size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), reqlen;
	unsigned int prevlensize, prevlen = 0;
	size_t offset;
	int nextdiff = 0;
	unsigned char encoding = 0;
	PORT_LONGLONG value = 123456789; /* initialized to avoid warning. Using a value
										that is easy to see if for some reason
										we use it uninitialized. */
	zlentry tail;

	/* 这段代码逻辑就是:获取前置元素的长度 */
	//如果插入位置不是尾元素,则直接获取元素长度
	if (p[0] != ZIP_END) {
		ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
	}
	//如果是插入尾部位置
	else {
		//获取尾节点
		unsigned char *ptail = ZIPLIST_ENTRY_TAIL(zl);
		if (ptail[0] != ZIP_END) {
			prevlen = zipRawEntryLength(ptail);
		}
	}

	/* 尝试进行整数编码 */
	if (zipTryEncoding(s, slen, &value, &encoding)) {
		/* 根据编码类型获取编码长度 */
		reqlen = zipIntSize(encoding);
	}
	else {
		/* 直接设置为字符串长度 */
		reqlen = slen;
	}
	/* reqlen是元素需要分配的内存空间大小,需要加上编码前置元素长度占用的空间和编码当前元素长度占用空间 */
	reqlen += zipPrevEncodeLength(NULL, (unsigned int)prevlen);                  WIN_PORT_FIX /* cast (unsigned int) */
	reqlen += zipEncodeLength(NULL, encoding, slen);

	/* 当插入位置不是尾部时需要计算出下一个元素保存本元素prevlen字段空间是否足够,不够计算欠缺的差值 */
	nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p, (unsigned int)reqlen) : 0; WIN_PORT_FIX /* cast (unsigned int) */

	/* realloc重新分配内存 */
	offset = p - zl;
	zl = ziplistResize(zl, (unsigned int)(curlen + reqlen + nextdiff));              WIN_PORT_FIX /* cast (unsigned int) */
		p = zl + offset;

	/* 更新tail_offset */
	if (p[0] != ZIP_END) {
		/* 移动原有位置和后面的内容到新的位置 */
		memmove(p + reqlen, p - nextdiff, curlen - offset - 1 + nextdiff);

		/* 更新下一个节点保存的待插入元素的prevlen */
		zipPrevEncodeLength(p + reqlen, (unsigned int)reqlen);                     WIN_PORT_FIX /* cast (unsigned int) */

		/* 更新tail_offset */
		ZIPLIST_TAIL_OFFSET(zl) =
		(uint32_t)intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + reqlen); WIN_PORT_FIX /* cast (uint32_t) */

		/* 修改尾部位置 */
		tail = zipEntry(p + reqlen);
		if (p[reqlen + tail.headersize + tail.len] != ZIP_END) {
			ZIPLIST_TAIL_OFFSET(zl) =
				intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + nextdiff);
		}
	}
	else {
		/* 插入的元素为新的尾节点 */
		ZIPLIST_TAIL_OFFSET(zl) = (uint32_t)intrev32ifbe(p - zl);                 WIN_PORT_FIX /* cast (uint32_) */
	}

	/* nextdiff值非0, 说明下一个元素需要扩展空间存放prevlen字段, 由于下一个元素空间变大, 有可能引起下下一个元素空间需要扩展, 下面函数检测后面元素, 并在需要时重置元素prevlen长度 */
	if (nextdiff != 0) {
		offset = p - zl;
		zl = __ziplistCascadeUpdate(zl, p + reqlen);
		p = zl + offset;
	}

	/* 根据不同编码插入元素 */
	p += zipPrevEncodeLength(p, (unsigned int)prevlen);                          WIN_PORT_FIX /* cast (unsigned int) */
	p += zipEncodeLength(p, encoding, slen);
	//如果是字符串
	if (ZIP_IS_STR(encoding)) {
		memcpy(p, s, slen);
	}
	//如果是整数
	else {
		zipSaveInteger(p, value, encoding);
	}
	//增加节点数
	ZIPLIST_INCR_LENGTH(zl, 1);
	return zl;    //返回ziplist指针
}

/* 插入元素,插入到头部或者尾部 */
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where) {
	unsigned char *p;
	p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);
	return __ziplistInsert(zl, p, s, slen);
}

/* 返回index位置的节点 */
unsigned char *ziplistIndex(unsigned char *zl, int index) {
	unsigned char *p;
	unsigned int prevlensize, prevlen = 0;
	//将传入的索引与0比较,比0大从头开始查找,比0小从尾部开始查找
	if (index < 0) {
		//从尾部开始查找
		index = (-index) - 1;
		p = ZIPLIST_ENTRY_TAIL(zl);
		if (p[0] != ZIP_END) {
			ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
			//不断取出prevlen向前查找
			while (prevlen > 0 && index--) {
				p -= prevlen;
				ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
			}
		}
	}
	//从头部开始查找
	else {
		p = ZIPLIST_ENTRY_HEAD(zl);
		while (p[0] != ZIP_END && index--) {
			p += zipRawEntryLength(p);
		}
	}
	return (p[0] == ZIP_END || index > 0) ? NULL : p;
}

/* 返回指针位置的下一个节点 */
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p) {
	((void)zl);

	//如果是尾节点,则下一个节点为空
	if (p[0] == ZIP_END) {
		return NULL;
	}
	//p+=当前元素的空间获得下一个元素的指针
	p += zipRawEntryLength(p);
	if (p[0] == ZIP_END) {
		return NULL;
	}

	return p;
}

/* 返回指针位置的前一个节点 */
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p) {
	unsigned int prevlensize, prevlen = 0;

	/* 如果是结尾标识符 */
	if (p[0] == ZIP_END) {
		//获取尾节点
		p = ZIPLIST_ENTRY_TAIL(zl);
		return (p[0] == ZIP_END) ? NULL : p;
	}
	/* 如果是头节点,返回空 */
	else if (p == ZIPLIST_ENTRY_HEAD(zl)) {
		return NULL;
	}
	else {
		ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
		assert(prevlen > 0);
		//通过指针相减获取前一个节点
		return p - prevlen;
	}
}

/*获取元素,成功返回1失败返回0,若果是字符串将获取的内容放入sstr,如果是整数将获取的内容放到sval */
unsigned int ziplistGet(unsigned char *p, unsigned char **sstr, unsigned int *slen, PORT_LONGLONG *sval) {
	zlentry entry;
	//如果ziplist没有元素则直接返回
	if (p == NULL || p[0] == ZIP_END) return 0;
	if (sstr) *sstr = NULL;

	entry = zipEntry(p);
	if (ZIP_IS_STR(entry.encoding)) {
		if (sstr) {
			*slen = entry.len;
			*sstr = p + entry.headersize;
		}
	}
	else {
		if (sval) {
			*sval = zipLoadInteger(p + entry.headersize, entry.encoding);
		}
	}
	return 1;
}

/* 插入一个元素 */
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
	return __ziplistInsert(zl, p, s, slen);
}

/* Delete a single entry from the ziplist, pointed to by *p.
* Also update *p in place, to be able to iterate over the
* ziplist, while deleting entries. */
/*删除一个元素*/
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p) {
	size_t offset = *p - zl;
	zl = __ziplistDelete(zl, *p, 1);

	/* Store pointer to current element in p, because ziplistDelete will
	* do a realloc which might result in a different "zl"-pointer.
	* When the delete direction is back to front, we might delete the last
	* entry and end up with "p" pointing to ZIP_END, so check this. */
	*p = zl + offset;
	return zl;
}

/* 删除一段数据 */
unsigned char *ziplistDeleteRange(unsigned char *zl, unsigned int index, unsigned int num) {
	unsigned char *p = ziplistIndex(zl, index);
	return (p == NULL) ? zl : __ziplistDelete(zl, p, num);
}

/* 比较p和sstr,相等返回1 */
/* Return 1 if equal. */
unsigned int ziplistCompare(unsigned char *p, unsigned char *sstr, unsigned int slen) {
	zlentry entry;
	unsigned char sencoding;
	PORT_LONGLONG zval, sval;
	if (p[0] == ZIP_END) return 0;

	entry = zipEntry(p);
	if (ZIP_IS_STR(entry.encoding)) {
		/* Raw compare */
		if (entry.len == slen) {
			return memcmp(p + entry.headersize, sstr, slen) == 0;
		}
		else {
			return 0;
		}
	}
	else {
		/* Try to compare encoded values. Don't compare encoding because
		* different implementations may encoded integers differently. */
		if (zipTryEncoding(sstr, slen, &sval, &sencoding)) {
			zval = zipLoadInteger(p + entry.headersize, entry.encoding);
			return zval == sval;
		}
	}
	return 0;
}

/* 从位置p开始查找元素,skip表示查找过的元素数目 */
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip) {
	int skipcnt = 0;
	unsigned char vencoding = 0;
	PORT_LONGLONG vll = 0;
	//没到结尾
	while (p[0] != ZIP_END) {
		unsigned int prevlensize, encoding, lensize, len;
		unsigned char *q;
		//取出元素内容放入q中,q是数据的指针
		ZIP_DECODE_PREVLENSIZE(p, prevlensize);
		ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
		q = p + prevlensize + lensize;

		if (skipcnt == 0) {
			/* 如果是字符串编码 */
			if (ZIP_IS_STR(encoding)) {
				if (len == vlen && memcmp(q, vstr, vlen) == 0) {
					return p;
				}
			}
			else {
				/* 如果是整数编码,按照整数进行比较 */
				if (vencoding == 0) {
					if (!zipTryEncoding(vstr, vlen, &vll, &vencoding)) {
						/* 如果无法进行整数编码,则直接赋值UCHAR_MAX */
						vencoding = UCHAR_MAX;
					}
					/* Must be non-zero by now */
					assert(vencoding);
				}

				/* 如果待查元素是整形编码则直接进行比较 */
				if (vencoding != UCHAR_MAX) {
					PORT_LONGLONG ll = zipLoadInteger(q, encoding);
					if (ll == vll) {
						return p;
					}
				}
			}

			/* Reset skip count */
			skipcnt = skip;
		}
		else {
			/* Skip entry */
			skipcnt--;
		}

		/* 移动到下一个位置 */
		p = q + len;
	}

	return NULL;
}

/* 返回ziplist的长度(元素个数) */
unsigned int ziplistLen(unsigned char *zl) {
	unsigned int len = 0;
	if (intrev16ifbe(ZIPLIST_LENGTH(zl)) < UINT16_MAX) {
		len = intrev16ifbe(ZIPLIST_LENGTH(zl));
	}
	else {
		unsigned char *p = zl + ZIPLIST_HEADER_SIZE;
		while (*p != ZIP_END) {
			p += zipRawEntryLength(p);
			len++;
		}

		/* Re-store length if small enough */
		if (len < UINT16_MAX) ZIPLIST_LENGTH(zl) = intrev16ifbe(len);
	}
	return len;
}

/* 返回ziplist的内存数 */
size_t ziplistBlobLen(unsigned char *zl) {
	return intrev32ifbe(ZIPLIST_BYTES(zl));
}

/*ziplist的一些基本信息的打印*/
void ziplistRepr(unsigned char *zl) {
	unsigned char *p;
	int index = 0;
	zlentry entry;

	printf(
		"{total bytes %d} "
		"{length %u}\n"
		"{tail offset %u}\n",
		intrev32ifbe(ZIPLIST_BYTES(zl)),
		intrev16ifbe(ZIPLIST_LENGTH(zl)),
		intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)));
	p = ZIPLIST_ENTRY_HEAD(zl);
	while (*p != ZIP_END) {
		entry = zipEntry(p);
		printf(
			"{"
			"addr 0x%08lx, "    /* TODO" verify 0x%08lx */
			"index %2d, "
			"offset %5ld, "
			"rl: %5u, "
			"hs %2u, "
			"pl: %5u, "
			"pls: %2u, "
			"payload %5u"
			"} ",
			(PORT_ULONG)p,
			index,
			(PORT_ULONG)(p - zl),
			entry.headersize + entry.len,
			entry.headersize,
			entry.prevrawlen,
			entry.prevrawlensize,
			entry.len);
		p += entry.headersize;
		if (ZIP_IS_STR(entry.encoding)) {
			if (entry.len > 40) {
				if (fwrite(p, 40, 1, stdout) == 0) perror("fwrite");
				printf("...");
			}
			else {
				if (entry.len &&
					fwrite(p, entry.len, 1, stdout) == 0) perror("fwrite");
			}
		}
		else {
			printf("%lld", (PORT_LONGLONG)zipLoadInteger(p, entry.encoding));
		}
		printf("\n");
		p += entry.len;
		index++;
	}
	printf("{end}\n\n");
}

#ifdef ZIPLIST_TEST_MAIN
#include <sys/time.h>
#include "adlist.h"
#include "sds.h"

#define debug(f, ...) { if (DEBUG) printf(f, __VA_ARGS__); }

	unsigned char *createList() {
		unsigned char *zl = ziplistNew();
		zl = ziplistPush(zl, (unsigned char*)"foo", 3, ZIPLIST_TAIL);
		zl = ziplistPush(zl, (unsigned char*)"quux", 4, ZIPLIST_TAIL);
		zl = ziplistPush(zl, (unsigned char*)"hello", 5, ZIPLIST_HEAD);
		zl = ziplistPush(zl, (unsigned char*)"1024", 4, ZIPLIST_TAIL);
		return zl;
	}

	unsigned char *createIntList() {
		unsigned char *zl = ziplistNew();
		char buf[32];

		sprintf(buf, "100");
		zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
		sprintf(buf, "128000");
		zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
		sprintf(buf, "-100");
		zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
		sprintf(buf, "4294967296");
		zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
		sprintf(buf, "non integer");
		zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
		sprintf(buf, "much much longer non integer");
		zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
		return zl;
	}

	PORT_LONGLONG usec(void) {
#ifdef _WIN32
		return GetHighResRelativeTime(1000000);
#else
		struct timeval tv;
		gettimeofday(&tv, NULL);
		return (((PORT_LONGLONG)tv.tv_sec) * 1000000) + tv.tv_usec;
#endif
	}

	void stress(int pos, int num, int maxsize, int dnum) {
		int i, j, k;
		unsigned char *zl;
		char posstr[2][5] = { "HEAD", "TAIL" };
		PORT_LONGLONG start;
		for (i = 0; i < maxsize; i += dnum) {
			zl = ziplistNew();
			for (j = 0; j < i; j++) {
				zl = ziplistPush(zl, (unsigned char*)"quux", 4, ZIPLIST_TAIL);
			}

			/* Do num times a push+pop from pos */
			start = usec();
			for (k = 0; k < num; k++) {
				zl = ziplistPush(zl, (unsigned char*)"quux", 4, pos);
				zl = ziplistDeleteRange(zl, 0, 1);
			}
			printf("List size: %8d, bytes: %8d, %dx push+pop (%s): %6lld usec\n",
				i, intrev32ifbe(ZIPLIST_BYTES(zl)), num, posstr[pos], usec() - start);
			zfree(zl);
		}
	}

	void pop(unsigned char *zl, int where) {
		unsigned char *p, *vstr;
		unsigned int vlen;
		PORT_LONGLONG vlong;

		p = ziplistIndex(zl, where == ZIPLIST_HEAD ? 0 : -1);
		if (ziplistGet(p, &vstr, &vlen, &vlong)) {
			if (where == ZIPLIST_HEAD)
				printf("Pop head: ");
			else
				printf("Pop tail: ");

			if (vstr)
			if (vlen && fwrite(vstr, vlen, 1, stdout) == 0) perror("fwrite");
			else
				printf("%lld", vlong);

			printf("\n");
			ziplistDeleteRange(zl, -1, 1);
		}
		else {
			printf("ERROR: Could not pop\n");
			exit(1);
		}
	}

	int randstring(char *target, unsigned int min, unsigned int max) {
		int p = 0;
		int len = min + rand() % (max - min + 1);
		int minval, maxval;
		switch (rand() % 3) {
		case 0:
			minval = 0;
			maxval = 255;
			break;
		case 1:
			minval = 48;
			maxval = 122;
			break;
		case 2:
			minval = 48;
			maxval = 52;
			break;
		default:
			assert(NULL);
		}

		while (p < len)
			target[p++] = minval + rand() % (maxval - minval + 1);
		return len;
	}

	void verify(unsigned char *zl, zlentry *e) {
		int i;
		int len = ziplistLen(zl);
		zlentry _e;

		for (i = 0; i < len; i++) {
			memset(&e[i], 0, sizeof(zlentry));
			e[i] = zipEntry(ziplistIndex(zl, i));

			memset(&_e, 0, sizeof(zlentry));
			_e = zipEntry(ziplistIndex(zl, -len + i));

			assert(memcmp(&e[i], &_e, sizeof(zlentry)) == 0);
		}
	}

	int main(int argc, char **argv) {
		unsigned char *zl, *p;
		unsigned char *entry;
		unsigned int elen;
		PORT_LONGLONG value;

		/* If an argument is given, use it as the random seed. */
		if (argc == 2)
			srand(atoi(argv[1]));

		zl = createIntList();
		ziplistRepr(zl);

		zl = createList();
		ziplistRepr(zl);

		pop(zl, ZIPLIST_TAIL);
		ziplistRepr(zl);

		pop(zl, ZIPLIST_HEAD);
		ziplistRepr(zl);

		pop(zl, ZIPLIST_TAIL);
		ziplistRepr(zl);

		pop(zl, ZIPLIST_TAIL);
		ziplistRepr(zl);

		printf("Get element at index 3:\n");
		{
			zl = createList();
			p = ziplistIndex(zl, 3);
			if (!ziplistGet(p, &entry, &elen, &value)) {
				printf("ERROR: Could not access index 3\n");
				return 1;
			}
			if (entry) {
				if (elen && fwrite(entry, elen, 1, stdout) == 0) perror("fwrite");
				printf("\n");
			}
			else {
				printf("%lld\n", value);
			}
			printf("\n");
		}

		printf("Get element at index 4 (out of range):\n");
		{
			zl = createList();
			p = ziplistIndex(zl, 4);
			if (p == NULL) {
				printf("No entry\n");
			}
			else {
				printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p - zl);
				return 1;
			}
			printf("\n");
		}

		printf("Get element at index -1 (last element):\n");
		{
			zl = createList();
			p = ziplistIndex(zl, -1);
			if (!ziplistGet(p, &entry, &elen, &value)) {
				printf("ERROR: Could not access index -1\n");
				return 1;
			}
			if (entry) {
				if (elen && fwrite(entry, elen, 1, stdout) == 0) perror("fwrite");
				printf("\n");
			}
			else {
				printf("%lld\n", value);
			}
			printf("\n");
		}

		printf("Get element at index -4 (first element):\n");
		{
			zl = createList();
			p = ziplistIndex(zl, -4);
			if (!ziplistGet(p, &entry, &elen, &value)) {
				printf("ERROR: Could not access index -4\n");
				return 1;
			}
			if (entry) {
				if (elen && fwrite(entry, elen, 1, stdout) == 0) perror("fwrite");
				printf("\n");
			}
			else {
				printf("%lld\n", value);
			}
			printf("\n");
		}

		printf("Get element at index -5 (reverse out of range):\n");
		{
			zl = createList();
			p = ziplistIndex(zl, -5);
			if (p == NULL) {
				printf("No entry\n");
			}
			else {
				printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p - zl);
				return 1;
			}
			printf("\n");
		}

		printf("Iterate list from 0 to end:\n");
		{
			zl = createList();
			p = ziplistIndex(zl, 0);
			while (ziplistGet(p, &entry, &elen, &value)) {
				printf("Entry: ");
				if (entry) {
					if (elen && fwrite(entry, elen, 1, stdout) == 0) perror("fwrite");
				}
				else {
					printf("%lld", value);
				}
				p = ziplistNext(zl, p);
				printf("\n");
			}
			printf("\n");
		}

		printf("Iterate list from 1 to end:\n");
		{
			zl = createList();
			p = ziplistIndex(zl, 1);
			while (ziplistGet(p, &entry, &elen, &value)) {
				printf("Entry: ");
				if (entry) {
					if (elen && fwrite(entry, elen, 1, stdout) == 0) perror("fwrite");
				}
				else {
					printf("%lld", value);
				}
				p = ziplistNext(zl, p);
				printf("\n");
			}
			printf("\n");
		}

		printf("Iterate list from 2 to end:\n");
		{
			zl = createList();
			p = ziplistIndex(zl, 2);
			while (ziplistGet(p, &entry, &elen, &value)) {
				printf("Entry: ");
				if (entry) {
					if (elen && fwrite(entry, elen, 1, stdout) == 0) perror("fwrite");
				}
				else {
					printf("%lld", value);
				}
				p = ziplistNext(zl, p);
				printf("\n");
			}
			printf("\n");
		}

		printf("Iterate starting out of range:\n");
		{
			zl = createList();
			p = ziplistIndex(zl, 4);
			if (!ziplistGet(p, &entry, &elen, &value)) {
				printf("No entry\n");
			}
			else {
				printf("ERROR\n");
			}
			printf("\n");
		}

		printf("Iterate from back to front:\n");
		{
			zl = createList();
			p = ziplistIndex(zl, -1);
			while (ziplistGet(p, &entry, &elen, &value)) {
				printf("Entry: ");
				if (entry) {
					if (elen && fwrite(entry, elen, 1, stdout) == 0) perror("fwrite");
				}
				else {
					printf("%lld", value);
				}
				p = ziplistPrev(zl, p);
				printf("\n");
			}
			printf("\n");
		}

		printf("Iterate from back to front, deleting all items:\n");
		{
			zl = createList();
			p = ziplistIndex(zl, -1);
			while (ziplistGet(p, &entry, &elen, &value)) {
				printf("Entry: ");
				if (entry) {
					if (elen && fwrite(entry, elen, 1, stdout) == 0) perror("fwrite");
				}
				else {
					printf("%lld", value);
				}
				zl = ziplistDelete(zl, &p);
				p = ziplistPrev(zl, p);
				printf("\n");
			}
			printf("\n");
		}

		printf("Delete inclusive range 0,0:\n");
		{
			zl = createList();
			zl = ziplistDeleteRange(zl, 0, 1);
			ziplistRepr(zl);
		}

		printf("Delete inclusive range 0,1:\n");
		{
			zl = createList();
			zl = ziplistDeleteRange(zl, 0, 2);
			ziplistRepr(zl);
		}

		printf("Delete inclusive range 1,2:\n");
		{
			zl = createList();
			zl = ziplistDeleteRange(zl, 1, 2);
			ziplistRepr(zl);
		}

		printf("Delete with start index out of range:\n");
		{
			zl = createList();
			zl = ziplistDeleteRange(zl, 5, 1);
			ziplistRepr(zl);
		}

		printf("Delete with num overflow:\n");
		{
			zl = createList();
			zl = ziplistDeleteRange(zl, 1, 5);
			ziplistRepr(zl);
		}

		printf("Delete foo while iterating:\n");
		{
			zl = createList();
			p = ziplistIndex(zl, 0);
			while (ziplistGet(p, &entry, &elen, &value)) {
				if (entry && strncmp("foo", (char*)entry, elen) == 0) {
					printf("Delete foo\n");
					zl = ziplistDelete(zl, &p);
				}
				else {
					printf("Entry: ");
					if (entry) {
						if (elen && fwrite(entry, elen, 1, stdout) == 0)
							perror("fwrite");
					}
					else {
						printf("%lld", value);
					}
					p = ziplistNext(zl, p);
					printf("\n");
				}
			}
			printf("\n");
			ziplistRepr(zl);
		}

		printf("Regression test for >255 byte strings:\n");
		{
			char v1[257], v2[257];
			memset(v1, 'x', 256);
			memset(v2, 'y', 256);
			zl = ziplistNew();
			zl = ziplistPush(zl, (unsigned char*)v1, strlen(v1), ZIPLIST_TAIL);
			zl = ziplistPush(zl, (unsigned char*)v2, strlen(v2), ZIPLIST_TAIL);

			/* Pop values again and compare their value. */
			p = ziplistIndex(zl, 0);
			assert(ziplistGet(p, &entry, &elen, &value));
			assert(strncmp(v1, (char*)entry, elen) == 0);
			p = ziplistIndex(zl, 1);
			assert(ziplistGet(p, &entry, &elen, &value));
			assert(strncmp(v2, (char*)entry, elen) == 0);
			printf("SUCCESS\n\n");
		}

		printf("Regression test deleting next to last entries:\n");
		{
			char v[3][257];
			zlentry e[3];
			int i;

			for (i = 0; i < (sizeof(v) / sizeof(v[0])); i++) {
				memset(v[i], 'a' + i, sizeof(v[0]));
			}

			v[0][256] = '\0';
			v[1][1] = '\0';
			v[2][256] = '\0';

			zl = ziplistNew();
			for (i = 0; i < (sizeof(v) / sizeof(v[0])); i++) {
				zl = ziplistPush(zl, (unsigned char *)v[i], strlen(v[i]), ZIPLIST_TAIL);
			}

			verify(zl, e);

			assert(e[0].prevrawlensize == 1);
			assert(e[1].prevrawlensize == 5);
			assert(e[2].prevrawlensize == 1);

			/* Deleting entry 1 will increase `prevrawlensize` for entry 2 */
			unsigned char *p = e[1].p;
			zl = ziplistDelete(zl, &p);

			verify(zl, e);

			assert(e[0].prevrawlensize == 1);
			assert(e[1].prevrawlensize == 5);

			printf("SUCCESS\n\n");
		}

		printf("Create long list and check indices:\n");
		{
			zl = ziplistNew();
			char buf[32];
			int i, len;
			for (i = 0; i < 1000; i++) {
				len = sprintf(buf, "%d", i);
				zl = ziplistPush(zl, (unsigned char*)buf, len, ZIPLIST_TAIL);
			}
			for (i = 0; i < 1000; i++) {
				p = ziplistIndex(zl, i);
				assert(ziplistGet(p, NULL, NULL, &value));
				assert(i == value);

				p = ziplistIndex(zl, -i - 1);
				assert(ziplistGet(p, NULL, NULL, &value));
				assert(999 - i == value);
			}
			printf("SUCCESS\n\n");
		}

		printf("Compare strings with ziplist entries:\n");
		{
			zl = createList();
			p = ziplistIndex(zl, 0);
			if (!ziplistCompare(p, (unsigned char*)"hello", 5)) {
				printf("ERROR: not \"hello\"\n");
				return 1;
			}
			if (ziplistCompare(p, (unsigned char*)"hella", 5)) {
				printf("ERROR: \"hella\"\n");
				return 1;
			}

			p = ziplistIndex(zl, 3);
			if (!ziplistCompare(p, (unsigned char*)"1024", 4)) {
				printf("ERROR: not \"1024\"\n");
				return 1;
			}
			if (ziplistCompare(p, (unsigned char*)"1025", 4)) {
				printf("ERROR: \"1025\"\n");
				return 1;
			}
			printf("SUCCESS\n\n");
		}

		printf("Stress with random payloads of different encoding:\n");
		{
			int i, j, len, where;
			unsigned char *p;
			char buf[1024];
			int buflen;
			list *ref;
			listNode *refnode;

			/* Hold temp vars from ziplist */
			unsigned char *sstr;
			unsigned int slen;
			PORT_LONGLONG sval;

			for (i = 0; i < 20000; i++) {
				zl = ziplistNew();
				ref = listCreate();
				listSetFreeMethod(ref, sdsfree);
				len = rand() % 256;

				/* Create lists */
				for (j = 0; j < len; j++) {
					where = (rand() & 1) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
					if (rand() % 2) {
						buflen = randstring(buf, 1, sizeof(buf)-1);
					}
					else {
						switch (rand() % 3) {
						case 0:
							buflen = sprintf(buf, "%lld", (0LL + rand()) >> 20);
							break;
						case 1:
							buflen = sprintf(buf, "%lld", (0LL + rand()));
							break;
						case 2:
							buflen = sprintf(buf, "%lld", (0LL + rand()) << 20);
							break;
						default:
							assert(NULL);
						}
					}

					/* Add to ziplist */
					zl = ziplistPush(zl, (unsigned char*)buf, buflen, where);

					/* Add to reference list */
					if (where == ZIPLIST_HEAD) {
						listAddNodeHead(ref, sdsnewlen(buf, buflen));
					}
					else if (where == ZIPLIST_TAIL) {
						listAddNodeTail(ref, sdsnewlen(buf, buflen));
					}
					else {
						assert(NULL);
					}
				}

				assert(listLength(ref) == ziplistLen(zl));
				for (j = 0; j < len; j++) {
					/* Naive way to get elements, but similar to the stresser
					* executed from the Tcl test suite. */
					p = ziplistIndex(zl, j);
					refnode = listIndex(ref, j);

					assert(ziplistGet(p, &sstr, &slen, &sval));
					if (sstr == NULL) {
						buflen = sprintf(buf, "%lld", sval);
					}
					else {
						buflen = slen;
						memcpy(buf, sstr, buflen);
						buf[buflen] = '\0';
					}
					assert(memcmp(buf, listNodeValue(refnode), buflen) == 0);
				}
				zfree(zl);
				listRelease(ref);
			}
			printf("SUCCESS\n\n");
		}

		printf("Stress with variable ziplist size:\n");
		{
			stress(ZIPLIST_HEAD, 100000, 16384, 256);
			stress(ZIPLIST_TAIL, 100000, 16384, 256);
		}

		return 0;
	}

#endif
