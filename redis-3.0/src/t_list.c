#include "redis.h"

/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/

/* Check the argument length to see if it requires us to convert the ziplist
 * to a real list. Only check raw-encoded objects because integer encoded
 * objects are never too long. */
//���Խ�ziplistת��Ϊlist
void listTypeTryConversion(robj *subject, robj *value) {
    if (subject->encoding != REDIS_ENCODING_ZIPLIST) return;
    if (sdsEncodedObject(value) &&
        sdslen(value->ptr) > server.list_max_ziplist_value)
            listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);  //ִ��ת���ĺ���
}

/*��where������һ��value,push����ĵײ�ʵ��*/
void listTypePush(robj *subject, robj *value, int where) {
    /* ����Ƿ���Ҫת������ */
    listTypeTryConversion(subject,value);
	// list_max_ziplist_entries��Ĭ��ֵΪ512�����ziplist�д�ŵĽڵ���������ֵҲ��Ҫת������
    if (subject->encoding == REDIS_ENCODING_ZIPLIST &&
        ziplistLen(subject->ptr) >= server.list_max_ziplist_entries)
            listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);
	//ziplist������
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
		//ȷ��Ԫ�ز���ͷ������β��
        int pos = (where == REDIS_HEAD) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
        value = getDecodedObject(value);
		//����ziplist���ڲ�ʵ�ֺ�����ʵ�ֲ������
        subject->ptr = ziplistPush(subject->ptr,value->ptr,(unsigned int)sdslen(value->ptr),pos); WIN_PORT_FIX /* cast (unsigned int) */
        decrRefCount(value);
	//linked list����
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
		//ֱ�ӵ���adlist���ڲ�ʵ�ֺ�������Ԫ��
        if (where == REDIS_HEAD) {
            listAddNodeHead(subject->ptr,value);
        } else {
            listAddNodeTail(subject->ptr,value);
        }
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }
}

//�б����͵Ĵ�where����һ��value��POP����ײ�ʵ��
robj *listTypePop(robj *subject, int where) {
    robj *value = NULL;
	//ziplist����
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p;
        unsigned char *vstr;
        unsigned int vlen;
        PORT_LONGLONG vlong;
        int pos = (where == REDIS_HEAD) ? 0 : -1;
		//��ȡposλ�õ�Ԫ��
        p = ziplistIndex(subject->ptr,pos);
		//���������ݷֱ�ŵ�������
        if (ziplistGet(p,&vstr,&vlen,&vlong)) {
            if (vstr) {
				//�����ַ�������
                value = createStringObject((char*)vstr,vlen);
            } else {
				//���������������ַ�������
                value = createStringObjectFromLongLong(vlong);
            }
            /* We only need to delete an element when it exists */
            subject->ptr = ziplistDelete(subject->ptr,&p);
        }
	//linkedlist����
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
        list *list = subject->ptr;
        listNode *ln;
        if (where == REDIS_HEAD) {
            ln = listFirst(list);
        } else {
            ln = listLast(list);
        }
        if (ln != NULL) {
            value = listNodeValue(ln);
            incrRefCount(value);
            listDelNode(list,ln);
        }
    } else {
        redisPanic("Unknown list encoding");
    }
    return value;
}

//���ض���ĳ���,entry�ڵ�ĸ���
PORT_ULONG listTypeLength(robj *subject) {
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        return ziplistLen(subject->ptr);
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
        return listLength((list*)subject->ptr);
    } else {
        redisPanic("Unknown list encoding");
    }
}

/* ��ʼ���б����͵ĵ�����Ϊһ��ָ�����±� */
listTypeIterator *listTypeInitIterator(robj *subject, PORT_LONG index, unsigned char direction) {
    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));
    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        li->zi = ziplistIndex(subject->ptr,(int)index);                         WIN_PORT_FIX /* cast (int) */
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        li->ln = listIndex(subject->ptr,index);
    } else {
        redisPanic("Unknown list encoding");
    }
    return li;
}

/* �ͷŵ������ռ� */
void listTypeReleaseIterator(listTypeIterator *li) {
    zfree(li);
}

// ���б����͵ĵ�����ָ���entry�������ṩ��listTypeEntry�ṹ�У����Ҹ��µ�������1��ʾ�ɹ���0ʧ��
int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    /* Protect from converting when iterating */
    redisAssert(li->subject->encoding == li->encoding);

    entry->li = li;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        entry->zi = li->zi;
        if (entry->zi != NULL) {
            if (li->direction == REDIS_TAIL)
                li->zi = ziplistNext(li->subject->ptr,li->zi);
            else
                li->zi = ziplistPrev(li->subject->ptr,li->zi);
            return 1;
        }
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        entry->ln = li->ln;
        if (entry->ln != NULL) {
            if (li->direction == REDIS_TAIL)
                li->ln = li->ln->next;
            else
                li->ln = li->ln->prev;
            return 1;
        }
    } else {
        redisPanic("Unknown list encoding");
    }
    return 0;
}

