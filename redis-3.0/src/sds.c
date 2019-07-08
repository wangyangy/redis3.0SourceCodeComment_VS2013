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
redis中没有使用c语言中传统的字符串,而是构建了自己的简单动态字符串SDS
*/

/*
   根据给定的初始化字符串init和字符串长度创建一个sds,
   创建一个sdshdr结构体
*/
sds sdsnewlen(const void *init, size_t initlen) {
    struct sdshdr *sh;
	//申请空间,整个结构体的空间,包括len,free和要初始化的char数组空间,所以空间由两部分组成
    if (init) {
		//zmalloc不初始化锁分配的内存空间
        sh = zmalloc(sizeof(struct sdshdr)+initlen+1);
    } else {
		//将分配的内存空间全部初始化为0
        sh = zcalloc(sizeof(struct sdshdr)+initlen+1);
    }
	//分配空间失败
    if (sh == NULL) return NULL;
	//设置初始化长度
    sh->len = (int)initlen;                                                     WIN_PORT_FIX /* cast (int) */
    sh->free = 0;
	//如果给定了init,且initlen不为0,则将init的内容复制到buf中
    if (initlen && init)
        memcpy(sh->buf, init, initlen);
    sh->buf[initlen] = '\0';
	//返回buf的地址
    return (char*)sh->buf;
}

/*创建一个只保存了空字符串""的sds,底层调用sdsnewlen函数实现*/
sds sdsempty(void) {
    return sdsnewlen("",0);
}

/* 根据给定的init创建一个sds */
sds sdsnew(const char *init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

/* 复制一个sds */
sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

/* 释放给定的sds */
void sdsfree(sds s) {
    if (s == NULL) return;
    zfree(s-sizeof(struct sdshdr));
}

/* 未使用的函数可能已经废弃
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
	在不释放sds空间的情况下,清空一个sds结构 
	重置sds所保存的字符串为空字符串
*/
void sdsclear(sds s) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    sh->free += sh->len;
    sh->len = 0;
    sh->buf[0] = '\0';
}

/* 扩大参数sds对应的sdshdr结构的free大小,相当于扩容,扩张大小为addlen */
sds sdsMakeRoomFor(sds s, size_t addlen) {
    struct sdshdr *sh, *newsh;
    size_t free = sdsavail(s);
    size_t len, newlen;
	//如果free大于addlen则直接返回
    if (free >= addlen) return s;
	//得到已经使用的长度
    len = sdslen(s);
	//得到sdshdr地址
    sh = (void*) (s-(sizeof(struct sdshdr)));
	//扩容后的大小
    newlen = (len+addlen);
    if (newlen < SDS_MAX_PREALLOC)
        newlen *= 2;
    else
        newlen += SDS_MAX_PREALLOC;
	//重新分配空间
    newsh = zrealloc(sh, sizeof(struct sdshdr)+newlen+1);
    if (newsh == NULL) return NULL;
	//调整大小
    newsh->free = (int)(newlen - len);                                          WIN_PORT_FIX /* cast (int) */
    return newsh->buf;
}

/* 回收sdshdr中的空闲空间 */
sds sdsRemoveFreeSpace(sds s) {
    struct sdshdr *sh;

    sh = (void*) (s-(sizeof(struct sdshdr)));
	//重新分配空间,去掉空余的长度
    sh = (struct sdshdr *)zrealloc(sh, sizeof(struct sdshdr)+sh->len+1);        WIN_PORT_FIX /* cast (struct sdshdr *) */
    sh->free = 0;
    return sh->buf;
}

/* 返回sdshdr的总空间,包含4部分的内容
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

/* 增加sdshdr中len的大小,通过减少free实现,增加的大小为incr*/
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

/* 用\0填充len大小的空余空间 */
sds sdsgrowzero(sds s, size_t len) {
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    size_t totlen, curlen = sh->len;

    if (len <= curlen) return s;
    s = sdsMakeRoomFor(s,len-curlen);
    if (s == NULL) return NULL;

    /* Make sure added region doesn't contain garbage */
    sh = (void*)(s-(sizeof(struct sdshdr)));
	//void *memset(void *s,int c,size_t n)
	//memset函数将已开辟内存空间 s 的首 n 个字节的值设为值 c。
    memset(s+curlen,0,(len-curlen+1)); /* 后面的空间用\0填充 */
    totlen = sh->len+sh->free;
    sh->len = (int)len;                                                         WIN_PORT_FIX /* cast (int) */
    sh->free = (int)(totlen-sh->len);                                           WIN_PORT_FIX /* cast (int) */
    return s;
}

