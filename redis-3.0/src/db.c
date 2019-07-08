#include "redis.h"
#ifdef _WIN32
#include "Win32_Interop/Win32_QFork.h"
#endif
#include "cluster.h"

#include <signal.h>
#include <ctype.h>

void slotToKeyAdd(robj *key);
void slotToKeyDel(robj *key);
void slotToKeyFlush(void);

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

/*从键值对字典中找到key对应的value*/
robj *lookupKey(redisDb *db, robj *key) {
	//根据key查找键值对
    dictEntry *de = dictFind(db->dict,key->ptr);
	//如果找到
    if (de) {
		//获取value
        robj *val = dictGetVal(de);

        //更新键的使用时间
        if (server.rdb_child_pid == -1 && server.aof_child_pid == -1)
            val->lru = LRU_CLOCK();
        return val; //返回值对象
    } else {
        return NULL;
    }
}

/*以读操作取出key的值对象*/
robj *lookupKeyRead(redisDb *db, robj *key) {
    robj *val;
	//检测键是否过期
    expireIfNeeded(db,key);
	//获取value
    val = lookupKey(db,key);
	//更新是否命中的信息
    if (val == NULL)
        server.stat_keyspace_misses++;
    else
        server.stat_keyspace_hits++;
    return val;
}

/*以写操作取出key的值对象,不更新是否命中的信息,返回值是value*/
robj *lookupKeyWrite(redisDb *db, robj *key) {
    expireIfNeeded(db,key);
    return lookupKey(db,key);
}

robj *lookupKeyReadOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

robj *lookupKeyWriteOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

/* 将key添加到数据库中,增加引用计数 
 Add the key to the DB. It's up to the caller to increment the reference
 * counter of the value if needed.
 *
 * The program is aborted if the key already exists. */
void dbAdd(redisDb *db, robj *key, robj *val) {
    sds copy = sdsdup(key->ptr);
    int retval = dictAdd(db->dict, copy, val);

    redisAssertWithInfo(NULL,key,retval == REDIS_OK);
    if (val->type == REDIS_LIST) signalListAsReady(db, key);
    if (server.cluster_enabled) slotToKeyAdd(key);
 }

/*重写键值对 */
void dbOverwrite(redisDb *db, robj *key, robj *val) {
    dictEntry *de = dictFind(db->dict,key->ptr);

    redisAssertWithInfo(NULL,key,de != NULL);
    dictReplace(db->dict, key->ptr, val);
}

/* 设置key */
void setKey(redisDb *db, robj *key, robj *val) {
	//如果没有value
    if (lookupKeyWrite(db,key) == NULL) {
		//添加key-value
        dbAdd(db,key,val);
    } else {
		//否则覆盖key-value
        dbOverwrite(db,key,val);
    }
	//增加引用计数
    incrRefCount(val);
	//删除过期时间
    removeExpire(db,key);
	//通知修改
    signalModifiedKey(db,key);
}

/*判断key是否存在*/
int dbExists(redisDb *db, robj *key) {
    return dictFind(db->dict,key->ptr) != NULL;
}

/* 以redisObject的形式随机返回一个没有过期的key对象
 如果没有key返回null
 */
robj *dbRandomKey(redisDb *db) {
    dictEntry *de;

    while(1) {
        sds key;
        robj *keyobj;

        de = dictGetRandomKey(db->dict);
        if (de == NULL) return NULL;

        key = dictGetKey(de);
        keyobj = createStringObject(key,sdslen(key));
        if (dictFind(db->expires,key)) {
            if (expireIfNeeded(db,keyobj)) {
                decrRefCount(keyobj);
                continue; /* search for another key. This expired. */
            }
        }
        return keyobj;
    }
}

/* 删除一个键值对,成功返回1,失败返回0 */
int dbDelete(redisDb *db, robj *key) {
    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. */
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
    if (dictDelete(db->dict,key->ptr) == DICT_OK) {
        if (server.cluster_enabled) slotToKeyDel(key);
        return 1;
    } else {
        return 0;
    }
}

/* Prepare the string object stored at 'key' to be modified destructively
 * to implement commands like SETBIT or APPEND.
 *
 * An object is usually ready to be modified unless one of the two conditions
 * are true:
 *
 * 1) The object 'o' is shared (refcount > 1), we don't want to affect
 *    other users.
 * 2) The object encoding is not "RAW".
 *
 * If the object is found in one of the above conditions (or both) by the
 * function, an unshared / not-encoded copy of the string object is stored
 * at 'key' in the specified 'db'. Otherwise the object 'o' itself is
 * returned.
 *
 * USAGE:
 *
 * The object 'o' is what the caller already obtained by looking up 'key'
 * in 'db', the usage pattern looks like this:
 *
 * o = lookupKeyWrite(db,key);
 * if (checkType(c,o,REDIS_STRING)) return;
 * o = dbUnshareStringValue(db,key,o);
 *
 * At this point the caller is ready to modify the object, for example
 * using an sdscat() call to append some data, or anything else.
 */
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o) {
    redisAssert(o->type == REDIS_STRING);
    if (o->refcount != 1 || o->encoding != REDIS_ENCODING_RAW) {
        robj *decoded = getDecodedObject(o);
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
        decrRefCount(decoded);
        dbOverwrite(db,key,o);
    }
    return o;
}

