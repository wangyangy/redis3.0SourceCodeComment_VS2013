#include "redis.h"
#include <math.h> /* isnan(), isinf() */

/*-----------------------------------------------------------------------------
 * String Commands
 t_string����Ҫ�Ƕ�sds�ַ����ĵĺ������ã��Լ��Ϳͻ��ˣ�redisClient�������ݽ������̣�
 ���ݿͻ��˵Ĳ�������磺SET /GET/SETNX/INCR�ȵȣ�����ͻ��˶��󡪡�>
 �Ӷ�������ݻ�������ȡ�������������������>����������>���õײ㺯���Լ���ִֵ����Ӧ�����������>
 �Ѳ���������ظ��ͻ��˶������ݻ�������
 *----------------------------------------------------------------------------*/

/*����ַ����Ƿ񳬹�512M������*/
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

/*�ú���������:SET,SETEX,PSETEX,SETNX����ײ��ʵ��,flags������NX��XX,������ĺ��ṩ
  expire����key�Ĺ���ʱ��,ok_reply��abort_reply�����Żظ�client������,���ok_replyΪ��
  ��ʹ��"+OK",���abort_replyΪ����ʹ��$-1*/
void setGenericCommand(redisClient *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {
    PORT_LONGLONG milliseconds = 0; /* initialized to avoid any harmness warning */
	//���������key�Ĺ���ʱ��
    if (expire) {
		//��expire������ȡ��ֵ��������milliseconds�У����������Ĭ�ϵ���Ϣ��client
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != REDIS_OK)
            return;
		// �������ʱ��С�ڵ���0�����ʹ�����Ϣ��client
        if (milliseconds <= 0) {
            addReplyErrorFormat(c,"invalid expire time in %s",c->cmd->name);
            return;
        }
		//���unit�ĵ�λ���룬����Ҫת��Ϊ���뱣��
        if (unit == UNIT_SECONDS) milliseconds *= 1000;
    }
	//lookupKeyWrite������Ϊִ��д������ȡ��key��ֵ����
	//���������NX(������)�����������ݿ��� �ҵ� ��key������
	//������XX(����)�����������ݿ��� û���ҵ� ��key
	//�ظ�abort_reply��client
    if ((flags & REDIS_SET_NX && lookupKeyWrite(c->db,key) != NULL) ||
        (flags & REDIS_SET_XX && lookupKeyWrite(c->db,key) == NULL))
    {
        addReply(c, abort_reply ? abort_reply : shared.nullbulk);
        return;
    }
	//�ڵ�ǰdb���ü�Ϊkey��ֵΪval
    setKey(c->db,key,val);
	//�������ݿ�Ϊ��(dirty)��������ÿ���޸�һ��key�󣬶�������(dirty)��1
    server.dirty++;
	//����key�Ĺ���ʱ��,mstime()���غ���Ϊ��λ�ĸ�������ʱ��
    if (expire) setExpire(c->db,key,mstime()+milliseconds);
	//����"set"�¼���֪ͨ�����ڷ�������ģʽ��֪ͨ�ͻ��˽��ܷ������¼�
    notifyKeyspaceEvent(REDIS_NOTIFY_STRING,"set",key,c->db->id);
	//����"expire"�¼�֪ͨ
    if (expire) notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,
        "expire",key,c->db->id);
	//���óɹ�������ͻ��˷���ok_reply
    addReply(c, ok_reply ? ok_reply : shared.ok);
}

/* SET key value [NX] [XX] [EX <seconds>] [PX <milliseconds>] */
/*�趨��Key����ָ�����ַ���Value�������Key�Ѿ����ڣ��򸲸���ԭ��ֵ��
  
*/
void setCommand(redisClient *c) {
    int j;
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = REDIS_SET_NO_FLAGS;
	//����ѡ�����
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
		//�����﷨����
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }
	//���Զ�ֵ������б���
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,flags,c->argv[1],c->argv[2],expire,unit,NULL,NULL);
}

/*���ָ����Key�����ڣ����趨��Key����ָ���ַ���Value����ʱ��Ч���ȼ���SET���
�෴�������Key�Ѿ����ڣ�����������κβ��������ء�*/
void setnxCommand(redisClient *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,REDIS_SET_NX,c->argv[1],c->argv[2],NULL,0,shared.cone,shared.czero);
}

