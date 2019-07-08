#include "redis.h"

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/

void sunionDiffGenericCommand(redisClient *c, robj **setkeys, int setnum, robj *dstkey, int op);

/* 创建一个保存value的集合 */
robj *setTypeCreate(robj *value) {
	//setType总共两种类型 intset或者 dict,intset是有序的
    if (isObjectRepresentableAsLongLong(value,NULL) == REDIS_OK)
        return createIntsetObject();
    return createSetObject();
}
//向subTypeAdd集合中添加value,添加成功返回1失败返回0 
int setTypeAdd(robj *subject, robj *value) {
    PORT_LONGLONG llval;
	//根据类型添加元素
    if (subject->encoding == REDIS_ENCODING_HT) {
		//若为hash编码则在集合中新加value 新键
        if (dictAdd(subject->ptr,value,NULL) == DICT_OK) {
            incrRefCount(value);
            return 1;
        }
	//若编码为intset 那么将value转化为整型
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value,&llval) == REDIS_OK) {
            uint8_t success = 0;
            subject->ptr = intsetAdd(subject->ptr,llval,&success);
			//将llval加入到ptr所指的intset数据结构中
            if (success) {
				//添加成功后则看看有没有超出最大值
                if (intsetLen(subject->ptr) > server.set_max_intset_entries)
                    setTypeConvert(subject,REDIS_ENCODING_HT);
                return 1;
            }
        } else {
			//若不能转化为整型，那么将subject转成dict
			//然后添加value - null 键值对
            setTypeConvert(subject,REDIS_ENCODING_HT);

            /* The set *was* an intset and this value is not integer
             * encodable, so dictAdd should always work. */
            redisAssertWithInfo(NULL,value,dictAdd(subject->ptr,value,NULL) == DICT_OK);
            incrRefCount(value);
            return 1;
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}
// 从集合对象中删除一个值为value的元素，删除成功返回1，失败返回0
int setTypeRemove(robj *setobj, robj *value) {
    PORT_LONGLONG llval;
    if (setobj->encoding == REDIS_ENCODING_HT) {
        if (dictDelete(setobj->ptr,value) == DICT_OK) {
            if (htNeedsResize(setobj->ptr)) dictResize(setobj->ptr);
            return 1;
        }
    } else if (setobj->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value,&llval) == REDIS_OK) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr,llval,&success);
            if (success) return 1;
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}
// 集合中是否存在值为value的元素，存在返回1，否则返回0
int setTypeIsMember(robj *subject, robj *value) {
    PORT_LONGLONG llval;
    if (subject->encoding == REDIS_ENCODING_HT) {
        return dictFind((dict*)subject->ptr,value) != NULL;
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value,&llval) == REDIS_OK) {
            return intsetFind((intset*)subject->ptr,llval);
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}
// 创建并初始化一个集合类型的迭代器
setTypeIterator *setTypeInitIterator(robj *subject) {
	//分配空间,初始化成员
    setTypeIterator *si = zmalloc(sizeof(setTypeIterator));
    si->subject = subject;
    si->encoding = subject->encoding;
	//初始化字典迭代器
    if (si->encoding == REDIS_ENCODING_HT) {
        si->di = dictGetIterator(subject->ptr);
	//初始化集合迭代器
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        si->ii = 0;
    } else {
        redisPanic("Unknown set encoding");
    }
    return si;
}
// 释放迭代器空间
void setTypeReleaseIterator(setTypeIterator *si) {
	//如果是字典,需要先字典类型的迭代器
    if (si->encoding == REDIS_ENCODING_HT)
        dictReleaseIterator(si->di);
    zfree(si);
}

/* Move to the next entry in the set. Returns the object at the current
 * position.
 *
 * Since set elements can be internally be stored as redis objects or
 * simple arrays of integers, setTypeNext returns the encoding of the
 * set object you are iterating, and will populate the appropriate pointer
 * (eobj) or (llobj) accordingly.
 *
 * When there are no longer elements -1 is returned.
 * Returned objects ref count is not incremented, so this function is
 * copy on write friendly. */