/*清空数据库*/
PORT_LONGLONG emptyDb(void(callback)(void*)) {
    int j;
    PORT_LONGLONG removed = 0;

    for (j = 0; j < server.dbnum; j++) {
        removed += dictSize(server.db[j].dict);
        dictEmpty(server.db[j].dict,callback);
        dictEmpty(server.db[j].expires,callback);
    }
    if (server.cluster_enabled) slotToKeyFlush();
    return removed;
}

/*根据id选择数据库*/
int selectDb(redisClient *c, int id) {
	//id非法返回错误
    if (id < 0 || id >= server.dbnum)
        return REDIS_ERR;
	//设置当前数据库
    c->db = &server.db[id];
    return REDIS_OK;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 *----------------------------------------------------------------------------*/

void signalModifiedKey(redisDb *db, robj *key) {
    touchWatchedKey(db,key);
}

void signalFlushedDb(int dbid) {
    touchWatchedKeysOnFlush(dbid);
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 *----------------------------------------------------------------------------*/

void flushdbCommand(redisClient *c) {
    server.dirty += dictSize(c->db->dict);
    signalFlushedDb(c->db->id);
    dictEmpty(c->db->dict,NULL);
    dictEmpty(c->db->expires,NULL);
    if (server.cluster_enabled) slotToKeyFlush();
    addReply(c,shared.ok);
}

void flushallCommand(redisClient *c) {
    signalFlushedDb(-1);
    server.dirty += emptyDb(NULL);
    addReply(c,shared.ok);
    if (server.rdb_child_pid != -1) {
#ifdef _WIN32
        AbortForkOperation();
#else
        kill(server.rdb_child_pid,SIGUSR1);
#endif
        rdbRemoveTempFile(server.rdb_child_pid);
    }
    if (server.saveparamslen > 0) {
        /* Normally rdbSave() will reset dirty, but we don't want this here
         * as otherwise FLUSHALL will not be replicated nor put into the AOF. */
        PORT_LONGLONG saved_dirty = server.dirty;                               /* UPSTREAM_FIX: server.dirty is a PORT_LONGLONG not an int */
        rdbSave(server.rdb_filename);
        server.dirty = saved_dirty;
    }
    server.dirty++;
}

void delCommand(redisClient *c) {
    int deleted = 0, j;

    for (j = 1; j < c->argc; j++) {
        expireIfNeeded(c->db,c->argv[j]);
        if (dbDelete(c->db,c->argv[j])) {
            signalModifiedKey(c->db,c->argv[j]);
            notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,
                "del",c->argv[j],c->db->id);
            server.dirty++;
            deleted++;
        }
    }
    addReplyLongLong(c,deleted);
}

/* EXISTS key1 key2 ... key_N.
 * Return value is the number of keys existing. */
void existsCommand(redisClient *c) {
    PORT_LONGLONG count = 0;
    int j;

    for (j = 1; j < c->argc; j++) {
        expireIfNeeded(c->db,c->argv[j]);
        if (dbExists(c->db,c->argv[j])) count++;
    }
    addReplyLongLong(c,count);
}

void selectCommand(redisClient *c) {
    PORT_LONG id;

    if (getLongFromObjectOrReply(c, c->argv[1], &id,
        "invalid DB index") != REDIS_OK)
        return;

    if (server.cluster_enabled && id != 0) {
        addReplyError(c,"SELECT is not allowed in cluster mode");
        return;
    }
    if (selectDb(c,(int)id) == REDIS_ERR) {                                     WIN_PORT_FIX /* cast (int) */

        addReplyError(c,"invalid DB index");
    } else {
        addReply(c,shared.ok);
    }
}

void randomkeyCommand(redisClient *c) {
    robj *key;

    if ((key = dbRandomKey(c->db)) == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }

    addReplyBulk(c,key);
    decrRefCount(key);
}

void keysCommand(redisClient *c) {
    dictIterator *di;
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;
    int plen = (int)sdslen(pattern), allkeys;
    PORT_ULONG numkeys = 0;
    void *replylen = addDeferredMultiBulkLength(c);

    di = dictGetSafeIterator(c->db->dict);
    allkeys = (pattern[0] == '*' && pattern[1] == '\0');
    while((de = dictNext(di)) != NULL) {
        sds key = dictGetKey(de);
        robj *keyobj;

        if (allkeys || stringmatchlen(pattern,plen,key,(int)sdslen(key),0)) {
            keyobj = createStringObject(key,sdslen(key));
            if (expireIfNeeded(c->db,keyobj) == 0) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);
        }
    }
    dictReleaseIterator(di);
    setDeferredMultiBulkLength(c,replylen,numkeys);
}

/* This callback is used by scanGenericCommand in order to collect elements
 * returned by the dictionary iterator into a list. */