//ԭ�����������������һ�����ø�Key��ֵΪָ���ַ�����ͬʱ���ø�Key��Redis�������еĴ��ʱ��(����)����������ҪӦ����Redis������Cache������ʹ��ʱ
void setexCommand(redisClient *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,REDIS_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS,NULL,NULL);
}

void psetexCommand(redisClient *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,REDIS_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS,NULL,NULL);
}


//GET  key��ֵ����
int getGenericCommand(redisClient *c) {
    robj *o;
	// ���Դ����ݿ���ȡ���� c->argv[1] ��Ӧ��ֵ����
	// �����������ʱ����ͻ��˷��ͻظ���Ϣ�������� NULL
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL)
        return REDIS_OK;
	// ֵ������ڣ������������
    if (o->type != REDIS_STRING) {
		// ���ʹ���
        addReply(c,shared.wrongtypeerr);
        return REDIS_ERR;
    } else {
		// ������ȷ����ͻ��˷��ض����ֵ
        addReplyBulk(c,o);
        return REDIS_OK;
    }
}

// ��ȡָ��Key��Value
void getCommand(redisClient *c) {
    getGenericCommand(c);
}


//ԭ���Ե����ø�KeyΪָ����Value��ͬʱ���ظ�Key��ԭ��ֵ����GET����һ����
//������Ҳֻ�ܴ���string Value������Redis��������صĴ�����Ϣ��
void getsetCommand(redisClient *c) {

	// ȡ�������ؼ���ֵ����
    if (getGenericCommand(c) == REDIS_ERR) return;
	// ���������ֵ c->argv[2]
    c->argv[2] = tryObjectEncoding(c->argv[2]);
	// �����ݿ��й����� c->argv[1] ����ֵ���� c->argv[2]
    setKey(c->db,c->argv[1],c->argv[2]);
	// �����¼�֪ͨ
    notifyKeyspaceEvent(REDIS_NOTIFY_STRING,"set",c->argv[1],c->db->id);
	// ����������Ϊ��
    server.dirty++;
}

/*�滻ָ��Key�Ĳ����ַ���ֵ����offset��ʼ���滻�ĳ���Ϊ���������������value���ַ������ȣ�
�������offset��ֵ���ڸ�Key��ԭ��ֵValue���ַ������ȣ�
Redis������Value�ĺ��油��(offset - strlen(value))������0x00��֮����׷����ֵ��
����ü������ڣ�������Ὣ��ԭֵ�ĳ��ȼ���Ϊ0�����������offset��0x00����׷����ֵ��
�����ַ���Value����󳤶�Ϊ512M�����offset�����ֵΪ536870911��
�����Ҫע����ǣ������������ִ��ʱ��ʹָ��Key��ԭ��ֵ�������ӣ�
�⽫�ᵼ��Redis���·����㹻���ڴ��������滻���ȫ���ַ�������˾ͻ����һ������������
*/
void setrangeCommand(redisClient *c) {
    robj *o;
    PORT_LONG offset;
    sds value = c->argv[3]->ptr;
	//ȡ��offset����
    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != REDIS_OK)
        return;

    if (offset < 0) {
        addReplyError(c,"offset is out of range");
        return;
    }
	//ȡ��key��Ӧ��value
    o = lookupKeyWrite(c->db,c->argv[1]);
	//valueΪ��
    if (o == NULL) {
        /* valueΪ��,û��ʲô��������,����0 */
        if (sdslen(value) == 0) {
            addReply(c,shared.czero);
            return;
        }

        /* ������ú�ĳ��Ȼᳬ��redis������,��������÷���error */
        if (checkStringLength(c,offset+sdslen(value)) != REDIS_OK)
            return;
		//��������,�򴴽�һ�����ַ������������ݿ��й�����c->argv[1]���������
        o = createObject(REDIS_STRING,sdsempty());
        dbAdd(c->db,c->argv[1],o);
	//value��Ϊ��
    } else {
        size_t olen;

        /* ���������� */
        if (checkType(c,o,REDIS_STRING))
            return;

        /* ȡ��ԭ���ַ������� */
        olen = stringObjectLen(o);
		//�ַ�������Ϊ0,û�п������õ�,��ͻ��˷���0
        if (sdslen(value) == 0) {
            addReplyLongLong(c,olen);
            return;
        }

        /* ������ú�ᳬ��redis������,���������,���ش��� */
        if (checkStringLength(c,offset+sdslen(value)) != REDIS_OK)
            return;

        /* Create a copy when the object is shared or encoded. */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }

    if (sdslen(value) > 0) {
		//��չ�ַ���ֵ����
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value));
		//��value���Ƶ�ָ����λ��
        memcpy((char*)o->ptr+offset,value,sdslen(value));
		//�����ݿⷢ�ͼ����޸ĵ��ź�
        signalModifiedKey(c->db,c->argv[1]);
		//�����¼�֪ͨ
        notifyKeyspaceEvent(REDIS_NOTIFY_STRING,
            "setrange",c->argv[1],c->db->id);
		//���÷�����Ϊ��
        server.dirty++;
    }
	//���óɹ�,�����µ��ַ������ͻ���
    addReplyLongLong(c,sdslen(o->ptr));
}