// 将当前迭代器指向的元素保存在objele或llele中，迭代完毕返回 - 1
// 返回的对象的引用计数不增加，支持 读时共享写时复制
int setTypeNext(setTypeIterator *si, robj **objele, int64_t *llele) {
	//迭代字典
    if (si->encoding == REDIS_ENCODING_HT) {
        dictEntry *de = dictNext(si->di);
        if (de == NULL) return -1;
        *objele = dictGetKey(de);
	//迭代整数集合
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
		//将intset中的元素保存到llele中
        if (!intsetGet(si->subject->ptr,si->ii++,llele))
            return -1;
    }
    return si->encoding;   //返回编码类型
}

/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new objects
 * or incrementing the ref count of returned objects. So if you don't
 * retain a pointer to this object you should call decrRefCount() against it.
 *
 * This function is the way to go for write operations where COW is not
 * an issue as the result will be anyway of incrementing the ref count. */
// 返回迭代器当前指向的元素对象的地址，需要手动释放返回的对象
robj *setTypeNextObject(setTypeIterator *si) {
    int64_t intele;
    robj *objele;
    int encoding;
	//得到当前集合的编码类型
    encoding = setTypeNext(si,&objele,&intele);
    switch(encoding) {
        case -1:    return NULL;       //迭代完成
        case REDIS_ENCODING_INTSET:    //整数集合返回一个字符串类型的对象
            return createStringObjectFromLongLong(intele);
        case REDIS_ENCODING_HT:        //字典集合,返回一个共享的该对象
            incrRefCount(objele);
            return objele;
        default:
            redisPanic("Unsupported encoding");
    }
    return NULL; /* just to suppress warnings */
}

// 从集合中随机取出一个对象，保存在参数中
int setTypeRandomElement(robj *setobj, robj **objele, int64_t *llele) {
    if (setobj->encoding == REDIS_ENCODING_HT) {
        dictEntry *de = dictGetRandomKey(setobj->ptr);
        *objele = dictGetKey(de);
    } else if (setobj->encoding == REDIS_ENCODING_INTSET) {
        *llele = intsetRandom(setobj->ptr);
    } else {
        redisPanic("Unknown set encoding");
    }
    return setobj->encoding;
}
// 返回集合的元素数量
PORT_ULONG setTypeSize(robj *subject) {
    if (subject->encoding == REDIS_ENCODING_HT) {
        return (PORT_ULONG) dictSize((dict*)subject->ptr);                     WIN_PORT_FIX /* cast (PORT_ULONG) */
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        return intsetLen((intset*)subject->ptr);
    } else {
        redisPanic("Unknown set encoding");
    }
}

/*转换集合的编码类型为enc */
void setTypeConvert(robj *setobj, int enc) {
    setTypeIterator *si;
    redisAssertWithInfo(NULL,setobj,setobj->type == REDIS_SET &&
                             setobj->encoding == REDIS_ENCODING_INTSET);
	//转换为REDIS_ENCODING_HT字典类型编码
    if (enc == REDIS_ENCODING_HT) {
        int64_t intele;
		//创建一个字典
        dict *d = dictCreate(&setDictType,NULL);
        robj *element;

        /* 扩展字典大小 */
        dictExpand(d,intsetLen(setobj->ptr));

        /* 创建并初始化一个集合类型的迭代器 */
        si = setTypeInitIterator(setobj);
		//迭代整数集合
        while (setTypeNext(si,NULL,&intele) != -1) {
            element = createStringObjectFromLongLong(intele);
            redisAssertWithInfo(NULL,element,dictAdd(d,element,NULL) == DICT_OK);
        }
		//释放迭代器空间
        setTypeReleaseIterator(si);
		//设置转换后集合对象的编码类型
        setobj->encoding = REDIS_ENCODING_HT;
		//更新集合对象的值对象
        zfree(setobj->ptr);
        setobj->ptr = d;
    } else {
        redisPanic("Unsupported set conversion");
    }
}

