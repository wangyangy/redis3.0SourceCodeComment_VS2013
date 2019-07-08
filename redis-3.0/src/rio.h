#ifndef __REDIS_RIO_H
#define __REDIS_RIO_H

#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#endif
#include <stdio.h>
#include <stdint.h>
#include "sds.h"

struct _rio {
	// ����д����дƫ������ˢ�²����ĺ���ָ�룬��0��ʾ�ɹ�
    size_t (*read)(struct _rio *, void *buf, size_t len);
    size_t (*write)(struct _rio *, const void *buf, size_t len);
    off_t (*tell)(struct _rio *);
    int (*flush)(struct _rio *);
	//�����У�麯��
    void (*update_cksum)(struct _rio *, const void *buf, size_t len);

    //��ǰУ���
    uint64_t cksum;

    //����д���ֽ���
    size_t processed_bytes;

    /* ����д������ֽ��� */
    size_t max_processing_chunk;

    /* ����д�ĸ��ֶ��� */
    union {
        /* �ڴ滺���� */
        struct {
            sds ptr;      //������ָ��,������char*
            off_t pos;    //������ƫ����
        } buffer;
        /* ��׼�ļ�io */
        struct {
            FILE *fp;          //�ļ�ָ��
            off_t buffered;    //���һ��ͬ��֮����д�ַ���
            off_t autosync;    //д�����õ�autosync�ֽں󣬻�ִ��fsync()ͬ��
        } file;
        /* �ļ������� */
        struct {
            int *fds;       /* �ļ����������� */
            int *state;     /* ÿһ��fd����Ӧ��erron */
            int numfds;     //���鳤��,�ļ�����������
            off_t pos;      //ƫ����
            sds buf;        //������
        } fdset;
    } io;
};

typedef struct _rio rio;

/* The following functions are our interface with the stream. They'll call the
 * actual implementation of read / write / tell, and will update the checksum
 * if needed. */
//д����
static inline size_t rioWrite(rio *r, const void *buf, size_t len) {
    while (len) {
		// д���ֽڳ��ȣ����ܳ���ÿ�ζ���д������ֽ���max_processing_chunk
        size_t bytes_to_write = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
		// ���º�У��
		if (r->update_cksum) r->update_cksum(r,buf,bytes_to_write);
        //����д����д����
		if (r->write(r,buf,bytes_to_write) == 0)
            return 0;
		// ����ƫ������ָ����һ��д��λ��
        buf = (char*)buf + bytes_to_write;
		// ����ʣ��д��ĳ���
        len -= bytes_to_write;
		// ���¶���д���ֽ���
        r->processed_bytes += bytes_to_write;
    }
    return 1;
}
//������
static inline size_t rioRead(rio *r, void *buf, size_t len) {
    while (len) {
		// �����ֽڳ��ȣ����ܳ���ÿ�ζ���д������ֽ���max_processing_chunk
        size_t bytes_to_read = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
		// ���������read��������buf��
        if (r->read(r,buf,bytes_to_read) == 0)
            return 0;
		// ���º�У��
        if (r->update_cksum) r->update_cksum(r,buf,bytes_to_read);
		// ����ƫ������ָ����һ������λ��
        buf = (char*)buf + bytes_to_read;
		// ����ʣ��Ҫ���ĳ���
        len -= bytes_to_read;
		// ���¶���д���ֽ���
        r->processed_bytes += bytes_to_read;
    }
    return 1;
}
//���ص�ǰƫ����
static inline off_t rioTell(rio *r) {
    return r->tell(r);
}
//������ˢ�º���
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