void scanCallback(void *privdata, const dictEntry *de) {
    void **pd = (void**) privdata;
    list *keys = pd[0];
    robj *o = pd[1];
    robj *key, *val = NULL;

    if (o == NULL) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey, sdslen(sdskey));
    } else if (o->type == REDIS_SET) {
        key = dictGetKey(de);
        incrRefCount(key);
    } else if (o->type == REDIS_HASH) {
        key = dictGetKey(de);
        incrRefCount(key);
        val = dictGetVal(de);
        incrRefCount(val);
    } else if (o->type == REDIS_ZSET) {
        key = dictGetKey(de);
        incrRefCount(key);
        val = createStringObjectFromLongDouble(*(double*)dictGetVal(de),0);
    } else {
        redisPanic("Type not handled in SCAN callback.");
    }

    listAddNodeTail(keys, key);
    if (val) listAddNodeTail(keys, val);
}

/* Try to parse a SCAN cursor stored at object 'o':
 * if the cursor is valid, store it as unsigned integer into *cursor and
 * returns REDIS_OK. Otherwise return REDIS_ERR and send an error to the
 * client. */
int parseScanCursorOrReply(redisClient *c, robj *o, PORT_ULONG *cursor) {
    char *eptr;

    /* Use strtoul() because we need an *unsigned* long, so
     * getLongLongFromObject() does not cover the whole cursor space. */
    errno = 0;
    *cursor = strtoul(o->ptr, &eptr, 10);
    if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE)
    {
        addReplyError(c, "invalid cursor");
        return REDIS_ERR;
    }
    return REDIS_OK;
}

/* This command implements SCAN, HSCAN and SSCAN commands.
 * If object 'o' is passed, then it must be a Hash or Set object, otherwise
 * if 'o' is NULL the command will operate on the dictionary associated with
 * the current database.
 *
 * When 'o' is not NULL the function assumes that the first argument in
 * the client arguments vector is a key so it skips it before iterating
 * in order to parse options.
 *
 * In the case of a Hash object the function returns both the field and value
 * of every element on the Hash. */