void saddCommand(redisClient *c) {
    robj *set;
    int j, added = 0;
	//以写操作获取value
    set = lookupKeyWrite(c->db,c->argv[1]);
	//如果set为空
    if (set == NULL) {
		//新建一个集合
        set = setTypeCreate(c->argv[2]);
        dbAdd(c->db,c->argv[1],set); //添加到数据库
    } else {
		//如果set不是REDIS_SET类型,回复类型错误
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }
	//遍历参数
    for (j = 2; j < c->argc; j++) {
        c->argv[j] = tryObjectEncoding(c->argv[j]);
        if (setTypeAdd(set,c->argv[j])) added++;
    }
	//发送通知
    if (added) {
        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_SET,"sadd",c->argv[1],c->db->id);
    }
    server.dirty += added;
    addReplyLongLong(c,added);
}

void sremCommand(redisClient *c) {
    robj *set;
    int j, deleted = 0, keyremoved = 0;
	//读取value检测类型
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,REDIS_SET)) return;
	//遍历参数,删除
    for (j = 2; j < c->argc; j++) {
        if (setTypeRemove(set,c->argv[j])) {
            deleted++;
            if (setTypeSize(set) == 0) {
                dbDelete(c->db,c->argv[1]);
                keyremoved = 1;
                break;
            }
        }
    }
	//如果删除成功,发送通知
    if (deleted) {
        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_SET,"srem",c->argv[1],c->db->id);
        if (keyremoved)
            notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",c->argv[1],
                                c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);
}

void smoveCommand(redisClient *c) {
    robj *srcset, *dstset, *ele;
    srcset = lookupKeyWrite(c->db,c->argv[1]);
    dstset = lookupKeyWrite(c->db,c->argv[2]);
    ele = c->argv[3] = tryObjectEncoding(c->argv[3]);

    /* source key不存在返回0 */
    if (srcset == NULL) {
        addReply(c,shared.czero);
        return;
    }

    /* 检测类型 */
    if (checkType(c,srcset,REDIS_SET) ||
        (dstset && checkType(c,dstset,REDIS_SET))) return;

    /* If srcset and dstset are equal, SMOVE is a no-op */
    if (srcset == dstset) {
        addReply(c,setTypeIsMember(srcset,ele) ? shared.cone : shared.czero);
        return;
    }

    /* If the element cannot be removed from the src set, return 0. */
    if (!setTypeRemove(srcset,ele)) {
        addReply(c,shared.czero);
        return;
    }
    notifyKeyspaceEvent(REDIS_NOTIFY_SET,"srem",c->argv[1],c->db->id);

    /* Remove the src set from the database when empty */
    if (setTypeSize(srcset) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }
    signalModifiedKey(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[2]);
    server.dirty++;

    /* Create the destination set when it doesn't exist */
    if (!dstset) {
        dstset = setTypeCreate(ele);
        dbAdd(c->db,c->argv[2],dstset);
    }

    /* An extra key has changed when ele was successfully added to dstset */
    if (setTypeAdd(dstset,ele)) {
        server.dirty++;
        notifyKeyspaceEvent(REDIS_NOTIFY_SET,"sadd",c->argv[2],c->db->id);
    }
    addReply(c,shared.cone);
}

void sismemberCommand(redisClient *c) {
    robj *set;

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    c->argv[2] = tryObjectEncoding(c->argv[2]);
    if (setTypeIsMember(set,c->argv[2]))
        addReply(c,shared.cone);
    else
        addReply(c,shared.czero);
}

void scardCommand(redisClient *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_SET)) return;

    addReplyLongLong(c,setTypeSize(o));
}

