#include "redis.h"
#include <math.h> /* isnan(), isinf() */

/*-----------------------------------------------------------------------------
 * String Commands
 t_string其主要是对sds字符串的的函数调用，以及和客户端（redisClient）的数据交互过程：
 根据客户端的操作命令（如：SET /GET/SETNX/INCR等等）传入客户端对象——>
 从对象的数据缓冲区中取出各命令操作参数——>检测参数——>调用底层函数对键、值执行相应命令操作——>
 把操作结果返回给客户端对象数据缓冲区。
 *----------------------------------------------------------------------------*/

/*检测字符串是否超过512M的限制*/
static int checkStringLength(redisClient *c, PORT_LONGLONG size) {
    if (size > 512*1024*1024) {
        addReplyError(c,"string exceeds maximum allowed size (512MB)");
        return REDIS_ERR;
    }
    return REDIS_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 * 'flags' changes the behavior of the command (NX or XX, see belove).
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used. */

#define REDIS_SET_NO_FLAGS 0
#define REDIS_SET_NX (1<<0)     /* Set if key not exists. */
#define REDIS_SET_XX (1<<1)     /* Set if key exists. */

/*该函数是命令:SET,SETEX,PSETEX,SETNX的最底层的实现,flags可以是NX或XX,由上面的宏提供
  expire定义key的过期时间,ok_reply和abort_reply保存着回复client的内容,如果ok_reply为空
  则使用"+OK",如果abort_reply为空则使用$-1*/
void setGenericCommand(redisClient *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {
    PORT_LONGLONG milliseconds = 0; /* initialized to avoid any harmness warning */
	//如果定义了key的过期时间
    if (expire) {
		//从expire对象中取出值，保存在milliseconds中，如果出错发送默认的信息给client
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != REDIS_OK)
            return;
		// 如果过期时间小于等于0，则发送错误信息给client
        if (milliseconds <= 0) {
            addReplyErrorFormat(c,"invalid expire time in %s",c->cmd->name);
            return;
        }
		//如果unit的单位是秒，则需要转换为毫秒保存
        if (unit == UNIT_SECONDS) milliseconds *= 1000;
    }
	//lookupKeyWrite函数是为执行写操作而取出key的值对象
	//如果设置了NX(不存在)，并且在数据库中 找到 该key，或者
	//设置了XX(存在)，并且在数据库中 没有找到 该key
	//回复abort_reply给client
    if ((flags & REDIS_SET_NX && lookupKeyWrite(c->db,key) != NULL) ||
        (flags & REDIS_SET_XX && lookupKeyWrite(c->db,key) == NULL))
    {
        addReply(c, abort_reply ? abort_reply : shared.nullbulk);
        return;
    }
	//在当前db设置键为key的值为val
    setKey(c->db,key,val);
	//设置数据库为脏(dirty)，服务器每次修改一个key后，都会对脏键(dirty)增1
    server.dirty++;
	//设置key的过期时间,mstime()返回毫秒为单位的格林威治时间
    if (expire) setExpire(c->db,key,mstime()+milliseconds);
	//发送"set"事件的通知，用于发布订阅模式，通知客户端接受发生的事件
    notifyKeyspaceEvent(REDIS_NOTIFY_STRING,"set",key,c->db->id);
	//发送"expire"事件通知
    if (expire) notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,
        "expire",key,c->db->id);
	//设置成功，则向客户端发送ok_reply
    addReply(c, ok_reply ? ok_reply : shared.ok);
}