// ����һ���ڵ��value���󣬸��ݵ�ǰ�ĵ�����
robj *listTypeGet(listTypeEntry *entry) {
    listTypeIterator *li = entry->li;
    robj *value = NULL;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *vstr;
        unsigned int vlen;
        PORT_LONGLONG vlong;
        redisAssert(entry->zi != NULL);
        if (ziplistGet(entry->zi,&vstr,&vlen,&vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr,vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
        }
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        redisAssert(entry->ln != NULL);
        value = listNodeValue(entry->ln);
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }
    return value;
}

//��value���뵽whereλ��
void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
    robj *subject = entry->li->subject;
    if (entry->li->encoding == REDIS_ENCODING_ZIPLIST) {
        value = getDecodedObject(value);
        if (where == REDIS_TAIL) {
            unsigned char *next = ziplistNext(subject->ptr,entry->zi);

            /* When we insert after the current element, but the current element
             * is the tail of the list, we need to do a push. */
            if (next == NULL) {
                subject->ptr = ziplistPush(subject->ptr,value->ptr,(unsigned int)sdslen(value->ptr),REDIS_TAIL); WIN_PORT_FIX /* cast (unsigned int) */
            } else {
                subject->ptr = ziplistInsert(subject->ptr,next,value->ptr,(unsigned int)sdslen(value->ptr)); WIN_PORT_FIX /* cast (unsigned int) */
            }
        } else {
            subject->ptr = ziplistInsert(subject->ptr,entry->zi,value->ptr,(unsigned int)sdslen(value->ptr)); WIN_PORT_FIX /* cast (unsigned int) */
        }
        decrRefCount(value);
    } else if (entry->li->encoding == REDIS_ENCODING_LINKEDLIST) {
        if (where == REDIS_TAIL) {
            listInsertNode(subject->ptr,entry->ln,value,AL_START_TAIL);
        } else {
            listInsertNode(subject->ptr,entry->ln,value,AL_START_HEAD);
        }
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }
}

//�Ƚ�list�е�entry������Ĳ���entry�Ƿ����
int listTypeEqual(listTypeEntry *entry, robj *o) {
    listTypeIterator *li = entry->li;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        redisAssertWithInfo(NULL,o,sdsEncodedObject(o));
        return ziplistCompare(entry->zi,o->ptr,(unsigned int)sdslen(o->ptr));   WIN_PORT_FIX /* cast (unsigned int) */
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        return equalStringObjects(o,listNodeValue(entry->ln));
    } else {
        redisPanic("Unknown list encoding");
    }
}

//ɾ��������ָ���entry
void listTypeDelete(listTypeEntry *entry) {
    listTypeIterator *li = entry->li;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p = entry->zi;
        li->subject->ptr = ziplistDelete(li->subject->ptr,&p);

        /* Update position of the iterator depending on the direction */
        if (li->direction == REDIS_TAIL)
            li->zi = p;
        else
            li->zi = ziplistPrev(li->subject->ptr,p);
    } else if (entry->li->encoding == REDIS_ENCODING_LINKEDLIST) {
        listNode *next;
        if (li->direction == REDIS_TAIL)
            next = entry->ln->next;
        else
            next = entry->ln->prev;
        listDelNode(li->subject->ptr,entry->ln);
        li->ln = next;
    } else {
        redisPanic("Unknown list encoding");
    }
}

//ת����������
void listTypeConvert(robj *subject, int enc) {
    listTypeIterator *li;
    listTypeEntry entry;
    redisAssertWithInfo(NULL,subject,subject->type == REDIS_LIST);

    if (enc == REDIS_ENCODING_LINKEDLIST) {
        list *l = listCreate();
        listSetFreeMethod(l,decrRefCountVoid);

        /* listTypeGet returns a robj with incremented refcount */
        li = listTypeInitIterator(subject,0,REDIS_TAIL);
        while (listTypeNext(li,&entry)) listAddNodeTail(l,listTypeGet(&entry));
        listTypeReleaseIterator(li);

        subject->encoding = REDIS_ENCODING_LINKEDLIST;
        zfree(subject->ptr);
        subject->ptr = l;
    } else {
        redisPanic("Unsupported list conversion");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/
/*pushϵ������ĵײ�ʵ��*/
void pushGenericCommand(redisClient *c, int where) {
    int j, waiting = 0, pushed = 0;
	//ȡ���б����
    robj *lobj = lookupKeyWrite(c->db,c->argv[1]);
	//�������
    if (lobj && lobj->type != REDIS_LIST) {
        addReply(c,shared.wrongtypeerr);
        return;
    }
	//��������ֵ���������õ��б���
    for (j = 2; j < c->argc; j++) {
		//���Ա���
        c->argv[j] = tryObjectEncoding(c->argv[j]);
		//����б���󲻴���,�򴴽�һ�������������ݿ�
        if (!lobj) {
            lobj = createZiplistObject();
            dbAdd(c->db,c->argv[1],lobj);
        }
		//��ֵ�����б�
        listTypePush(lobj,c->argv[j],where);
        pushed++;
    }
	//������ӽڵ������
    addReplyLongLong(c, waiting + (lobj ? listTypeLength(lobj) : 0));
	//���������һ������ӳɹ�,��ֱ������Ĵ���
    if (pushed) {
        char *event = (where == REDIS_HEAD) ? "lpush" : "rpush";
		//�����޸��ź�
        signalModifiedKey(c->db,c->argv[1]);
		//�����¼�֪ͨ
        notifyKeyspaceEvent(REDIS_NOTIFY_LIST,event,c->argv[1],c->db->id);
    }
    server.dirty += pushed;
}

/*ʹ��ͷ�巨����������б�*/
void lpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_HEAD);
}
/*ʹ��β�巨����������б�*/
void rpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_TAIL);
}

