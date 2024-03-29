#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#include "win32_interop/win32_types.h"
#include "Win32_Interop/Win32_FDAPI.h"
#endif

#include "fmacros.h"
#include <string.h>
#include <stdio.h>
POSIX_ONLY(#include <unistd.h>)
#include "rio.h"
#include "util.h"
#include "config.h"
#include "redis.h"
#include "crc64.h"
#include "config.h"

/* ------------------------- Buffer I/O implementation ----------------------- */

// 将len长的buf写到一个缓冲区对象r中
static size_t rioBufferWrite(rio *r, const void *buf, size_t len) {
	//追加操作
    r->io.buffer.ptr = sdscatlen(r->io.buffer.ptr,(char*)buf,len);
    r->io.buffer.pos += (off_t)len;   //更新偏移量                                            WIN_PORT_FIX /* cast (off_t) */
    return 1;
}

// 讲缓冲区对象r读到buf中，读len长
static size_t rioBufferRead(rio *r, void *buf, size_t len) {
	//缓冲区的长度小于len,空间不够
    if (sdslen(r->io.buffer.ptr)-r->io.buffer.pos < len)
        return 0; /* not enough buffer to return len bytes. */
	//读数据
    memcpy(buf,r->io.buffer.ptr+r->io.buffer.pos,len);
	//更新偏移量
    r->io.buffer.pos += (off_t)len;                                             WIN_PORT_FIX /* cast (off_t) */
    return 1;
}

// 返回缓冲区对象r当前的偏移量
static off_t rioBufferTell(rio *r) {
    return r->io.buffer.pos;
}

// 刷新缓冲区
static int rioBufferFlush(rio *r) {
    REDIS_NOTUSED(r);
    return 1; /* Nothing to do, our write just appends to the buffer. */
}
//定义缓冲区对象并初始化方法和成员
static const rio rioBufferIO = {
    rioBufferRead,
    rioBufferWrite,
    rioBufferTell,
    rioBufferFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL, 0 } } /* union for io-specific vars */
};
// 初始化缓冲区对象r并设置缓冲区的地址
void rioInitWithBuffer(rio *r, sds s) {
    *r = rioBufferIO;
    r->io.buffer.ptr = s;
    r->io.buffer.pos = 0;
}

/* --------------------- Stdio file pointer implementation ------------------- */

// 标准文件IO实现, 将len长的buf写入一个文件流对象
static size_t rioFileWrite(rio *r, const void *buf, size_t len) {
    size_t retval;
	// 调用底层库函数
    retval = fwrite(buf,len,1,r->io.file.fp);
    r->io.file.buffered += (off_t)len;         //更新写的长度                                   WIN_PORT_FIX /* cast (off_t) */
	// 如果已经达到自动的同步autosync所设置的字节数
    if (r->io.file.autosync &&
        r->io.file.buffered >= r->io.file.autosync)
    {
        fflush(r->io.file.fp);   // 刷新缓冲区中的数据到文件中
        aof_fsync(fileno(r->io.file.fp));  //同步操作
        r->io.file.buffered = 0;  //长度置为0
    }
    return retval;
}

// 从文件流对象r中读出len长度的字节到buf中
static size_t rioFileRead(rio *r, void *buf, size_t len) {
    return fread(buf,len,1,r->io.file.fp);
}

// 返回文件流对象的偏移量
static off_t rioFileTell(rio *r) {
    return (off_t)ftello(r->io.file.fp);                                        WIN_PORT_FIX /* cast (int) */
}

// 刷新文件流
static int rioFileFlush(rio *r) {
    return (fflush(r->io.file.fp) == 0) ? 1 : 0;
}
// 初始化一个文件流对象
static const rio rioFileIO = {
    rioFileRead,
    rioFileWrite,
    rioFileTell,
    rioFileFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL, 0 } } /* union for io-specific vars */
};
// 初始化一个文件流对象且设置对应文件
void rioInitWithFile(rio *r, FILE *fp) {
    *r = rioFileIO;
    r->io.file.fp = fp;
    r->io.file.buffered = 0;
    r->io.file.autosync = 0;
}

/* ------------------- File descriptors set implementation ------------------- */

/* Returns 1 or 0 for success/failure.
 * The function returns success as long as we are able to correctly write
 * to at least one file descriptor.
 *
 * When buf is NULL adn len is 0, the function performs a flush operation
 * if there is some pending buffer, so this function is also used in order
 * to implement rioFdsetFlush(). */