/* 将长度为t的字符串追加到sds字符串末尾 */
sds sdscatlen(sds s, const void *t, size_t len) {
    struct sdshdr *sh;
    size_t curlen = sdslen(s);
	//扩容
    s = sdsMakeRoomFor(s,len);
    if (s == NULL) return NULL;
    sh = (void*) (s-(sizeof(struct sdshdr)));
	//将字符串t的内容复制到sds的末尾
    memcpy(s+curlen, t, len);
    sh->len = (int)(curlen+len);                                                WIN_PORT_FIX /* cast (int) */
    sh->free = (int)(sh->free-len);                                             WIN_PORT_FIX /* cast (int) */
    s[curlen+len] = '\0';
    return s;
}

/* 给定字符串t追加到sds的尾部 */
sds sdscat(sds s, const char *t) {
    return sdscatlen(s, t, strlen(t));
}

/* 给定sds追加到源sds的尾部 */
sds sdscatsds(sds s, const sds t) {
    return sdscatlen(s, t, sdslen(t));
}

/* 
	将字符串t的前len个字符复制到sds中,并在字符串末尾添加终结符
	如果sds的长度小于len,则扩容.
*/
sds sdscpylen(sds s, const char *t, size_t len) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t totlen = sh->free+sh->len;
	//扩容
    if (totlen < len) {
        s = sdsMakeRoomFor(s,len-sh->len);
        if (s == NULL) return NULL;
        sh = (void*) (s-(sizeof(struct sdshdr)));
        totlen = sh->free+sh->len;
    }
	//复制t前len个字符到sds中,覆盖sds原有的内容
    memcpy(s, t, len);
    s[len] = '\0';
    sh->len = (int)len;                                                         WIN_PORT_FIX /* cast (int) */
    sh->free = (int)(totlen-len);                                               WIN_PORT_FIX /* cast (int) */
    return s;
}

/* 将字符串复制到sds中,覆盖原有的数据 */
sds sdscpy(sds s, const char *t) {
    return sdscpylen(s, t, strlen(t));
}

/* 工具函数将数值转化为字符串 */
#define SDS_LLSTR_SIZE 21
int sdsll2str(char *s, PORT_LONGLONG value) {
    char *p, aux;
    PORT_ULONGLONG v;
    size_t l;

    v = (value < 0) ? -value : value;
	//将数值转化为字符串
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);
	//如果是负数则添加负号
    if (value < 0) *p++ = '-';

    /* 计算转化后的长度 */
    l = p-s;
    *p = '\0';

    /* 将字符串逆序 */
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
	//返回转化后的长度
    return (int)l;                                                              WIN_PORT_FIX /* cast (int) */
}

/* 工具函数将数值转化为字符串 */
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
	根据输入的long long值value转化为sds,将数值转化为sds
 */
sds sdsfromlonglong(PORT_LONGLONG value) {
    char buf[SDS_LLSTR_SIZE];
	//将数值转化为字符串,并获取字符串的长度
    int len = sdsll2str(buf,value);
	//创建sds对象
    return sdsnewlen(buf,len);
}

/* 打印函数,被sdscatprinf调用,va_list是一个可变参数类型*/
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
		//将参数拷贝到cpy中
        va_copy(cpy,ap);
		//c语言库函数,用于可变参数,将可变参数格式化输出到字符数组中
        vsnprintf(buf, buflen WIN32_ONLY(-1), fmt, cpy);    // WIN_PORT_FIX: see comment below
        //结束可变参数操作
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
	打印任意数量个字符串,并将这些给定的字符串追加到sds的末尾
 */
sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap;
    char *t;
	//将第一个可变参数的地址复给ap,即ap指向可变参数的开始位置
    va_start(ap, fmt);
    t = sdscatvprintf(s,fmt,ap);
    va_end(ap);
    return t;
}

