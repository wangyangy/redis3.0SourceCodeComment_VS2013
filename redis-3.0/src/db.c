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

/*�Ӽ�ֵ���ֵ����ҵ�key��Ӧ��value*/
robj *lookupKey(redisDb *db, robj *key) {
	//����key���Ҽ�ֵ��
    dictEntry *de = dictFind(db->dict,key->ptr);
	//����ҵ�
    if (de) {
		//��ȡvalue
        robj *val = dictGetVal(de);

        //���¼���ʹ��ʱ��
        if (server.rdb_child_pid == -1 && server.aof_child_pid == -1)
            val->lru = LRU_CLOCK();
        return val; //����ֵ����
    } else {
        return NULL;
    }
}

/*�Զ�����ȡ��key��ֵ����*/
robj *lookupKeyRead(redisDb *db, robj *key) {
    robj *val;
	//�����Ƿ����
    expireIfNeeded(db,key);
	//��ȡvalue
    val = lookupKey(db,key);
	//�����Ƿ����е���Ϣ
    if (val == NULL)
        server.stat_keyspace_misses++;
    else
        server.stat_keyspace_hits++;
    return val;
}

/*��д����ȡ��key��ֵ����,�������Ƿ����е���Ϣ,����ֵ��value*/
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

/* ��key��ӵ����ݿ���,�������ü��� 
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

/*��д��ֵ�� */
void dbOverwrite(redisDb *db, robj *key, robj *val) {
    dictEntry *de = dictFind(db->dict,key->ptr);

    redisAssertWithInfo(NULL,key,de != NULL);
    dictReplace(db->dict, key->ptr, val);
}

/* ����key */
void setKey(redisDb *db, robj *key, robj *val) {
	//���û��value
    if (lookupKeyWrite(db,key) == NULL) {
		//���key-value
        dbAdd(db,key,val);
    } else {
		//���򸲸�key-value
        dbOverwrite(db,key,val);
    }
	//�������ü���
    incrRefCount(val);
	//ɾ������ʱ��
    removeExpire(db,key);
	//֪ͨ�޸�
    signalModifiedKey(db,key);
}

/*�ж�key�Ƿ����*/
int dbExists(redisDb *db, robj *key) {
    return dictFind(db->dict,key->ptr) != NULL;
}

/* ��redisObject����ʽ�������һ��û�й��ڵ�key����
 ���û��key����null
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

/* ɾ��һ����ֵ��,�ɹ�����1,ʧ�ܷ���0 */
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

/*������ݿ�*/
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

