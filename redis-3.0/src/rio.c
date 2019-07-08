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

// ��len����bufд��һ������������r��
static size_t rioBufferWrite(rio *r, const void *buf, size_t len) {
	//׷�Ӳ���
    r->io.buffer.ptr = sdscatlen(r->io.buffer.ptr,(char*)buf,len);
    r->io.buffer.pos += (off_t)len;   //����ƫ����                                            WIN_PORT_FIX /* cast (off_t) */
    return 1;
}

// ������������r����buf�У���len��
static size_t rioBufferRead(rio *r, void *buf, size_t len) {
	//�������ĳ���С��len,�ռ䲻��
    if (sdslen(r->io.buffer.ptr)-r->io.buffer.pos < len)
        return 0; /* not enough buffer to return len bytes. */
	//������
    memcpy(buf,r->io.buffer.ptr+r->io.buffer.pos,len);
	//����ƫ����
    r->io.buffer.pos += (off_t)len;                                             WIN_PORT_FIX /* cast (off_t) */
    return 1;
}

// ���ػ���������r��ǰ��ƫ����
static off_t rioBufferTell(rio *r) {
    return r->io.buffer.pos;
}

// ˢ�»�����
static int rioBufferFlush(rio *r) {
    REDIS_NOTUSED(r);
    return 1; /* Nothing to do, our write just appends to the buffer. */
}
//���建�������󲢳�ʼ�������ͳ�Ա
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
// ��ʼ������������r�����û������ĵ�ַ
void rioInitWithBuffer(rio *r, sds s) {
    *r = rioBufferIO;
    r->io.buffer.ptr = s;
    r->io.buffer.pos = 0;
}

/* --------------------- Stdio file pointer implementation ------------------- */

// ��׼�ļ�IOʵ��, ��len����bufд��һ���ļ�������
static size_t rioFileWrite(rio *r, const void *buf, size_t len) {
    size_t retval;
	// ���õײ�⺯��
    retval = fwrite(buf,len,1,r->io.file.fp);
    r->io.file.buffered += (off_t)len;         //����д�ĳ���                                   WIN_PORT_FIX /* cast (off_t) */
	// ����Ѿ��ﵽ�Զ���ͬ��autosync�����õ��ֽ���
    if (r->io.file.autosync &&
        r->io.file.buffered >= r->io.file.autosync)
    {
        fflush(r->io.file.fp);   // ˢ�»������е����ݵ��ļ���
        aof_fsync(fileno(r->io.file.fp));  //ͬ������
        r->io.file.buffered = 0;  //������Ϊ0
    }
    return retval;
}

// ���ļ�������r�ж���len���ȵ��ֽڵ�buf��
static size_t rioFileRead(rio *r, void *buf, size_t len) {
    return fread(buf,len,1,r->io.file.fp);
}

// �����ļ��������ƫ����
static off_t rioFileTell(rio *r) {
    return (off_t)ftello(r->io.file.fp);                                        WIN_PORT_FIX /* cast (int) */
}

// ˢ���ļ���
static int rioFileFlush(rio *r) {
    return (fflush(r->io.file.fp) == 0) ? 1 : 0;
}
// ��ʼ��һ���ļ�������
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
// ��ʼ��һ���ļ������������ö�Ӧ�ļ�
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
// ��bufд���ļ����������϶���
static size_t rioFdsetWrite(rio *r, const void *buf, size_t len) {
    ssize_t retval;
    int j;
    unsigned char *p = (unsigned char*) buf;
    int doflush = (buf == NULL && len == 0);

	// ��buf�е�����д���ļ����������϶���Ļ�������
    if (len) {
        r->io.fdset.buf = sdscatlen(r->io.fdset.buf,buf,len);
        len = 0; /* Prevent entering the while belove if we don't flush. */
        if (sdslen(r->io.fdset.buf) > REDIS_IOBUF_LEN) doflush = 1;
    }
	// ˢ���ļ����������϶������ü��ϻ��������Ⱥͼ��ϻ�������ַ
    if (doflush) {
        p = (unsigned char*) r->io.fdset.buf;
        len = sdslen(r->io.fdset.buf);
    }

	// һ�ο����޷���ϴ�꣬��Ҫѭ�����
    while(len) {
		// һ�����ˢ��1M�ֽ�
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
			// ��д������һ�λ���д��count���ֽ�����һ���ļ�������fd
            while(nwritten != count) {
                retval = write(r->io.fdset.fds[j],p+nwritten,count-nwritten);
				// дʧ�ܣ��ж��ǲ���д�������������ó�ʱ
                if (retval <= 0) {
                    /* With blocking sockets, which is the sole user of this
                     * rio target, EWOULDBLOCK is returned only because of
                     * the SO_SNDTIMEO socket option, so we translate the error
                     * into one more recognizable by the user. */
                    if (retval == -1 && errno == EWOULDBLOCK) errno = ETIMEDOUT;
                    break;
                }
                nwritten += retval;//ÿ�μ���д�ɹ����ֽ���
            }
			// ����ղ�дʧ�ܵ�������򽫵�ǰ���ļ�������״̬����Ϊ����ı����
            if (nwritten != count) {
                /* Mark this FD as broken. */
                r->io.fdset.state[j] = errno;
                if (r->io.fdset.state[j] == 0) r->io.fdset.state[j] = EIO;
            }
        }
		// ���е��ļ���������������0
        if (broken == r->io.fdset.numfds) return 0; /* All the FDs in error. */
        p += count;// �����´�Ҫд��ĵ�ַ�ͳ���
        len -= count;
        r->io.fdset.pos += count;//��д���ƫ����
    }
	//�ͷż��ϻ�����
    if (doflush) sdsclear(r->io.fdset.buf);
    return 1;
}

