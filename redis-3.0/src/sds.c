#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#include "Win32_Interop/win32fixes.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include "sds.h"
#include "zmalloc.h"
/*
redis��û��ʹ��c�����д�ͳ���ַ���,���ǹ������Լ��ļ򵥶�̬�ַ���SDS
*/

/*
   ���ݸ����ĳ�ʼ���ַ���init���ַ������ȴ���һ��sds,
   ����һ��sdshdr�ṹ��
*/
sds sdsnewlen(const void *init, size_t initlen) {
    struct sdshdr *sh;
	//����ռ�,�����ṹ��Ŀռ�,����len,free��Ҫ��ʼ����char����ռ�,���Կռ������������
    if (init) {
		//zmalloc����ʼ����������ڴ�ռ�
        sh = zmalloc(sizeof(struct sdshdr)+initlen+1);
    } else {
		//��������ڴ�ռ�ȫ����ʼ��Ϊ0
        sh = zcalloc(sizeof(struct sdshdr)+initlen+1);
    }
	//����ռ�ʧ��
    if (sh == NULL) return NULL;
	//���ó�ʼ������
    sh->len = (int)initlen;                                                     WIN_PORT_FIX /* cast (int) */
    sh->free = 0;
	//���������init,��initlen��Ϊ0,��init�����ݸ��Ƶ�buf��
    if (initlen && init)
        memcpy(sh->buf, init, initlen);
    sh->buf[initlen] = '\0';
	//����buf�ĵ�ַ
    return (char*)sh->buf;
}

/*����һ��ֻ�����˿��ַ���""��sds,�ײ����sdsnewlen����ʵ��*/
sds sdsempty(void) {
    return sdsnewlen("",0);
}

/* ���ݸ�����init����һ��sds */
sds sdsnew(const char *init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

/* ����һ��sds */
sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

/* �ͷŸ�����sds */
void sdsfree(sds s) {
    if (s == NULL) return;
    zfree(s-sizeof(struct sdshdr));
}

/* δʹ�õĺ��������Ѿ�����
 * s = sdsnew("foobar");
 * s[2] = '\0';
 * sdsupdatelen(s);
 * printf("%d\n", sdslen(s));
 *
 * The output will be "2", but if we comment out the call to sdsupdatelen()
 * the output will be "6" as the string was modified but the logical length
 * remains 6 bytes. */
void sdsupdatelen(sds s) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    int reallen = (int)strlen(s);                                               WIN_PORT_FIX /* cast (int) */
    sh->free += (sh->len-reallen);
    sh->len = reallen;
}

/* 
	�ڲ��ͷ�sds�ռ�������,���һ��sds�ṹ 
	����sds��������ַ���Ϊ���ַ���
*/
void sdsclear(sds s) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    sh->free += sh->len;
    sh->len = 0;
    sh->buf[0] = '\0';
}

/* �������sds��Ӧ��sdshdr�ṹ��free��С,�൱������,���Ŵ�СΪaddlen */
sds sdsMakeRoomFor(sds s, size_t addlen) {
    struct sdshdr *sh, *newsh;
    size_t free = sdsavail(s);
    size_t len, newlen;
	//���free����addlen��ֱ�ӷ���
    if (free >= addlen) return s;
	//�õ��Ѿ�ʹ�õĳ���
    len = sdslen(s);
	//�õ�sdshdr��ַ
    sh = (void*) (s-(sizeof(struct sdshdr)));
	//���ݺ�Ĵ�С
    newlen = (len+addlen);
    if (newlen < SDS_MAX_PREALLOC)
        newlen *= 2;
    else
        newlen += SDS_MAX_PREALLOC;
	//���·���ռ�
    newsh = zrealloc(sh, sizeof(struct sdshdr)+newlen+1);
    if (newsh == NULL) return NULL;
	//������С
    newsh->free = (int)(newlen - len);                                          WIN_PORT_FIX /* cast (int) */
    return newsh->buf;
}