/*scan类命令的底层实现,o对象必须是hash对象或集合对象,否则命令将操作当前数据库库,如果o不是kong
那么说明他是一个哈希或集合对象，函数将跳过这些键对象，对参数进行分析如果是哈希对象，返回的是键值对*/
void scanGenericCommand(redisClient *c, robj *o, PORT_ULONG cursor) {
    int i, j;
    list *keys = listCreate();  //创建一个列表
    listNode *node, *nextnode;
    PORT_LONG count = 10;
    sds pat;
    int patlen, use_pattern = 0;
    dict *ht;

    
	// 输入类型的检查，要么迭代键名，要么当前集合对象，要么迭代哈希对象，要么迭代有序集合对象
    redisAssert(o == NULL || o->type == REDIS_SET || o->type == REDIS_HASH ||
                o->type == REDIS_ZSET);

    /* 计算第一个参数的下标,如果是键名,要跳过该键 */
    i = (o == NULL) ? 2 : 3; /* Skip the key argument if needed. */

    /* 解析选项 */
    while (i < c->argc) {
        j = c->argc - i;
		// 设定COUNT参数，COUNT 选项的作用就是让用户告知迭代命令， 在每次迭代中应该返回多少元素。
        if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
            //保存个数到count
			if (getLongFromObjectOrReply(c, c->argv[i+1], &count, NULL)
                != REDIS_OK)
            {
                goto cleanup;
            }
			//如果个数小于1语法错误
            if (count < 1) {
                addReply(c,shared.syntaxerr);
                goto cleanup;
            }

            i += 2;
		// 设定MATCH参数，让命令只返回和给定模式相匹配的元素。
        } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {
            pat = c->argv[i+1]->ptr;
            patlen = (int)sdslen(pat);    //pattern字符串长度                                       WIN_PORT_FIX /* cast (int) */

			// 如果pattern是"*"，就不用匹配，全部返回，设置为0
            use_pattern = !(pat[0] == '*' && patlen == 1);

            i += 2;
        } else {
            addReply(c,shared.syntaxerr);
            goto cleanup;
        }
    }

	// 2.如果对象是ziplist、intset或其他而不是哈希表，那么这些类型只是包含少量的元素
	// 我们一次将其所有的元素全部返回给调用者，并设置游标cursor为0，标示迭代完成
    ht = NULL;
	//迭代数据库
    if (o == NULL) {
        ht = c->db->dict;
	//迭代HT编码的集合对象
    } else if (o->type == REDIS_SET && o->encoding == REDIS_ENCODING_HT) {
        ht = o->ptr;
	//迭代HT编码的hash对象
    } else if (o->type == REDIS_HASH && o->encoding == REDIS_ENCODING_HT) {
        ht = o->ptr;
        count *= 2; /* We return key / value for this type. */
	//迭代skiplist编码的语序集合对象
    } else if (o->type == REDIS_ZSET && o->encoding == REDIS_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        ht = zs->dict;
        count *= 2; /* We return key / value for this type. */
    }

    if (ht) {
        void *privdata[2];
        /* We set the max number of iterations to ten times the specified
         * COUNT, so if the hash table is in a pathological state (very
         * sparsely populated) we avoid to block too much time at the cost
         * of returning no or very few elements. */
		// 设置最大的迭代长度为10*count次
        PORT_LONG maxiterations = count * 10;

        /* We pass two pointers to the callback: the list to which it will
         * add new elements, and the object containing the dictionary so that
         * it is possible to fetch more data in a type-dependent way. */
		// 回调函数scanCallback的参数privdata是一个数组，保存的是被迭代对象的键和值
		// 回调函数scanCallback的另一个参数，是一个字典对象
		// 回调函数scanCallback的作用，从字典对象中将键值对提取出来，不用管字典对象是什么数据类型
        privdata[0] = keys;
        privdata[1] = o;
		// 循环扫描ht，从游标cursor开始，调用指定的scanCallback函数，提出ht中的数据到刚开始创建的列表keys中
        do {
            cursor = dictScan(ht, cursor, scanCallback, privdata);
        } while (cursor &&
              maxiterations-- &&
              listLength(keys) < (PORT_ULONG)count);
	// 如果是集合对象但编码不是HT是整数集合
    } else if (o->type == REDIS_SET) {
        int pos = 0;
        int64_t ll;
		// 将整数值取出来，构建成字符串对象加入到keys列表中，游标设置为0，表示迭代完成
        while(intsetGet(o->ptr,pos++,&ll))
            listAddNodeTail(keys,createStringObjectFromLongLong(ll));
        cursor = 0;
	// 如果是哈希对象，或有序集合对象，但是编码都不是HT，是ziplist
    } else if (o->type == REDIS_HASH || o->type == REDIS_ZSET) {
        unsigned char *p = ziplistIndex(o->ptr,0);
        unsigned char *vstr;
        unsigned int vlen;
        PORT_LONGLONG vll;
		// 将值取出来，根据不同类型的值，构建成相同的字符串对象，加入到keys列表中
        while(p) {
            ziplistGet(p,&vstr,&vlen,&vll);
            listAddNodeTail(keys,
                (vstr != NULL) ? createStringObject((char*)vstr,vlen) :
                                 createStringObjectFromLongLong(vll));
            p = ziplistNext(o->ptr,p);
        }
        cursor = 0;
    } else {
        redisPanic("Not handled encoding in SCAN.");
    }

	// 3. 如果设置MATCH参数，要进行过滤
    node = listFirst(keys);//链表首地址
    while (node) {
        robj *kobj = listNodeValue(node);  //key
        nextnode = listNextNode(node);     //下一个节点
        int filter = 0;     //默认不过滤

		//pattern不是"*"因此要过滤
        if (!filter && use_pattern) {
			// 如果kobj是字符串对象
            if (sdsEncodedObject(kobj)) {
				// kobj的值不匹配pattern，设置过滤标志
                if (!stringmatchlen(pat, patlen, kobj->ptr, (int)sdslen(kobj->ptr), 0)) WIN_PORT_FIX /* cast (int) */
                    filter = 1;
			// 如果kobj是整数对象
            } else {
                char buf[REDIS_LONGSTR_SIZE];
                int len;

                redisAssert(kobj->encoding == REDIS_ENCODING_INT);
				// 将整数转换为字符串类型，保存到buf中
                len = ll2string(buf,sizeof(buf),(PORT_LONG)kobj->ptr);          WIN_PORT_FIX /* cast (PORT_LONG) */
				//buf的值不匹配pattern，设置过滤标志
                if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
            }
        }

		// 迭代目标是数据库，如果kobj是过期键，则过滤
        if (!filter && o == NULL && expireIfNeeded(c->db, kobj)) filter = 1;

		// 如果该键满足了上述的过滤条件，那么将其从keys列表删除并释放
        if (filter) {
            decrRefCount(kobj);
            listDelNode(keys, node);
        }

        
		// 如果当前迭代目标是有序集合或哈希对象，因此keys列表中保存的是键值对，如果key键对象被过滤，值对象也应当被过滤
        if (o && (o->type == REDIS_ZSET || o->type == REDIS_HASH)) {
            node = nextnode;
            nextnode = listNextNode(node);
			// 如果该键满足了上述的过滤条件，那么将其从keys列表删除并释放
            if (filter) {
                kobj = listNodeValue(node);  //取出value
                decrRefCount(kobj);
                listDelNode(keys, node);  //删除
            }
        }
        node = nextnode;
    }

	// 4. 回复信息给client
    addReplyMultiBulkLen(c, 2);    //2部分，一个是游标，一个是列表
    addReplyBulkLongLong(c,cursor);   //回复游标

    addReplyMultiBulkLen(c, listLength(keys));
	//循环回复列表中的元素，并释放
    while ((node = listFirst(keys)) != NULL) {
        robj *kobj = listNodeValue(node);
        addReplyBulk(c, kobj);
        decrRefCount(kobj);
        listDelNode(keys, node);
    }

cleanup:
    listSetFreeMethod(keys,decrRefCountVoid);  //设置特定的释放列表的方式decrRefCountVoid
	listRelease(keys);               // 释放
}