// �ļ����������϶���֧�ֶ���ֱ�ӷ���0
static size_t rioFdsetRead(rio *r, void *buf, size_t len) {
    REDIS_NOTUSED(r);
    REDIS_NOTUSED(buf);
    REDIS_NOTUSED(len);
    return 0; /* Error, this target does not support reading. */
}


// ��ȡƫ����
static off_t rioFdsetTell(rio *r) {
    return r->io.fdset.pos;
}
// ˢ�»�������ֵ
static int rioFdsetFlush(rio *r) {
    /* Our flush is implemented by the write method, that recognizes a
     * buffer set to NULL with a count of zero as a flush request. */
    return (int)rioFdsetWrite(r,NULL,0);                                        WIN_PORT_FIX /* cast (int) */
}
// ��ʼ��һ���ļ����������϶���
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
// ��ʼ��һ���ļ����������϶������ó�Ա����
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
// �ͷ��ļ�����������������
void rioFreeFdset(rio *r) {
    zfree(r->io.fdset.fds);
    zfree(r->io.fdset.state);
    sdsfree(r->io.fdset.buf);
}

/* ---------------------------- Generic functions ---------------------------- */

// ����CRC64�㷨����У���
void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len) {
    r->cksum = crc64(r->cksum,buf,len);
}

// �����Զ�ͬ�����ֽ������ƣ����bytesΪ0������ζ�Ų�ִ��
void rioSetAutoSync(rio *r, off_t bytes) {
    redisAssert(r->read == rioFileIO.read);
    r->io.file.autosync = bytes;
}

/* --------------------------- Higher level interface --------------------------
 *
 * The following higher level functions use lower level rio.c functions to help
 * generating the Redis protocol for the Append Only File. */

// ��"*<count>\r\n"��ʽΪд��һ��int���͵�count
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

// ��"$<count>\r\n<payload>\r\n"Ϊ��ʽд��һ���ַ���
size_t rioWriteBulkString(rio *r, const char *buf, size_t len) {
    size_t nwritten;

    if ((nwritten = rioWriteBulkCount(r,'$',(int)len)) == 0) return 0;          WIN_PORT_FIX /* cast (int) */
    if (len > 0 && rioWrite(r,buf,len) == 0) return 0;
    if (rioWrite(r,"\r\n",2) == 0) return 0;
    return nwritten+len+2;
}

// ��"$<count>\r\n<payload>\r\n"Ϊ��ʽд��һ��longlong ֵ
size_t rioWriteBulkLongLong(rio *r, PORT_LONGLONG l) {
    char lbuf[32];
    unsigned int llen;

    llen = ll2string(lbuf,sizeof(lbuf),l);
    return rioWriteBulkString(r,lbuf,llen);
}

// ��"$<count>\r\n<payload>\r\n"Ϊ��ʽд��һ�� double ֵ
size_t rioWriteBulkDouble(rio *r, double d) {
    char dbuf[128];
    unsigned int dlen;

    dlen = snprintf(dbuf,sizeof(dbuf),"%.17g",d);
    return rioWriteBulkString(r,dbuf,dlen);
}
