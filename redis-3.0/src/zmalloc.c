#ifdef _WIN32
#include "Win32_Interop/Win32_Portability.h"
#include "Win32_Interop/win32_types.h"
#include "Win32_Interop/win32fixes.h"
#include "Win32_Interop/Win32_QFork.h"
#include "Win32_Interop/Win32_PThread.h"
#endif

#include <stdio.h>
#include <stdlib.h>

/*将这个函数定义在"zmalloc.h"之前就是为了避免调用jemallor或者是其他库的标准操作器，而且调用原始C语言函数库"libc"中的free()函数*/
void zlibc_free(void *ptr) {
    free(ptr);
}

#include <string.h>
POSIX_ONLY(#include <pthread.h>)
#include "config.h"
#include "zmalloc.h"

#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(PORT_LONGLONG))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#endif

/* 条件编译指令,使用TCMALLOC库 */
#if defined(USE_TCMALLOC)
#define malloc(size) tc_malloc(size)
#define calloc(count,size) tc_calloc(count,size)
#define realloc(ptr,size) tc_realloc(ptr,size)
#define free(ptr) tc_free(ptr)
/* 条件编译指令,使用JEMALLOC库 */
#elif defined(USE_JEMALLOC)
#define malloc(size) je_malloc(size)
#define calloc(count,size) je_calloc(count,size)
#define realloc(ptr,size) je_realloc(ptr,size)
#define free(ptr) je_free(ptr)
/* 条件编译指令,使用DLMALLOC库 */
#elif defined(USE_DLMALLOC)
#define malloc(size) g_malloc(size)
#define calloc(count,size) g_calloc(count,size)
#define realloc(ptr,size) g_realloc(ptr,size)
#define free(ptr) g_free(ptr)
#endif

#if defined(__ATOMIC_RELAXED)
#define update_zmalloc_stat_add(__n) __atomic_add_fetch(&used_memory, (__n), __ATOMIC_RELAXED)
#define update_zmalloc_stat_sub(__n) __atomic_sub_fetch(&used_memory, (__n), __ATOMIC_RELAXED)
#elif defined(HAVE_ATOMIC)
#define update_zmalloc_stat_add(__n) __sync_add_and_fetch(&used_memory, (__n))
#define update_zmalloc_stat_sub(__n) __sync_sub_and_fetch(&used_memory, (__n))
#else
/*pthread_mutex_lock()和pthread_mutex_unlock()使用互斥锁（mutex）来实现线程同步*/
#define update_zmalloc_stat_add(__n) do { \
    pthread_mutex_lock(&used_memory_mutex); \
    used_memory += (__n); \
    pthread_mutex_unlock(&used_memory_mutex); \
} while(0)

/*线程安全的执行used_memory减去操作*/
#define update_zmalloc_stat_sub(__n) do { \
    pthread_mutex_lock(&used_memory_mutex); \
    used_memory -= (__n); \
    pthread_mutex_unlock(&used_memory_mutex); \
} while(0)

#endif

/*
这个宏外部有一个dowhile循环,主要是为了多条语句在进行宏替换时避免分号的错误
第一个if语句等价于:  if(_n&7) _n += 8 - (_n&7);  这段代码是判断分配的内存
空间的大小是不是8的倍数,如果不是8的倍数就增加偏移量变为8的倍数,(_n&7)等价于(_n%8)
第2个if判断是不是线程安全的
*/
#define update_zmalloc_stat_alloc(__n) do { \
    size_t _n = (__n); \
    if (_n&(sizeof(PORT_LONG)-1)) _n += sizeof(PORT_LONG)-(_n&(sizeof(PORT_LONG)-1)); \
    if (zmalloc_thread_safe) { \
        update_zmalloc_stat_add(_n); \
    } else { \
        used_memory += _n; \
    } \
} while(0)

/* 这个宏函数和上面的宏大致相同,只不过是used_memory减去相应的值 */
#define update_zmalloc_stat_free(__n) do { \
    size_t _n = (__n); \
    if (_n&(sizeof(PORT_LONG)-1)) _n += sizeof(PORT_LONG)-(_n&(sizeof(PORT_LONG)-1)); \
    if (zmalloc_thread_safe) { \
        update_zmalloc_stat_sub(_n); \
    } else { \
        used_memory -= _n; \
    } \
} while(0)

