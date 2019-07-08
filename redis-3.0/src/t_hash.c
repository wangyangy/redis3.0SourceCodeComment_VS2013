#include "redis.h"
#include <math.h>

/*-----------------------------------------------------------------------------
 * Hash type API
 *----------------------------------------------------------------------------*/

/* 当每个元素的值的字节长都小于默认的64字节时，以及总长度小于默认的512个时，
哈希对象会采用ziplist来保存数据，在插入新的元素的时候都会检查是否会满足这两个条件，不满足则进行编码的转换 */
void hashTypeTryConversion(robj *o, robj **argv, int start, int end) {
    int i;
	// 如果对象不是 ziplist 编码，那么直接返回
    if (o->encoding != REDIS_ENCODING_ZIPLIST) return;
	// 检查所有输入对象，看它们的字符串值是否超过了指定长度
    for (i = start; i <= end; i++) {
        if (sdsEncodedObject(argv[i]) &&
            sdslen(argv[i]->ptr) > server.hash_max_ziplist_value)
        {
			// 将对象的编码转换成 REDIS_ENCODING_HT
            hashTypeConvert(o, REDIS_ENCODING_HT);
            break;
        }
    }
}

/* Encode given objects in-place when the hash uses a dict. */
void hashTypeTryObjectEncoding(robj *subject, robj **o1, robj **o2) {
    if (subject->encoding == REDIS_ENCODING_HT) {
        if (o1) *o1 = tryObjectEncoding(*o1);
        if (o2) *o2 = tryObjectEncoding(*o2);
    }
}

/* 检测域是否存在
 参数:field:域,vstr:值是字符串时将它保存到这个指针,vlen:保存字符串的长度,ll:值是整数时，将它保存到这个指针,查找失败时函数返回-1,查找成功时返回 0 。 */
int hashTypeGetFromZiplist(robj *o, robj *field,
                           unsigned char **vstr,
                           unsigned int *vlen,
                           PORT_LONGLONG *vll)
{
    unsigned char *zl, *fptr = NULL, *vptr = NULL;
    int ret;
	// 确保编码正确
    redisAssert(o->encoding == REDIS_ENCODING_ZIPLIST);
	// 取出未编码的域
    field = getDecodedObject(field);
	// 遍历 ziplist ，查找域的位置
    zl = o->ptr;
    fptr = ziplistIndex(zl, ZIPLIST_HEAD);
    if (fptr != NULL) {
		// 定位包含域的节点
        fptr = ziplistFind(fptr, field->ptr, (unsigned int)sdslen(field->ptr), 1); WIN_PORT_FIX /* cast (unsigned int) */
		// 域已经找到，取出和它相对应的值的位置
        if (fptr != NULL) {
            /* Grab pointer to the value (fptr points to the field) */
            vptr = ziplistNext(zl, fptr);
            redisAssert(vptr != NULL);
        }
    }

    decrRefCount(field);
	// 从 ziplist 节点中取出值
    if (vptr != NULL) {
        ret = ziplistGet(vptr, vstr, vlen, vll);
        redisAssert(ret);
        return 0;
    }

    return -1;  //没找到返回-1
}

/*
* 从 REDIS_ENCODING_HT 编码的 hash 中取出和 field 相对应的值。
* 成功找到值时返回 0 ，没找到返回 -1 。
*/
int hashTypeGetFromHashTable(robj *o, robj *field, robj **value) {
    dictEntry *de;
	// 确保编码正确
    redisAssert(o->encoding == REDIS_ENCODING_HT);
	// 在字典中查找域（键）
    de = dictFind(o->ptr, field);
    if (de == NULL) return -1;// 键不存在
    *value = dictGetVal(de);// 取出域（键）的值
    return 0;
}

/*
* 多态 GET 函数，从 hash 中取出域 field 的值，并返回一个值对象。
* 找到返回值对象，没找到返回 NULL 。
*/
robj *hashTypeGetObject(robj *o, robj *field) {
    robj *value = NULL;
	// 从 ziplist 中取出值
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        PORT_LONGLONG vll = LLONG_MAX;

        if (hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll) == 0) {
			// 创建值对象
            if (vstr) {
                value = createStringObject((char*)vstr, vlen);
            } else {
                value = createStringObjectFromLongLong(vll);
            }
        }
	// 从字典中取出值
    } else if (o->encoding == REDIS_ENCODING_HT) {
        robj *aux;

        if (hashTypeGetFromHashTable(o, field, &aux) == 0) {
            incrRefCount(aux);
            value = aux;
        }
    } else {
        redisPanic("Unknown hash encoding");
    }
    return value;// 返回值对象，或者 NULL
}