/*�����ȡ���ַ������Ⱥ̣ܶ����ǿ��Ը������ʱ�临�Ӷ���ΪO(1)���������O(N)��
����N��ʾ��ȡ�����ַ������ȡ��������ڽ�ȡ���ַ���ʱ��
���Ա�����ķ�ʽͬʱ����start(0��ʾ��һ���ַ�)��end���ڵ��ַ���
���endֵ����Value���ַ����ȣ������ֻ�ǽ�ȡ��start��ʼ֮�����е��ַ����ݡ�
*/
void getrangeCommand(redisClient *c) {
    robj *o;
    PORT_LONGLONG start, end;
    char *str, llbuf[32];
    size_t strlen;
	// ȡ�� start ����
    if (getLongLongFromObjectOrReply(c,c->argv[2],&start,NULL) != REDIS_OK)
        return;
	// ȡ�� end ����
    if (getLongLongFromObjectOrReply(c,c->argv[3],&end,NULL) != REDIS_OK)
        return;
	// �����ݿ��в��Ҽ� c->argv[1] 
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||
        checkType(c,o,REDIS_STRING)) return;
	// ���ݱ��룬�Զ����ֵ���д���
    if (o->encoding == REDIS_ENCODING_INT) {
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(PORT_LONG)o->ptr);
    } else {
        str = o->ptr;
        strlen = sdslen(str);
    }

	// ����������ת��Ϊ��������
	if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if ((PORT_ULONGLONG)end >= strlen) end = strlen-1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end || strlen == 0) {
		// ����������ΧΪ�յ����
        addReply(c,shared.emptybulk);
    } else {
		// ��ͻ��˷��ظ�����Χ�ڵ��ַ�������
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1);
    }
}

/*N��ʾ��ȡKey����������������ָ��Keys��Values��
�������ĳ��Key�����ڣ�������ֵ��Ϊstring���ͣ���Key��Value������nil��
*/
void mgetCommand(redisClient *c) {
    int j;

    addReplyMultiBulkLen(c,c->argc-1);
	// ���Ҳ����������������ֵ
    for (j = 1; j < c->argc; j++) {
		// ���Ҽ� c->argc[j] ��ֵ
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) {
			// ֵ�����ڣ���ͻ��˷��Ϳջظ�
            addReply(c,shared.nullbulk);
        } else {
			// ֵ���ڣ��������ַ�������
            if (o->type != REDIS_STRING) {
                addReply(c,shared.nullbulk);
			// ֵ���ڣ��������ַ���
            } else {
                addReplyBulk(c,o);
            }
        }
    }
}

