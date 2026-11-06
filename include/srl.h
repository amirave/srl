#include <assert.h>
#include <stdbool.h>

#define SRL_STR_DEFAULT_LENGTH (32)
#define SRL_ATTRIB_DATA_SECTION __attribute__((section(".data")))

#define SRL_INT(__name, __val)                    \
    int __name SRL_ATTRIB_DATA_SECTION = (__val); \
    int *__name##_SRL_INT = &__name;
#define SRL_INT_RANGE(__name, __val, __min, __max)                                    \
    static_assert(__min <= __max, "Minimum should be smaller than maximum in range"); \
    int __name##_SRL_RANGE_MIN = __min;                                               \
    int __name##_SRL_RANGE_MAX = __max;                                               \
    SRL_INT(__name, __val)

#define SRL_FLOAT(__name, __val)                    \
    float __name SRL_ATTRIB_DATA_SECTION = (__val); \
    float *__name##_SRL_FLOAT = &__name
#define SRL_FLOAT_RANGE(__name, __val, __min, __max)                                  \
    static_assert(__min <= __max, "Minimum should be smaller than maximum in range"); \
    float __name##_SRL_RANGE_MIN = __min;                                             \
    float __name##_SRL_RANGE_MAX = __max;                                             \
    SRL_FLOAT(__name, __val)

#define SRL_STR_LEN(__name, __val, __str_len)                 \
    char __name[__str_len] SRL_ATTRIB_DATA_SECTION = (__val); \
    char *__name##_SRL_STR = __name
#define SRL_STR(__name, __val) SRL_STR_LEN(__name, __val, SRL_STR_DEFAULT_LENGTH)

#define SRL_BOOL(__name, __val)                    \
    bool __name SRL_ATTRIB_DATA_SECTION = (__val); \
    bool *__name##_SRL_BOOL = &__name