void spopCommand(redisClient *c) {
    robj *set, *ele, *aux;
    int64_t llele;
    int encoding;

    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    encoding = setTypeRandomElement(set,&ele,&llele);
    if (encoding == REDIS_ENCODING_INTSET) {
        ele = createStringObjectFromLongLong(llele);
        set->ptr = intsetRemove(set->ptr,llele,NULL);
    } else {
        incrRefCount(ele);
        setTypeRemove(set,ele);
    }
    notifyKeyspaceEvent(REDIS_NOTIFY_SET,"spop",c->argv[1],c->db->id);

    /* Replicate/AOF this command as an SREM operation */
    aux = createStringObject("SREM",4);
    rewriteClientCommandVector(c,3,aux,c->argv[1],ele);
    decrRefCount(ele);
    decrRefCount(aux);

    addReplyBulk(c,ele);
    if (setTypeSize(set) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }
    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
}

/* handle the "SRANDMEMBER key <count>" variant. The normal version of the
 * command is handled by the srandmemberCommand() function itself. */

/* How many times bigger should be the set compared to the requested size
 * for us to don't use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define SRANDMEMBER_SUB_STRATEGY_MUL 3

void srandmemberWithCountCommand(redisClient *c) {
    PORT_LONG l;
    PORT_ULONG count, size;
    int uniq = 1;
    robj *set, *ele;
    int64_t llele;
    int encoding;

    dict *d;

    if (getLongFromObjectOrReply(c,c->argv[2],&l,NULL) != REDIS_OK) return;
    if (l >= 0) {
        count = (unsigned) l;
    } else {
        /* A negative count means: return the same elements multiple times
         * (i.e. don't remove the extracted element after every extraction). */
        count = -l;
        uniq = 0;
    }

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk))
        == NULL || checkType(c,set,REDIS_SET)) return;
    size = setTypeSize(set);

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) {
        addReply(c,shared.emptymultibulk);
        return;
    }

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. */
    if (!uniq) {
        addReplyMultiBulkLen(c,count);
        while(count--) {
            encoding = setTypeRandomElement(set,&ele,&llele);
            if (encoding == REDIS_ENCODING_INTSET) {
                addReplyBulkLongLong(c,llele);
            } else {
                addReplyBulk(c,ele);
            }
        }
        return;
    }

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the set: simply return the whole set. */
    if (count >= size) {
        sunionDiffGenericCommand(c,c->argv+1,1,NULL,REDIS_OP_UNION);
        return;
    }

    /* For CASE 3 and CASE 4 we need an auxiliary dictionary. */
    d = dictCreate(&setDictType,NULL);

    /* CASE 3:
     * The number of elements inside the set is not greater than
     * SRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a set from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requsted elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 3 is highly inefficient. */
    if (count*SRANDMEMBER_SUB_STRATEGY_MUL > size) {
        setTypeIterator *si;

        /* Add all the elements into the temporary dictionary. */
        si = setTypeInitIterator(set);
        while((encoding = setTypeNext(si,&ele,&llele)) != -1) {
            int retval = DICT_ERR;

            if (encoding == REDIS_ENCODING_INTSET) {
                retval = dictAdd(d,createStringObjectFromLongLong(llele),NULL);
            } else {
                retval = dictAdd(d,dupStringObject(ele),NULL);
            }
            redisAssert(retval == DICT_OK);
        }
        setTypeReleaseIterator(si);
        redisAssert(dictSize(d) == size);

        /* Remove random elements to reach the right count. */
        while(size > count) {
            dictEntry *de;

            de = dictGetRandomKey(d);
            dictDelete(d,dictGetKey(de));
            size--;
        }
    }

    /* CASE 4: We have a big set compared to the requested number of elements.
     * In this case we can simply get random elements from the set and add
     * to the temporary set, trying to eventually get enough unique elements
     * to reach the specified count. */
    else {
        PORT_ULONG added = 0;

        while(added < count) {
            encoding = setTypeRandomElement(set,&ele,&llele);
            if (encoding == REDIS_ENCODING_INTSET) {
                ele = createStringObjectFromLongLong(llele);
            } else {
                ele = dupStringObject(ele);
            }
            /* Try to add the object to the dictionary. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result dictionary. */
            if (dictAdd(d,ele,NULL) == DICT_OK)
                added++;
            else
                decrRefCount(ele);
        }
    }

    /* CASE 3 & 4: send the result to the user. */
    {
        dictIterator *di;
        dictEntry *de;

        addReplyMultiBulkLen(c,count);
        di = dictGetIterator(d);
        while((de = dictNext(di)) != NULL)
            addReplyBulk(c,dictGetKey(de));
        dictReleaseIterator(di);
        dictRelease(d);
    }
}