/* The SCAN command completely relies on scanGenericCommand. */
void scanCommand(redisClient *c) {
    PORT_ULONG cursor;
    if (parseScanCursorOrReply(c,c->argv[1],&cursor) == REDIS_ERR) return;
    scanGenericCommand(c,NULL,cursor);
}

void dbsizeCommand(redisClient *c) {
    addReplyLongLong(c,dictSize(c->db->dict));
}

void lastsaveCommand(redisClient *c) {
    addReplyLongLong(c,server.lastsave);
}
//命令类型
void typeCommand(redisClient *c) {
    robj *o;
    char *type;

    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        type = "none";
    } else {
        switch(o->type) {
        case REDIS_STRING: type = "string"; break;
        case REDIS_LIST: type = "list"; break;
        case REDIS_SET: type = "set"; break;
        case REDIS_ZSET: type = "zset"; break;
        case REDIS_HASH: type = "hash"; break;
        default: type = "unknown"; break;
        }
    }
    addReplyStatus(c,type);
}

//关闭命令
void shutdownCommand(redisClient *c) {
    int flags = 0;

    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);
        return;
    } else if (c->argc == 2) {
        if (!strcasecmp(c->argv[1]->ptr,"nosave")) {
            flags |= REDIS_SHUTDOWN_NOSAVE;
        } else if (!strcasecmp(c->argv[1]->ptr,"save")) {
            flags |= REDIS_SHUTDOWN_SAVE;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }
    /* When SHUTDOWN is called while the server is loading a dataset in
     * memory we need to make sure no attempt is performed to save
     * the dataset on shutdown (otherwise it could overwrite the current DB
     * with half-read data).
     *
     * Also when in Sentinel mode clear the SAVE flag and force NOSAVE. */
    if (server.loading || server.sentinel_mode)
        flags = (flags & ~REDIS_SHUTDOWN_SAVE) | REDIS_SHUTDOWN_NOSAVE;
    if (prepareForShutdown(flags) == REDIS_OK) exit(0);
    addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}

/*rename,reanemx命令的底层实现*/
void renameGenericCommand(redisClient *c, int nx) {
    robj *o;
    PORT_LONGLONG expire;

	// key和newkey相同的话，设置samekey标志
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) {
        addReply(c,shared.sameobjecterr);
        return;
    }
	//以写操作读取key的value
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;
	// 增加值对象的引用计数，保护起来，用于关联newkey，以防删除了key顺带将值对象也删除
    incrRefCount(o);
	//获取过期时间
    expire = getExpire(c->db,c->argv[1]);
	// 判断newkey的值对象是否存在
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
		// 设置了nx标志，则不符合已存在的条件，发送0
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        /*删除旧的键,在创建新的之前*/
        dbDelete(c->db,c->argv[2]);
    }
	// 将newkey和key的值对象关联
    dbAdd(c->db,c->argv[2],o);
	// 如果newkey设置过过期时间，则为newkey设置过期时间
    if (expire != -1) setExpire(c->db,c->argv[2],expire);
    //删除key
	dbDelete(c->db,c->argv[1]);
	//发送两个键被修改的信号
    signalModifiedKey(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[2]);
	//发送事件通知
    notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"rename_from",
        c->argv[1],c->db->id);
    notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"rename_to",
        c->argv[2],c->db->id);
    server.dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}

void renameCommand(redisClient *c) {
    renameGenericCommand(c,0);
}

void renamenxCommand(redisClient *c) {
    renameGenericCommand(c,1);
}