/* ����sdshdr�еĿ��пռ� */
sds sdsRemoveFreeSpace(sds s) {
    struct sdshdr *sh;

    sh = (void*) (s-(sizeof(struct sdshdr)));
	//���·���ռ�,ȥ������ĳ���
    sh = (struct sdshdr *)zrealloc(sh, sizeof(struct sdshdr)+sh->len+1);        WIN_PORT_FIX /* cast (struct sdshdr *) */
    sh->free = 0;
    return sh->buf;
}

/* ����sdshdr���ܿռ�,����4���ֵ�����
 * including:
 * 1) The sds header before the pointer.
 * 2) The string.
 * 3) The free buffer at the end if any.
 * 4) The implicit null term.
 */
size_t sdsAllocSize(sds s) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));

    return sizeof(*sh)+sh->len+sh->free+1;
}

/* ����sdshdr��len�Ĵ�С,ͨ������freeʵ��,���ӵĴ�СΪincr*/
void sdsIncrLen(sds s, int incr) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));

    if (incr >= 0)
        assert(sh->free >= (unsigned int)incr);
    else
        assert(sh->len >= (unsigned int)(-incr));
    sh->len += incr;
    sh->free -= incr;
    s[sh->len] = '\0';
}

/* ��\0���len��С�Ŀ���ռ� */
sds sdsgrowzero(sds s, size_t len) {
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    size_t totlen, curlen = sh->len;

    if (len <= curlen) return s;
    s = sdsMakeRoomFor(s,len-curlen);
    if (s == NULL) return NULL;

    /* Make sure added region doesn't contain garbage */
    sh = (void*)(s-(sizeof(struct sdshdr)));
	//void *memset(void *s,int c,size_t n)
	//memset�������ѿ����ڴ�ռ� s ���� n ���ֽڵ�ֵ��Ϊֵ c��
    memset(s+curlen,0,(len-curlen+1)); /* ����Ŀռ���\0��� */
    totlen = sh->len+sh->free;
    sh->len = (int)len;                                                         WIN_PORT_FIX /* cast (int) */
    sh->free = (int)(totlen-sh->len);                                           WIN_PORT_FIX /* cast (int) */
    return s;
}

/* ������Ϊt���ַ���׷�ӵ�sds�ַ���ĩβ */
sds sdscatlen(sds s, const void *t, size_t len) {
    struct sdshdr *sh;
    size_t curlen = sdslen(s);
	//����
    s = sdsMakeRoomFor(s,len);
    if (s == NULL) return NULL;
    sh = (void*) (s-(sizeof(struct sdshdr)));
	//���ַ���t�����ݸ��Ƶ�sds��ĩβ
    memcpy(s+curlen, t, len);
    sh->len = (int)(curlen+len);                                                WIN_PORT_FIX /* cast (int) */
    sh->free = (int)(sh->free-len);                                             WIN_PORT_FIX /* cast (int) */
    s[curlen+len] = '\0';
    return s;
}

/* �����ַ���t׷�ӵ�sds��β�� */
sds sdscat(sds s, const char *t) {
    return sdscatlen(s, t, strlen(t));
}

/* ����sds׷�ӵ�Դsds��β�� */
sds sdscatsds(sds s, const sds t) {
    return sdscatlen(s, t, sdslen(t));
}

/* 
	���ַ���t��ǰlen���ַ����Ƶ�sds��,�����ַ���ĩβ����ս��
	���sds�ĳ���С��len,������.
*/
sds sdscpylen(sds s, const char *t, size_t len) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t totlen = sh->free+sh->len;
	//����
    if (totlen < len) {
        s = sdsMakeRoomFor(s,len-sh->len);
        if (s == NULL) return NULL;
        sh = (void*) (s-(sizeof(struct sdshdr)));
        totlen = sh->free+sh->len;
    }
	//����tǰlen���ַ���sds��,����sdsԭ�е�����
    memcpy(s, t, len);
    s[len] = '\0';
    sh->len = (int)len;                                                         WIN_PORT_FIX /* cast (int) */
    sh->free = (int)(totlen-len);                                               WIN_PORT_FIX /* cast (int) */
    return s;
}