/* 检测field对象是否存在 */
int hashTypeExists(robj *o, robj *field) {
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        PORT_LONGLONG vll = LLONG_MAX;

        if (hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll) == 0) return 1;
    } else if (o->encoding == REDIS_ENCODING_HT) {
        robj *aux;

        if (hashTypeGetFromHashTable(o, field, &aux) == 0) return 1;
    } else {
        redisPanic("Unknown hash encoding");
    }
    return 0;
}

/*
* 将给定的 field-value 对添加到 hash 中，
* 如果 field 已经存在，那么删除旧的值，并关联新值。
* 这个函数负责对 field 和 value 参数进行引用计数自增。
* 返回 0 表示元素已经存在，这次函数调用执行的是更新操作。
* 返回 1 则表示函数执行的是新添加操作。
*/
int hashTypeSet(robj *o, robj *field, robj *value) {
    int update = 0;
	// 添加到 ziplist
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *zl, *fptr, *vptr;
		// 解码成字符串或者数字,ziplist需要编码与解码操作
        field = getDecodedObject(field);
        value = getDecodedObject(value);
		// 遍历整个 ziplist ，尝试查找并更新 field （如果它已经存在的话）
        zl = o->ptr;
        fptr = ziplistIndex(zl, ZIPLIST_HEAD);
        if (fptr != NULL) {
			// 定位到域 field
            fptr = ziplistFind(fptr, field->ptr, (unsigned int)sdslen(field->ptr), 1); WIN_PORT_FIX /* cast (unsigned int) */
            if (fptr != NULL) {
				// 定位到域的值
                vptr = ziplistNext(zl, fptr);
                redisAssert(vptr != NULL);
                update = 1;// 标识这次操作为更新操作

				// 删除旧的键值对
                zl = ziplistDelete(zl, &vptr);

				// 添加新的键值对
                zl = ziplistInsert(zl, vptr, value->ptr, (unsigned int)sdslen(value->ptr)); WIN_PORT_FIX /* cast (unsigned int) */
            }
        }
		// 如果这不是更新操作，那么这就是一个添加操作
        if (!update) {
			// 将新的 field-value 对推入到 ziplist 的末尾
            zl = ziplistPush(zl, field->ptr, (unsigned int)sdslen(field->ptr), ZIPLIST_TAIL); WIN_PORT_FIX /* cast (unsigned int) */
            zl = ziplistPush(zl, value->ptr, (unsigned int)sdslen(value->ptr), ZIPLIST_TAIL); WIN_PORT_FIX /* cast (unsigned int) */
        }
        o->ptr = zl;  // 更新对象指针
        decrRefCount(field);// 释放临时对象
        decrRefCount(value);

		// 检查在添加操作完成之后，是否需要将 ZIPLIST 编码转换成 HT 编码
        if (hashTypeLength(o) > server.hash_max_ziplist_entries)
            hashTypeConvert(o, REDIS_ENCODING_HT);
	// 添加到字典
    } else if (o->encoding == REDIS_ENCODING_HT) {
		// 添加或替换键值对到字典
		// 添加返回 1 ，替换返回 0
        if (dictReplace(o->ptr, field, value)) { /* Insert */
            incrRefCount(field);
        } else { /* Update */
            update = 1;
        }
        incrRefCount(value);
    } else {
        redisPanic("Unknown hash encoding");
    }
    return update;
}

/*
* 删除成功返回 1 ，因为域不存在而造成的删除失败返回 0 。
*/
int hashTypeDelete(robj *o, robj *field) {
    int deleted = 0;
	// 从 ziplist 中删除
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *zl, *fptr;
		// 定位到域
        field = getDecodedObject(field);

        zl = o->ptr;
        fptr = ziplistIndex(zl, ZIPLIST_HEAD);
        if (fptr != NULL) {
            fptr = ziplistFind(fptr, field->ptr, (unsigned int)sdslen(field->ptr), 1); WIN_PORT_FIX /* cast (unsigned int) */
            if (fptr != NULL) {
                zl = ziplistDelete(zl,&fptr);// 删除域和值
                zl = ziplistDelete(zl,&fptr);
                o->ptr = zl;
                deleted = 1;
            }
        }

        decrRefCount(field);
	// 从字典中删除
    } else if (o->encoding == REDIS_ENCODING_HT) {
        if (dictDelete((dict*)o->ptr, field) == REDIS_OK) {
            deleted = 1;

			// 删除成功时，看字典是否需要收缩
            if (htNeedsResize(o->ptr)) dictResize(o->ptr);
        }

    } else {
        redisPanic("Unknown hash encoding");
    }

    return deleted;
}