/*move命令实现*/
void moveCommand(redisClient *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;
    PORT_LONGLONG dbid, expire;
	// 服务器处于集群模式，不支持多数据库
    if (server.cluster_enabled) {
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

	// 获得源数据库和源数据库的id
    src = c->db;
    srcid = c->db->id;
	// 将参数db的值保存到dbid，并且切换到该数据库中
    if (getLongLongFromObject(c->argv[2],&dbid) == REDIS_ERR ||
        dbid < INT_MIN || dbid > INT_MAX ||
        selectDb(c,(int)dbid) == REDIS_ERR)                                     WIN_PORT_FIX /* cast (int) */
    {
        addReply(c,shared.outofrangeerr);
        return;
    }
	// 目标数据库
    dst = c->db;
	// 切换回源数据库
    selectDb(c,srcid); /* Back to the source DB */

    
	// 如果前后切换的数据库相同，则返回有关错误
    if (src == dst) {
        addReply(c,shared.sameobjecterr);
        return;
    }

	// 以写操作取出源数据库的对象
    o = lookupKeyWrite(c->db,c->argv[1]);
	// 不存在发送0
    if (!o) {
        addReply(c,shared.czero);
        return;
    }
	// 备份key的过期时间
    expire = getExpire(c->db,c->argv[1]);

	// 判断当前key是否存在于目标数据库，存在直接返回，发送0
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
        addReply(c,shared.czero);
        return;
    }
	// 将key-value对象添加到目标数据库中
    dbAdd(dst,c->argv[1],o);
	//设置过期时间
    if (expire != -1) setExpire(dst,c->argv[1],expire);
    incrRefCount(o);//增加引用计数

	// 从源数据库中将key和关联的值对象删除
    dbDelete(src,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);  //回复1
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/
//删除过期时间
int removeExpire(redisDb *db, robj *key) {
    /* An expire may only be removed if there is a corresponding entry in the
     * main dict. Otherwise, the key will never be freed. */
    redisAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}
//设置过期时间
void setExpire(redisDb *db, robj *key, PORT_LONGLONG when) {
    dictEntry *kde, *de;

    /* Reuse the sds from the main dict in the expire dict */
    kde = dictFind(db->dict,key->ptr);
    redisAssertWithInfo(NULL,key,kde != NULL);
    de = dictReplaceRaw(db->expires,dictGetKey(kde));
    dictSetSignedIntegerVal(de,when);
}

//获取过期时间
PORT_LONGLONG getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key->ptr)) == NULL) return -1;

    /* The entry was found in the expire dict, this means it should also
     * be present in the main dict (safety check). */
    redisAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    return dictGetSignedIntegerVal(de);
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys. */
//过期键传播
void propagateExpire(redisDb *db, robj *key) {
    robj *argv[2];

    argv[0] = shared.del;
    argv[1] = key;
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);
	//将过期健以删除命令写入aof文件
    if (server.aof_state != REDIS_AOF_OFF)
        feedAppendOnlyFile(server.delCommand,db->id,argv,2);
	//传播过期健给slave
    replicationFeedSlaves(server.slaves,db->id,argv,2);

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/*检查键是否过期,如果过期从数据库中删除,返回0表示没有过期,返回1表示键被删除*/
int expireIfNeeded(redisDb *db, robj *key) {
    //得到过期时间,单位毫秒
	mstime_t when = getExpire(db,key);
    mstime_t now;
	//没有设置过期时间直接返回
    if (when < 0) return 0; /* No expire for this key */

    /* 服务器正在载入则不进行检查 */
    if (server.loading) return 0;

    /* If we are in the context of a Lua script, we claim that time is
     * blocked to when the Lua script started. This way a key can expire
     * only the first time it is accessed and not in the middle of the
     * script execution, making propagation to slaves / AOF consistent.
     * See issue #1525 on Github for more information. */
	//返回一个unix时间单位毫秒
    now = server.lua_caller ? server.lua_time_start : mstime();

    /* If we are running in the context of a slave, return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. */
	// 如果服务器正在进行主从节点的复制，从节点的过期键应该被 主节点发送同步删除的操作 删除，而自己不主动删除
	// 从节点只返回正确的逻辑信息，0表示key仍然没有过期，1表示key过期。
    if (server.masterhost != NULL) return now > when;

    /* 将没有过期直接返回0 */
    if (now <= when) return 0;

    /* 删除已经过期的键 */
    server.stat_expiredkeys++; //过期键数量加一
    propagateExpire(db,key);  //将过期键传递给AOF文件和从节点
	//发送expired事件通知
	notifyKeyspaceEvent(REDIS_NOTIFY_EXPIRED,
        "expired",key,db->id);
	//从数据库中删除key
    return dbDelete(db,key);
}

/*-----------------------------------------------------------------------------
 * Expires Commands
 *----------------------------------------------------------------------------*/

/* This is the generic command implementation for EXPIRE, PEXPIRE, EXPIREAT
 * and PEXPIREAT. Because the commad second argument may be relative or absolute
 * the "basetime" argument is used to signal what the base time is (either 0
 * for *AT variants of the command, or the current time for relative expires).
 *
 * unit is either UNIT_SECONDS or UNIT_MILLISECONDS, and is only used for
 * the argv[2] parameter. The basetime is always specified in milliseconds. */
void expireGenericCommand(redisClient *c, PORT_LONGLONG basetime, int unit) {
    robj *key = c->argv[1], *param = c->argv[2];
    PORT_LONGLONG when; /* unix time in milliseconds when the key will expire. */
	// 取出时间参数保存到when中
    if (getLongLongFromObjectOrReply(c, param, &when, NULL) != REDIS_OK)
        return;
	// 如果过期时间是以秒为单位，则转换为毫秒值
    if (unit == UNIT_SECONDS) when *= 1000;
	// 绝对时间
    when += basetime;

	// 判断key是否在数据库中，不在返回0
    if (lookupKeyRead(c->db,key) == NULL) {
        addReply(c,shared.czero);
        return;
    }

	// 如果当前正在载入AOF数据或者在从节点环境中，即使EXPIRE的TTL为负数，或者EXPIREAT的时间戳已经过期
	// 服务器都不会执行DEL命令，且将过期TTL设置为键的过期时间，等待主节点发来的DEL命令
	// 如果when已经过时，服务器为主节点且没有载入AOF数据
    if (when <= mstime() && !server.loading && !server.masterhost) {
        robj *aux;
		// 将key从数据库中删除
        redisAssertWithInfo(c,key,dbDelete(c->db,key));
        server.dirty++;

		// 创建一个"DEL"命令
        aux = createStringObject("DEL",3);
		//修改客户端的参数列表为DEL命令
        rewriteClientCommandVector(c,2,aux,key);
        decrRefCount(aux);
		// 发送键被修改的信号
        signalModifiedKey(c->db,key);
		// 发送"del"的事件通知
        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",key,c->db->id);
        addReply(c, shared.cone);
        return;
	// 如果当前服务器是从节点，或者服务器正在载入AOF数据
	// 不管when有没有过时，都设置为过期时间
    } else {
		// 设置过期时间
        setExpire(c->db,key,when);
        addReply(c,shared.cone);
		signalModifiedKey(c->db, key); //发送键被修改的信号
		//发送"expire"的事件通知
        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"expire",key,c->db->id);
        server.dirty++;
        return;
    }
}

