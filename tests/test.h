#ifndef TINYOPT_TEST_H
#define TINYOPT_TEST_H

#include <stdio.h>

extern int g_tests_run;
extern int g_tests_failed;

#define CHECK(cond) do { \
    g_tests_run++; \
    if (!(cond)) { \
        g_tests_failed++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

void test_parser(void);
void test_rewrite(void);
void test_cardinality(void);
void test_dp(void);
void test_e2e(void);

#endif
