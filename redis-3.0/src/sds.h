#ifndef __SDS_H
#define __SDS_H

#define SDS_MAX_PREALLOC (1024*1024)

#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#include "Win32_Interop/win32_types.h"
#endif
#include <sys/types.h>
#include <stdarg.h>

typedef char *sds;

//sdshdr�ṹ
struct sdshdr {
    unsigned int len;      //buf�Ѿ�ʹ�õĳ���
    unsigned int free;     //buf������ʹ�õĳ���
    char buf[];            //�洢���ݵ�char����
};

//����sdshdr��buf�Ѿ�ʹ�õĳ���
static inline size_t sdslen(const sds s) {
	//Ϊʲôs-(sizeof(struct sdshdr))��sdshdr���׵�ַ?
	//������������ĺ���������,�ڴ���sdshdr�����ʱ��,�ᴴ������������,һ���ǽṹ��Ĵ�С�������ֶ�ռ�Ĵ�С��8,��һ�����Ǵ洢���ݵ�char����Ĵ�С
	//�����������,���ص���һ��char*������(��sds s),��buf���׵�ַ,ͨ����ȥ8���ܵõ�sdshdr���׵�ַ(��Ϊ�ڷ����ʱ�������������)
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    return sh->len;
}

//����sdshdr�ṹ��buf���еĳ���
static inline size_t sdsavail(const sds s) {
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    return sh->free;
}

sds sdsnewlen(const void *init, size_t initlen);
sds sdsnew(const char *init);
sds sdsempty(void);
size_t sdslen(const sds s);
sds sdsdup(const sds s);
void sdsfree(sds s);
size_t sdsavail(const sds s);
sds sdsgrowzero(sds s, size_t len);
sds sdscatlen(sds s, const void *t, size_t len);
sds sdscat(sds s, const char *t);
sds sdscatsds(sds s, const sds t);
sds sdscpylen(sds s, const char *t, size_t len);
sds sdscpy(sds s, const char *t);

sds sdscatvprintf(sds s, const char *fmt, va_list ap);
#ifdef __GNUC__
sds sdscatprintf(sds s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
sds sdscatprintf(sds s, const char *fmt, ...);
#endif

sds sdscatfmt(sds s, char const *fmt, ...);
sds sdstrim(sds s, const char *cset);
void sdsrange(sds s, int start, int end);
void sdsupdatelen(sds s);
void sdsclear(sds s);
int sdscmp(const sds s1, const sds s2);
sds *sdssplitlen(const char *s, int len, const char *sep, int seplen, int *count);
void sdsfreesplitres(sds *tokens, int count);
void sdstolower(sds s);
void sdstoupper(sds s);
sds sdsfromlonglong(PORT_LONGLONG value);
sds sdscatrepr(sds s, const char *p, size_t len);
sds *sdssplitargs(const char *line, int *argc);
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen);
sds sdsjoin(char **argv, int argc, char *sep);

/* Low level functions exposed to the user API */
sds sdsMakeRoomFor(sds s, size_t addlen);
void sdsIncrLen(sds s, int incr);
sds sdsRemoveFreeSpace(sds s);
size_t sdsAllocSize(sds s);

#endif