void srandmemberCommand(redisClient *c) {
    robj *set, *ele;
    int64_t llele;
    int encoding;

    if (c->argc == 3) {
        srandmemberWithCountCommand(c);
        return;
    } else if (c->argc > 3) {
        addReply(c,shared.syntaxerr);
        return;
    }

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    encoding = setTypeRandomElement(set,&ele,&llele);
    if (encoding == REDIS_ENCODING_INTSET) {
        addReplyBulkLongLong(c,llele);
    } else {
        addReplyBulk(c,ele);
    }
}

int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    return (int)(setTypeSize(*(robj**)s1)-setTypeSize(*(robj**)s2));            WIN_PORT_FIX /* cast (int) */
}

/* This is used by SDIFF and in this case we can receive NULL that should
 * be handled as empty sets. */
int qsortCompareSetsByRevCardinality(const void *s1, const void *s2) {
    robj *o1 = *(robj**)s1, *o2 = *(robj**)s2;

    return (int)((o2 ? setTypeSize(o2) : 0) - (o1 ? setTypeSize(o1) : 0));      WIN_PORT_FIX /* cast (int) */
}

/*SINTER,SINTERSTORE(交集和并集的底层实现)一类命令的底层实现*/
void sinterGenericCommand(redisClient *c, robj **setkeys, PORT_ULONG setnum, robj *dstkey) {
    //分配存储集合的数组
	robj **sets = zmalloc(sizeof(robj*)*setnum);
    setTypeIterator *si;
    robj *eleobj, *dstset = NULL;
    int64_t intobj;
    void *replylen = NULL;
    PORT_ULONG j, cardinality = 0;
    int encoding;
	//遍历集合数组
    for (j = 0; j < setnum; j++) {
		//如果dstkey为空,则是SINTER命令,不为空则是SINTERSTORE命令
		//如果是SINTER命令,则以读操作取出集合对象,否则以写操作取出集合对象
        robj *setobj = dstkey ?
            lookupKeyWrite(c->db,setkeys[j]) :
            lookupKeyRead(c->db,setkeys[j]);
		//取出的集合对象不存在,则执行清理操作
        if (!setobj) {
            zfree(sets);    //释放结合数组空间
            if (dstkey) {
				//从数据库中删除存储的目标集合对象dstkey
                if (dbDelete(c->db,dstkey)) {
					//发送信号
                    signalModifiedKey(c->db,dstkey);
                    server.dirty++;
                }
                addReply(c,shared.czero);
			//如果是SINTER命令,发送空回复
            } else {
                addReply(c,shared.emptymultibulk);
            }
            return;
        }
		// 读取集合对象成功，检查其数据类型
        if (checkType(c,setobj,REDIS_SET)) {
            zfree(sets);
            return;
        }
		// 将读取出的对象保存在集合数组中
        sets[j] = setobj;
    }
	// 从小到大排序集合数组中的集合大小，能够提高算法的性能
    qsort(sets,setnum,sizeof(robj*),qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
	// 首先我们应该输出集合中元素的数量，但是现在不知道交集的大小
	// 因此创建一个空对象的链表，然后保存所有的回复
    if (!dstkey) {
        replylen = addDeferredMultiBulkLength(c);   // STINER命令创建一个链表
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        dstset = createIntsetObject();    //STINERSTORE命令创建要给整数集合对象
    }

	// 迭代第一个也是集合元素数量最小的集合的每一个元素，将该集合中的所有元素和其他集合作比较
	// 如果至少有一个集合不包括该元素，则该元素不属于交集
    si = setTypeInitIterator(sets[0]);
	// 创建集合类型的迭代器并迭代集合数组中的第一个集合的所有元素
    while((encoding = setTypeNext(si,&eleobj,&intobj)) != -1) {
		// 遍历其他集合
        for (j = 1; j < setnum; j++) {
			// 跳过与第一个集合相等的集合，没有必要比较两个相同集合的元素，而且第一个集合作为结果的交集
            if (sets[j] == sets[0]) continue;
			// 当前元素为INTSET类型
            if (encoding == REDIS_ENCODING_INTSET) {
				// 如果在当前intset集合中没有找到该元素则直接跳过当前元素，操作下一个元素
                if (sets[j]->encoding == REDIS_ENCODING_INTSET &&
                    !intsetFind((intset*)sets[j]->ptr,intobj))
                {
                    break;
				// 在字典中查找
                } else if (sets[j]->encoding == REDIS_ENCODING_HT) {
                    eleobj = createStringObjectFromLongLong(intobj);   // 创建字符串对象
					// 如果当前元素不是当前集合中的元素，则释放字符串对象跳过for循环体，操作下一个元素
                    if (!setTypeIsMember(sets[j],eleobj)) {
                        decrRefCount(eleobj);
                        break;
                    }
                    decrRefCount(eleobj);
                }
			// 当前元素为HT字典类型
            } else if (encoding == REDIS_ENCODING_HT) {
				// 当前元素的编码是int类型且当前集合为整数集合，如果该集合不包含该元素，则跳过循环
                if (eleobj->encoding == REDIS_ENCODING_INT &&
                    sets[j]->encoding == REDIS_ENCODING_INTSET &&
                    !intsetFind((intset*)sets[j]->ptr,(PORT_LONG)eleobj->ptr))
                {
                    break;
				// 其他类型，在当前集合中查找该元素是否存在
                } else if (!setTypeIsMember(sets[j],eleobj)) {
                    break;
                }
            }
        }

		// 执行到这里，该元素为结果集合中的元素
        if (j == setnum) {
			// 如果是SINTER命令，回复集合
            if (!dstkey) {
                if (encoding == REDIS_ENCODING_HT)
                    addReplyBulk(c,eleobj);
                else
                    addReplyBulkLongLong(c,intobj);
                cardinality++;
			// 如果是SINTERSTORE命令，先将结果添加到集合中，因为还要store到数据库中
            } else {
                if (encoding == REDIS_ENCODING_INTSET) {
                    eleobj = createStringObjectFromLongLong(intobj);
                    setTypeAdd(dstset,eleobj);
                    decrRefCount(eleobj);
                } else {
                    setTypeAdd(dstset,eleobj);
                }
            }
        }
    }
    setTypeReleaseIterator(si);//释放迭代器
	// SINTERSTORE命令，要将结果的集合添加到数据库中
    if (dstkey) {
		// 如果之前存在该集合则先删除
        int deleted = dbDelete(c->db,dstkey);
		// 结果集大小非空，则将其添加到数据库中
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db,dstkey,dstset);
			// 回复结果集的大小
            addReplyLongLong(c,setTypeSize(dstset));
			// 发送"sinterstore"事件通知
            notifyKeyspaceEvent(REDIS_NOTIFY_SET,"sinterstore",
                dstkey,c->db->id);
		// 结果集为空，释放空间
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);    // 发送0给client
			// 发送"del"事件通知
            if (deleted)
                notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",
                    dstkey,c->db->id);
        }
		// 键被修改，发送信号。更新脏键
        signalModifiedKey(c->db,dstkey);
        server.dirty++;
	// SINTER命令，回复结果集合给client
    } else {
        setDeferredMultiBulkLength(c,replylen,cardinality);
    }
    zfree(sets);//释放集合数组空间
}

void sinterCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+1,c->argc-1,NULL);
}

void sinterstoreCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+2,c->argc-2,c->argv[1]);
}
 
#define REDIS_OP_UNION 0   //并集
#define REDIS_OP_DIFF 1    //差集
#define REDIS_OP_INTER 2   //交集
/*并集差集的底层实现,计算差集的算法*/
void sunionDiffGenericCommand(redisClient *c, robj **setkeys, int setnum, robj *dstkey, int op) {
    //分配集合数组空间
	robj **sets = zmalloc(sizeof(robj*)*setnum);
    setTypeIterator *si;
    robj *ele, *dstset = NULL;
    int j, cardinality = 0;
    int diff_algo = 1;
	//遍历数组中集合键对象
    for (j = 0; j < setnum; j++) {
		// 如果dstkey为空，则是SUNION或SDIFF命令，不为空则是SUNIONSTORE或SDIFFSTORE命令
		// 如果是SUNION或SDIFF命令，则以读操作读取出集合对象，否则以写操作读取出集合对象
        robj *setobj = dstkey ?
            lookupKeyWrite(c->db,setkeys[j]) :
            lookupKeyRead(c->db,setkeys[j]);
		// 不存在的集合键设置为空
        if (!setobj) {
            sets[j] = NULL;
            continue;
        }
		// 检查存在的集合键是否是集合对象，不是则释放空间
        if (checkType(c,setobj,REDIS_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;//保存到集合数组中
    }

	// 计算差集共有两种算法
	// 1.时间复杂度O(N*M)，N是第一个集合中元素的总个数，M是集合的总个数
	// 2.时间复杂度O(N)，N是所有集合中元素的总个数
    if (op == REDIS_OP_DIFF && sets[0]) {
        PORT_LONGLONG algo_one_work = 0, algo_two_work = 0;
		// 遍历集合数组
        for (j = 0; j < setnum; j++) {
            if (sets[j] == NULL) continue;
			// 计算sets[0] × setnum的值
            algo_one_work += setTypeSize(sets[0]);
			// 计算所有集合的元素总个数
            algo_two_work += setTypeSize(sets[j]);
        }

        /* Algorithm 1 has better constant times and performs less operations
         * if there are elements in common. Give it some advantage. */
        algo_one_work /= 2;
		//根据algo_one_work和algo_two_work选择不同算法
        diff_algo = (algo_one_work <= algo_two_work) ? 1 : 2;
		// 如果是算法1，M较小，执行操作少
        if (diff_algo == 1 && setnum > 1) {
			// 将集合数组除第一个集合以外的所有集合，按照集合的元素排序
            qsort(sets+1,setnum-1,sizeof(robj*),
                qsortCompareSetsByRevCardinality);
        }
    }

	// 创建一个临时集合对象作为结果集
    dstset = createIntsetObject();
	// 执行并集操作
    if (op == REDIS_OP_UNION) {
		// 仅仅将每一个集合中的每一个元素加入到结果集中,遍历每一个集合
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets */
			// 创建一个集合类型的迭代器
            si = setTypeInitIterator(sets[j]);
			// 遍历当前集合中的所有元素
            while((ele = setTypeNextObject(si)) != NULL) {
				// 讲迭代器指向的当前元素对象加入到结果集中,如果结果集中不存在新加入的元素，则更新结果集的元素个数计数器
                if (setTypeAdd(dstset,ele)) cardinality++;
                decrRefCount(ele);  //否则直接释放元素对象空间
            }
            setTypeReleaseIterator(si);//释放迭代器空间
        }
	// 执行差集操作并且使用算法1
    } else if (op == REDIS_OP_DIFF && sets[0] && diff_algo == 1) {
		// 我们执行差集操作通过遍历第一个集合中的所有元素，并且将其他集合中不存在元素加入到结果集中
		// 时间复杂度O(N*M)，N是第一个集合中元素的总个数，M是集合的总个数
        si = setTypeInitIterator(sets[0]);
		// 创建集合类型迭代器遍历第一个集合中的所有元素
        while((ele = setTypeNextObject(si)) != NULL) {
			// 遍历集合数组中的除了第一个的所有集合，检查元素是否存在在每一个集合
            for (j = 1; j < setnum; j++) {
                if (!sets[j]) continue; //集合键不存在跳过本次循环
                if (sets[j] == sets[0]) break; //相同的集合没必要比较
                if (setTypeIsMember(sets[j],ele)) break;  //如果元素存在后面的集合中，遍历下一个元素
            }
			// 执行到这里，说明当前元素不存在于 除了第一个的所有集合
            if (j == setnum) {
				// 因此将当前元素添加到结果集合中，更新计数器
                setTypeAdd(dstset,ele);
                cardinality++;
            }
            decrRefCount(ele);    //释放元素对象空间
        }
        setTypeReleaseIterator(si); //释放迭代器空间
	// 执行差集操作并且使用算法2
    } else if (op == REDIS_OP_DIFF && sets[0] && diff_algo == 2) {
		// 将第一个集合的所有元素加入到结果集中，然后遍历其后的所有集合，将有交集的元素从结果集中删除
		// 2.时间复杂度O(N)，N是所有集合中元素的总个数
		// 遍历所有的集合
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets */
			// 创建集合类型迭代器遍历每一个集合中的所有元素
            si = setTypeInitIterator(sets[j]);
            while((ele = setTypeNextObject(si)) != NULL) {
				// 如果是第一个集合，将每一个元素加入到结果集中
                if (j == 0) {
                    if (setTypeAdd(dstset,ele)) cardinality++;
				// 如果是其后的集合，将当前元素从结果集中删除，如结果集中有的话
                } else {
                    if (setTypeRemove(dstset,ele)) cardinality--;
                }
                decrRefCount(ele);
            }
            setTypeReleaseIterator(si); //释放迭代器空间

			// 只要结果集为空，那么差集结果就为空，不用比较后续的集合
            if (cardinality == 0) break;
        }
    }

	// 如果不是STORE一类的命令，输出所有的结果
    if (!dstkey) {
		// 发送结果集的元素个数给client
        addReplyMultiBulkLen(c,cardinality);
		// 遍历结果集中的每一个元素，并发送给client
        si = setTypeInitIterator(dstset);
        while((ele = setTypeNextObject(si)) != NULL) {
            addReplyBulk(c,ele);
            decrRefCount(ele);//发送完要释放空间
        }
        setTypeReleaseIterator(si); //释放迭代器
        decrRefCount(dstset);  //发送集合后要释放结果集的空间
	// STORE一类的命令，输出所有的结果
    } else {
		// 先将目标集合从数据库中删除，如果存在的话
        int deleted = dbDelete(c->db,dstkey);
		// 如果结果集合非空
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db,dstkey,dstset);//将结果集加入到数据库中
            addReplyLongLong(c,setTypeSize(dstset));//发送结果集的元素个数给client
			// 发送对应的事件通知
            notifyKeyspaceEvent(REDIS_NOTIFY_SET,
                op == REDIS_OP_UNION ? "sunionstore" : "sdiffstore",
                dstkey,c->db->id);
		// 结果集为空，则释放空间
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);//发送0给client
			// 发送"del"事件通知
            if (deleted)
                notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",
                    dstkey,c->db->id);
        }
		// 键被修改，发送信号通知，更新脏键
        signalModifiedKey(c->db,dstkey);
        server.dirty++;
    }
    zfree(sets);//释放集合数组空间
}

void sunionCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_UNION);
}

void sunionstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_UNION);
}

void sdiffCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_DIFF);
}

void sdiffstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_DIFF);
}

void sscanCommand(redisClient *c) {
    robj *set;
    PORT_ULONG cursor;

    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == REDIS_ERR) return;
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,set,REDIS_SET)) return;
    scanGenericCommand(c,set,cursor);
}