void expireCommand(redisClient *c) {
    expireGenericCommand(c,mstime(),UNIT_SECONDS);
}

void expireatCommand(redisClient *c) {
    expireGenericCommand(c,0,UNIT_SECONDS);
}

void pexpireCommand(redisClient *c) {
    expireGenericCommand(c,mstime(),UNIT_MILLISECONDS);
}

void pexpireatCommand(redisClient *c) {
    expireGenericCommand(c,0,UNIT_MILLISECONDS);
}

// TTL、PTTL命令底层实现，output_ms为1，返回毫秒，为0返回秒
void ttlGenericCommand(redisClient *c, int output_ms) {
    PORT_LONGLONG expire, ttl = -1;

	// 判断key是否存在于数据库，并且不修改键的使用时间
    if (lookupKeyRead(c->db,c->argv[1]) == NULL) {
        addReplyLongLong(c,-2);
        return;
    }
	// 如果key存在，则备份当前key的过期时间
    expire = getExpire(c->db,c->argv[1]);
	// 如果设置了过期时间
    if (expire != -1) {
        ttl = expire-mstime();//计算生存时间
        if (ttl < 0) ttl = 0;
    }
	// 如果键是永久的
    if (ttl == -1) {
        addReplyLongLong(c,-1); //发送-1
    } else {
        addReplyLongLong(c,output_ms ? ttl : ((ttl+500)/1000));
    }
}

void ttlCommand(redisClient *c) {
    ttlGenericCommand(c, 0);
}

void pttlCommand(redisClient *c) {
    ttlGenericCommand(c, 1);
}

void persistCommand(redisClient *c) {
    dictEntry *de;

    de = dictFind(c->db->dict,c->argv[1]->ptr);
    if (de == NULL) {
        addReply(c,shared.czero);
    } else {
        if (removeExpire(c->db,c->argv[1])) {
            addReply(c,shared.cone);
            server.dirty++;
        } else {
            addReply(c,shared.czero);
        }
    }
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands
 * ---------------------------------------------------------------------------*/

/* The base case is to use the keys position as given in the command table
 * (firstkey, lastkey, step). */
int *getKeysUsingCommandTable(struct redisCommand *cmd,robj **argv, int argc, int *numkeys) {
    int j, i = 0, last, *keys;
    REDIS_NOTUSED(argv);

    if (cmd->firstkey == 0) {
        *numkeys = 0;
        return NULL;
    }
    last = cmd->lastkey;
    if (last < 0) last = argc+last;
    keys = zmalloc(sizeof(int)*((last - cmd->firstkey)+1));
    for (j = cmd->firstkey; j <= last; j += cmd->keystep) {
        redisAssert(j < argc);
        keys[i++] = j;
    }
    *numkeys = i;
    return keys;
}

/* Return all the arguments that are keys in the command passed via argc / argv.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is an heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0].
 *
 * This function uses the command table if a command-specific helper function
 * is not required, otherwise it calls the command-specific function. */
int *getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    if (cmd->getkeys_proc) {
        return cmd->getkeys_proc(cmd,argv,argc,numkeys);
    } else {
        return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
    }
}

/* Free the result of getKeysFromCommand. */
void getKeysFreeResult(int *result) {
    zfree(result);
}

/* 工具函数从command中提取keys
Helper function to extract keys from following commands:
 * ZUNIONSTORE <destkey> <num-keys> <key> <key> ... <key> <options>
 * ZINTERSTORE <destkey> <num-keys> <key> <key> ... <key> <options> */
int *zunionInterGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    REDIS_NOTUSED(cmd);

    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    if (num > (argc-3)) {
        *numkeys = 0;
        return NULL;
    }

    /* Keys in z{union,inter}store come from two places:
     * argv[1] = storage key,
     * argv[3...n] = keys to intersect */
    keys = zmalloc(sizeof(int)*(num+1));

    /* Add all key positions for argv[3...n] to keys[] */
    for (i = 0; i < num; i++) keys[i] = 3+i;

    /* Finally add the argv[1] key position (the storage key target). */
    keys[num] = 1;
    *numkeys = num+1;  /* Total keys = {union,inter} keys + storage key */
    return keys;
}

/* 工具函数从command中提取keys
    Helper function to extract keys from the following commands:
 * EVAL <script> <num-keys> <key> <key> ... <key> [more stuff]
 * EVALSHA <script> <num-keys> <key> <key> ... <key> [more stuff] */
