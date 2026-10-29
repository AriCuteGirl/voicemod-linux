/*
 * The shim is linked without a C runtime. GCC still emits calls to memset (struct
 * initialisers) and memcmp (IsEqualIID), so provide them here. Loop-to-libcall
 * conversion is disabled so these do not call themselves.
 */
#include <stddef.h>

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memset(void *dst, int value, size_t size)
{
    unsigned char *p = dst;
    while (size--) *p++ = (unsigned char)value;
    return dst;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
int memcmp(const void *a, const void *b, size_t size)
{
    const unsigned char *x = a, *y = b;
    for (; size; size--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}