/* 返回hash表中的元素数量 */
PORT_ULONG hashTypeLength(robj *o) {
    PORT_ULONG length = PORT_ULONG_MAX;

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        length = ziplistLen(o->ptr) / 2;
    } else if (o->encoding == REDIS_ENCODING_HT) {
        length = (PORT_ULONG)dictSize((dict*)o->ptr);                           WIN_PORT_FIX /* cast (PORT_ULONG) */
    } else {
        redisPanic("Unknown hash encoding");
    }

    return length;
}
/*创建hash表迭代器*/
hashTypeIterator *hashTypeInitIterator(robj *subject) {
    hashTypeIterator *hi = zmalloc(sizeof(hashTypeIterator));
    hi->subject = subject;
    hi->encoding = subject->encoding;

    if (hi->encoding == REDIS_ENCODING_ZIPLIST) {
        hi->fptr = NULL;
        hi->vptr = NULL;
    } else if (hi->encoding == REDIS_ENCODING_HT) {
        hi->di = dictGetIterator(subject->ptr);
    } else {
        redisPanic("Unknown hash encoding");
    }

    return hi;
}
/*释放hash表迭代器*/
void hashTypeReleaseIterator(hashTypeIterator *hi) {
    if (hi->encoding == REDIS_ENCODING_HT) {
        dictReleaseIterator(hi->di);
    }

    zfree(hi);
}

/* 移动到hash表中的下一个元素的位置 */
int hashTypeNext(hashTypeIterator *hi) {
    if (hi->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *zl;
        unsigned char *fptr, *vptr;

        zl = hi->subject->ptr;
        fptr = hi->fptr;
        vptr = hi->vptr;

        if (fptr == NULL) {
            /* Initialize cursor */
            redisAssert(vptr == NULL);
            fptr = ziplistIndex(zl, 0);
        } else {
            /* Advance cursor */
            redisAssert(vptr != NULL);
            fptr = ziplistNext(zl, vptr);
        }
        if (fptr == NULL) return REDIS_ERR;

        /* Grab pointer to the value (fptr points to the field) */
        vptr = ziplistNext(zl, fptr);
        redisAssert(vptr != NULL);

        /* fptr, vptr now point to the first or next pair */
        hi->fptr = fptr;
        hi->vptr = vptr;
    } else if (hi->encoding == REDIS_ENCODING_HT) {
        if ((hi->de = dictNext(hi->di)) == NULL) return REDIS_ERR;
    } else {
        redisPanic("Unknown hash encoding");
    }
    return REDIS_OK;
}

/* 获取迭代器游标位置的元素的键值 */
void hashTypeCurrentFromZiplist(hashTypeIterator *hi, int what,
                                unsigned char **vstr,
                                unsigned int *vlen,
                                PORT_LONGLONG *vll)
{
    int ret;

    redisAssert(hi->encoding == REDIS_ENCODING_ZIPLIST);

    if (what & REDIS_HASH_KEY) {
        ret = ziplistGet(hi->fptr, vstr, vlen, vll);
        redisAssert(ret);
    } else {
        ret = ziplistGet(hi->vptr, vstr, vlen, vll);
        redisAssert(ret);
    }
}

/*根据迭代器获取当前位置的key-value */
void hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what, robj **dst) {
    redisAssert(hi->encoding == REDIS_ENCODING_HT);

    if (what & REDIS_HASH_KEY) {
        *dst = dictGetKey(hi->de);
    } else {
        *dst = dictGetVal(hi->de);
    }
}