int *evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    REDIS_NOTUSED(cmd);

    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    if (num > (argc-3)) {
        *numkeys = 0;
        return NULL;
    }

    keys = zmalloc(sizeof(int)*num);
    *numkeys = num;

    /* Add all key positions for argv[3...n] to keys[] */
    for (i = 0; i < num; i++) keys[i] = 3+i;

    return keys;
}

/*工具函数从SORT command中提取keys
  Helper function to extract keys from the SORT command.
 *
 * SORT <sort-key> ... STORE <store-key> ...
 *
 * The first argument of SORT is always a key, however a list of options
 * follow in SQL-alike style. Here we parse just the minimum in order to
 * correctly identify keys in the "STORE" option. */
int *sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, j, num, *keys, found_store = 0;
    REDIS_NOTUSED(cmd);

    num = 0;
    keys = zmalloc(sizeof(int)*2); /* Alloc 2 places for the worst case. */

    keys[num++] = 1; /* <sort-key> is always present. */

    /* Search for STORE option. By default we consider options to don't
     * have arguments, so if we find an unknown option name we scan the
     * next. However there are options with 1 or 2 arguments, so we
     * provide a list here in order to skip the right number of args. */
    struct {
        char *name;
        int skip;
    } skiplist[] = {
        {"limit", 2},
        {"get", 1},
        {"by", 1},
        {NULL, 0} /* End of elements. */
    };

    for (i = 2; i < argc; i++) {
        for (j = 0; skiplist[j].name != NULL; j++) {
            if (!strcasecmp(argv[i]->ptr,skiplist[j].name)) {
                i += skiplist[j].skip;
                break;
            } else if (!strcasecmp(argv[i]->ptr,"store") && i+1 < argc) {
                /* Note: we don't increment "num" here and continue the loop
                 * to be sure to process the *last* "STORE" option if multiple
                 * ones are provided. This is same behavior as SORT. */
                found_store = 1;
                keys[num] = i+1; /* <store-key> */
                break;
            }
        }
    }
    *numkeys = num + found_store;
    return keys;
}

//将key添加跳跃表slots_to_keys中对应的slot中
void slotToKeyAdd(robj *key) {
    unsigned int hashslot = keyHashSlot(key->ptr,(int)sdslen(key->ptr));        WIN_PORT_FIX /* cast (int) */

    zslInsert(server.cluster->slots_to_keys,hashslot,key);
    incrRefCount(key);
}
//从key所在slot删除指定key
void slotToKeyDel(robj *key) {
    unsigned int hashslot = keyHashSlot(key->ptr,(int)sdslen(key->ptr));        WIN_PORT_FIX /* cast (int) */

    zslDelete(server.cluster->slots_to_keys,hashslot,key);
}
//重置跳跃表的slots_to_keys中的所有信息
void slotToKeyFlush(void) {
    zslFree(server.cluster->slots_to_keys);
    server.cluster->slots_to_keys = zslCreate();
}

//从指定的slot随机获取count个key
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count) {
    zskiplistNode *n;
    zrangespec range;
    int j = 0;

    range.min = range.max = hashslot;
    range.minex = range.maxex = 0;

    n = zslFirstInRange(server.cluster->slots_to_keys, &range);
    while(n && n->score == hashslot && count--) {
        keys[j++] = n->obj;
        n = n->level[0].forward;
    }
    return j;
}

// 删除指定slot中所有的key信息
unsigned int delKeysInSlot(unsigned int hashslot) {
    zskiplistNode *n;
    zrangespec range;
    int j = 0;

    range.min = range.max = hashslot;
    range.minex = range.maxex = 0;

    n = zslFirstInRange(server.cluster->slots_to_keys, &range);
    while(n && n->score == hashslot) {
        robj *key = n->obj;
        n = n->level[0].forward; /* Go to the next item before freeing it. */
        incrRefCount(key); /* Protect the object while freeing it. */
        dbDelete(&server.db[0],key);
        decrRefCount(key);
        j++;
    }
    return j;
}
//获取指定槽中key的数量
unsigned int countKeysInSlot(unsigned int hashslot) {
    zskiplist *zsl = server.cluster->slots_to_keys;
    zskiplistNode *zn;
    zrangespec range;
    int rank, count = 0;

    range.min = range.max = hashslot;
    range.minex = range.maxex = 0;

    /* Find first element in range */
    zn = zslFirstInRange(zsl, &range);

    /* Use rank of first element, if any, to determine preliminary count */
    if (zn != NULL) {
        rank = (int) zslGetRank(zsl, zn->score, zn->obj);                       WIN_PORT_FIX /* cast (int) */
        count = (int) (zsl->length - (rank - 1));                               WIN_PORT_FIX /* cast (int) */

        /* Find last element in range */
        zn = zslLastInRange(zsl, &range);

        /* Use rank of last element, if any, to determine the actual count */
        if (zn != NULL) {
            rank = (int) zslGetRank(zsl, zn->score, zn->obj);                   WIN_PORT_FIX /* cast (int) */
            count -= (int) (zsl->length - rank);                                WIN_PORT_FIX /* cast (int) */
        }
    }
    return count;
}