/* ���ַ������Ƶ�sds��,����ԭ�е����� */
sds sdscpy(sds s, const char *t) {
    return sdscpylen(s, t, strlen(t));
}

/* ���ߺ�������ֵת��Ϊ�ַ��� */
#define SDS_LLSTR_SIZE 21
int sdsll2str(char *s, PORT_LONGLONG value) {
    char *p, aux;
    PORT_ULONGLONG v;
    size_t l;

    v = (value < 0) ? -value : value;
	//����ֵת��Ϊ�ַ���
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);
	//����Ǹ�������Ӹ���
    if (value < 0) *p++ = '-';

    /* ����ת����ĳ��� */
    l = p-s;
    *p = '\0';

    /* ���ַ������� */
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
	//����ת����ĳ���
    return (int)l;                                                              WIN_PORT_FIX /* cast (int) */
}

/* ���ߺ�������ֵת��Ϊ�ַ��� */
int sdsull2str(char *s, PORT_ULONGLONG v) {
    char *p, aux;
    size_t l;

    /* Generate the string representation, this method produces
     * an reversed string. */
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);

    /* Compute length and add null term. */
    l = p-s;
    *p = '\0';

    /* Reverse the string. */
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return (int)l;
}

/* 
	���������long longֵvalueת��Ϊsds,����ֵת��Ϊsds
 */
sds sdsfromlonglong(PORT_LONGLONG value) {
    char buf[SDS_LLSTR_SIZE];
	//����ֵת��Ϊ�ַ���,����ȡ�ַ����ĳ���
    int len = sdsll2str(buf,value);
	//����sds����
    return sdsnewlen(buf,len);
}

/* ��ӡ����,��sdscatprinf����,va_list��һ���ɱ��������*/
sds sdscatvprintf(sds s, const char *fmt, va_list ap) {
    va_list cpy;
    char staticbuf[1024], *buf = staticbuf, *t;
    size_t buflen = strlen(fmt)*2;

    /* We try to start using a static buffer for speed.
     * If not possible we revert to heap allocation. */
    if (buflen > sizeof(staticbuf)) {
        buf = zmalloc(buflen);
        if (buf == NULL) return NULL;
    } else {
        buflen = sizeof(staticbuf);
    }

    /* Try with buffers two times bigger every time we fail to
     * fit the string in the current buffer size. */
    while(1) {
        buf[buflen-2] = '\0';
		//������������cpy��
        va_copy(cpy,ap);
		//c���Կ⺯��,���ڿɱ����,���ɱ������ʽ��������ַ�������
        vsnprintf(buf, buflen WIN32_ONLY(-1), fmt, cpy);    // WIN_PORT_FIX: see comment below
        //�����ɱ��������
		va_end(cpy);
        if (buf[buflen-2] != '\0') {
            if (buf != staticbuf) zfree(buf);
            buflen *= 2;

            // WIN_PORT_FIX: from the vsnprintf documentation in MSDN:
            // "To ensure that there is room for the terminating null, be sure
            //  that count is strictly less than the buffer length and
            //  initialize the buffer to null prior to calling the function."
            buf = IF_WIN32(zcalloc,zmalloc)(buflen);
            if (buf == NULL) return NULL;
            continue;
        }
        break;
    }

    /* Finally concat the obtained string to the SDS string and return it. */
    t = sdscat(s, buf);
    if (buf != staticbuf) zfree(buf);
    return t;
}

/*
	��ӡ�����������ַ���,������Щ�������ַ���׷�ӵ�sds��ĩβ
 */
sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap;
    char *t;
	//����һ���ɱ�����ĵ�ַ����ap,��apָ��ɱ�����Ŀ�ʼλ��
    va_start(ap, fmt);
    t = sdscatvprintf(s,fmt,ap);
    va_end(ap);
    return t;
}

/* ���������sdscatprintf����һ��,ֻ������sdscatprintfҪ��ܶ�
This function is similar to sdscatprintf, but much faster as it does
 * not rely on sprintf() family functions implemented by the libc that
 * are often very slow. Moreover directly handling the sds string as
 * new data is concatenated provides a performance improvement.
 *
 * However this function only handles an incompatible subset of printf-alike
 * format specifiers:
 *
 * %s - C String
 * %S - SDS string
 * %i - signed int
 * %I - 64 bit signed integer (PORT_LONGLONG, int64_t)
 * %u - unsigned int
 * %U - 64 bit unsigned integer (PORT_ULONGLONG, uint64_t)
 * %% - Verbatim "%" character.
 */
sds sdscatfmt(sds s, char const *fmt, ...) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t initlen = sdslen(s);
    const char *f = fmt;
    int i;
    va_list ap;
	//��ȡ�ɱ�������׵�ַ,��ap
    va_start(ap,fmt);
    f = fmt;    /* Next format specifier byte to process. */
    i = (int)initlen; /* Position of the next byte to write to dest str. */
    while(*f) {
        char next, *str;
        unsigned int l;
        PORT_LONGLONG num;
        PORT_ULONGLONG unum;

        /* Make sure there is always space for at least 1 char. */
        if (sh->free == 0) {
            s = sdsMakeRoomFor(s,1);
            sh = (void*) (s-(sizeof(struct sdshdr)));
        }

        switch(*f) {
        case '%':
            next = *(f+1);
            f++;
            switch(next) {
            case 's':
            case 'S':
				//��ȡ�ɱ�����б��е�һ������,ȡ��ap�����һ���ɱ����,ָ����Զ�����
				//����char*����ʵ�ʵĲ������ͱ仯
                str = va_arg(ap,char*);
                l = (int)((next == 's') ? strlen(str) : sdslen(str));           WIN_PORT_FIX /* cast (int) */
                if (sh->free < l) {
                    s = sdsMakeRoomFor(s,l);
                    sh = (void*) (s-(sizeof(struct sdshdr)));
                }
                memcpy(s+i,str,l);
                sh->len += l;
                sh->free -= l;
                i += l;
                break;
            case 'i':
            case 'I':
                if (next == 'i')
                    num = va_arg(ap,int);
                else
                    num = va_arg(ap,PORT_LONGLONG);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = sdsll2str(buf,num);
                    if (sh->free < l) {
                        s = sdsMakeRoomFor(s,l);
                        sh = (void*) (s-(sizeof(struct sdshdr)));
                    }
                    memcpy(s+i,buf,l);
                    sh->len += l;
                    sh->free -= l;
                    i += l;
                }
                break;
            case 'u':
            case 'U':
                if (next == 'u')
                    unum = va_arg(ap,unsigned int);
                else
                    unum = va_arg(ap,PORT_ULONGLONG);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = sdsull2str(buf,unum);
                    if (sh->free < l) {
                        s = sdsMakeRoomFor(s,l);
                        sh = (void*) (s-(sizeof(struct sdshdr)));
                    }
                    memcpy(s+i,buf,l);
                    sh->len += l;
                    sh->free -= l;
                    i += l;
                }
                break;
            default: /* Handle %% and generally %<unknown>. */
                s[i++] = next;
                sh->len += 1;
                sh->free -= 1;
                break;
            }
            break;
        default:
            s[i++] = *f;
            sh->len += 1;
            sh->free -= 1;
            break;
        }
        f++;
    }
    va_end(ap);

    /* Add null-term */
    s[i] = '\0';
    return s;
}

/* ��sds�������˽��м���,������*cest�е�����ɾ��
 * s = sdsnew("AA...AA.a.aa.aHelloWorld     :::");
 * s = sdstrim(s,"Aa. :");
 * printf("%s\n", s);
 *
 * Output will be just "Hello World".
 */