/*��key����ʱ��push,pushx,insert�ĵײ�ʵ��,��key������ʱʲôҲ����,��pushGenericCommand���key������ʱ�ᴴ��һ���µ�key*/
void pushxGenericCommand(redisClient *c, robj *refval, robj *val, int where) {
    robj *subject;
    listTypeIterator *iter;
    listTypeEntry entry;
    int inserted = 0;
	//�Զ�������ȡkey�����value,�����ȡʧ�ܻ��߶�ȡ��value�������б�����,���ؿ�
    if ((subject = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,subject,REDIS_LIST)) return;
	//Ѱ�һ�׼ֵrefval
    if (refval != NULL) {
        /* We're not sure if this value can be inserted yet, but we cannot
         * convert the list inside the iterator. We don't want to loop over
         * the list twice (once to see if the value can be inserted and once
         * to do the actual insert), so we assume this value can be inserted
         * and convert the ziplist to a regular list if necessary. */
        listTypeTryConversion(subject,val);

        /* �����б������ */
        iter = listTypeInitIterator(subject,0,REDIS_TAIL);
		//��������ָ��Ľڵ㱣�浽����entry�к�ָ����һ���ڵ�
        while (listTypeNext(iter,&entry)) {
			//��ǰ�ڵ����׼ֵ���бȽ��Ƿ����
            if (listTypeEqual(&entry,refval)) {
				//��������where����val����
                listTypeInsert(&entry,val,where);
                inserted = 1;
                break;
            }
        }
		//�ͷŵ�����
        listTypeReleaseIterator(iter);
		//�������ɹ�,��ֵ���޸������źŲ�����linsert�¼�֪ͨ
        if (inserted) {
            /* Check if the length exceeds the ziplist length threshold. */
            if (subject->encoding == REDIS_ENCODING_ZIPLIST &&
                ziplistLen(subject->ptr) > server.list_max_ziplist_entries)
                    listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);
            signalModifiedKey(c->db,c->argv[1]);
            notifyKeyspaceEvent(REDIS_NOTIFY_LIST,"linsert",
                                c->argv[1],c->db->id);
            server.dirty++;
        } else {
            /* Notify client of a failed insert */
			//����ʧ��,���Ͳ���ʧ�ܵ���Ϣ
            addReply(c,shared.cnegone);
            return;
        }
	//�����׼ֵΪ��
    } else {
		//����where�ж��¼�����
        char *event = (where == REDIS_HEAD) ? "lpush" : "rpush";
		//��val��������б��ͷ������β��
        listTypePush(subject,val,where);
		//�����Ķ������ź�
        signalModifiedKey(c->db,c->argv[1]);
		//�����¼�֪ͨ
        notifyKeyspaceEvent(REDIS_NOTIFY_LIST,event,c->argv[1],c->db->id);
        server.dirty++;
    }
	//������val����б�Ԫ�ظ������͸��ͻ���
    addReplyLongLong(c,listTypeLength(subject));
}

/*lpushX:X����key������ʱʲôҲ����*/
void lpushxCommand(redisClient *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxGenericCommand(c,NULL,c->argv[2],REDIS_HEAD);
}
/*rpushX:X����key������ʱʲôҲ����*/
void rpushxCommand(redisClient *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxGenericCommand(c,NULL,c->argv[2],REDIS_TAIL);
}

//�������Ԫ��
void linsertCommand(redisClient *c) {
    c->argv[4] = tryObjectEncoding(c->argv[4]);
    if (strcasecmp(c->argv[2]->ptr,"after") == 0) {
        pushxGenericCommand(c,c->argv[3],c->argv[4],REDIS_TAIL);
    } else if (strcasecmp(c->argv[2]->ptr,"before") == 0) {
        pushxGenericCommand(c,c->argv[3],c->argv[4],REDIS_HEAD);
    } else {
        addReply(c,shared.syntaxerr);
    }
}

