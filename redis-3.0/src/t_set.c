#include "redis.h"

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/

void sunionDiffGenericCommand(redisClient *c, robj **setkeys, int setnum, robj *dstkey, int op);

/* ����һ������value�ļ��� */
robj *setTypeCreate(robj *value) {
	//setType�ܹ��������� intset���� dict,intset�������
    if (isObjectRepresentableAsLongLong(value,NULL) == REDIS_OK)
        return createIntsetObject();
    return createSetObject();
}
//��subTypeAdd���������value,��ӳɹ�����1ʧ�ܷ���0 
int setTypeAdd(robj *subject, robj *value) {
    PORT_LONGLONG llval;
	//�����������Ԫ��
    if (subject->encoding == REDIS_ENCODING_HT) {
		//��Ϊhash�������ڼ������¼�value �¼�
        if (dictAdd(subject->ptr,value,NULL) == DICT_OK) {
            incrRefCount(value);
            return 1;
        }
	//������Ϊintset ��ô��valueת��Ϊ����
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value,&llval) == REDIS_OK) {
            uint8_t success = 0;
            subject->ptr = intsetAdd(subject->ptr,llval,&success);
			//��llval���뵽ptr��ָ��intset���ݽṹ��
            if (success) {
				//��ӳɹ����򿴿���û�г������ֵ
                if (intsetLen(subject->ptr) > server.set_max_intset_entries)
                    setTypeConvert(subject,REDIS_ENCODING_HT);
                return 1;
            }
        } else {
			//������ת��Ϊ���ͣ���ô��subjectת��dict
			//Ȼ�����value - null ��ֵ��
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
// �Ӽ��϶�����ɾ��һ��ֵΪvalue��Ԫ�أ�ɾ���ɹ�����1��ʧ�ܷ���0
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
// �������Ƿ����ֵΪvalue��Ԫ�أ����ڷ���1�����򷵻�0
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
// ��������ʼ��һ���������͵ĵ�����
setTypeIterator *setTypeInitIterator(robj *subject) {
	//����ռ�,��ʼ����Ա
    setTypeIterator *si = zmalloc(sizeof(setTypeIterator));
    si->subject = subject;
    si->encoding = subject->encoding;
	//��ʼ���ֵ������
    if (si->encoding == REDIS_ENCODING_HT) {
        si->di = dictGetIterator(subject->ptr);
	//��ʼ�����ϵ�����
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        si->ii = 0;
    } else {
        redisPanic("Unknown set encoding");
    }
    return si;
}
// �ͷŵ������ռ�
void setTypeReleaseIterator(setTypeIterator *si) {
	//������ֵ�,��Ҫ���ֵ����͵ĵ�����
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
// ����ǰ������ָ���Ԫ�ر�����objele��llele�У�������Ϸ��� - 1
// ���صĶ�������ü��������ӣ�֧�� ��ʱ����дʱ����
int setTypeNext(setTypeIterator *si, robj **objele, int64_t *llele) {
	//�����ֵ�
    if (si->encoding == REDIS_ENCODING_HT) {
        dictEntry *de = dictNext(si->di);
        if (de == NULL) return -1;
        *objele = dictGetKey(de);
	//������������
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
		//��intset�е�Ԫ�ر��浽llele��
        if (!intsetGet(si->subject->ptr,si->ii++,llele))
            return -1;
    }
    return si->encoding;   //���ر�������
}

/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new objects
 * or incrementing the ref count of returned objects. So if you don't
 * retain a pointer to this object you should call decrRefCount() against it.
 *
 * This function is the way to go for write operations where COW is not
 * an issue as the result will be anyway of incrementing the ref count. */
// ���ص�������ǰָ���Ԫ�ض���ĵ�ַ����Ҫ�ֶ��ͷŷ��صĶ���
robj *setTypeNextObject(setTypeIterator *si) {
    int64_t intele;
    robj *objele;
    int encoding;
	//�õ���ǰ���ϵı�������
    encoding = setTypeNext(si,&objele,&intele);
    switch(encoding) {
        case -1:    return NULL;       //�������
        case REDIS_ENCODING_INTSET:    //�������Ϸ���һ���ַ������͵Ķ���
            return createStringObjectFromLongLong(intele);
        case REDIS_ENCODING_HT:        //�ֵ伯��,����һ������ĸö���
            incrRefCount(objele);
            return objele;
        default:
            redisPanic("Unsupported encoding");
    }
    return NULL; /* just to suppress warnings */
}

// �Ӽ��������ȡ��һ�����󣬱����ڲ�����
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
// ���ؼ��ϵ�Ԫ������
PORT_ULONG setTypeSize(robj *subject) {
    if (subject->encoding == REDIS_ENCODING_HT) {
        return (PORT_ULONG) dictSize((dict*)subject->ptr);                     WIN_PORT_FIX /* cast (PORT_ULONG) */
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        return intsetLen((intset*)subject->ptr);
    } else {
        redisPanic("Unknown set encoding");
    }
}

/*ת�����ϵı�������Ϊenc */
void setTypeConvert(robj *setobj, int enc) {
    setTypeIterator *si;
    redisAssertWithInfo(NULL,setobj,setobj->type == REDIS_SET &&
                             setobj->encoding == REDIS_ENCODING_INTSET);
	//ת��ΪREDIS_ENCODING_HT�ֵ����ͱ���
    if (enc == REDIS_ENCODING_HT) {
        int64_t intele;
		//����һ���ֵ�
        dict *d = dictCreate(&setDictType,NULL);
        robj *element;

        /* ��չ�ֵ��С */
        dictExpand(d,intsetLen(setobj->ptr));

        /* ��������ʼ��һ���������͵ĵ����� */
        si = setTypeInitIterator(setobj);
		//������������
        while (setTypeNext(si,NULL,&intele) != -1) {
            element = createStringObjectFromLongLong(intele);
            redisAssertWithInfo(NULL,element,dictAdd(d,element,NULL) == DICT_OK);
        }
		//�ͷŵ������ռ�
        setTypeReleaseIterator(si);
		//����ת���󼯺϶���ı�������
        setobj->encoding = REDIS_ENCODING_HT;
		//���¼��϶����ֵ����
        zfree(setobj->ptr);
        setobj->ptr = d;
    } else {
        redisPanic("Unsupported set conversion");
    }
}

void saddCommand(redisClient *c) {
    robj *set;
    int j, added = 0;
	//��д������ȡvalue
    set = lookupKeyWrite(c->db,c->argv[1]);
	//���setΪ��
    if (set == NULL) {
		//�½�һ������
        set = setTypeCreate(c->argv[2]);
        dbAdd(c->db,c->argv[1],set); //��ӵ����ݿ�
    } else {
		//���set����REDIS_SET����,�ظ����ʹ���
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }
	//��������
    for (j = 2; j < c->argc; j++) {
        c->argv[j] = tryObjectEncoding(c->argv[j]);
        if (setTypeAdd(set,c->argv[j])) added++;
    }
	//����֪ͨ
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
	//��ȡvalue�������
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,REDIS_SET)) return;
	//��������,ɾ��
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
	//���ɾ���ɹ�,����֪ͨ
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

    /* source key�����ڷ���0 */
    if (srcset == NULL) {
        addReply(c,shared.czero);
        return;
    }

    /* ������� */
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

/*SINTER,SINTERSTORE(�����Ͳ����ĵײ�ʵ��)һ������ĵײ�ʵ��*/
void sinterGenericCommand(redisClient *c, robj **setkeys, PORT_ULONG setnum, robj *dstkey) {
    //����洢���ϵ�����
	robj **sets = zmalloc(sizeof(robj*)*setnum);
    setTypeIterator *si;
    robj *eleobj, *dstset = NULL;
    int64_t intobj;
    void *replylen = NULL;
    PORT_ULONG j, cardinality = 0;
    int encoding;
	//������������
    for (j = 0; j < setnum; j++) {
		//���dstkeyΪ��,����SINTER����,��Ϊ������SINTERSTORE����
		//�����SINTER����,���Զ�����ȡ�����϶���,������д����ȡ�����϶���
        robj *setobj = dstkey ?
            lookupKeyWrite(c->db,setkeys[j]) :
            lookupKeyRead(c->db,setkeys[j]);
		//ȡ���ļ��϶��󲻴���,��ִ���������
        if (!setobj) {
            zfree(sets);    //�ͷŽ������ռ�
            if (dstkey) {
				//�����ݿ���ɾ���洢��Ŀ�꼯�϶���dstkey
                if (dbDelete(c->db,dstkey)) {
					//�����ź�
                    signalModifiedKey(c->db,dstkey);
                    server.dirty++;
                }
                addReply(c,shared.czero);
			//�����SINTER����,���Ϳջظ�
            } else {
                addReply(c,shared.emptymultibulk);
            }
            return;
        }
		// ��ȡ���϶���ɹ����������������
        if (checkType(c,setobj,REDIS_SET)) {
            zfree(sets);
            return;
        }
		// ����ȡ���Ķ��󱣴��ڼ���������
        sets[j] = setobj;
    }
	// ��С�������򼯺������еļ��ϴ�С���ܹ�����㷨������
    qsort(sets,setnum,sizeof(robj*),qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
	// ��������Ӧ�����������Ԫ�ص��������������ڲ�֪�������Ĵ�С
	// ��˴���һ���ն��������Ȼ�󱣴����еĻظ�
    if (!dstkey) {
        replylen = addDeferredMultiBulkLength(c);   // STINER�����һ������
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        dstset = createIntsetObject();    //STINERSTORE�����Ҫ���������϶���
    }

	// ������һ��Ҳ�Ǽ���Ԫ��������С�ļ��ϵ�ÿһ��Ԫ�أ����ü����е�����Ԫ�غ������������Ƚ�
	// ���������һ�����ϲ�������Ԫ�أ����Ԫ�ز����ڽ���
    si = setTypeInitIterator(sets[0]);
	// �����������͵ĵ��������������������еĵ�һ�����ϵ�����Ԫ��
    while((encoding = setTypeNext(si,&eleobj,&intobj)) != -1) {
		// ������������
        for (j = 1; j < setnum; j++) {
			// �������һ��������ȵļ��ϣ�û�б�Ҫ�Ƚ�������ͬ���ϵ�Ԫ�أ����ҵ�һ��������Ϊ����Ľ���
            if (sets[j] == sets[0]) continue;
			// ��ǰԪ��ΪINTSET����
            if (encoding == REDIS_ENCODING_INTSET) {
				// ����ڵ�ǰintset������û���ҵ���Ԫ����ֱ��������ǰԪ�أ�������һ��Ԫ��
                if (sets[j]->encoding == REDIS_ENCODING_INTSET &&
                    !intsetFind((intset*)sets[j]->ptr,intobj))
                {
                    break;
				// ���ֵ��в���
                } else if (sets[j]->encoding == REDIS_ENCODING_HT) {
                    eleobj = createStringObjectFromLongLong(intobj);   // �����ַ�������
					// �����ǰԪ�ز��ǵ�ǰ�����е�Ԫ�أ����ͷ��ַ�����������forѭ���壬������һ��Ԫ��
                    if (!setTypeIsMember(sets[j],eleobj)) {
                        decrRefCount(eleobj);
                        break;
                    }
                    decrRefCount(eleobj);
                }
			// ��ǰԪ��ΪHT�ֵ�����
            } else if (encoding == REDIS_ENCODING_HT) {
				// ��ǰԪ�صı�����int�����ҵ�ǰ����Ϊ�������ϣ�����ü��ϲ�������Ԫ�أ�������ѭ��
                if (eleobj->encoding == REDIS_ENCODING_INT &&
                    sets[j]->encoding == REDIS_ENCODING_INTSET &&
                    !intsetFind((intset*)sets[j]->ptr,(PORT_LONG)eleobj->ptr))
                {
                    break;
				// �������ͣ��ڵ�ǰ�����в��Ҹ�Ԫ���Ƿ����
                } else if (!setTypeIsMember(sets[j],eleobj)) {
                    break;
                }
            }
        }

		// ִ�е������Ԫ��Ϊ��������е�Ԫ��
        if (j == setnum) {
			// �����SINTER����ظ�����
            if (!dstkey) {
                if (encoding == REDIS_ENCODING_HT)
                    addReplyBulk(c,eleobj);
                else
                    addReplyBulkLongLong(c,intobj);
                cardinality++;
			// �����SINTERSTORE����Ƚ������ӵ������У���Ϊ��Ҫstore�����ݿ���
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
    setTypeReleaseIterator(si);//�ͷŵ�����
	// SINTERSTORE���Ҫ������ļ�����ӵ����ݿ���
    if (dstkey) {
		// ���֮ǰ���ڸü�������ɾ��
        int deleted = dbDelete(c->db,dstkey);
		// �������С�ǿգ�������ӵ����ݿ���
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db,dstkey,dstset);
			// �ظ�������Ĵ�С
            addReplyLongLong(c,setTypeSize(dstset));
			// ����"sinterstore"�¼�֪ͨ
            notifyKeyspaceEvent(REDIS_NOTIFY_SET,"sinterstore",
                dstkey,c->db->id);
		// �����Ϊ�գ��ͷſռ�
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);    // ����0��client
			// ����"del"�¼�֪ͨ
            if (deleted)
                notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",
                    dstkey,c->db->id);
        }
		// �����޸ģ������źš��������
        signalModifiedKey(c->db,dstkey);
        server.dirty++;
	// SINTER����ظ�������ϸ�client
    } else {
        setDeferredMultiBulkLength(c,replylen,cardinality);
    }
    zfree(sets);//�ͷż�������ռ�
}

void sinterCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+1,c->argc-1,NULL);
}

void sinterstoreCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+2,c->argc-2,c->argv[1]);
}
 
#define REDIS_OP_UNION 0   //����
#define REDIS_OP_DIFF 1    //�
#define REDIS_OP_INTER 2   //����
/*������ĵײ�ʵ��,�������㷨*/
void sunionDiffGenericCommand(redisClient *c, robj **setkeys, int setnum, robj *dstkey, int op) {
    //���伯������ռ�
	robj **sets = zmalloc(sizeof(robj*)*setnum);
    setTypeIterator *si;
    robj *ele, *dstset = NULL;
    int j, cardinality = 0;
    int diff_algo = 1;
	//���������м��ϼ�����
    for (j = 0; j < setnum; j++) {
		// ���dstkeyΪ�գ�����SUNION��SDIFF�����Ϊ������SUNIONSTORE��SDIFFSTORE����
		// �����SUNION��SDIFF������Զ�������ȡ�����϶��󣬷�����д������ȡ�����϶���
        robj *setobj = dstkey ?
            lookupKeyWrite(c->db,setkeys[j]) :
            lookupKeyRead(c->db,setkeys[j]);
		// �����ڵļ��ϼ�����Ϊ��
        if (!setobj) {
            sets[j] = NULL;
            continue;
        }
		// �����ڵļ��ϼ��Ƿ��Ǽ��϶��󣬲������ͷſռ�
        if (checkType(c,setobj,REDIS_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;//���浽����������
    }

	// �������������㷨
	// 1.ʱ�临�Ӷ�O(N*M)��N�ǵ�һ��������Ԫ�ص��ܸ�����M�Ǽ��ϵ��ܸ���
	// 2.ʱ�临�Ӷ�O(N)��N�����м�����Ԫ�ص��ܸ���
    if (op == REDIS_OP_DIFF && sets[0]) {
        PORT_LONGLONG algo_one_work = 0, algo_two_work = 0;
		// ������������
        for (j = 0; j < setnum; j++) {
            if (sets[j] == NULL) continue;
			// ����sets[0] �� setnum��ֵ
            algo_one_work += setTypeSize(sets[0]);
			// �������м��ϵ�Ԫ���ܸ���
            algo_two_work += setTypeSize(sets[j]);
        }

        /* Algorithm 1 has better constant times and performs less operations
         * if there are elements in common. Give it some advantage. */
        algo_one_work /= 2;
		//����algo_one_work��algo_two_workѡ��ͬ�㷨
        diff_algo = (algo_one_work <= algo_two_work) ? 1 : 2;
		// ������㷨1��M��С��ִ�в�����
        if (diff_algo == 1 && setnum > 1) {
			// �������������һ��������������м��ϣ����ռ��ϵ�Ԫ������
            qsort(sets+1,setnum-1,sizeof(robj*),
                qsortCompareSetsByRevCardinality);
        }
    }

	// ����һ����ʱ���϶�����Ϊ�����
    dstset = createIntsetObject();
	// ִ�в�������
    if (op == REDIS_OP_UNION) {
		// ������ÿһ�������е�ÿһ��Ԫ�ؼ��뵽�������,����ÿһ������
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets */
			// ����һ���������͵ĵ�����
            si = setTypeInitIterator(sets[j]);
			// ������ǰ�����е�����Ԫ��
            while((ele = setTypeNextObject(si)) != NULL) {
				// ��������ָ��ĵ�ǰԪ�ض�����뵽�������,���������в������¼����Ԫ�أ�����½������Ԫ�ظ���������
                if (setTypeAdd(dstset,ele)) cardinality++;
                decrRefCount(ele);  //����ֱ���ͷ�Ԫ�ض���ռ�
            }
            setTypeReleaseIterator(si);//�ͷŵ������ռ�
        }
	// ִ�в��������ʹ���㷨1
    } else if (op == REDIS_OP_DIFF && sets[0] && diff_algo == 1) {
		// ����ִ�в����ͨ��������һ�������е�����Ԫ�أ����ҽ����������в�����Ԫ�ؼ��뵽�������
		// ʱ�临�Ӷ�O(N*M)��N�ǵ�һ��������Ԫ�ص��ܸ�����M�Ǽ��ϵ��ܸ���
        si = setTypeInitIterator(sets[0]);
		// �����������͵�����������һ�������е�����Ԫ��
        while((ele = setTypeNextObject(si)) != NULL) {
			// �������������еĳ��˵�һ�������м��ϣ����Ԫ���Ƿ������ÿһ������
            for (j = 1; j < setnum; j++) {
                if (!sets[j]) continue; //���ϼ���������������ѭ��
                if (sets[j] == sets[0]) break; //��ͬ�ļ���û��Ҫ�Ƚ�
                if (setTypeIsMember(sets[j],ele)) break;  //���Ԫ�ش��ں���ļ����У�������һ��Ԫ��
            }
			// ִ�е����˵����ǰԪ�ز������� ���˵�һ�������м���
            if (j == setnum) {
				// ��˽���ǰԪ����ӵ���������У����¼�����
                setTypeAdd(dstset,ele);
                cardinality++;
            }
            decrRefCount(ele);    //�ͷ�Ԫ�ض���ռ�
        }
        setTypeReleaseIterator(si); //�ͷŵ������ռ�
	// ִ�в��������ʹ���㷨2
    } else if (op == REDIS_OP_DIFF && sets[0] && diff_algo == 2) {
		// ����һ�����ϵ�����Ԫ�ؼ��뵽������У�Ȼ������������м��ϣ����н�����Ԫ�شӽ������ɾ��
		// 2.ʱ�临�Ӷ�O(N)��N�����м�����Ԫ�ص��ܸ���
		// �������еļ���
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets */
			// �����������͵���������ÿһ�������е�����Ԫ��
            si = setTypeInitIterator(sets[j]);
            while((ele = setTypeNextObject(si)) != NULL) {
				// ����ǵ�һ�����ϣ���ÿһ��Ԫ�ؼ��뵽�������
                if (j == 0) {
                    if (setTypeAdd(dstset,ele)) cardinality++;
				// ��������ļ��ϣ�����ǰԪ�شӽ������ɾ�������������еĻ�
                } else {
                    if (setTypeRemove(dstset,ele)) cardinality--;
                }
                decrRefCount(ele);
            }
            setTypeReleaseIterator(si); //�ͷŵ������ռ�

			// ֻҪ�����Ϊ�գ���ô������Ϊ�գ����ñȽϺ����ļ���
            if (cardinality == 0) break;
        }
    }

	// �������STOREһ������������еĽ��
    if (!dstkey) {
		// ���ͽ������Ԫ�ظ�����client
        addReplyMultiBulkLen(c,cardinality);
		// ����������е�ÿһ��Ԫ�أ������͸�client
        si = setTypeInitIterator(dstset);
        while((ele = setTypeNextObject(si)) != NULL) {
            addReplyBulk(c,ele);
            decrRefCount(ele);//������Ҫ�ͷſռ�
        }
        setTypeReleaseIterator(si); //�ͷŵ�����
        decrRefCount(dstset);  //���ͼ��Ϻ�Ҫ�ͷŽ�����Ŀռ�
	// STOREһ������������еĽ��
    } else {
		// �Ƚ�Ŀ�꼯�ϴ����ݿ���ɾ����������ڵĻ�
        int deleted = dbDelete(c->db,dstkey);
		// ���������Ϸǿ�
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db,dstkey,dstset);//����������뵽���ݿ���
            addReplyLongLong(c,setTypeSize(dstset));//���ͽ������Ԫ�ظ�����client
			// ���Ͷ�Ӧ���¼�֪ͨ
            notifyKeyspaceEvent(REDIS_NOTIFY_SET,
                op == REDIS_OP_UNION ? "sunionstore" : "sdiffstore",
                dstkey,c->db->id);
		// �����Ϊ�գ����ͷſռ�
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);//����0��client
			// ����"del"�¼�֪ͨ
            if (deleted)
                notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",
                    dstkey,c->db->id);
        }
		// �����޸ģ������ź�֪ͨ���������
        signalModifiedKey(c->db,dstkey);
        server.dirty++;
    }
    zfree(sets);//�ͷż�������ռ�
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