sds sdstrim(sds s, const char *cset) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    char *start, *end, *sp, *ep;
    size_t len;

    sp = start = s;
    ep = end = s+sdslen(s)-1;
	//strchr:���ַ���cset����*sp��һ�γ��ֵ�λ��,û�з���NULL
    while(sp <= end && strchr(cset, *sp)) sp++;
    while(ep > start && strchr(cset, *ep)) ep--;
    len = (sp > ep) ? 0 : ((ep-sp)+1);
	//memmove:��sp�е�len���ֽڸ��Ƶ�sh->buf��,����memmove��memcpy���ܻ���һ��
	//���ǵ�src��dest�ڴ��������ص����ֵ�ʱ��memcpy���ܻᱨ��,��memmove���ᱨ��
    if (sh->buf != sp) memmove(sh->buf, sp, len);
    sh->buf[len] = '\0';
    sh->free = sh->free+(int)(sh->len-len);                                     WIN_PORT_FIX /* cast (int) */
    sh->len = (int)len;                                                         WIN_PORT_FIX /* cast (int) */
    return s;
}

/* �������Խ�ȡsds�е�һ������
 * Example:
 * s = sdsnew("Hello World");
 * sdsrange(s,1,-1); => "ello World"
 */
void sdsrange(sds s, int start, int end) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t newlen, len = sdslen(s);

    if (len == 0) return;
    if (start < 0) {
        start = (int)len+start;                                                 WIN_PORT_FIX /* cast (int) */
        if (start < 0) start = 0;
    }
    if (end < 0) {
        end = (int)len+end;                                                     WIN_PORT_FIX /* cast (int) */
        if (end < 0) end = 0;
    }
	//�����ȡ�ĳ���
    newlen = (start > end) ? 0 : (end-start)+1;
    if (newlen != 0) {
        if (start >= (signed)len) {
            newlen = 0;
        } else if (end >= (signed)len) {
            end = (int)len-1;                                                   WIN_PORT_FIX /* cast (int) */
            newlen = (start > end) ? 0 : (end-start)+1;
        }
    } else {
        start = 0;
    }
	//����ȡ�����ݸ��Ƶ�sds->buf��
    if (start && newlen) memmove(sh->buf, sh->buf+start, newlen);
    sh->buf[newlen] = 0;
    sh->free = (int)(sh->free+(sh->len-newlen));                                WIN_PORT_FIX /* cast (int) */
    sh->len = (int)newlen;                                                      WIN_PORT_FIX /* cast (int) */
}

/* ��sds�ַ���Сд */
void sdstolower(sds s) {
    int len = (int)sdslen(s), j;                                                WIN_PORT_FIX /* cast (int) */

    for (j = 0; j < len; j++) s[j] = tolower(s[j]);
}

/* ���ַ���sds��д */
void sdstoupper(sds s) {
    int len = (int)sdslen(s), j;                                                WIN_PORT_FIX /* cast (int) */

    for (j = 0; j < len; j++) s[j] = toupper(s[j]);
}

/* �Ƚ�����sds�ṹ
 * Return value:
 *     positive if s1 > s2.
 *     negative if s1 < s2.
 *     0 if s1 and s2 are exactly the same binary string.
 * If two strings share exactly the same prefix, but one of the two has
 * additional characters, the longer string is considered to be greater than
 * the smaller one. */
int sdscmp(const sds s1, const sds s2) {
    size_t l1, l2, minlen;
    int cmp;

    l1 = sdslen(s1);
    l2 = sdslen(s2);
    minlen = (l1 < l2) ? l1 : l2;
	//���ÿ⺯��ʵ���ַ����ĶԱ�
    cmp = memcmp(s1,s2,minlen);
    if (cmp == 0) return (int)(l1-l2);                                          WIN_PORT_FIX /* cast (int) */
    return cmp;
}

