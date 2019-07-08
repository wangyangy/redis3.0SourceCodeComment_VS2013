#include "redis.h"
#include <math.h>

/*-----------------------------------------------------------------------------
 * Hash type API
 *----------------------------------------------------------------------------*/

/* ��ÿ��Ԫ�ص�ֵ���ֽڳ���С��Ĭ�ϵ�64�ֽ�ʱ���Լ��ܳ���С��Ĭ�ϵ�512��ʱ��
��ϣ��������ziplist���������ݣ��ڲ����µ�Ԫ�ص�ʱ�򶼻����Ƿ����������������������������б����ת�� */
void hashTypeTryConversion(robj *o, robj **argv, int start, int end) {
    int i;
	// ��������� ziplist ���룬��ôֱ�ӷ���
    if (o->encoding != REDIS_ENCODING_ZIPLIST) return;
	// �������������󣬿����ǵ��ַ���ֵ�Ƿ񳬹���ָ������
    for (i = start; i <= end; i++) {
        if (sdsEncodedObject(argv[i]) &&
            sdslen(argv[i]->ptr) > server.hash_max_ziplist_value)
        {
			// ������ı���ת���� REDIS_ENCODING_HT
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

/* ������Ƿ����
 ����:field:��,vstr:ֵ���ַ���ʱ�������浽���ָ��,vlen:�����ַ����ĳ���,ll:ֵ������ʱ���������浽���ָ��,����ʧ��ʱ��������-1,���ҳɹ�ʱ���� 0 �� */
int hashTypeGetFromZiplist(robj *o, robj *field,
                           unsigned char **vstr,
                           unsigned int *vlen,
                           PORT_LONGLONG *vll)
{
    unsigned char *zl, *fptr = NULL, *vptr = NULL;
    int ret;
	// ȷ��������ȷ
    redisAssert(o->encoding == REDIS_ENCODING_ZIPLIST);
	// ȡ��δ�������
    field = getDecodedObject(field);
	// ���� ziplist ���������λ��
    zl = o->ptr;
    fptr = ziplistIndex(zl, ZIPLIST_HEAD);
    if (fptr != NULL) {
		// ��λ������Ľڵ�
        fptr = ziplistFind(fptr, field->ptr, (unsigned int)sdslen(field->ptr), 1); WIN_PORT_FIX /* cast (unsigned int) */
		// ���Ѿ��ҵ���ȡ���������Ӧ��ֵ��λ��
        if (fptr != NULL) {
            /* Grab pointer to the value (fptr points to the field) */
            vptr = ziplistNext(zl, fptr);
            redisAssert(vptr != NULL);
        }
    }

    decrRefCount(field);
	// �� ziplist �ڵ���ȡ��ֵ
    if (vptr != NULL) {
        ret = ziplistGet(vptr, vstr, vlen, vll);
        redisAssert(ret);
        return 0;
    }

    return -1;  //û�ҵ�����-1
}

/*
* �� REDIS_ENCODING_HT ����� hash ��ȡ���� field ���Ӧ��ֵ��
* �ɹ��ҵ�ֵʱ���� 0 ��û�ҵ����� -1 ��
*/
int hashTypeGetFromHashTable(robj *o, robj *field, robj **value) {
    dictEntry *de;
	// ȷ��������ȷ
    redisAssert(o->encoding == REDIS_ENCODING_HT);
	// ���ֵ��в����򣨼���
    de = dictFind(o->ptr, field);
    if (de == NULL) return -1;// ��������
    *value = dictGetVal(de);// ȡ���򣨼�����ֵ
    return 0;
}

/*
* ��̬ GET �������� hash ��ȡ���� field ��ֵ��������һ��ֵ����
* �ҵ�����ֵ����û�ҵ����� NULL ��
*/
robj *hashTypeGetObject(robj *o, robj *field) {
    robj *value = NULL;
	// �� ziplist ��ȡ��ֵ
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        PORT_LONGLONG vll = LLONG_MAX;

        if (hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll) == 0) {
			// ����ֵ����
            if (vstr) {
                value = createStringObject((char*)vstr, vlen);
            } else {
                value = createStringObjectFromLongLong(vll);
            }
        }
	// ���ֵ���ȡ��ֵ
    } else if (o->encoding == REDIS_ENCODING_HT) {
        robj *aux;

        if (hashTypeGetFromHashTable(o, field, &aux) == 0) {
            incrRefCount(aux);
            value = aux;
        }
    } else {
        redisPanic("Unknown hash encoding");
    }
    return value;// ����ֵ���󣬻��� NULL
}

/* ���field�����Ƿ���� */
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
* �������� field-value ����ӵ� hash �У�
* ��� field �Ѿ����ڣ���ôɾ���ɵ�ֵ����������ֵ��
* ������������ field �� value �����������ü���������
* ���� 0 ��ʾԪ���Ѿ����ڣ���κ�������ִ�е��Ǹ��²�����
* ���� 1 ���ʾ����ִ�е�������Ӳ�����
*/
int hashTypeSet(robj *o, robj *field, robj *value) {
    int update = 0;
	// ��ӵ� ziplist
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *zl, *fptr, *vptr;
		// ������ַ�����������,ziplist��Ҫ������������
        field = getDecodedObject(field);
        value = getDecodedObject(value);
		// �������� ziplist �����Բ��Ҳ����� field ��������Ѿ����ڵĻ���
        zl = o->ptr;
        fptr = ziplistIndex(zl, ZIPLIST_HEAD);
        if (fptr != NULL) {
			// ��λ���� field
            fptr = ziplistFind(fptr, field->ptr, (unsigned int)sdslen(field->ptr), 1); WIN_PORT_FIX /* cast (unsigned int) */
            if (fptr != NULL) {
				// ��λ�����ֵ
                vptr = ziplistNext(zl, fptr);
                redisAssert(vptr != NULL);
                update = 1;// ��ʶ��β���Ϊ���²���

				// ɾ���ɵļ�ֵ��
                zl = ziplistDelete(zl, &vptr);

				// ����µļ�ֵ��
                zl = ziplistInsert(zl, vptr, value->ptr, (unsigned int)sdslen(value->ptr)); WIN_PORT_FIX /* cast (unsigned int) */
            }
        }
		// ����ⲻ�Ǹ��²�������ô�����һ����Ӳ���
        if (!update) {
			// ���µ� field-value �����뵽 ziplist ��ĩβ
            zl = ziplistPush(zl, field->ptr, (unsigned int)sdslen(field->ptr), ZIPLIST_TAIL); WIN_PORT_FIX /* cast (unsigned int) */
            zl = ziplistPush(zl, value->ptr, (unsigned int)sdslen(value->ptr), ZIPLIST_TAIL); WIN_PORT_FIX /* cast (unsigned int) */
        }
        o->ptr = zl;  // ���¶���ָ��
        decrRefCount(field);// �ͷ���ʱ����
        decrRefCount(value);

		// �������Ӳ������֮���Ƿ���Ҫ�� ZIPLIST ����ת���� HT ����
        if (hashTypeLength(o) > server.hash_max_ziplist_entries)
            hashTypeConvert(o, REDIS_ENCODING_HT);
	// ��ӵ��ֵ�
    } else if (o->encoding == REDIS_ENCODING_HT) {
		// ��ӻ��滻��ֵ�Ե��ֵ�
		// ��ӷ��� 1 ���滻���� 0
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
* ɾ���ɹ����� 1 ����Ϊ�򲻴��ڶ���ɵ�ɾ��ʧ�ܷ��� 0 ��
*/
int hashTypeDelete(robj *o, robj *field) {
    int deleted = 0;
	// �� ziplist ��ɾ��
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *zl, *fptr;
		// ��λ����
        field = getDecodedObject(field);

        zl = o->ptr;
        fptr = ziplistIndex(zl, ZIPLIST_HEAD);
        if (fptr != NULL) {
            fptr = ziplistFind(fptr, field->ptr, (unsigned int)sdslen(field->ptr), 1); WIN_PORT_FIX /* cast (unsigned int) */
            if (fptr != NULL) {
                zl = ziplistDelete(zl,&fptr);// ɾ�����ֵ
                zl = ziplistDelete(zl,&fptr);
                o->ptr = zl;
                deleted = 1;
            }
        }

        decrRefCount(field);
	// ���ֵ���ɾ��
    } else if (o->encoding == REDIS_ENCODING_HT) {
        if (dictDelete((dict*)o->ptr, field) == REDIS_OK) {
            deleted = 1;

			// ɾ���ɹ�ʱ�����ֵ��Ƿ���Ҫ����
            if (htNeedsResize(o->ptr)) dictResize(o->ptr);
        }

    } else {
        redisPanic("Unknown hash encoding");
    }

    return deleted;
}

/* ����hash���е�Ԫ������ */
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
/*����hash�������*/
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
/*�ͷ�hash�������*/
void hashTypeReleaseIterator(hashTypeIterator *hi) {
    if (hi->encoding == REDIS_ENCODING_HT) {
        dictReleaseIterator(hi->di);
    }

    zfree(hi);
}

/* �ƶ���hash���е���һ��Ԫ�ص�λ�� */
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

/* ��ȡ�������α�λ�õ�Ԫ�صļ�ֵ */
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

/*���ݵ�������ȡ��ǰλ�õ�key-value */
void hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what, robj **dst) {
    redisAssert(hi->encoding == REDIS_ENCODING_HT);

    if (what & REDIS_HASH_KEY) {
        *dst = dictGetKey(hi->de);
    } else {
        *dst = dictGetVal(hi->de);
    }
}