/*获取key-value*/
robj *hashTypeCurrentObject(hashTypeIterator *hi, int what) {
    robj *dst;

    if (hi->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        PORT_LONGLONG vll = LLONG_MAX;

        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
        if (vstr) {
            dst = createStringObject((char*)vstr, vlen);
        } else {
            dst = createStringObjectFromLongLong(vll);
        }
    } else if (hi->encoding == REDIS_ENCODING_HT) {
        hashTypeCurrentFromHashTable(hi, what, &dst);
        incrRefCount(dst);
    } else {
        redisPanic("Unknown hash encoding");
    }
    return dst;
}
/*查找robj对象,不存在则创建,返回找到的或创建的对象*/
robj *hashTypeLookupWriteOrCreate(redisClient *c, robj *key) {
    robj *o = lookupKeyWrite(c->db,key);
    if (o == NULL) {
        o = createHashObject();
        dbAdd(c->db,key,o);
    } else {
        if (o->type != REDIS_HASH) {
            addReply(c,shared.wrongtypeerr);
            return NULL;
        }
    }
    return o;
}

/*将一个ziplist编码的哈希对象o转换成其他编码*/
void hashTypeConvertZiplist(robj *o, int enc) {
    redisAssert(o->encoding == REDIS_ENCODING_ZIPLIST);
	// 如果输入是 ZIPLIST ，那么不做动作
    if (enc == REDIS_ENCODING_ZIPLIST) {
        /* Nothing to do... */
	// 转换成HT编码
    } else if (enc == REDIS_ENCODING_HT) {
        hashTypeIterator *hi;
        dict *dict;
        int ret;
		// 创建哈希迭代器
        hi = hashTypeInitIterator(o);
        dict = dictCreate(&hashDictType, NULL);// 创建空白的新字典
		// 遍历整个 ziplist
        while (hashTypeNext(hi) != REDIS_ERR) {
            robj *field, *value;
			// 取出 ziplist 里的键
            field = hashTypeCurrentObject(hi, REDIS_HASH_KEY);
            field = tryObjectEncoding(field);
			// 取出 ziplist 里的值
            value = hashTypeCurrentObject(hi, REDIS_HASH_VALUE);
            value = tryObjectEncoding(value);
			//将键值对添加到字典中
            ret = dictAdd(dict, field, value);
            if (ret != DICT_OK) {
                redisLogHexDump(REDIS_WARNING,"ziplist with dup elements dump",
                    o->ptr,ziplistBlobLen(o->ptr));
                redisAssert(ret == DICT_OK);
            }
        }
		//释放ziplist的迭代器
        hashTypeReleaseIterator(hi);
        zfree(o->ptr);// 释放对象原来的 ziplist
		// 更新哈希的编码和值对象
        o->encoding = REDIS_ENCODING_HT;
        o->ptr = dict;

    } else {
        redisPanic("Unknown hash encoding");
    }
}
/*
* 对哈希对象o的编码方式进行转换
* 目前只支持将ZIPLIST编码转换成HT编码
*/
void hashTypeConvert(robj *o, int enc) {
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        hashTypeConvertZiplist(o, enc);
    } else if (o->encoding == REDIS_ENCODING_HT) {
        redisPanic("Not implemented");
    } else {
        redisPanic("Unknown hash encoding");
    }
}

/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/
/*hset命令实现*/
void hsetCommand(redisClient *c) {
    int update;
    robj *o;

    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTypeTryConversion(o,c->argv,2,3);
    hashTypeTryObjectEncoding(o,&c->argv[2], &c->argv[3]);
    update = hashTypeSet(o,c->argv[2],c->argv[3]);
    addReply(c, update ? shared.czero : shared.cone);
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(REDIS_NOTIFY_HASH,"hset",c->argv[1],c->db->id);
    server.dirty++;
}
/*hsetnx命令实现*/
void hsetnxCommand(redisClient *c) {
    robj *o;
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTypeTryConversion(o,c->argv,2,3);

    if (hashTypeExists(o, c->argv[2])) {
        addReply(c, shared.czero);
    } else {
        hashTypeTryObjectEncoding(o,&c->argv[2], &c->argv[3]);
        hashTypeSet(o,c->argv[2],c->argv[3]);
        addReply(c, shared.cone);
        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_HASH,"hset",c->argv[1],c->db->id);
        server.dirty++;
    }
}
/*hmset命令实现*/
void hmsetCommand(redisClient *c) {
    int i;
    robj *o;

    if ((c->argc % 2) == 1) {
        addReplyError(c,"wrong number of arguments for HMSET");
        return;
    }

    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTypeTryConversion(o,c->argv,2,c->argc-1);
    for (i = 2; i < c->argc; i += 2) {
        hashTypeTryObjectEncoding(o,&c->argv[i], &c->argv[i+1]);
        hashTypeSet(o,c->argv[i],c->argv[i+1]);
    }
    addReply(c, shared.ok);
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(REDIS_NOTIFY_HASH,"hset",c->argv[1],c->db->id);
    server.dirty++;
}
/*hincrby命令实现*/
void hincrbyCommand(redisClient *c) {
    PORT_LONGLONG value, incr, oldvalue;
    robj *o, *current, *new;

    if (getLongLongFromObjectOrReply(c,c->argv[3],&incr,NULL) != REDIS_OK) return;
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    if ((current = hashTypeGetObject(o,c->argv[2])) != NULL) {
        if (getLongLongFromObjectOrReply(c,current,&value,
            "hash value is not an integer") != REDIS_OK) {
            decrRefCount(current);
            return;
        }
        decrRefCount(current);
    } else {
        value = 0;
    }

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    value += incr;
    new = createStringObjectFromLongLong(value);
    hashTypeTryObjectEncoding(o,&c->argv[2],NULL);
    hashTypeSet(o,c->argv[2],new);
    decrRefCount(new);
    addReplyLongLong(c,value);
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(REDIS_NOTIFY_HASH,"hincrby",c->argv[1],c->db->id);
    server.dirty++;
}