static size_t used_memory = 0;
static int zmalloc_thread_safe = 0;
#ifdef _WIN32
pthread_mutex_t used_memory_mutex;
#else
pthread_mutex_t used_memory_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/*打印异常终止程序*/
static void zmalloc_default_oom(size_t size) {
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %Iu bytes\n",    WIN_PORT_FIX /* %zu -> %Iu */
        size);
    fflush(stderr);
    abort();
}

static void (*zmalloc_oom_handler)(size_t) = zmalloc_default_oom;

//size要分配的内存大小
void *zmalloc(size_t size) {
	//通过库函数实现分配内存
    void *ptr = malloc(size+PREFIX_SIZE);
	//如果分配失败,执行zmalloc_oom_handler
	//zmalloc_oom_handler是一个函数指针,指向zmalloc_default_oom,主要功能就是打印错误信息并终止程序
    if (!ptr) zmalloc_oom_handler(size);
//宏的条件编译
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
#else
	//在已经分配的空间的第一个字节处存储需要分配的字节大小
    *((size_t*)ptr) = size;
	//宏,更新全局变量used_memory的值
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
	//返回申请的内存之地,但不是首地址,而是跳过存储size的那些内存
    return (char*)ptr+PREFIX_SIZE;
#endif
}

/*
zcalloc()和zmalloc()具有相同的编程接口，实现功能基本相同，
唯一不同之处是zcalloc()会做初始化工作，而zmalloc()不会。
*/
void *zcalloc(size_t size) {
	//每次分配 size+PREFIX_SIZE 的空间，并初始化。
    void *ptr = calloc(1, size+PREFIX_SIZE);

    if (!ptr) zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
#else
    *((size_t*)ptr) = size;
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
    return (char*)ptr+PREFIX_SIZE;
#endif
}


/*
realloc()要完成的功能是给首地址ptr的内存空间，重新分配大小。
如果失败了，则在其它位置新建一块大小为size字节的空间，
将原先的数据复制到新的内存空间，并返回这段内存首地址【原内存会被系统自然释放】。
zrealloc函数实现的功能与之相同,内部使用其余函数实现
*/
void *zrealloc(void *ptr, size_t size) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *newptr;

    if (ptr == NULL) return zmalloc(size);
#ifdef HAVE_MALLOC_SIZE
    oldsize = zmalloc_size(ptr);
    newptr = realloc(ptr,size);
    if (!newptr) zmalloc_oom_handler(size);

    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(zmalloc_size(newptr));
    return newptr;
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    newptr = realloc(realptr,size+PREFIX_SIZE);
    if (!newptr) zmalloc_oom_handler(size);

    *((size_t*)newptr) = size;
    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(size);
    return (char*)newptr+PREFIX_SIZE;
#endif
}

/* Provide zmalloc_size() for systems where this function is not provided by
 * malloc itself, given that in that case we store a header with this
 * information as the first bytes of every allocation. */
#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr) {
    void *realptr = (char*)ptr-PREFIX_SIZE;
    size_t size = *((size_t*)realptr);
    /* Assume at least that all the allocations are padded at sizeof(PORT_LONG) by
     * the underlying allocator. */
    if (size&(sizeof(PORT_LONG)-1)) size += sizeof(PORT_LONG)-(size&(sizeof(PORT_LONG)-1));
    return size+PREFIX_SIZE;
}
#endif

/*释放空间函数*/
void zfree(void *ptr) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL) return;
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(zmalloc_size(ptr));
    free(ptr);
#else
	//ptr指针向前偏移8字节的长度,即回退到初始malloc返回的地址得到realptr
    realptr = (char*)ptr-PREFIX_SIZE;
	//先类型转换再取出指针所指向的值,这里存储着最初要分配的内存大小
    oldsize = *((size_t*)realptr);
	//宏红函数
    update_zmalloc_stat_free(oldsize+PREFIX_SIZE);
	//最后free释放空间
    free(realptr);
#endif
}

/*字符串复制函数*/
char *zstrdup(const char *s) {
	//首先，先获得字符串s的长度，新闻strlen()函数是不统计'\0'的，所以最后要加1。
    size_t l = strlen(s)+1;
	//然后调用zmalloc()来分配足够的空间，首地址为p。
	char *p = zmalloc(l);
	//调用memcpy来完成复制。
    memcpy(p,s,l);
    return p;
}

size_t zmalloc_used_memory(void) {
    size_t um;

    if (zmalloc_thread_safe) {
#if defined(__ATOMIC_RELAXED) || defined(HAVE_ATOMIC)
        um = update_zmalloc_stat_add(0);
#else
        pthread_mutex_lock(&used_memory_mutex);
        um = used_memory;
        pthread_mutex_unlock(&used_memory_mutex);
#endif
    }
    else {
        um = used_memory;
    }

    return um;
}

