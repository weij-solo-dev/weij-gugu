// test/minitest.h — the smallest test harness that does the job on host and device.
#pragma once
#include <stdio.h>
#include <string.h>

extern int mt_failures, mt_checks;
#define CHECK(cond) do { mt_checks++; if (!(cond)) { mt_failures++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ_INT(a, b) do { long long _a = (long long)(a), _b = (long long)(b); mt_checks++; if (_a != _b) { mt_failures++; printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)
#define CHECK_EQ_STR(a, b) do { const char *_a = (a), *_b = (b); mt_checks++; if (!_a || !_b || strcmp(_a, _b)) { mt_failures++; printf("  FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b, _a ? _a : "(null)", _b ? _b : "(null)"); } } while (0)
#define TEST(name) static void name(void)
#define RUN(name) do { int before = mt_failures; printf("- %s\n", #name); name(); if (mt_failures == before) printf("  ok\n"); } while (0)