/*N��ʾָ��Key��������������ԭ���Ե���ɲ���������key/value�����ò�����
�������Ϊ���Կ����Ƕ�ε���ִ��SET��� */
void msetGenericCommand(redisClient *c, int nx) {
    int j, busykeys = 0;
	// ��ֵ�������ǳ���ɶԳ��ֵģ���ʽ����ȷ
    if ((c->argc % 2) == 0) {
        addReplyError(c,"wrong number of arguments for MSET");
        return;
    }
    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set nothing at all if at least one already key exists. */
	// ��� nx ����Ϊ�棬��ô�����������������ݿ����Ƿ����
	// ֻҪ��һ�����Ǵ��ڵģ���ô����ͻ��˷��Ϳջظ�
	// ������ִ�н����������ò���
    if (nx) {
        for (j = 1; j < c->argc; j += 2) {
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) {
                busykeys++;
            }
        }
		// ������,���Ϳհ׻ظ���������ִ�н����������ò���
        if (busykeys) {
            addReply(c, shared.czero);
            return;
        }
    }

	// �������м�ֵ��
    for (j = 1; j < c->argc; j += 2) {
		// ��ֵ������н���
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);
		// ����ֵ�Թ��������ݿ�,c->argc[j] Ϊ��,c->argc[j+1] Ϊֵ
        setKey(c->db,c->argv[j],c->argv[j+1]);
		// �����¼�֪ͨ
        notifyKeyspaceEvent(REDIS_NOTIFY_STRING,"set",c->argv[j],c->db->id);
    }
    server.dirty += (c->argc-1)/2;
	// ���óɹ�,MSET ���� OK ���� MSETNX ���� 1
    addReply(c, nx ? shared.cone : shared.ok);
}

void msetCommand(redisClient *c) {
    msetGenericCommand(c,0);
}

void msetnxCommand(redisClient *c) {
    msetGenericCommand(c,1);
}


/*����key��Ӧ��value����*/
void incrDecrCommand(redisClient *c, PORT_LONGLONG incr) {
    PORT_LONGLONG value, oldvalue;
    robj *o, *new;
	// ȡ��ֵ����
    o = lookupKeyWrite(c->db,c->argv[1]);
	// �������Ƿ���ڣ��Լ������Ƿ���ȷ
    if (o != NULL && checkType(c,o,REDIS_STRING)) return;
	// ȡ�����������ֵ�������浽 value ������
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != REDIS_OK) return;
	// ���ӷ�����ִ��֮��ֵ�Ƿ�����
	// ����ǵĻ�������ͻ��˷���һ������ظ������������ò���
    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
	// ���мӷ����㣬����ֵ���浽�µ�ֵ������,Ȼ�����µ�ֵ�����滻ԭ����ֵ����
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
	// �����ݿⷢ�ͼ����޸ĵ��ź�
    signalModifiedKey(c->db,c->argv[1]);
	// �����¼�֪ͨ
    notifyKeyspaceEvent(REDIS_NOTIFY_STRING,"incrby",c->argv[1],c->db->id);
    server.dirty++;
	// ���ػظ�
    addReply(c,shared.colon);
    addReply(c,new);
    addReply(c,shared.crlf);
}


/*��ָ��Key��Valueԭ���Եĵ���1��
�����Key�����ڣ����ʼֵΪ0����incr֮����ֵΪ1��
���Value��ֵ����ת��Ϊ����ֵ����Hello���ò�����ִ��ʧ�ܲ�������Ӧ�Ĵ�����Ϣ��
ע�⣺�ò�����ȡֵ��Χ��64λ�з������͡� */
void incrCommand(redisClient *c) {
    incrDecrCommand(c,1);
}

/*��ָ��Key��Valueԭ���Եĵݼ�1��
�����Key�����ڣ����ʼֵΪ0����decr֮����ֵΪ-1��
���Value��ֵ����ת��Ϊ����ֵ����Hello���ò�����ִ��ʧ�ܲ�������Ӧ�Ĵ�����Ϣ��
ע�⣺�ò�����ȡֵ��Χ��64λ�з������͡�*/
void decrCommand(redisClient *c) {
    incrDecrCommand(c,-1);
}
/*��ָ��Key��Valueԭ���Ե�����increment��
�����Key�����ڣ����ʼֵΪ0����incrby֮����ֵΪincrement��
���Value��ֵ����ת��Ϊ����ֵ����Hello���ò�����ִ��ʧ�ܲ�������Ӧ�Ĵ�����Ϣ��
ע�⣺�ò�����ȡֵ��Χ��64λ�з������͡� */
void incrbyCommand(redisClient *c) {
    PORT_LONGLONG incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;
    incrDecrCommand(c,incr);
}
/*��ָ��Key��Valueԭ���Եļ���decrement�������Key�����ڣ�
���ʼֵΪ0����decrby֮����ֵΪ-decrement�����Value��ֵ����ת��Ϊ����ֵ��
��Hello���ò�����ִ��ʧ�ܲ�������Ӧ�Ĵ�����Ϣ��
ע�⣺�ò�����ȡֵ��Χ��64λ�з������͡� */
void decrbyCommand(redisClient *c) {
    PORT_LONGLONG incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;
    incrDecrCommand(c,-incr);
}

