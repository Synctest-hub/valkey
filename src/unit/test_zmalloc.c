#include "../zmalloc.c"
#undef UNUSED
#include "test_help.h"

int test_zmallocInitialUsedMemory(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    printf("Malloc prefix size: %d\n", (int) PREFIX_SIZE);

    TEST_ASSERT(zmalloc_used_memory() == 0);

    return 0;
}

int test_zmallocAllocReallocCallocAndFree(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    void *ptr, *ptr2;

    ptr = zmalloc(123);
    TEST_PRINT_INFO("Allocated 123 bytes; used: %zu\n", zmalloc_used_memory());

    ptr = zrealloc(ptr, 456);
    TEST_PRINT_INFO("Reallocated to 456 bytes; used: %zu\n", zmalloc_used_memory());

    ptr2 = zcalloc(123);
    TEST_PRINT_INFO("Callocated 123 bytes; used: %zu\n", zmalloc_used_memory());

    zfree(ptr);
    zfree(ptr2);
    TEST_PRINT_INFO("Freed pointers; used: %zu\n", zmalloc_used_memory());

    TEST_ASSERT(zmalloc_used_memory() == 0);

    return 0;
}

int test_zmallocAllocZeroByteAndFree(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    void *ptr;

    ptr = zmalloc(0);
    printf("Allocated 0 bytes; used: %zu\n", zmalloc_used_memory());
    zfree(ptr);

    TEST_ASSERT(zmalloc_used_memory() == 0);

    return 0;
}