//����������
void llenCommand(redisClient *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.czero);
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;
	//������ӽڵ������
    addReplyLongLong(c,listTypeLength(o));
}
//����index�ϵ�Ԫ��
void lindexCommand(redisClient *c) {
	PORT_LONG index;
	robj *value = NULL;
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk);
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;

	//ȡ������ֵ����index
    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != REDIS_OK))
        return;
	//������������ziplist����,ֱ��ָ����λ��(����������:ziplist��linkedlist)
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p;
        unsigned char *vstr;
        unsigned int vlen;
        PORT_LONGLONG vlong;
        p = ziplistIndex(o->ptr,(int) index);                                   WIN_PORT_FIX /* cast (int) */
        if (ziplistGet(p,&vstr,&vlen,&vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr,vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
            addReplyBulk(c,value);
            decrRefCount(value);
        } else {
            addReply(c,shared.nullbulk);
        }
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        listNode *ln = listIndex(o->ptr,index);
        if (ln != NULL) {
            value = listNodeValue(ln);
            addReplyBulk(c,value);
        } else {
            addReply(c,shared.nullbulk);
        }
    } else {
        redisPanic("Unknown list encoding");
    }
}
//���б�indexλ���ϵ�ֵ�����滻
void lsetCommand(redisClient *c) {
	PORT_LONG index;
	robj *value;
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;
    value = (c->argv[3] = tryObjectEncoding(c->argv[3]));

    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != REDIS_OK))
        return;

    listTypeTryConversion(o,value);
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p, *zl = o->ptr;
        p = ziplistIndex(zl, (int) index);                                      WIN_PORT_FIX /* cast (int) */
        if (p == NULL) {
            addReply(c,shared.outofrangeerr);
        } else {
            o->ptr = ziplistDelete(o->ptr,&p);
            value = getDecodedObject(value);
            o->ptr = ziplistInsert(o->ptr,p,value->ptr,(unsigned int)sdslen(value->ptr));
            decrRefCount(value);
            addReply(c,shared.ok);
            signalModifiedKey(c->db,c->argv[1]);
            notifyKeyspaceEvent(REDIS_NOTIFY_LIST,"lset",c->argv[1],c->db->id);
            server.dirty++;
        }
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        listNode *ln = listIndex(o->ptr,(int)index);                            WIN_PORT_FIX /* cast (int) */
        if (ln == NULL) {
            addReply(c,shared.outofrangeerr);
        } else {
            decrRefCount((robj*)listNodeValue(ln));
            listNodeValue(ln) = value;
            incrRefCount(value);
            addReply(c,shared.ok);
            signalModifiedKey(c->db,c->argv[1]);
            notifyKeyspaceEvent(REDIS_NOTIFY_LIST,"lset",c->argv[1],c->db->id);
            server.dirty++;
        }
    } else {
        redisPanic("Unknown list encoding");
    }
}
//���������ͨ�ú���
void popGenericCommand(redisClient *c, int where) {
	robj *value;
	//��д����ȥ�������valueֵ
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk);
    //���keyû�ҵ�����value�������б�������ֱ�ӷ���
	if (o == NULL || checkType(c,o,REDIS_LIST)) return;
	//��where������value
    value = listTypePop(o,where);
	//valueΪ�����Ϳ���Ϣ
    if (value == NULL) {
        addReply(c,shared.nullbulk);
    } else {
		//�����¼�����
        char *event = (where == REDIS_HEAD) ? "lpop" : "rpop";
		//����value��client
        addReplyBulk(c,value);
		//�ͷ�value
        decrRefCount(value);
		//�����¼�֪ͨ
        notifyKeyspaceEvent(REDIS_NOTIFY_LIST,event,c->argv[1],c->db->id);
		//�������һ��Ԫ�غ��б�Ϊ��
        if (listTypeLength(o) == 0) {
			//����del�¼�֪ͨ
            notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",
                                c->argv[1],c->db->id);
			//ɾ��key
            dbDelete(c->db,c->argv[1]);
        }
		//�����ݿ�ļ����Ķ�,�����øú��������ź�
        signalModifiedKey(c->db,c->argv[1]);
        server.dirty++;
    }
}

void lpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_HEAD);
}

void rpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_TAIL);
}
//��������
void lrangeCommand(redisClient *c) {
    robj *o;
    PORT_LONG start, end, llen, rangelen;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
         || checkType(c,o,REDIS_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply */
    addReplyMultiBulkLen(c,rangelen);
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p = ziplistIndex(o->ptr,(int)start);                     WIN_PORT_FIX /* cast (int) */
        unsigned char *vstr;
        unsigned int vlen;
        PORT_LONGLONG vlong;

        while(rangelen--) {
            ziplistGet(p,&vstr,&vlen,&vlong);
            if (vstr) {
                addReplyBulkCBuffer(c,vstr,vlen);
            } else {
                addReplyBulkLongLong(c,vlong);
            }
            p = ziplistNext(o->ptr,p);
        }
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        listNode *ln;

        /* If we are nearest to the end of the list, reach the element
         * starting from tail and going backward, as it is faster. */
        if (start > llen/2) start -= llen;
        ln = listIndex(o->ptr,start);

        while(rangelen--) {
            addReplyBulk(c,ln->value);
            ln = ln->next;
        }
    } else {
        redisPanic("List encoding is not LINKEDLIST nor ZIPLIST!");
    }
}