#ifdef _WIN32
void zmalloc_free_used_memory_mutex(void) {
    /* Windows fix: Callabe mutex destroy.  */
    if (zmalloc_thread_safe)
        pthread_mutex_destroy(&used_memory_mutex);
}
void zmalloc_enable_thread_safeness(void) {
	if (!zmalloc_thread_safe){
		//下面这个函数是一个宏定义:pthread_mutex_init(a,b) (InitializeCriticalSectionAndSpinCount((a), 0x80000400),0)
		//InitializeCriticalSectionAndSpinCount:是windows中自带的一个临界区初始化的函数,循环锁相关内容
		pthread_mutex_init(&used_memory_mutex, 0);
	}

    zmalloc_thread_safe = 1;
}
#else
void zmalloc_enable_thread_safeness(void) {
    zmalloc_thread_safe = 1;
}
#endif

void zmalloc_set_oom_handler(void (*oom_handler)(size_t)) {
    zmalloc_oom_handler = oom_handler;
}

/* Get the RSS information in an OS-specific way.
 *
 * WARNING: the function zmalloc_get_rss() is not designed to be fast
 * and may not be called in the busy loops where Redis tries to release
 * memory expiring or swapping out objects.
 *
 * For this kind of "fast RSS reporting" usages use instead the
 * function RedisEstimateRSS() that is a much faster (and less precise)
 * version of the function. */

#if defined(HAVE_PROC_STAT)
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

size_t zmalloc_get_rss(void) {
    int page = sysconf(_SC_PAGESIZE);
    size_t rss;
    char buf[4096];
    char filename[256];
    int fd, count;
    char *p, *x;

    snprintf(filename,256,"/proc/%d/stat",getpid());
    if ((fd = open(filename,O_RDONLY)) == -1) return 0;
    if (read(fd,buf,4096) <= 0) {
        close(fd);
        return 0;
    }
    close(fd);

    p = buf;
    count = 23; /* RSS is the 24th field in /proc/<pid>/stat */
    while(p && count--) {
        p = strchr(p,' ');
        if (p) p++;
    }
    if (!p) return 0;
    x = strchr(p,' ');
    if (!x) return 0;
    *x = '\0';

    rss = strtoll(p,NULL,10);
    rss *= page;
    return rss;
}
#elif defined(HAVE_TASKINFO)
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/task.h>
#include <mach/mach_init.h>

size_t zmalloc_get_rss(void) {
    task_t task = MACH_PORT_NULL;
    struct task_basic_info t_info;
    mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

    if (task_for_pid(current_task(), getpid(), &task) != KERN_SUCCESS)
        return 0;
    task_info(task, TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count);

    return t_info.resident_size;
}
#else
size_t zmalloc_get_rss(void) {
    /* If we can't get the RSS in an OS-specific way for this system just
     * return the memory usage we estimated in zmalloc()..
     *
     * Fragmentation will appear to be always 1 (no fragmentation)
     * of course... */
    return zmalloc_used_memory();
}
#endif

/* Fragmentation = RSS / allocated-bytes */
float zmalloc_get_fragmentation_ratio(size_t rss) {
    return (float)rss/zmalloc_used_memory();
}

/* Get the sum of the specified field (converted form kb to bytes) in
 * /proc/self/smaps. The field must be specified with trailing ":" as it
 * apperas in the smaps output.
 *
 * Example: zmalloc_get_smap_bytes_by_field("Rss:");
 */
#if defined(HAVE_PROC_SMAPS)
size_t zmalloc_get_smap_bytes_by_field(char *field) {
    char line[1024];
    size_t bytes = 0;
    FILE *fp = fopen("/proc/self/smaps","r");
    int flen = strlen(field);

    if (!fp) return 0;
    while(fgets(line,sizeof(line),fp) != NULL) {
        if (strncmp(line,field,flen) == 0) {
            char *p = strchr(line,'k');
            if (p) {
                *p = '\0';
                bytes += strtol(line+flen,NULL,10) * 1024;
            }
        }
    }
    fclose(fp);
    return bytes;
}
#else
size_t zmalloc_get_smap_bytes_by_field(char *field) {
    ((void) field);
    return 0;
}
#endif

size_t zmalloc_get_private_dirty(void) {
    return zmalloc_get_smap_bytes_by_field("Private_Dirty:");
}
