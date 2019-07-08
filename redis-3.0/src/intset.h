
#ifndef __INTSET_H
#define __INTSET_H
#include <stdint.h>
//redis用于保存整数值集合
typedef struct intset {
	//编码方式,encoding有三种取值:INTSET_ENC_INT16,INTSET_ENC_INT32,INTSET_ENC_INT64
	//INTSET_ENC_INT16:[-32768,32767],INTSET_ENC_INT32:[-21467483648,2147483647]
	//INTSET_ENC_INT64:[-9223372036854775808,9223372036854775807]
    uint32_t encoding;     
    uint32_t length;       //包含元素的数量
    int8_t contents[];     //保存元素的数组
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
