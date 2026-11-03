// test/test_main.c — host unit test runner. Each test_<area>.c exposes void test_<area>(void).
#include "minitest.h"
int mt_failures, mt_checks;
void test_foundation(void);
void test_tags(void);
void test_provider(void);
void test_mind(void);
void test_engine(void);
void test_world(void);
void test_art(void);
void test_life(void);
void test_live(void);

int main(void)
{
    test_foundation();
    test_tags();
    test_provider();
    test_mind();
    test_engine();
    test_world();
    test_art();
    test_life();
    test_live();
    printf("%d checks, %d failures\n", mt_checks, mt_failures);
    return mt_failures ? 1 : 0;
}