/*HINCRBYFLOAT命令的实现*/
void hincrbyfloatCommand(redisClient *c) {
    PORT_LONGDOUBLE value, incr;
    robj *o, *current, *new, *aux;
	// 得到一个long double类型的增量increment
    if (getLongDoubleFromObjectOrReply(c,c->argv[3],&incr,NULL) != REDIS_OK) return;
	// 以写方式取出哈希对象，失败则直接返回
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
	// 返回field在哈希对象o中的值对象
    if ((current = hashTypeGetObject(o,c->argv[2])) != NULL) {
		//从值对象中得到一个long double类型的value，如果不是浮点数的值，则发送"hash value is not a valid float"信息给client
        if (getLongDoubleFromObjectOrReply(c,current,&value,
            "hash value is not a valid float") != REDIS_OK) {
			decrRefCount(current); //取值成功，释放临时的value对象空间，直接返回
            return;
        }
        decrRefCount(current);//取值失败也要释放空间
    } else {
		value = 0; //如果没有值，则设置为默认的0
    }

    value += incr;//备份原先的值
    new = createStringObjectFromLongDouble(value,1);// 将value转换为字符串类型的对象
	//将键和值对象的编码进行优化，以节省空间，是以embstr或raw或整型存储
	hashTypeTryObjectEncoding(o,&c->argv[2],NULL);
    hashTypeSet(o,c->argv[2],new);// 设置原来的key为新的值对象
	addReplyBulk(c, new); // 讲新的值对象发送给client
	// 修改数据库的键则发送信号，发送"hincrbyfloat"事件通知，更新脏键
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(REDIS_NOTIFY_HASH,"hincrbyfloat",c->argv[1],c->db->id);
    server.dirty++;

	// 用HSET命令代替HINCRBYFLOAT，以防不同的浮点精度造成的误差
	// 创建HSET字符串对象
    aux = createStringObject("HSET",4);
	// 修改HINCRBYFLOAT命令为HSET对象
    rewriteClientCommandArgument(c,0,aux);
	decrRefCount(aux); // 释放空间
	rewriteClientCommandArgument(c, 3, new); // 修改increment为新的值对象new
    decrRefCount(new);// 释放空间
}

static void addHashFieldToReply(redisClient *c, robj *o, robj *field) {
    int ret;

    if (o == NULL) {
        addReply(c, shared.nullbulk);
        return;
    }

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        PORT_LONGLONG vll = LLONG_MAX;

        ret = hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll);
        if (ret < 0) {
            addReply(c, shared.nullbulk);
        } else {
            if (vstr) {
                addReplyBulkCBuffer(c, vstr, vlen);
            } else {
                addReplyBulkLongLong(c, vll);
            }
        }

    } else if (o->encoding == REDIS_ENCODING_HT) {
        robj *value;

        ret = hashTypeGetFromHashTable(o, field, &value);
        if (ret < 0) {
            addReply(c, shared.nullbulk);
        } else {
            addReplyBulk(c, value);
        }

    } else {
        redisPanic("Unknown hash encoding");
    }
}
/* hget命令实现 */
void hgetCommand(redisClient *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;

    addHashFieldToReply(c, o, c->argv[2]);
}