/*��ȡkey-value*/
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
/*����robj����,�������򴴽�,�����ҵ��Ļ򴴽��Ķ���*/
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

/*��һ��ziplist����Ĺ�ϣ����oת������������*/
void hashTypeConvertZiplist(robj *o, int enc) {
    redisAssert(o->encoding == REDIS_ENCODING_ZIPLIST);
	// ��������� ZIPLIST ����ô��������
    if (enc == REDIS_ENCODING_ZIPLIST) {
        /* Nothing to do... */
	// ת����HT����
    } else if (enc == REDIS_ENCODING_HT) {
        hashTypeIterator *hi;
        dict *dict;
        int ret;
		// ������ϣ������
        hi = hashTypeInitIterator(o);
        dict = dictCreate(&hashDictType, NULL);// �����հ׵����ֵ�
		// �������� ziplist
        while (hashTypeNext(hi) != REDIS_ERR) {
            robj *field, *value;
			// ȡ�� ziplist ��ļ�
            field = hashTypeCurrentObject(hi, REDIS_HASH_KEY);
            field = tryObjectEncoding(field);
			// ȡ�� ziplist ���ֵ
            value = hashTypeCurrentObject(hi, REDIS_HASH_VALUE);
            value = tryObjectEncoding(value);
			//����ֵ����ӵ��ֵ���
            ret = dictAdd(dict, field, value);
            if (ret != DICT_OK) {
                redisLogHexDump(REDIS_WARNING,"ziplist with dup elements dump",
                    o->ptr,ziplistBlobLen(o->ptr));
                redisAssert(ret == DICT_OK);
            }
        }
		//�ͷ�ziplist�ĵ�����
        hashTypeReleaseIterator(hi);
        zfree(o->ptr);// �ͷŶ���ԭ���� ziplist
		// ���¹�ϣ�ı����ֵ����
        o->encoding = REDIS_ENCODING_HT;
        o->ptr = dict;

    } else {
        redisPanic("Unknown hash encoding");
    }
}
/*
* �Թ�ϣ����o�ı��뷽ʽ����ת��
* Ŀǰֻ֧�ֽ�ZIPLIST����ת����HT����
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
/*hset����ʵ��*/
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
/*hsetnx����ʵ��*/
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
/*hmset����ʵ��*/
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
/*hincrby����ʵ��*/
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