void ltrimCommand(redisClient *c) {
    robj *o;
    PORT_LONG start, end, llen, j, ltrim, rtrim;
    list *list;
    listNode *ln;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.ok)) == NULL ||
        checkType(c,o,REDIS_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen-1;
        ltrim = start;
        rtrim = llen-end-1;
    }

    /* Remove list elements to perform the trim */
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        o->ptr = ziplistDeleteRange(o->ptr,0,(unsigned int)ltrim);              WIN_PORT_FIX /* cast (unsigned int) */
        o->ptr = ziplistDeleteRange(o->ptr,(unsigned int)-rtrim,(unsigned int)rtrim); WIN_PORT_FIX /* cast (unsigned int) */
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        list = o->ptr;
        for (j = 0; j < ltrim; j++) {
            ln = listFirst(list);
            listDelNode(list,ln);
        }
        for (j = 0; j < rtrim; j++) {
            ln = listLast(list);
            listDelNode(list,ln);
        }
    } else {
        redisPanic("Unknown list encoding");
    }

    notifyKeyspaceEvent(REDIS_NOTIFY_LIST,"ltrim",c->argv[1],c->db->id);
    if (listTypeLength(o) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }
    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
    addReply(c,shared.ok);
}

void lremCommand(redisClient *c) {
	listTypeIterator *li;
    robj *subject, *obj;
    obj = c->argv[3] = tryObjectEncoding(c->argv[3]);
    PORT_LONG toremove;
    PORT_LONG removed = 0;
    listTypeEntry entry;

    if ((getLongFromObjectOrReply(c, c->argv[2], &toremove, NULL) != REDIS_OK))
        return;

    subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero);
    if (subject == NULL || checkType(c,subject,REDIS_LIST)) return;

    /* Make sure obj is raw when we're dealing with a ziplist */
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        obj = getDecodedObject(obj);


    if (toremove < 0) {
        toremove = -toremove;
        li = listTypeInitIterator(subject,-1,REDIS_HEAD);
    } else {
        li = listTypeInitIterator(subject,0,REDIS_TAIL);
    }

    while (listTypeNext(li,&entry)) {
        if (listTypeEqual(&entry,obj)) {
            listTypeDelete(&entry);
            server.dirty++;
            removed++;
            if (toremove && removed == toremove) break;
        }
    }
    listTypeReleaseIterator(li);

    /* Clean up raw encoded object */
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        decrRefCount(obj);

    if (listTypeLength(subject) == 0) dbDelete(c->db,c->argv[1]);
    addReplyLongLong(c,removed);
    if (removed) signalModifiedKey(c->db,c->argv[1]);
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *    IF LLEN(srclist) > 0
 *      element = RPOP srclist
 *      LPUSH dstlist element
 *      RETURN element
 *    ELSE
 *      RETURN nil
 *    END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 */

void rpoplpushHandlePush(redisClient *c, robj *dstkey, robj *dstobj, robj *value) {
    /* Create the list if the key does not exist */
    if (!dstobj) {
        dstobj = createZiplistObject();
        dbAdd(c->db,dstkey,dstobj);
    }
    signalModifiedKey(c->db,dstkey);
    listTypePush(dstobj,value,REDIS_HEAD);
    notifyKeyspaceEvent(REDIS_NOTIFY_LIST,"lpush",dstkey,c->db->id);
    /* Always send the pushed value to the client. */
    addReplyBulk(c,value);
}

void rpoplpushCommand(redisClient *c) {
    robj *sobj, *value;
    if ((sobj = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,sobj,REDIS_LIST)) return;

    if (listTypeLength(sobj) == 0) {
        /* This may only happen after loading very old RDB files. Recent
         * versions of Redis delete keys of empty lists. */
        addReply(c,shared.nullbulk);
    } else {
        robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
        robj *touchedkey = c->argv[1];

        if (dobj && checkType(c,dobj,REDIS_LIST)) return;
        value = listTypePop(sobj,REDIS_TAIL);
        /* We saved touched key, and protect it, since rpoplpushHandlePush
         * may change the client command argument vector (it does not
         * currently). */
        incrRefCount(touchedkey);
        rpoplpushHandlePush(c,c->argv[2],dobj,value);

        /* listTypePop returns an object with its refcount incremented */
        decrRefCount(value);

        /* Delete the source list when it is empty */
        notifyKeyspaceEvent(REDIS_NOTIFY_LIST,"rpop",touchedkey,c->db->id);
        if (listTypeLength(sobj) == 0) {
            dbDelete(c->db,touchedkey);
            notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",
                                touchedkey,c->db->id);
        }
        signalModifiedKey(c->db,touchedkey);
        decrRefCount(touchedkey);
        server.dirty++;
    }
}

/*-----------------------------------------------------------------------------
 * Blocking POP operations
 *----------------------------------------------------------------------------*/

/* This is how the current blocking POP works, we use BLPOP as example:
 * - If the user calls BLPOP and the key exists and contains a non empty list
 *   then LPOP is called instead. So BLPOP is semantically the same as LPOP
 *   if blocking is not required.
 * - If instead BLPOP is called and the key does not exists or the list is
 *   empty we need to block. In order to do so we remove the notification for
 *   new data to read in the client socket (so that we'll not serve new
 *   requests if the blocking request is not served). Also we put the client
 *   in a dictionary (db->blocking_keys) mapping keys to a list of clients
 *   blocking for this keys.
 * - If a PUSH operation against a key with blocked clients waiting is
 *   performed, we mark this key as "ready", and after the current command,
 *   MULTI/EXEC block, or script, is executed, we serve all the clients waiting
 *   for this list, from the one that blocked first, to the last, accordingly
 *   to the number of elements we have in the ready list.
 */

