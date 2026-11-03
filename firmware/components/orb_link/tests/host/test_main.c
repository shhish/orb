#include "test.h"

int g_checks, g_failures;

int main(void)
{
    test_sas();
    test_proto();
    test_backoff();
    test_control();
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