/*HINCRBYFLOAT�����ʵ��*/
void hincrbyfloatCommand(redisClient *c) {
    PORT_LONGDOUBLE value, incr;
    robj *o, *current, *new, *aux;
	// �õ�һ��long double���͵�����increment
    if (getLongDoubleFromObjectOrReply(c,c->argv[3],&incr,NULL) != REDIS_OK) return;
	// ��д��ʽȡ����ϣ����ʧ����ֱ�ӷ���
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
	// ����field�ڹ�ϣ����o�е�ֵ����
    if ((current = hashTypeGetObject(o,c->argv[2])) != NULL) {
		//��ֵ�����еõ�һ��long double���͵�value��������Ǹ�������ֵ������"hash value is not a valid float"��Ϣ��client
        if (getLongDoubleFromObjectOrReply(c,current,&value,
            "hash value is not a valid float") != REDIS_OK) {
			decrRefCount(current); //ȡֵ�ɹ����ͷ���ʱ��value����ռ䣬ֱ�ӷ���
            return;
        }
        decrRefCount(current);//ȡֵʧ��ҲҪ�ͷſռ�
    } else {
		value = 0; //���û��ֵ��������ΪĬ�ϵ�0
    }

    value += incr;//����ԭ�ȵ�ֵ
    new = createStringObjectFromLongDouble(value,1);// ��valueת��Ϊ�ַ������͵Ķ���
	//������ֵ����ı�������Ż����Խ�ʡ�ռ䣬����embstr��raw�����ʹ洢
	hashTypeTryObjectEncoding(o,&c->argv[2],NULL);
    hashTypeSet(o,c->argv[2],new);// ����ԭ����keyΪ�µ�ֵ����
	addReplyBulk(c, new); // ���µ�ֵ�����͸�client
	// �޸����ݿ�ļ������źţ�����"hincrbyfloat"�¼�֪ͨ���������
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(REDIS_NOTIFY_HASH,"hincrbyfloat",c->argv[1],c->db->id);
    server.dirty++;

	// ��HSET�������HINCRBYFLOAT���Է���ͬ�ĸ��㾫����ɵ����
	// ����HSET�ַ�������
    aux = createStringObject("HSET",4);
	// �޸�HINCRBYFLOAT����ΪHSET����
    rewriteClientCommandArgument(c,0,aux);
	decrRefCount(aux); // �ͷſռ�
	rewriteClientCommandArgument(c, 3, new); // �޸�incrementΪ�µ�ֵ����new
    decrRefCount(new);// �ͷſռ�
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
/* hget����ʵ�� */
void hgetCommand(redisClient *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;

    addHashFieldToReply(c, o, c->argv[2]);
}

/* hmget����ʵ�� */
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

/*HDEL����ĵײ�ʵ��*/
void hdelCommand(redisClient *c) {
    robj *o;
    int j, deleted = 0, keyremoved = 0;
	// ��д����ȡ����ϣ������ʧ�ܣ���ȡ���Ķ����ǹ�ϣ���͵Ķ�������0��ֱ�ӷ���
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;
	// �������е��ֶ�field
    for (j = 2; j < c->argc; j++) {
		// �ӹ�ϣ������ɾ����ǰ�ֶ�
        if (hashTypeDelete(o,c->argv[j])) {
			deleted++; //����ɾ���ĸ���
			// �����ϣ����Ϊ�գ���ɾ���ö���
            if (hashTypeLength(o) == 0) {
                dbDelete(c->db,c->argv[1]);
                keyremoved = 1;//����ɾ����־
                break;
            }
        }
    }
	//ɾ�����ֶ�
    if (deleted) {
		signalModifiedKey(c->db, c->argv[1]); // �����źű�ʾ�����ı�
		// ����"hdel"�¼�֪ͨ
		notifyKeyspaceEvent(REDIS_NOTIFY_HASH,"hdel",c->argv[1],c->db->id);
		// �����ϣ����ɾ��
        if (keyremoved)
			// ����"hdel"�¼�֪ͨ
            notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",c->argv[1],
                                c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);  //����ɾ���ĸ�����client
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
/*HKEYS,HVALS,HGETALL������ĵײ�ʵ��,ȫ������KEYS����ĵײ�ʵ��*/
void genericHgetallCommand(redisClient *c, int flags) {
    robj *o;
    hashTypeIterator *hi;
    int multiplier = 0;
    int length, count = 0;
	// ȡ����ϣ����
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
        || checkType(c,o,REDIS_HASH)) return;
	// ����Ҫȡ����Ԫ������
    if (flags & REDIS_HASH_KEY) multiplier++;
    if (flags & REDIS_HASH_VALUE) multiplier++;

    length = (int)(hashTypeLength(o) * multiplier);                             WIN_PORT_FIX /* cast (int) */
    addReplyMultiBulkLen(c, length);
	// �����ڵ㣬��ȡ��Ԫ��
    hi = hashTypeInitIterator(o);
    while (hashTypeNext(hi) != REDIS_ERR) {
        if (flags & REDIS_HASH_KEY) {// ȡ����
            addHashIteratorCursorToReply(c, hi, REDIS_HASH_KEY);
            count++;
        }
        if (flags & REDIS_HASH_VALUE) {// ȡ��ֵ
            addHashIteratorCursorToReply(c, hi, REDIS_HASH_VALUE);
            count++;
        }
    }
	// �ͷŵ�����
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
/*hscan�����ʵ��*/
void hscanCommand(redisClient *c) {
    robj *o;
    PORT_ULONG cursor;
	//// ��ȡscan������α�cursor
    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == REDIS_ERR) return;
	//��ȡ���󣬼������
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;
    scanGenericCommand(c,o,cursor);
}