/* ���ݸ�����key��client���� */
void blockForKeys(redisClient *c, robj **keys, int numkeys, mstime_t timeout, robj *target) {
    dictEntry *de;
    list *l;
    int j;
	//���ó�ʱʱ���target���ýṹ�������г�
    c->bpop.timeout = timeout;
    c->bpop.target = target;
	//����target�����ü���
    if (target != NULL) incrRefCount(target);
	//����ǰclient��numkeys��key�������������Ҳ�������client�����ļ��Ǹ�����numkeys��key
    for (j = 0; j < numkeys; j++) {
        /* If the key already exists in the dict ignore it. */
		//��Ҫ�����ļ�����bpop.keys�ֵ���
		if (dictAdd(c->bpop.keys,keys[j],NULL) != DICT_OK) continue;
		//��ǰ��key���ü�����1
		incrRefCount(keys[j]);

        /* And in the other "side", to map keys -> clients */
		//db->blocking_keys��һ���ֵ䣬�ֵ�ļ�Ϊ���client������һ������ֵ��һ���������������б��ü�������client
		//��ǰ���client�������ļ���û�е�ǰ��key�����û����Ҫ���й���
        de = dictFind(c->db->blocking_keys,keys[j]);
		//û�е�ǰ��key����ӽ�ȥ
		if (de == NULL) {
            int retval;

			//����һ���б�
            l = listCreate();
			//����������ļ����б���ӵ�db->blocking_keys�ֵ���
            retval = dictAdd(c->db->blocking_keys,keys[j],l);
            incrRefCount(keys[j]);
            redisAssertWithInfo(c,keys[j],retval == DICT_OK);
		//����Ѿ����ˣ���ǰkey��ֵ����������ֵ��һ���б�
        } else {
            l = dictGetVal(de);
        }
		//����ǰclient���뵽������client���б�
        listAddNodeTail(l,c);
    }
    blockClient(c,REDIS_BLOCKED_LIST);   //����client
}

//�������һ�����������е�client
void unblockClientWaitingData(redisClient *c) {
    dictEntry *de;
    dictIterator *di;
    list *l;

    redisAssertWithInfo(c,NULL,dictSize(c->bpop.keys) != 0);
	//����һ���ֵ�ĵ�������ָ��������client�����ļ�����ɵ��ֵ�
	di = dictGetIterator(c->bpop.keys);
	//��Ϊclient���ܱ����key������������Ҫ�������еļ�
    while((de = dictNext(di)) != NULL) {
		robj *key = dictGetKey(de); //���key����

		//����key�ҵ���Ӧ���б�����ֵ��ֵ�����ű�������client��������c->db->blocking_keys��Ѱ��
        l = dictFetchValue(c->db->blocking_keys,key);
        redisAssertWithInfo(c,key,l != NULL);
		// ��������client���б����Ƴ�
		listDelNode(l,listSearchKey(l,c));
		//�����ǰ�б�Ϊ���ˣ����c->db->blocking_keys�н�keyɾ��
        if (listLength(l) == 0)
            dictDelete(c->db->blocking_keys,key);
    }
	dictReleaseIterator(di); //�ͷŵ�����

	//���bpop.keys�����нڵ�
    dictEmpty(c->bpop.keys,NULL);
	//�������������ӵ�Ԫ�أ���Ӧ���ͷ�
    if (c->bpop.target) {
        decrRefCount(c->bpop.target);
        c->bpop.target = NULL;
    }
}

/* If the specified key has clients blocked waiting for list pushes, this
 * function will put the key reference into the server.ready_keys list.
 * Note that db->ready_keys is a hash table that allows us to avoid putting
 * the same key again and again in the list in case of multiple pushes
 * made by a script or in the context of MULTI/EXEC.
 *
 * The list will be finally processed by handleClientsBlockedOnLists() */
void signalListAsReady(redisDb *db, robj *key) {
    readyList *rl;

    /* No clients blocking for this key? No need to queue it. */
    if (dictFind(db->blocking_keys,key) == NULL) return;

    /* Key was already signaled? No need to queue it again. */
    if (dictFind(db->ready_keys,key) != NULL) return;

    /* Ok, we need to queue this key into server.ready_keys. */
    rl = zmalloc(sizeof(*rl));
    rl->key = key;
    rl->db = db;
    incrRefCount(key);
    listAddNodeTail(server.ready_keys,rl);

    /* We also add the key in the db->ready_keys dictionary in order
     * to avoid adding it multiple times into a list with a simple O(1)
     * check. */
    incrRefCount(key);
    redisAssert(dictAdd(db->ready_keys,key,NULL) == DICT_OK);
}