/* SET key value [NX] [XX] [EX <seconds>] [PX <milliseconds>] */
/*设定该Key持有指定的字符串Value，如果该Key已经存在，则覆盖其原有值。
  
*/
void setCommand(redisClient *c) {
    int j;
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = REDIS_SET_NO_FLAGS;
	//设置选项参数
    for (j = 3; j < c->argc; j++) {
        char *a = c->argv[j]->ptr;
        robj *next = (j == c->argc-1) ? NULL : c->argv[j+1];
		//NX
        if ((a[0] == 'n' || a[0] == 'N') &&
            (a[1] == 'x' || a[1] == 'X') && a[2] == '\0') {
            flags |= REDIS_SET_NX;
		//XX
        } else if ((a[0] == 'x' || a[0] == 'X') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0') {
            flags |= REDIS_SET_XX;
		//EX
        } else if ((a[0] == 'e' || a[0] == 'E') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' && next) {
            unit = UNIT_SECONDS;
            expire = next;
            j++;
		//PX
        } else if ((a[0] == 'p' || a[0] == 'P') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' && next) {
            unit = UNIT_MILLISECONDS;
            expire = next;
            j++;
		//否则语法错误
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }
	//尝试对值对象进行编码
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,flags,c->argv[1],c->argv[2],expire,unit,NULL,NULL);
}

/*如果指定的Key不存在，则设定该Key持有指定字符串Value，此时其效果等价于SET命令。
相反，如果该Key已经存在，该命令将不做任何操作并返回。*/
void setnxCommand(redisClient *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,REDIS_SET_NX,c->argv[1],c->argv[2],NULL,0,shared.cone,shared.czero);
}

//原子性完成两个操作，一是设置该Key的值为指定字符串，同时设置该Key在Redis服务器中的存活时间(秒数)。该命令主要应用于Redis被当做Cache服务器使用时
void setexCommand(redisClient *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,REDIS_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS,NULL,NULL);
}

void psetexCommand(redisClient *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,REDIS_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS,NULL,NULL);
}


//GET  key的值对象
int getGenericCommand(redisClient *c) {
    robj *o;
	// 尝试从数据库中取出键 c->argv[1] 对应的值对象
	// 如果键不存在时，向客户端发送回复信息，并返回 NULL
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL)
        return REDIS_OK;
	// 值对象存在，检查它的类型
    if (o->type != REDIS_STRING) {
		// 类型错误
        addReply(c,shared.wrongtypeerr);
        return REDIS_ERR;
    } else {
		// 类型正确，向客户端返回对象的值
        addReplyBulk(c,o);
        return REDIS_OK;
    }
}

// 获取指定Key的Value
void getCommand(redisClient *c) {
    getGenericCommand(c);
}


//原子性的设置该Key为指定的Value，同时返回该Key的原有值。和GET命令一样，
//该命令也只能处理string Value，否则Redis将给出相关的错误信息。
void getsetCommand(redisClient *c) {

	// 取出并返回键的值对象
    if (getGenericCommand(c) == REDIS_ERR) return;
	// 编码键的新值 c->argv[2]
    c->argv[2] = tryObjectEncoding(c->argv[2]);
	// 将数据库中关联键 c->argv[1] 和新值对象 c->argv[2]
    setKey(c->db,c->argv[1],c->argv[2]);
	// 发送事件通知
    notifyKeyspaceEvent(REDIS_NOTIFY_STRING,"set",c->argv[1],c->db->id);
	// 将服务器设为脏
    server.dirty++;
}