/* ʹ�÷ָ���sep��sds���зָ�,����һ��sds����,*count�ᱻ����Ϊ���������Ԫ������
 * ��������ڴ治��,�ַ�������Ϊ0���߷ָ�������Ϊ0�ͷ���NULL
 * �ָ����������ַ���
 * This version of the function is binary-safe but
 * requires length arguments. sdssplit() is just the
 * same function but for zero-terminated strings.
 */
sds *sdssplitlen(const char *s, int len, const char *sep, int seplen, int *count) {
    //slots=5˵��Ĭ�ϵķ������鳤����5
	int elements = 0, slots = 5, start = 0, j;
    sds *tokens;

    if (seplen < 1 || len < 0) return NULL;

    tokens = zmalloc(sizeof(sds)*slots);
    if (tokens == NULL) return NULL;

    if (len == 0) {
        *count = 0;
        return tokens;
    }
    for (j = 0; j < (len-(seplen-1)); j++) {
        /* make sure there is room for the next element and the final one */
        if (slots < elements+2) {
            sds *newtokens;
			//����2��������
            slots *= 2;
            newtokens = zrealloc(tokens,sizeof(sds)*slots);
            if (newtokens == NULL) goto cleanup;
            tokens = newtokens;
        }
        /* search the separator */
        if ((seplen == 1 && *(s+j) == sep[0]) || (memcmp(s+j,sep,seplen) == 0)) {
            tokens[elements] = sdsnewlen(s+start,j-start);
            if (tokens[elements] == NULL) goto cleanup;
            elements++;
            start = j+seplen;
            j = j+seplen-1; /* skip the separator */
        }
    }
    /* ����ʣ���sds */
    tokens[elements] = sdsnewlen(s+start,len-start);
    if (tokens[elements] == NULL) goto cleanup;
    elements++;
    *count = elements;
	//����sds����
    return tokens;
//�ͷſռ�
cleanup:
    {
        int i;
        for (i = 0; i < elements; i++) sdsfree(tokens[i]);
        zfree(tokens);
        *count = 0;
        return NULL;
    }
}

/* �ͷ�sds���� */
void sdsfreesplitres(sds *tokens, int count) {
    if (!tokens) return;
    while(count--)
        sdsfree(tokens[count]);
    zfree(tokens);
}

/* ������Ϊlen���ַ���p�Դ����ŵĸ�ʽ׷�ӵ�sds��ĩβ */
sds sdscatrepr(sds s, const char *p, size_t len) {
	//���ǰ�������
    s = sdscatlen(s,"\"",1);
    while(len--) {
        switch(*p) {
        case '\\':
        case '"':
            s = sdscatprintf(s,"\\%c",*p);
            break;
        case '\n': s = sdscatlen(s,"\\n",2); break;
        case '\r': s = sdscatlen(s,"\\r",2); break;
        case '\t': s = sdscatlen(s,"\\t",2); break;
        case '\a': s = sdscatlen(s,"\\a",2); break;
        case '\b': s = sdscatlen(s,"\\b",2); break;
        default:
            if (isprint((unsigned char)*p))                                     WIN_PORT_FIX /* cast (unsigned char) */
                s = sdscatprintf(s,"%c",*p);
            else
                s = sdscatprintf(s,"\\x%02x",(unsigned char)*p);
            break;
        }
        p++;
    }
	//���ĩβ������
    return sdscatlen(s,"\"",1);
}

/* ���ߺ���,���c��16�����е�һ��,��������(1)����˵�ж�һ���ַ��ǲ������ֻ�����ĸ */
int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/* ���ߺ���,��16���Ƶķ���ת��Ϊ10���� */
int hex_digit_to_int(char c) {
    switch(c) {
    case '0': return 0;
    case '1': return 1;
    case '2': return 2;
    case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case '7': return 7;
    case '8': return 8;
    case '9': return 9;
    case 'a': case 'A': return 10;
    case 'b': case 'B': return 11;
    case 'c': case 'C': return 12;
    case 'd': case 'D': return 13;
    case 'e': case 'E': return 14;
    case 'f': case 'F': return 15;
    default: return 0;
    }
}

