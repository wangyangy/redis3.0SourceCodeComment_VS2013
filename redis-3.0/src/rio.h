#ifndef __REDIS_RIO_H
#define __REDIS_RIO_H

#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#endif
#include <stdio.h>
#include <stdint.h>
#include "sds.h"

struct _rio {
	// 读，写，读写偏移量、刷新操作的函数指针，非0表示成功
    size_t (*read)(struct _rio *, void *buf, size_t len);
    size_t (*write)(struct _rio *, const void *buf, size_t len);
    off_t (*tell)(struct _rio *);
    int (*flush)(struct _rio *);
	//计算和校验函数
    void (*update_cksum)(struct _rio *, const void *buf, size_t len);

    //当前校验和
    uint64_t cksum;

    //读或写的字节数
    size_t processed_bytes;

    /* 读或写的最大字节数 */
    size_t max_processing_chunk;

    /* 读或写的各种对象 */
    union {
        /* 内存缓冲区 */
        struct {
            sds ptr;      //缓冲区指针,本质是char*
            off_t pos;    //缓冲区偏移量
        } buffer;
        /* 标准文件io */
        struct {
            FILE *fp;          //文件指针
            off_t buffered;    //最近一次同步之后所写字符数
            off_t autosync;    //写入设置的autosync字节后，会执行fsync()同步
        } file;
        /* 文件描述符 */
        struct {
            int *fds;       /* 文件描述符数组 */
            int *state;     /* 每一个fd所对应的erron */
            int numfds;     //数组长度,文件描述符个数
            off_t pos;      //偏移量
            sds buf;        //缓冲区
        } fdset;
    } io;
};

typedef struct _rio rio;

/* The following functions are our interface with the stream. They'll call the
 * actual implementation of read / write / tell, and will update the checksum
 * if needed. */
//写操作
static inline size_t rioWrite(rio *r, const void *buf, size_t len) {
    while (len) {
		// 写的字节长度，不能超过每次读或写的最大字节数max_processing_chunk
        size_t bytes_to_write = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
		// 更新和校验
		if (r->update_cksum) r->update_cksum(r,buf,bytes_to_write);
        //调用写函数写数据
		if (r->write(r,buf,bytes_to_write) == 0)
            return 0;
		// 更新偏移量，指向下一个写的位置
        buf = (char*)buf + bytes_to_write;
		// 计算剩余写入的长度
        len -= bytes_to_write;
		// 更新读或写的字节数
        r->processed_bytes += bytes_to_write;
    }
    return 1;
}
//读操作
static inline size_t rioRead(rio *r, void *buf, size_t len) {
    while (len) {
		// 读的字节长度，不能超过每次读或写的最大字节数max_processing_chunk
        size_t bytes_to_read = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
		// 调用自身的read方法读到buf中
        if (r->read(r,buf,bytes_to_read) == 0)
            return 0;
		// 更新和校验
        if (r->update_cksum) r->update_cksum(r,buf,bytes_to_read);
		// 更新偏移量，指向下一个读的位置
        buf = (char*)buf + bytes_to_read;
		// 计算剩余要读的长度
        len -= bytes_to_read;
		// 更新读或写的字节数
        r->processed_bytes += bytes_to_read;
    }
    return 1;
}
//返回当前偏移量
static inline off_t rioTell(rio *r) {
    return r->tell(r);
}
//缓冲区刷新函数
static inline int rioFlush(rio *r) {
    return r->flush(r);
}

void rioInitWithFile(rio *r, FILE *fp);
void rioInitWithBuffer(rio *r, sds s);
void rioInitWithFdset(rio *r, int *fds, int numfds);

size_t rioWriteBulkCount(rio *r, char prefix, int count);
size_t rioWriteBulkString(rio *r, const char *buf, size_t len);
size_t rioWriteBulkLongLong(rio *r, PORT_LONGLONG l);
size_t rioWriteBulkDouble(rio *r, double d);

void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len);
void rioSetAutoSync(rio *r, off_t bytes);

#endif