/*替换指定Key的部分字符串值。从offset开始，替换的长度为该命令第三个参数value的字符串长度，
其中如果offset的值大于该Key的原有值Value的字符串长度，
Redis将会在Value的后面补齐(offset - strlen(value))数量的0x00，之后再追加新值。
如果该键不存在，该命令会将其原值的长度假设为0，并在其后添补offset个0x00后再追加新值。
鉴于字符串Value的最大长度为512M，因此offset的最大值为536870911。
最后需要注意的是，如果该命令在执行时致使指定Key的原有值长度增加，
这将会导致Redis重新分配足够的内存以容纳替换后的全部字符串，因此就会带来一定的性能折损。
*/
void setrangeCommand(redisClient *c) {
    robj *o;
    PORT_LONG offset;
    sds value = c->argv[3]->ptr;
	//取出offset参数
    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != REDIS_OK)
        return;

    if (offset < 0) {
        addReplyError(c,"offset is out of range");
        return;
    }
	//取出key对应的value
    o = lookupKeyWrite(c->db,c->argv[1]);
	//value为空
    if (o == NULL) {
        /* value为空,没有什么可以设置,返回0 */
        if (sdslen(value) == 0) {
            addReply(c,shared.czero);
            return;
        }

        /* 如果设置后的长度会超过redis的限制,则放弃设置返回error */
        if (checkStringLength(c,offset+sdslen(value)) != REDIS_OK)
            return;
		//可以设置,则创建一个空字符串对象并在数据库中关联键c->argv[1]和这个对象
        o = createObject(REDIS_STRING,sdsempty());
        dbAdd(c->db,c->argv[1],o);
	//value不为空
    } else {
        size_t olen;

        /* 检查对象类型 */
        if (checkType(c,o,REDIS_STRING))
            return;

        /* 取出原有字符串长度 */
        olen = stringObjectLen(o);
		//字符串长度为0,没有可以设置的,向客户端返回0
        if (sdslen(value) == 0) {
            addReplyLongLong(c,olen);
            return;
        }

        /* 如果设置后会超出redis的限制,则放弃设置,返回错误 */
        if (checkStringLength(c,offset+sdslen(value)) != REDIS_OK)
            return;

        /* Create a copy when the object is shared or encoded. */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }

    if (sdslen(value) > 0) {
		//扩展字符串值对象
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value));
		//将value复制到指定的位置
        memcpy((char*)o->ptr+offset,value,sdslen(value));
		//向数据库发送键被修改的信号
        signalModifiedKey(c->db,c->argv[1]);
		//发送事件通知
        notifyKeyspaceEvent(REDIS_NOTIFY_STRING,
            "setrange",c->argv[1],c->db->id);
		//设置服务器为脏
        server.dirty++;
    }
	//设置成功,返回新的字符串给客户端
    addReplyLongLong(c,sdslen(o->ptr));
}


/*如果截取的字符串长度很短，我们可以该命令的时间复杂度视为O(1)，否则就是O(N)，
这里N表示截取的子字符串长度。该命令在截取子字符串时，
将以闭区间的方式同时包含start(0表示第一个字符)和end所在的字符，
如果end值超过Value的字符长度，该命令将只是截取从start开始之后所有的字符数据。
*/
void getrangeCommand(redisClient *c) {
    robj *o;
    PORT_LONGLONG start, end;
    char *str, llbuf[32];
    size_t strlen;
	// 取出 start 参数
    if (getLongLongFromObjectOrReply(c,c->argv[2],&start,NULL) != REDIS_OK)
        return;
	// 取出 end 参数
    if (getLongLongFromObjectOrReply(c,c->argv[3],&end,NULL) != REDIS_OK)
        return;
	// 从数据库中查找键 c->argv[1] 
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||
        checkType(c,o,REDIS_STRING)) return;
	// 根据编码，对对象的值进行处理
    if (o->encoding == REDIS_ENCODING_INT) {
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(PORT_LONG)o->ptr);
    } else {
        str = o->ptr;
        strlen = sdslen(str);
    }

	// 将负数索引转换为正数索引
	if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if ((PORT_ULONGLONG)end >= strlen) end = strlen-1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end || strlen == 0) {
		// 处理索引范围为空的情况
        addReply(c,shared.emptybulk);
    } else {
		// 向客户端返回给定范围内的字符串内容
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1);
    }
}

/*N表示获取Key的数量。返回所有指定Keys的Values，
如果其中某个Key不存在，或者其值不为string类型，该Key的Value将返回nil。
*/
void mgetCommand(redisClient *c) {
    int j;

    addReplyMultiBulkLen(c,c->argc-1);
	// 查找并返回所有输入键的值
    for (j = 1; j < c->argc; j++) {
		// 查找键 c->argc[j] 的值
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) {
			// 值不存在，向客户端发送空回复
            addReply(c,shared.nullbulk);
        } else {
			// 值存在，但不是字符串类型
            if (o->type != REDIS_STRING) {
                addReply(c,shared.nullbulk);
			// 值存在，并且是字符串
            } else {
                addReplyBulk(c,o);
            }
        }
    }
}