void incrbyfloatCommand(redisClient *c) {
    PORT_LONGDOUBLE incr, value;
    robj *o, *new, *aux;
	//ȡ��ֵ����
    o = lookupKeyWrite(c->db,c->argv[1]);
	// �������Ƿ���ڣ��Լ������Ƿ���ȷ
    if (o != NULL && checkType(c,o,REDIS_STRING)) return;
	// �����������ֵ���浽 value ������
	// ��ȡ�� incr ������ֵ
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != REDIS_OK ||
        getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != REDIS_OK)
        return;
	// ���мӷ����㣬������Ƿ����
    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }
	// ��һ��������ֵ���¶����滻���е�ֵ����
    new = createStringObjectFromLongDouble(value,1);
    if (o)
        dbOverwrite(c->db,c->argv[1],new);
    else
        dbAdd(c->db,c->argv[1],new);
	// �����ݿⷢ�ͼ����޸ĵ��ź�
    signalModifiedKey(c->db,c->argv[1]);
	// �����¼�֪ͨ
    notifyKeyspaceEvent(REDIS_NOTIFY_STRING,"incrbyfloat",c->argv[1],c->db->id);
    server.dirty++;
	// �ظ�
    addReplyBulk(c,new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
	// �ڴ��� INCRBYFLOAT ����ʱ�������� SET �������滻 INCRBYFLOAT ����
	// �Ӷ���ֹ��Ϊ��ͬ�ĸ��㾫�Ⱥ͸�ʽ����� AOF ����ʱ�����ݲ�һ��
    aux = createStringObject("SET",3);
    rewriteClientCommandArgument(c,0,aux);
    decrRefCount(aux);
    rewriteClientCommandArgument(c,2,new);
}

/*�����Key�Ѿ����ڣ�APPEND�������Value������׷�ӵ��Ѵ���Value��ĩβ��
�����Key�����ڣ�APPEND����ᴴ��һ���µ�Key/Value
*/
void appendCommand(redisClient *c) {
    size_t totlen;
    robj *o, *append;
	// ȡ������Ӧ��ֵ����
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* Create the key */
		// ��ֵ�Բ����ڣ�����һ���µ�
        c->argv[2] = tryObjectEncoding(c->argv[2]);
        dbAdd(c->db,c->argv[1],c->argv[2]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    } else {
        /* Key exists, check type */
        if (checkType(c,o,REDIS_STRING))
            return;

        /* "append" is an argument, so always an sds */
		// ���׷�Ӳ���֮���ַ����ĳ����Ƿ���� Redis ������
        append = c->argv[2];
        totlen = stringObjectLen(o)+sdslen(append->ptr);
        if (checkStringLength(c,totlen) != REDIS_OK)
            return;

        /* Append the value */
		// ִ��׷�Ӳ���
        o = dbUnshareStringValue(c->db,c->argv[1],o);
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));
        totlen = sdslen(o->ptr);
    }
	// �����ݿⷢ�ͼ����޸ĵ��ź�
    signalModifiedKey(c->db,c->argv[1]);
	// �����¼�֪ͨ
    notifyKeyspaceEvent(REDIS_NOTIFY_STRING,"append",c->argv[1],c->db->id);
    server.dirty++;
	// ���ͻظ�
    addReplyLongLong(c,totlen);
}
/*����ָ��Key���ַ�ֵ���ȣ����Value����string���ͣ�
Redis��ִ��ʧ�ܲ�������صĴ�����Ϣ��
*/
void strlenCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_STRING)) return;
    addReplyLongLong(c,stringObjectLen(o));
}