/* This is a helper function for handleClientsBlockedOnLists(). It's work
 * is to serve a specific client (receiver) that is blocked on 'key'
 * in the context of the specified 'db', doing the following:
 *
 * 1) Provide the client with the 'value' element.
 * 2) If the dstkey is not NULL (we are serving a BRPOPLPUSH) also push the
 *    'value' element on the destination list (the LPUSH side of the command).
 * 3) Propagate the resulting BRPOP, BLPOP and additional LPUSH if any into
 *    the AOF and replication channel.
 *
 * The argument 'where' is REDIS_TAIL or REDIS_HEAD, and indicates if the
 * 'value' element was popped fron the head (BLPOP) or tail (BRPOP) so that
 * we can propagate the command properly.
 *
 * The function returns REDIS_OK if we are able to serve the client, otherwise
 * REDIS_ERR is returned to signal the caller that the list POP operation
 * should be undone as the client was not served: This only happens for
 * BRPOPLPUSH that fails to push the value to the destination key as it is
 * of the wrong type. */
int serveClientBlockedOnList(redisClient *receiver, robj *key, robj *dstkey, redisDb *db, robj *value, int where)
{
    robj *argv[3];

    if (dstkey == NULL) {
        /* Propagate the [LR]POP operation. */
        argv[0] = (where == REDIS_HEAD) ? shared.lpop :
                                          shared.rpop;
        argv[1] = key;
        propagate((where == REDIS_HEAD) ?
            server.lpopCommand : server.rpopCommand,
            db->id,argv,2,REDIS_PROPAGATE_AOF|REDIS_PROPAGATE_REPL);

        /* BRPOP/BLPOP */
        addReplyMultiBulkLen(receiver,2);
        addReplyBulk(receiver,key);
        addReplyBulk(receiver,value);
    } else {
        /* BRPOPLPUSH */
        robj *dstobj =
            lookupKeyWrite(receiver->db,dstkey);
        if (!(dstobj &&
             checkType(receiver,dstobj,REDIS_LIST)))
        {
            /* Propagate the RPOP operation. */
            argv[0] = shared.rpop;
            argv[1] = key;
            propagate(server.rpopCommand,
                db->id,argv,2,
                REDIS_PROPAGATE_AOF|
                REDIS_PROPAGATE_REPL);
            rpoplpushHandlePush(receiver,dstkey,dstobj,
                value);
            /* Propagate the LPUSH operation. */
            argv[0] = shared.lpush;
            argv[1] = dstkey;
            argv[2] = value;
            propagate(server.lpushCommand,
                db->id,argv,3,
                REDIS_PROPAGATE_AOF|
                REDIS_PROPAGATE_REPL);
        } else {
            /* BRPOPLPUSH failed because of wrong
             * destination type. */
            return REDIS_ERR;
        }
    }
    return REDIS_OK;
}

/* This function should be called by Redis every time a single command,
 * a MULTI/EXEC block, or a Lua script, terminated its execution after
 * being called by a client.
 *
 * All the keys with at least one client blocked that received at least
 * one new element via some PUSH operation are accumulated into
 * the server.ready_keys list. This function will run the list and will
 * serve clients accordingly. Note that the function will iterate again and
 * again as a result of serving BRPOPLPUSH we can have new blocking clients
 * to serve because of the PUSH side of BRPOPLPUSH. */
void handleClientsBlockedOnLists(void) {
    while(listLength(server.ready_keys) != 0) {
        list *l;

        /* Point server.ready_keys to a fresh list and save the current one
         * locally. This way as we run the old list we are free to call
         * signalListAsReady() that may push new elements in server.ready_keys
         * when handling clients blocked into BRPOPLPUSH. */
        l = server.ready_keys;
        server.ready_keys = listCreate();

        while(listLength(l) != 0) {
            listNode *ln = listFirst(l);
            readyList *rl = ln->value;

            /* First of all remove this key from db->ready_keys so that
             * we can safely call signalListAsReady() against this key. */
            dictDelete(rl->db->ready_keys,rl->key);

            /* If the key exists and it's a list, serve blocked clients
             * with data. */
            robj *o = lookupKeyWrite(rl->db,rl->key);
            if (o != NULL && o->type == REDIS_LIST) {
                dictEntry *de;

                /* We serve clients in the same order they blocked for
                 * this key, from the first blocked to the last. */
                de = dictFind(rl->db->blocking_keys,rl->key);
                if (de) {
                    list *clients = dictGetVal(de);
                    int numclients = (int)listLength(clients);                  WIN_PORT_FIX /* cast (int) */

                    while(numclients--) {
                        listNode *clientnode = listFirst(clients);
                        redisClient *receiver = clientnode->value;
                        robj *dstkey = receiver->bpop.target;
                        int where = (receiver->lastcmd &&
                                     receiver->lastcmd->proc == blpopCommand) ?
                                    REDIS_HEAD : REDIS_TAIL;
                        robj *value = listTypePop(o,where);

                        if (value) {
                            /* Protect receiver->bpop.target, that will be
                             * freed by the next unblockClient()
                             * call. */
                            if (dstkey) incrRefCount(dstkey);
                            unblockClient(receiver);

                            if (serveClientBlockedOnList(receiver,
                                rl->key,dstkey,rl->db,value,
                                where) == REDIS_ERR)
                            {
                                /* If we failed serving the client we need
                                 * to also undo the POP operation. */
                                    listTypePush(o,value,where);
                            }

                            if (dstkey) decrRefCount(dstkey);
                            decrRefCount(value);
                        } else {
                            break;
                        }
                    }
                }

                if (listTypeLength(o) == 0) dbDelete(rl->db,rl->key);
                /* We don't call signalModifiedKey() as it was already called
                 * when an element was pushed on the list. */
            }

            /* Free this item. */
            decrRefCount(rl->key);
            zfree(rl);
            listDelNode(l,ln);
        }
        listRelease(l); /* We have the new list on place at this point. */
    }
}

