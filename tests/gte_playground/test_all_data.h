/* test_all_data.h — 1150 GTE test cases from ps1-tests/gte/test-all */
#ifndef TEST_ALL_DATA_H
#define TEST_ALL_DATA_H

#include <stdint.h>

struct test_t {
    const char* name;
    uint32_t input[64];
    uint32_t opcode;
    uint32_t output[64];
};

#define TEST_COUNT 1150

extern const struct test_t tests[];

#endif /* TEST_ALL_DATA_H */
