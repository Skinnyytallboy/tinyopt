#include <stdio.h>
#include "test.h"

int g_tests_run = 0;
int g_tests_failed = 0;

int main(void) {
    test_parser();
    test_rewrite();
    test_cardinality();
    test_dp();
    test_e2e();

    printf("%d/%d tests passed\n", g_tests_run - g_tests_failed, g_tests_run);
    return g_tests_failed > 0 ? 1 : 0;
}
