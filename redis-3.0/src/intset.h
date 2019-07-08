
#ifndef __INTSET_H
#define __INTSET_H
#include <stdint.h>
//redis���ڱ�������ֵ����
typedef struct intset {
	//���뷽ʽ,encoding������ȡֵ:INTSET_ENC_INT16,INTSET_ENC_INT32,INTSET_ENC_INT64
	//INTSET_ENC_INT16:[-32768,32767],INTSET_ENC_INT32:[-21467483648,2147483647]
	//INTSET_ENC_INT64:[-9223372036854775808,9223372036854775807]
    uint32_t encoding;     
    uint32_t length;       //����Ԫ�ص�����
    int8_t contents[];     //����Ԫ�ص�����
} intset;

intset *intsetNew(void);
intset *intsetAdd(intset *is, int64_t value, uint8_t *success);
intset *intsetRemove(intset *is, int64_t value, int *success);
uint8_t intsetFind(intset *is, int64_t value);
int64_t intsetRandom(intset *is);
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value);
uint32_t intsetLen(intset *is);
size_t intsetBlobLen(intset *is);

#endif // __INTSET_H