/* 这个函数和sdscatprintf功能一样,只不过比sdscatprintf要快很多
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
	//获取可变参数的首地址,给ap
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
				//获取可变参数列表中的一个参数,取出ap里面的一个可变参数,指针会自动后移
				//参数char*根据实际的参数类型变化
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

/* 对sds左右两端进行剪修,将参数*cest中的内容删除
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
	//strchr:从字符串cset中找*sp第一次出现的位置,没有返回NULL
    while(sp <= end && strchr(cset, *sp)) sp++;
    while(ep > start && strchr(cset, *ep)) ep--;
    len = (sp > ep) ? 0 : ((ep-sp)+1);
	//memmove:将sp中的len个字节复制到sh->buf中,函数memmove与memcpy功能基本一致
	//但是当src和dest内存区域有重叠部分的时候memcpy可能会报错,而memmove不会报错
    if (sh->buf != sp) memmove(sh->buf, sp, len);
    sh->buf[len] = '\0';
    sh->free = sh->free+(int)(sh->len-len);                                     WIN_PORT_FIX /* cast (int) */
    sh->len = (int)len;                                                         WIN_PORT_FIX /* cast (int) */
    return s;
}

/* 按索引对截取sds中的一段数据
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
	//计算截取的长度
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
	//将截取的内容复制到sds->buf中
    if (start && newlen) memmove(sh->buf, sh->buf+start, newlen);
    sh->buf[newlen] = 0;
    sh->free = (int)(sh->free+(sh->len-newlen));                                WIN_PORT_FIX /* cast (int) */
    sh->len = (int)newlen;                                                      WIN_PORT_FIX /* cast (int) */
}

/* 将sds字符串小写 */
void sdstolower(sds s) {
    int len = (int)sdslen(s), j;                                                WIN_PORT_FIX /* cast (int) */

    for (j = 0; j < len; j++) s[j] = tolower(s[j]);
}

/* 将字符串sds大写 */
void sdstoupper(sds s) {
    int len = (int)sdslen(s), j;                                                WIN_PORT_FIX /* cast (int) */

    for (j = 0; j < len; j++) s[j] = toupper(s[j]);
}

/* 比较两个sds结构
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
	//调用库函数实现字符串的对比
    cmp = memcmp(s1,s2,minlen);
    if (cmp == 0) return (int)(l1-l2);                                          WIN_PORT_FIX /* cast (int) */
    return cmp;
}

/* 使用分隔符sep对sds进行分割,返回一个sds数组,*count会被设置为返回数组的元素数量
 * 如果出现内存不足,字符串长度为0或者分隔符长度为0就返回NULL
 * 分隔符可以是字符串
 * This version of the function is binary-safe but
 * requires length arguments. sdssplit() is just the
 * same function but for zero-terminated strings.
 */
sds *sdssplitlen(const char *s, int len, const char *sep, int seplen, int *count) {
    //slots=5说明默认的分配数组长度是5
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
			//按照2倍的增长
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
    /* 创建剩余的sds */
    tokens[elements] = sdsnewlen(s+start,len-start);
    if (tokens[elements] == NULL) goto cleanup;
    elements++;
    *count = elements;
	//返回sds数组
    return tokens;
//释放空间
cleanup:
    {
        int i;
        for (i = 0; i < elements; i++) sdsfree(tokens[i]);
        zfree(tokens);
        *count = 0;
        return NULL;
    }
}

/* 释放sds数组 */
void sdsfreesplitres(sds *tokens, int count) {
    if (!tokens) return;
    while(count--)
        sdsfree(tokens[count]);
    zfree(tokens);
}

/* 将长度为len的字符串p以带引号的格式追加到sds的末尾 */
sds sdscatrepr(sds s, const char *p, size_t len) {
	//添加前面的引号
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
	//添加末尾的引号
    return sdscatlen(s,"\"",1);
}

/* 工具函数,如果c是16进制中的一个,返回正数(1)或者说判断一个字符是不是数字或者字母 */
int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/* 工具函数,将16进制的符号转换为10进制 */
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
	将一行文本分割为多个参数,参数的个数会保存在*argc中,函数返回一个sds数组
	这个函数主要用于对config.c中对配置文件进行解析.
	例子：
    sds *arr = sdssplitargs("timeout 10086\r\nport 123321\r\n");
	会得出
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
        /* 跳过空格 */
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

/* 将字符串s中,所有在from中出现的字符,替换为to中的字符 */
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