/*N表示指定Key的数量。该命令原子性的完成参数中所有key/value的设置操作，
其具体行为可以看成是多次迭代执行SET命令。 */
void msetGenericCommand(redisClient *c, int nx) {
    int j, busykeys = 0;
	// 键值参数不是成相成对出现的，格式不正确
    if ((c->argc % 2) == 0) {
        addReplyError(c,"wrong number of arguments for MSET");
        return;
    }
    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set nothing at all if at least one already key exists. */
	// 如果 nx 参数为真，那么检查所有输入键在数据库中是否存在
	// 只要有一个键是存在的，那么就向客户端发送空回复
	// 并放弃执行接下来的设置操作
    if (nx) {
        for (j = 1; j < c->argc; j += 2) {
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) {
                busykeys++;
            }
        }
		// 键存在,发送空白回复，并放弃执行接下来的设置操作
        if (busykeys) {
            addReply(c, shared.czero);
            return;
        }
    }

	// 设置所有键值对
    for (j = 1; j < c->argc; j += 2) {
		// 对值对象进行解码
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);
		// 将键值对关联到数据库,c->argc[j] 为键,c->argc[j+1] 为值
        setKey(c->db,c->argv[j],c->argv[j+1]);
		// 发送事件通知
        notifyKeyspaceEvent(REDIS_NOTIFY_STRING,"set",c->argv[j],c->db->id);
    }
    server.dirty += (c->argc-1)/2;
	// 设置成功,MSET 返回 OK ，而 MSETNX 返回 1
    addReply(c, nx ? shared.cone : shared.ok);
}

void msetCommand(redisClient *c) {
    msetGenericCommand(c,0);
}

void msetnxCommand(redisClient *c) {
    msetGenericCommand(c,1);
}


/*增减key对应的value对象*/
void incrDecrCommand(redisClient *c, PORT_LONGLONG incr) {
    PORT_LONGLONG value, oldvalue;
    robj *o, *new;
	// 取出值对象
    o = lookupKeyWrite(c->db,c->argv[1]);
	// 检查对象是否存在，以及类型是否正确
    if (o != NULL && checkType(c,o,REDIS_STRING)) return;
	// 取出对象的整数值，并保存到 value 参数中
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != REDIS_OK) return;
	// 检查加法操作执行之后值是否会溢出
	// 如果是的话，就向客户端发送一个出错回复，并放弃设置操作
    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
	// 进行加法计算，并将值保存到新的值对象中,然后用新的值对象替换原来的值对象
    value += incr;
    if (o && o->refcount == 1 && o->encoding == REDIS_ENCODING_INT &&
        (value < 0 || value >= REDIS_SHARED_INTEGERS) &&
        value >= LONG_MIN && value <= LONG_MAX)
    {
        new = o;
        o->ptr = (void*)((PORT_LONG)(value));
    } else {
        new = createStringObjectFromLongLong(value);
        if (o) {
            dbOverwrite(c->db,c->argv[1],new);
        } else {
            dbAdd(c->db,c->argv[1],new);
        }
    }
	// 向数据库发送键被修改的信号
    signalModifiedKey(c->db,c->argv[1]);
	// 发送事件通知
    notifyKeyspaceEvent(REDIS_NOTIFY_STRING,"incrby",c->argv[1],c->db->id);
    server.dirty++;
	// 返回回复
    addReply(c,shared.colon);
    addReply(c,new);
    addReply(c,shared.crlf);
}


/*将指定Key的Value原子性的递增1。
如果该Key不存在，其初始值为0，在incr之后其值为1。
如果Value的值不能转换为整型值，如Hello，该操作将执行失败并返回相应的错误信息。
注意：该操作的取值范围是64位有符号整型。 */
void incrCommand(redisClient *c) {
    incrDecrCommand(c,1);
}