/* BRPOP BLPOP����ĵײ�ʵ�� */
void blockingPopGenericCommand(redisClient *c, int where) {
    robj *o;
    mstime_t timeout;
    int j;
	//����Ϊ��λ����timeout��ֵ
    if (getTimeoutFromObjectOrReply(c,c->argv[c->argc-1],&timeout,UNIT_SECONDS)
        != REDIS_OK) return;
	//��������Ϊ,�������е�key,���key���б���ֵ,��ִ����ѭ��һ����ֱ�ӷ���
    for (j = 1; j < c->argc-1; j++) {
		//��д������ȡkey��Ӧ��val
        o = lookupKeyWrite(c->db,c->argv[j]);
		// value����Ϊ��
        if (o != NULL) {
			// ���value��������Ͳ����б����ͣ��������ʹ�����Ϣ��ֱ�ӷ���
            if (o->type != REDIS_LIST) {
                addReply(c,shared.wrongtypeerr);
                return;
			// �б��Ȳ�Ϊ0
            } else {
                if (listTypeLength(o) != 0) {
					// �����¼�����
                    char *event = (where == REDIS_HEAD) ? "lpop" : "rpop";
                    //���浯����value����
					robj *value = listTypePop(o,where);
                    redisAssert(value != NULL);
					// ���ͻظ���client
                    addReplyMultiBulkLen(c,2);
                    addReplyBulk(c,c->argv[j]);
                    addReplyBulk(c,value);
					//�ͷ�value
                    decrRefCount(value);
					//�����¼�֪ͨ
                    notifyKeyspaceEvent(REDIS_NOTIFY_LIST,event,
                                        c->argv[j],c->db->id);
					//�������Ԫ�غ��б�Ϊ��
                    if (listTypeLength(o) == 0) {
						//�����ݿ���ɾ����ǰ��key
                        dbDelete(c->db,c->argv[j]);
						//����del�¼�֪ͨ
                        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",
                                            c->argv[j],c->db->id);
                    }
					//�����޸ķ����ź�
                    signalModifiedKey(c->db,c->argv[j]);
					//�������
                    server.dirty++;

					// ����һ��[LR]POP ������B[LR]POP���޸�clientԭ�����������
                    rewriteClientCommandVector(c,2,
                        (where == REDIS_HEAD) ? shared.lpop : shared.rpop,
                        c->argv[j]);
                    return;
                }
            }
        }
    }

    /* If we are inside a MULTI/EXEC and the list is empty the only thing
     * we can do is treating it as a timeout (even with timeout 0). */
	// ���������һ��������ִ�У�����һ���ջظ��Ա������ȴ�����ΪҪִ������������顣
    if (c->flags & REDIS_MULTI) {
        addReply(c,shared.nullmultibulk);
        return;
    }

    /* If the list is empty or the key does not exists we must block */
	// �����е����м��������ڣ���������Щ��
    blockForKeys(c, c->argv + 1, c->argc - 2, timeout, NULL);
}

void blpopCommand(redisClient *c) {
    blockingPopGenericCommand(c,REDIS_HEAD);
}

void brpopCommand(redisClient *c) {
    blockingPopGenericCommand(c,REDIS_TAIL);
}
// BRPOPLPUSH�����ʵ��
void brpoplpushCommand(redisClient *c) {
    mstime_t timeout;
	robj *key;
	//����Ϊ��λȡ����ʱʱ��
    if (getTimeoutFromObjectOrReply(c,c->argv[3],&timeout,UNIT_SECONDS)
        != REDIS_OK) return;
	//��д������ȡ�� source��ֵ
    key = lookupKeyWrite(c->db, c->argv[1]);
	//�����Ϊ�գ�������Ϊ
    if (key == NULL) {
		// ���������һ��������ִ�У�����һ���ջظ��Ա������ȴ�
        if (c->flags & REDIS_MULTI) {
            /* Blocking against an empty list in a multi state
             * returns immediately. */
            addReply(c, shared.nullbulk);
        } else {
			// �б�Ϊ�գ���client����
            blockForKeys(c, c->argv + 1, 1, timeout, c->argv[2]);
        }
	//��������Ϊ
	//�������Ϊ�գ�ִ��RPOPLPUSH
    } else {
		//�ж�ȡ����value�����Ƿ�Ϊ�б����ͣ����ǵĻ��������ʹ�����Ϣ
        if (key->type != REDIS_LIST) {
            addReply(c, shared.wrongtypeerr);
        } else {
            /* The list exists and has elements, so
             * the regular rpoplpushCommand is executed. */
			// value������б��������Ԫ�أ����Ե�����ͨ��rpoplpush����
            redisAssertWithInfo(c,key,listTypeLength(key) > 0);
            rpoplpushCommand(c);
        }
    }
}