/* 
	��һ���ı��ָ�Ϊ�������,�����ĸ����ᱣ����*argc��,��������һ��sds����
	���������Ҫ���ڶ�config.c�ж������ļ����н���.
	���ӣ�
    sds *arr = sdssplitargs("timeout 10086\r\nport 123321\r\n");
	��ó�
	  arr[0] = "timeout"
	  arr[1] = "10086"
	  arr[2] = "port"
	  arr[3] = "123321"
 */
sds *sdssplitargs(const char *line, int *argc) {
    const char *p = line;
    char *current = NULL;
    char **vector = NULL;

    *argc = 0;
    while(1) {
        /* �����ո� */
        while(*p && isspace(*p)) p++;
        if (*p) {
            /* get a token */
            int inq=0;  /* set to 1 if we are in "quotes" */
            int insq=0; /* set to 1 if we are in 'single quotes' */
            int done=0;

            if (current == NULL) current = sdsempty();
            while(!done) {
                if (inq) {
                    if (*p == '\\' && *(p+1) == 'x' &&
                                             is_hex_digit(*(p+2)) &&
                                             is_hex_digit(*(p+3)))
                    {
                        unsigned char byte;

                        byte = (hex_digit_to_int(*(p+2))*16)+
                                hex_digit_to_int(*(p+3));
                        current = sdscatlen(current,(char*)&byte,1);
                        p += 3;
                    } else if (*p == '\\' && *(p+1)) {
                        char c;

                        p++;
                        switch(*p) {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case 'b': c = '\b'; break;
                        case 'a': c = '\a'; break;
                        default: c = *p; break;
                        }
                        current = sdscatlen(current,&c,1);
                    } else if (*p == '"') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else if (insq) {
                    if (*p == '\\' && *(p+1) == '\'') {
                        p++;
                        current = sdscatlen(current,"'",1);
                    } else if (*p == '\'') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else {
                    switch(*p) {
                    case ' ':
                    case '\n':
                    case '\r':
                    case '\t':
                    case '\0':
                        done=1;
                        break;
                    case '"':
                        inq=1;
                        break;
                    case '\'':
                        insq=1;
                        break;
                    default:
                        current = sdscatlen(current,p,1);
                        break;
                    }
                }
                if (*p) p++;
            }
            /* add the token to the vector */
            vector = zrealloc(vector,((*argc)+1)*sizeof(char*));
            vector[*argc] = current;
            (*argc)++;
            current = NULL;
        } else {
            /* Even on empty input string return something not NULL. */
            if (vector == NULL) vector = zmalloc(sizeof(void*));
            return vector;
        }
    }

err:
    while((*argc)--)
        sdsfree(vector[*argc]);
    zfree(vector);
    if (current) sdsfree(current);
    *argc = 0;
    return NULL;
}

/* ���ַ���s��,������from�г��ֵ��ַ�,�滻Ϊto�е��ַ� */
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen) {
    size_t j, i, l = sdslen(s);

    for (j = 0; j < l; j++) {
        for (i = 0; i < setlen; i++) {
            if (s[j] == from[i]) {
                s[j] = to[i];
                break;
            }
        }
    }
    return s;
}

/* Join an array of C strings using the specified separator (also a C string).
 * Returns the result as an sds string. */
sds sdsjoin(char **argv, int argc, char *sep) {
    sds join = sdsempty();
    int j;

    for (j = 0; j < argc; j++) {
        join = sdscat(join, argv[j]);
        if (j != argc-1) join = sdscat(join,sep);
    }
    return join;
}

//#define SDS_TEST_MAIN

#ifdef SDS_TEST_MAIN
#include <stdio.h>
#include "testhelp.h"
#include "limits.h"

int main(void) {
    {
        struct sdshdr *sh;
        sds x = sdsnew("foo"), y;

        test_cond("Create a string and obtain the length",
            sdslen(x) == 3 && memcmp(x,"foo\0",4) == 0)

        sdsfree(x);
        x = sdsnewlen("foo",2);
        test_cond("Create a string with specified length",
            sdslen(x) == 2 && memcmp(x,"fo\0",3) == 0)

        x = sdscat(x,"bar");
        test_cond("Strings concatenation",
            sdslen(x) == 5 && memcmp(x,"fobar\0",6) == 0);

        x = sdscpy(x,"a");
        test_cond("sdscpy() against an originally longer string",
            sdslen(x) == 1 && memcmp(x,"a\0",2) == 0)

        x = sdscpy(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
        test_cond("sdscpy() against an originally shorter string",
            sdslen(x) == 33 &&
            memcmp(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0",33) == 0)

        sdsfree(x);
        x = sdscatprintf(sdsempty(),"%d",123);
        test_cond("sdscatprintf() seems working in the base case",
            sdslen(x) == 3 && memcmp(x,"123\0",4) == 0)

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "Hello %s World %I,%I--", "Hi!", LLONG_MIN,LLONG_MAX);
        test_cond("sdscatfmt() seems working in the base case",
            sdslen(x) == 60 &&
            memcmp(x,"--Hello Hi! World -9223372036854775808,"
                     "9223372036854775807--",60) == 0)

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "%u,%U--", UINT_MAX, ULLONG_MAX);
        test_cond("sdscatfmt() seems working with unsigned numbers",
            sdslen(x) == 35 &&
            memcmp(x,"--4294967295,18446744073709551615--",35) == 0)

        sdsfree(x);
        x = sdsnew("xxciaoyyy");
        sdstrim(x,"xy");
        test_cond("sdstrim() correctly trims characters",
            sdslen(x) == 4 && memcmp(x,"ciao\0",5) == 0)

        y = sdsdup(x);
        sdsrange(y,1,1);
        test_cond("sdsrange(...,1,1)",
            sdslen(y) == 1 && memcmp(y,"i\0",2) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,-1);
        test_cond("sdsrange(...,1,-1)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,-2,-1);
        test_cond("sdsrange(...,-2,-1)",
            sdslen(y) == 2 && memcmp(y,"ao\0",3) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,2,1);
        test_cond("sdsrange(...,2,1)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,100);
        test_cond("sdsrange(...,1,100)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,100,100);
        test_cond("sdsrange(...,100,100)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("foo");
        y = sdsnew("foa");
        test_cond("sdscmp(foo,foa)", sdscmp(x,y) > 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("bar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) == 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("aar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) < 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnewlen("\a\n\0foo\r",7);
        y = sdscatrepr(sdsempty(),x,sdslen(x));
        test_cond("sdscatrepr(...data...)",
            memcmp(y,"\"\\a\\n\\x00foo\\r\"",15) == 0)

        {
            int oldfree;

            sdsfree(x);
            sdsfree(y);
            x = sdsnew("0");
            sh = (void*) (x-(sizeof(struct sdshdr)));
            test_cond("sdsnew() free/len buffers", sh->len == 1 && sh->free == 0);
            x = sdsMakeRoomFor(x,1);
            sh = (void*) (x-(sizeof(struct sdshdr)));
            test_cond("sdsMakeRoomFor()", sh->len == 1 && sh->free > 0);
            oldfree = sh->free;
            x[1] = '1';
            sdsIncrLen(x,1);
            test_cond("sdsIncrLen() -- content", x[0] == '0' && x[1] == '1');
            test_cond("sdsIncrLen() -- len", sh->len == 2);
            test_cond("sdsIncrLen() -- free", sh->free == oldfree-1);

            sdsfree(x);
        }
    }
    test_report()
    return 0;
}
#endif