/*����idѡ�����ݿ�*/
int selectDb(redisClient *c, int id) {
	//id�Ƿ����ش���
    if (id < 0 || id >= server.dbnum)
        return REDIS_ERR;
	//���õ�ǰ���ݿ�
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
/*scan������ĵײ�ʵ��,o���������hash����򼯺϶���,�������������ǰ���ݿ��,���o����kong
��ô˵������һ����ϣ�򼯺϶��󣬺�����������Щ�����󣬶Բ������з�������ǹ�ϣ���󣬷��ص��Ǽ�ֵ��*/
void scanGenericCommand(redisClient *c, robj *o, PORT_ULONG cursor) {
    int i, j;
    list *keys = listCreate();  //����һ���б�
    listNode *node, *nextnode;
    PORT_LONG count = 10;
    sds pat;
    int patlen, use_pattern = 0;
    dict *ht;

    
	// �������͵ļ�飬Ҫô����������Ҫô��ǰ���϶���Ҫô������ϣ����Ҫô�������򼯺϶���
    redisAssert(o == NULL || o->type == REDIS_SET || o->type == REDIS_HASH ||
                o->type == REDIS_ZSET);

    /* �����һ���������±�,����Ǽ���,Ҫ�����ü� */
    i = (o == NULL) ? 2 : 3; /* Skip the key argument if needed. */

    /* ����ѡ�� */
    while (i < c->argc) {
        j = c->argc - i;
		// �趨COUNT������COUNT ѡ������þ������û���֪������� ��ÿ�ε�����Ӧ�÷��ض���Ԫ�ء�
        if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
            //���������count
			if (getLongFromObjectOrReply(c, c->argv[i+1], &count, NULL)
                != REDIS_OK)
            {
                goto cleanup;
            }
			//�������С��1�﷨����
            if (count < 1) {
                addReply(c,shared.syntaxerr);
                goto cleanup;
            }

            i += 2;
		// �趨MATCH������������ֻ���غ͸���ģʽ��ƥ���Ԫ�ء�
        } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {
            pat = c->argv[i+1]->ptr;
            patlen = (int)sdslen(pat);    //pattern�ַ�������                                       WIN_PORT_FIX /* cast (int) */

			// ���pattern��"*"���Ͳ���ƥ�䣬ȫ�����أ�����Ϊ0
            use_pattern = !(pat[0] == '*' && patlen == 1);

            i += 2;
        } else {
            addReply(c,shared.syntaxerr);
            goto cleanup;
        }
    }

	// 2.���������ziplist��intset�����������ǹ�ϣ����ô��Щ����ֻ�ǰ���������Ԫ��
	// ����һ�ν������е�Ԫ��ȫ�����ظ������ߣ��������α�cursorΪ0����ʾ�������
    ht = NULL;
	//�������ݿ�
    if (o == NULL) {
        ht = c->db->dict;
	//����HT����ļ��϶���
    } else if (o->type == REDIS_SET && o->encoding == REDIS_ENCODING_HT) {
        ht = o->ptr;
	//����HT�����hash����
    } else if (o->type == REDIS_HASH && o->encoding == REDIS_ENCODING_HT) {
        ht = o->ptr;
        count *= 2; /* We return key / value for this type. */
	//����skiplist��������򼯺϶���
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
		// �������ĵ�������Ϊ10*count��
        PORT_LONG maxiterations = count * 10;

        /* We pass two pointers to the callback: the list to which it will
         * add new elements, and the object containing the dictionary so that
         * it is possible to fetch more data in a type-dependent way. */
		// �ص�����scanCallback�Ĳ���privdata��һ�����飬������Ǳ���������ļ���ֵ
		// �ص�����scanCallback����һ����������һ���ֵ����
		// �ص�����scanCallback�����ã����ֵ�����н���ֵ����ȡ���������ù��ֵ������ʲô��������
        privdata[0] = keys;
        privdata[1] = o;
		// ѭ��ɨ��ht�����α�cursor��ʼ������ָ����scanCallback���������ht�е����ݵ��տ�ʼ�������б�keys��
        do {
            cursor = dictScan(ht, cursor, scanCallback, privdata);
        } while (cursor &&
              maxiterations-- &&
              listLength(keys) < (PORT_ULONG)count);
	// ����Ǽ��϶��󵫱��벻��HT����������
    } else if (o->type == REDIS_SET) {
        int pos = 0;
        int64_t ll;
		// ������ֵȡ�������������ַ���������뵽keys�б��У��α�����Ϊ0����ʾ�������
        while(intsetGet(o->ptr,pos++,&ll))
            listAddNodeTail(keys,createStringObjectFromLongLong(ll));
        cursor = 0;
	// ����ǹ�ϣ���󣬻����򼯺϶��󣬵��Ǳ��붼����HT����ziplist
    } else if (o->type == REDIS_HASH || o->type == REDIS_ZSET) {
        unsigned char *p = ziplistIndex(o->ptr,0);
        unsigned char *vstr;
        unsigned int vlen;
        PORT_LONGLONG vll;
		// ��ֵȡ���������ݲ�ͬ���͵�ֵ����������ͬ���ַ������󣬼��뵽keys�б���
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

	// 3. �������MATCH������Ҫ���й���
    node = listFirst(keys);//�����׵�ַ
    while (node) {
        robj *kobj = listNodeValue(node);  //key
        nextnode = listNextNode(node);     //��һ���ڵ�
        int filter = 0;     //Ĭ�ϲ�����

		//pattern����"*"���Ҫ����
        if (!filter && use_pattern) {
			// ���kobj���ַ�������
            if (sdsEncodedObject(kobj)) {
				// kobj��ֵ��ƥ��pattern�����ù��˱�־
                if (!stringmatchlen(pat, patlen, kobj->ptr, (int)sdslen(kobj->ptr), 0)) WIN_PORT_FIX /* cast (int) */
                    filter = 1;
			// ���kobj����������
            } else {
                char buf[REDIS_LONGSTR_SIZE];
                int len;

                redisAssert(kobj->encoding == REDIS_ENCODING_INT);
				// ������ת��Ϊ�ַ������ͣ����浽buf��
                len = ll2string(buf,sizeof(buf),(PORT_LONG)kobj->ptr);          WIN_PORT_FIX /* cast (PORT_LONG) */
				//buf��ֵ��ƥ��pattern�����ù��˱�־
                if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
            }
        }

		// ����Ŀ�������ݿ⣬���kobj�ǹ��ڼ��������
        if (!filter && o == NULL && expireIfNeeded(c->db, kobj)) filter = 1;

		// ����ü������������Ĺ�����������ô�����keys�б�ɾ�����ͷ�
        if (filter) {
            decrRefCount(kobj);
            listDelNode(keys, node);
        }

        
		// �����ǰ����Ŀ�������򼯺ϻ��ϣ�������keys�б��б�����Ǽ�ֵ�ԣ����key�����󱻹��ˣ�ֵ����ҲӦ��������
        if (o && (o->type == REDIS_ZSET || o->type == REDIS_HASH)) {
            node = nextnode;
            nextnode = listNextNode(node);
			// ����ü������������Ĺ�����������ô�����keys�б�ɾ�����ͷ�
            if (filter) {
                kobj = listNodeValue(node);  //ȡ��value
                decrRefCount(kobj);
                listDelNode(keys, node);  //ɾ��
            }
        }
        node = nextnode;
    }

	// 4. �ظ���Ϣ��client
    addReplyMultiBulkLen(c, 2);    //2���֣�һ�����α꣬һ�����б�
    addReplyBulkLongLong(c,cursor);   //�ظ��α�

    addReplyMultiBulkLen(c, listLength(keys));
	//ѭ���ظ��б��е�Ԫ�أ����ͷ�
    while ((node = listFirst(keys)) != NULL) {
        robj *kobj = listNodeValue(node);
        addReplyBulk(c, kobj);
        decrRefCount(kobj);
        listDelNode(keys, node);
    }

cleanup:
    listSetFreeMethod(keys,decrRefCountVoid);  //�����ض����ͷ��б�ķ�ʽdecrRefCountVoid
	listRelease(keys);               // �ͷ�
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
//��������
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

//�ر�����
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

/*rename,reanemx����ĵײ�ʵ��*/
void renameGenericCommand(redisClient *c, int nx) {
    robj *o;
    PORT_LONGLONG expire;

	// key��newkey��ͬ�Ļ�������samekey��־
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) {
        addReply(c,shared.sameobjecterr);
        return;
    }
	//��д������ȡkey��value
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;
	// ����ֵ��������ü������������������ڹ���newkey���Է�ɾ����key˳����ֵ����Ҳɾ��
    incrRefCount(o);
	//��ȡ����ʱ��
    expire = getExpire(c->db,c->argv[1]);
	// �ж�newkey��ֵ�����Ƿ����
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
		// ������nx��־���򲻷����Ѵ��ڵ�����������0
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        /*ɾ���ɵļ�,�ڴ����µ�֮ǰ*/
        dbDelete(c->db,c->argv[2]);
    }
	// ��newkey��key��ֵ�������
    dbAdd(c->db,c->argv[2],o);
	// ���newkey���ù�����ʱ�䣬��Ϊnewkey���ù���ʱ��
    if (expire != -1) setExpire(c->db,c->argv[2],expire);
    //ɾ��key
	dbDelete(c->db,c->argv[1]);
	//�������������޸ĵ��ź�
    signalModifiedKey(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[2]);
	//�����¼�֪ͨ
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

/*move����ʵ��*/
void moveCommand(redisClient *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;
    PORT_LONGLONG dbid, expire;
	// ���������ڼ�Ⱥģʽ����֧�ֶ����ݿ�
    if (server.cluster_enabled) {
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

	// ���Դ���ݿ��Դ���ݿ��id
    src = c->db;
    srcid = c->db->id;
	// ������db��ֵ���浽dbid�������л��������ݿ���
    if (getLongLongFromObject(c->argv[2],&dbid) == REDIS_ERR ||
        dbid < INT_MIN || dbid > INT_MAX ||
        selectDb(c,(int)dbid) == REDIS_ERR)                                     WIN_PORT_FIX /* cast (int) */
    {
        addReply(c,shared.outofrangeerr);
        return;
    }
	// Ŀ�����ݿ�
    dst = c->db;
	// �л���Դ���ݿ�
    selectDb(c,srcid); /* Back to the source DB */

    
	// ���ǰ���л������ݿ���ͬ���򷵻��йش���
    if (src == dst) {
        addReply(c,shared.sameobjecterr);
        return;
    }

	// ��д����ȡ��Դ���ݿ�Ķ���
    o = lookupKeyWrite(c->db,c->argv[1]);
	// �����ڷ���0
    if (!o) {
        addReply(c,shared.czero);
        return;
    }
	// ����key�Ĺ���ʱ��
    expire = getExpire(c->db,c->argv[1]);

	// �жϵ�ǰkey�Ƿ������Ŀ�����ݿ⣬����ֱ�ӷ��أ�����0
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
        addReply(c,shared.czero);
        return;
    }
	// ��key-value������ӵ�Ŀ�����ݿ���
    dbAdd(dst,c->argv[1],o);
	//���ù���ʱ��
    if (expire != -1) setExpire(dst,c->argv[1],expire);
    incrRefCount(o);//�������ü���

	// ��Դ���ݿ��н�key�͹�����ֵ����ɾ��
    dbDelete(src,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);  //�ظ�1
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/
//ɾ������ʱ��
int removeExpire(redisDb *db, robj *key) {
    /* An expire may only be removed if there is a corresponding entry in the
     * main dict. Otherwise, the key will never be freed. */
    redisAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}
//���ù���ʱ��
void setExpire(redisDb *db, robj *key, PORT_LONGLONG when) {
    dictEntry *kde, *de;

    /* Reuse the sds from the main dict in the expire dict */
    kde = dictFind(db->dict,key->ptr);
    redisAssertWithInfo(NULL,key,kde != NULL);
    de = dictReplaceRaw(db->expires,dictGetKey(kde));
    dictSetSignedIntegerVal(de,when);
}

//��ȡ����ʱ��
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
//���ڼ�����
void propagateExpire(redisDb *db, robj *key) {
    robj *argv[2];

    argv[0] = shared.del;
    argv[1] = key;
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);
	//�����ڽ���ɾ������д��aof�ļ�
    if (server.aof_state != REDIS_AOF_OFF)
        feedAppendOnlyFile(server.delCommand,db->id,argv,2);
	//�������ڽ���slave
    replicationFeedSlaves(server.slaves,db->id,argv,2);

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/*�����Ƿ����,������ڴ����ݿ���ɾ��,����0��ʾû�й���,����1��ʾ����ɾ��*/
int expireIfNeeded(redisDb *db, robj *key) {
    //�õ�����ʱ��,��λ����
	mstime_t when = getExpire(db,key);
    mstime_t now;
	//û�����ù���ʱ��ֱ�ӷ���
    if (when < 0) return 0; /* No expire for this key */

    /* ���������������򲻽��м�� */
    if (server.loading) return 0;

    /* If we are in the context of a Lua script, we claim that time is
     * blocked to when the Lua script started. This way a key can expire
     * only the first time it is accessed and not in the middle of the
     * script execution, making propagation to slaves / AOF consistent.
     * See issue #1525 on Github for more information. */
	//����һ��unixʱ�䵥λ����
    now = server.lua_caller ? server.lua_time_start : mstime();

    /* If we are running in the context of a slave, return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. */
	// ������������ڽ������ӽڵ�ĸ��ƣ��ӽڵ�Ĺ��ڼ�Ӧ�ñ� ���ڵ㷢��ͬ��ɾ���Ĳ��� ɾ�������Լ�������ɾ��
	// �ӽڵ�ֻ������ȷ���߼���Ϣ��0��ʾkey��Ȼû�й��ڣ�1��ʾkey���ڡ�
    if (server.masterhost != NULL) return now > when;

    /* ��û�й���ֱ�ӷ���0 */
    if (now <= when) return 0;

    /* ɾ���Ѿ����ڵļ� */
    server.stat_expiredkeys++; //���ڼ�������һ
    propagateExpire(db,key);  //�����ڼ����ݸ�AOF�ļ��ʹӽڵ�
	//����expired�¼�֪ͨ
	notifyKeyspaceEvent(REDIS_NOTIFY_EXPIRED,
        "expired",key,db->id);
	//�����ݿ���ɾ��key
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
	// ȡ��ʱ��������浽when��
    if (getLongLongFromObjectOrReply(c, param, &when, NULL) != REDIS_OK)
        return;
	// �������ʱ��������Ϊ��λ����ת��Ϊ����ֵ
    if (unit == UNIT_SECONDS) when *= 1000;
	// ����ʱ��
    when += basetime;

	// �ж�key�Ƿ������ݿ��У����ڷ���0
    if (lookupKeyRead(c->db,key) == NULL) {
        addReply(c,shared.czero);
        return;
    }

	// �����ǰ��������AOF���ݻ����ڴӽڵ㻷���У���ʹEXPIRE��TTLΪ����������EXPIREAT��ʱ����Ѿ�����
	// ������������ִ��DEL����ҽ�����TTL����Ϊ���Ĺ���ʱ�䣬�ȴ����ڵ㷢����DEL����
	// ���when�Ѿ���ʱ��������Ϊ���ڵ���û������AOF����
    if (when <= mstime() && !server.loading && !server.masterhost) {
        robj *aux;
		// ��key�����ݿ���ɾ��
        redisAssertWithInfo(c,key,dbDelete(c->db,key));
        server.dirty++;

		// ����һ��"DEL"����
        aux = createStringObject("DEL",3);
		//�޸Ŀͻ��˵Ĳ����б�ΪDEL����
        rewriteClientCommandVector(c,2,aux,key);
        decrRefCount(aux);
		// ���ͼ����޸ĵ��ź�
        signalModifiedKey(c->db,key);
		// ����"del"���¼�֪ͨ
        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",key,c->db->id);
        addReply(c, shared.cone);
        return;
	// �����ǰ�������Ǵӽڵ㣬���߷�������������AOF����
	// ����when��û�й�ʱ��������Ϊ����ʱ��
    } else {
		// ���ù���ʱ��
        setExpire(c->db,key,when);
        addReply(c,shared.cone);
		signalModifiedKey(c->db, key); //���ͼ����޸ĵ��ź�
		//����"expire"���¼�֪ͨ
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

// TTL��PTTL����ײ�ʵ�֣�output_msΪ1�����غ��룬Ϊ0������
void ttlGenericCommand(redisClient *c, int output_ms) {
    PORT_LONGLONG expire, ttl = -1;

	// �ж�key�Ƿ���������ݿ⣬���Ҳ��޸ļ���ʹ��ʱ��
    if (lookupKeyRead(c->db,c->argv[1]) == NULL) {
        addReplyLongLong(c,-2);
        return;
    }
	// ���key���ڣ��򱸷ݵ�ǰkey�Ĺ���ʱ��
    expire = getExpire(c->db,c->argv[1]);
	// ��������˹���ʱ��
    if (expire != -1) {
        ttl = expire-mstime();//��������ʱ��
        if (ttl < 0) ttl = 0;
    }
	// ����������õ�
    if (ttl == -1) {
        addReplyLongLong(c,-1); //����-1
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

/* ���ߺ�����command����ȡkeys
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

/* ���ߺ�����command����ȡkeys
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

/*���ߺ�����SORT command����ȡkeys
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

//��key�����Ծ��slots_to_keys�ж�Ӧ��slot��
void slotToKeyAdd(robj *key) {
    unsigned int hashslot = keyHashSlot(key->ptr,(int)sdslen(key->ptr));        WIN_PORT_FIX /* cast (int) */

    zslInsert(server.cluster->slots_to_keys,hashslot,key);
    incrRefCount(key);
}
//��key����slotɾ��ָ��key
void slotToKeyDel(robj *key) {
    unsigned int hashslot = keyHashSlot(key->ptr,(int)sdslen(key->ptr));        WIN_PORT_FIX /* cast (int) */

    zslDelete(server.cluster->slots_to_keys,hashslot,key);
}
//������Ծ���slots_to_keys�е�������Ϣ
void slotToKeyFlush(void) {
    zslFree(server.cluster->slots_to_keys);
    server.cluster->slots_to_keys = zslCreate();
}

//��ָ����slot�����ȡcount��key
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

// ɾ��ָ��slot�����е�key��Ϣ
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
//��ȡָ������key������
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