/*将指定Key的Value原子性的递减1。
如果该Key不存在，其初始值为0，在decr之后其值为-1。
如果Value的值不能转换为整型值，如Hello，该操作将执行失败并返回相应的错误信息。
注意：该操作的取值范围是64位有符号整型。*/
void decrCommand(redisClient *c) {
    incrDecrCommand(c,-1);
}
/*将指定Key的Value原子性的增加increment。
如果该Key不存在，其初始值为0，在incrby之后其值为increment。
如果Value的值不能转换为整型值，如Hello，该操作将执行失败并返回相应的错误信息。
注意：该操作的取值范围是64位有符号整型。 */
void incrbyCommand(redisClient *c) {
    PORT_LONGLONG incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;
    incrDecrCommand(c,incr);
}
/*将指定Key的Value原子性的减少decrement。如果该Key不存在，
其初始值为0，在decrby之后其值为-decrement。如果Value的值不能转换为整型值，
如Hello，该操作将执行失败并返回相应的错误信息。
注意：该操作的取值范围是64位有符号整型。 */
void decrbyCommand(redisClient *c) {
    PORT_LONGLONG incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;
    incrDecrCommand(c,-incr);
}

void incrbyfloatCommand(redisClient *c) {
    PORT_LONGDOUBLE incr, value;
    robj *o, *new, *aux;
	//取出值对象
    o = lookupKeyWrite(c->db,c->argv[1]);
	// 检查对象是否存在，以及类型是否正确
    if (o != NULL && checkType(c,o,REDIS_STRING)) return;
	// 将对象的整数值保存到 value 参数中
	// 并取出 incr 参数的值
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != REDIS_OK ||
        getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != REDIS_OK)
        return;
	// 进行加法计算，并检查是否溢出
    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }
	// 用一个包含新值的新对象替换现有的值对象
    new = createStringObjectFromLongDouble(value,1);
    if (o)
        dbOverwrite(c->db,c->argv[1],new);
    else
        dbAdd(c->db,c->argv[1],new);
	// 向数据库发送键被修改的信号
    signalModifiedKey(c->db,c->argv[1]);
	// 发送事件通知
    notifyKeyspaceEvent(REDIS_NOTIFY_STRING,"incrbyfloat",c->argv[1],c->db->id);
    server.dirty++;
	// 回复
    addReplyBulk(c,new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
	// 在传播 INCRBYFLOAT 命令时，总是用 SET 命令来替换 INCRBYFLOAT 命令
	// 从而防止因为不同的浮点精度和格式化造成 AOF 重启时的数据不一致
    aux = createStringObject("SET",3);
    rewriteClientCommandArgument(c,0,aux);
    decrRefCount(aux);
    rewriteClientCommandArgument(c,2,new);
}

/*如果该Key已经存在，APPEND命令将参数Value的数据追加到已存在Value的末尾。
如果该Key不存在，APPEND命令将会创建一个新的Key/Value
*/
void appendCommand(redisClient *c) {
    size_t totlen;
    robj *o, *append;
	// 取出键相应的值对象
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* Create the key */
		// 键值对不存在，创建一个新的
        c->argv[2] = tryObjectEncoding(c->argv[2]);
        dbAdd(c->db,c->argv[1],c->argv[2]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    } else {
        /* Key exists, check type */
        if (checkType(c,o,REDIS_STRING))
            return;

        /* "append" is an argument, so always an sds */
		// 检查追加操作之后，字符串的长度是否符合 Redis 的限制
        append = c->argv[2];
        totlen = stringObjectLen(o)+sdslen(append->ptr);
        if (checkStringLength(c,totlen) != REDIS_OK)
            return;

        /* Append the value */
		// 执行追加操作
        o = dbUnshareStringValue(c->db,c->argv[1],o);
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));
        totlen = sdslen(o->ptr);
    }
	// 向数据库发送键被修改的信号
    signalModifiedKey(c->db,c->argv[1]);
	// 发送事件通知
    notifyKeyspaceEvent(REDIS_NOTIFY_STRING,"append",c->argv[1],c->db->id);
    server.dirty++;
	// 发送回复
    addReplyLongLong(c,totlen);
}
/*返回指定Key的字符值长度，如果Value不是string类型，
Redis将执行失败并给出相关的错误信息。
*/
void strlenCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_STRING)) return;
    addReplyLongLong(c,stringObjectLen(o));
}