// 将buf写入文件描述符集合对象
static size_t rioFdsetWrite(rio *r, const void *buf, size_t len) {
    ssize_t retval;
    int j;
    unsigned char *p = (unsigned char*) buf;
    int doflush = (buf == NULL && len == 0);

	// 将buf中的内容写到文件描述符集合对象的缓冲区中
    if (len) {
        r->io.fdset.buf = sdscatlen(r->io.fdset.buf,buf,len);
        len = 0; /* Prevent entering the while belove if we don't flush. */
        if (sdslen(r->io.fdset.buf) > REDIS_IOBUF_LEN) doflush = 1;
    }
	// 刷新文件描述符集合对象，设置集合缓冲区长度和集合缓冲区地址
    if (doflush) {
        p = (unsigned char*) r->io.fdset.buf;
        len = sdslen(r->io.fdset.buf);
    }

	// 一次可能无法冲洗完，需要循环多次
    while(len) {
		// 一次最多刷新1M字节
        size_t count = len < 1024 ? len : 1024;
        int broken = 0;
        for (j = 0; j < r->io.fdset.numfds; j++) {
            if (r->io.fdset.state[j] != 0) {
                /* Skip FDs alraedy in error. */
                broken++;
                continue;
            }

            /* Make sure to write 'count' bytes to the socket regardless
             * of short writes. */
            size_t nwritten = 0;
			// 新写的数据一次或多次写够count个字节往第一个文件描述符fd
            while(nwritten != count) {
                retval = write(r->io.fdset.fds[j],p+nwritten,count-nwritten);
				// 写失败，判断是不是写阻塞，是则设置超时
                if (retval <= 0) {
                    /* With blocking sockets, which is the sole user of this
                     * rio target, EWOULDBLOCK is returned only because of
                     * the SO_SNDTIMEO socket option, so we translate the error
                     * into one more recognizable by the user. */
                    if (retval == -1 && errno == EWOULDBLOCK) errno = ETIMEDOUT;
                    break;
                }
                nwritten += retval;//每次加上写成功的字节数
            }
			// 如果刚才写失败的情况，则将当前的文件描述符状态设置为错误的标记码
            if (nwritten != count) {
                /* Mark this FD as broken. */
                r->io.fdset.state[j] = errno;
                if (r->io.fdset.state[j] == 0) r->io.fdset.state[j] = EIO;
            }
        }
		// 所有的文件描述符都出错返回0
        if (broken == r->io.fdset.numfds) return 0; /* All the FDs in error. */
        p += count;// 更新下次要写入的地址和长度
        len -= count;
        r->io.fdset.pos += count;//已写入的偏移量
    }
	//释放集合缓冲区
    if (doflush) sdsclear(r->io.fdset.buf);
    return 1;
}

// 文件描述符集合对象不支持读，直接返回0
static size_t rioFdsetRead(rio *r, void *buf, size_t len) {
    REDIS_NOTUSED(r);
    REDIS_NOTUSED(buf);
    REDIS_NOTUSED(len);
    return 0; /* Error, this target does not support reading. */
}


// 获取偏移量
static off_t rioFdsetTell(rio *r) {
    return r->io.fdset.pos;
}
// 刷新缓冲区的值
static int rioFdsetFlush(rio *r) {
    /* Our flush is implemented by the write method, that recognizes a
     * buffer set to NULL with a count of zero as a flush request. */
    return (int)rioFdsetWrite(r,NULL,0);                                        WIN_PORT_FIX /* cast (int) */
}
// 初始化一个文件描述符集合对象
static const rio rioFdsetIO = {
    rioFdsetRead,
    rioFdsetWrite,
    rioFdsetTell,
    rioFdsetFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL, 0 } } /* union for io-specific vars */
};
// 初始化一个文件描述符集合对象并设置成员变量
void rioInitWithFdset(rio *r, int *fds, int numfds) {
    int j;

    *r = rioFdsetIO;
    r->io.fdset.fds = zmalloc(sizeof(int)*numfds);
    r->io.fdset.state = zmalloc(sizeof(int)*numfds);
    memcpy(r->io.fdset.fds,fds,sizeof(int)*numfds);
    for (j = 0; j < numfds; j++) r->io.fdset.state[j] = 0;
    r->io.fdset.numfds = numfds;
    r->io.fdset.pos = 0;
    r->io.fdset.buf = sdsempty();
}
// 释放文件描述符集合流对象
void rioFreeFdset(rio *r) {
    zfree(r->io.fdset.fds);
    zfree(r->io.fdset.state);
    sdsfree(r->io.fdset.buf);
}

/* ---------------------------- Generic functions ---------------------------- */

// 根据CRC64算法进行校验和
void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len) {
    r->cksum = crc64(r->cksum,buf,len);
}

// 设置自动同步的字节数限制，如果bytes为0，则意味着不执行
void rioSetAutoSync(rio *r, off_t bytes) {
    redisAssert(r->read == rioFileIO.read);
    r->io.file.autosync = bytes;
}

/* --------------------------- Higher level interface --------------------------
 *
 * The following higher level functions use lower level rio.c functions to help
 * generating the Redis protocol for the Append Only File. */

// 以"*<count>\r\n"格式为写如一个int整型的count
size_t rioWriteBulkCount(rio *r, char prefix, int count) {
    char cbuf[128];
    int clen;

    cbuf[0] = prefix;
    clen = 1+ll2string(cbuf+1,sizeof(cbuf)-1,count);
    cbuf[clen++] = '\r';
    cbuf[clen++] = '\n';
    if (rioWrite(r,cbuf,clen) == 0) return 0;
    return clen;
}

// 以"$<count>\r\n<payload>\r\n"为格式写入一个字符串
size_t rioWriteBulkString(rio *r, const char *buf, size_t len) {
    size_t nwritten;

    if ((nwritten = rioWriteBulkCount(r,'$',(int)len)) == 0) return 0;          WIN_PORT_FIX /* cast (int) */
    if (len > 0 && rioWrite(r,buf,len) == 0) return 0;
    if (rioWrite(r,"\r\n",2) == 0) return 0;
    return nwritten+len+2;
}

// 以"$<count>\r\n<payload>\r\n"为格式写入一个longlong 值
size_t rioWriteBulkLongLong(rio *r, PORT_LONGLONG l) {
    char lbuf[32];
    unsigned int llen;

    llen = ll2string(lbuf,sizeof(lbuf),l);
    return rioWriteBulkString(r,lbuf,llen);
}

// 以"$<count>\r\n<payload>\r\n"为格式写入一个 double 值
size_t rioWriteBulkDouble(rio *r, double d) {
    char dbuf[128];
    unsigned int dlen;

    dlen = snprintf(dbuf,sizeof(dbuf),"%.17g",d);
    return rioWriteBulkString(r,dbuf,dlen);
}