/* hmget命令实现 */
void hmgetCommand(redisClient *c) {
    robj *o;
    int i;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * hashes, where HMGET should respond with a series of null bulks. */
    o = lookupKeyRead(c->db, c->argv[1]);
    if (o != NULL && o->type != REDIS_HASH) {
        addReply(c, shared.wrongtypeerr);
        return;
    }

    addReplyMultiBulkLen(c, c->argc-2);
    for (i = 2; i < c->argc; i++) {
        addHashFieldToReply(c, o, c->argv[i]);
    }
}

/*HDEL命令的底层实现*/
void hdelCommand(redisClient *c) {
    robj *o;
    int j, deleted = 0, keyremoved = 0;
	// 以写操作取出哈希对象，若失败，或取出的对象不是哈希类型的对象，则发送0后直接返回
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;
	// 遍历所有的字段field
    for (j = 2; j < c->argc; j++) {
		// 从哈希对象中删除当前字段
        if (hashTypeDelete(o,c->argv[j])) {
			deleted++; //更新删除的个数
			// 如果哈希对象为空，则删除该对象
            if (hashTypeLength(o) == 0) {
                dbDelete(c->db,c->argv[1]);
                keyremoved = 1;//设置删除标志
                break;
            }
        }
    }
	//删除了字段
    if (deleted) {
		signalModifiedKey(c->db, c->argv[1]); // 发送信号表示键被改变
		// 发送"hdel"事件通知
		notifyKeyspaceEvent(REDIS_NOTIFY_HASH,"hdel",c->argv[1],c->db->id);
		// 如果哈希对象被删除
        if (keyremoved)
			// 发送"hdel"事件通知
            notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",c->argv[1],
                                c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);  //发送删除的个数给client
}

void hlenCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;

    addReplyLongLong(c,hashTypeLength(o));
}

static void addHashIteratorCursorToReply(redisClient *c, hashTypeIterator *hi, int what) {
    if (hi->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        PORT_LONGLONG vll = LLONG_MAX;

        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
        if (vstr) {
            addReplyBulkCBuffer(c, vstr, vlen);
        } else {
            addReplyBulkLongLong(c, vll);
        }

    } else if (hi->encoding == REDIS_ENCODING_HT) {
        robj *value;

        hashTypeCurrentFromHashTable(hi, what, &value);
        addReplyBulk(c, value);

    } else {
        redisPanic("Unknown hash encoding");
    }
}
/*HKEYS,HVALS,HGETALL等命令的底层实现,全量遍历KEYS命令的底层实现*/
void genericHgetallCommand(redisClient *c, int flags) {
    robj *o;
    hashTypeIterator *hi;
    int multiplier = 0;
    int length, count = 0;
	// 取出哈希对象
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
        || checkType(c,o,REDIS_HASH)) return;
	// 计算要取出的元素数量
    if (flags & REDIS_HASH_KEY) multiplier++;
    if (flags & REDIS_HASH_VALUE) multiplier++;

    length = (int)(hashTypeLength(o) * multiplier);                             WIN_PORT_FIX /* cast (int) */
    addReplyMultiBulkLen(c, length);
	// 迭代节点，并取出元素
    hi = hashTypeInitIterator(o);
    while (hashTypeNext(hi) != REDIS_ERR) {
        if (flags & REDIS_HASH_KEY) {// 取出键
            addHashIteratorCursorToReply(c, hi, REDIS_HASH_KEY);
            count++;
        }
        if (flags & REDIS_HASH_VALUE) {// 取出值
            addHashIteratorCursorToReply(c, hi, REDIS_HASH_VALUE);
            count++;
        }
    }
	// 释放迭代器
    hashTypeReleaseIterator(hi);
    redisAssert(count == length);
}

void hkeysCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_KEY);
}

void hvalsCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_VALUE);
}

void hgetallCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_KEY|REDIS_HASH_VALUE);
}

void hexistsCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;

    addReply(c, hashTypeExists(o,c->argv[2]) ? shared.cone : shared.czero);
}
/*hscan命令的实现*/
void hscanCommand(redisClient *c) {
    robj *o;
    PORT_ULONG cursor;
	//// 获取scan命令的游标cursor
    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == REDIS_ERR) return;
	//读取对象，检查类型
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;
    scanGenericCommand(c,o,cursor);
}
