/* Minimal host test harness. No dependencies beyond libc so the pure firmware
 * core can be tested with a bare `gcc` invocation. */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int  g_tests_run;
extern int  g_tests_failed;
extern const char *g_current_test;

#define TEST_STATE_DEFS \
    int g_tests_run = 0; \
    int g_tests_failed = 0; \
    const char *g_current_test = "";

#define RUN_TEST(fn) do {                                    \
    g_current_test = #fn;                                    \
    int before = g_tests_failed;                             \
    fn();                                                    \
    g_tests_run++;                                           \
    if (g_tests_failed == before) {                          \
        printf("  ok    %s\n", #fn);                         \
    }                                                        \
} while (0)

#define FAIL(fmt, ...) do {                                  \
    g_tests_failed++;                                        \
    printf("  FAIL  %s\n        " fmt "\n",                  \
           g_current_test, ##__VA_ARGS__);                   \
} while (0)

#define CHECK(cond) do {                                     \
    if (!(cond)) { FAIL("%s:%d: %s", __FILE__, __LINE__, #cond); return; } \
} while (0)

#define CHECK_INT(actual, expected) do {                     \
    long _a = (long)(actual), _e = (long)(expected);         \
    if (_a != _e) {                                          \
        FAIL("%s:%d: %s == %ld, expected %ld",               \
             __FILE__, __LINE__, #actual, _a, _e);           \
        return;                                              \
    }                                                        \
} while (0)

#define CHECK_NEAR(actual, expected, tol) do {               \
    double _a = (double)(actual), _e = (double)(expected);   \
    double _d = _a - _e; if (_d < 0) _d = -_d;               \
    if (_d > (double)(tol)) {                                \
        FAIL("%s:%d: %s == %.3f, expected %.3f +/- %.3f",    \
             __FILE__, __LINE__, #actual, _a, _e, (double)(tol)); \
        return;                                              \
    }                                                        \
} while (0)

#define CHECK_BYTES(actual, expected, n) do {                \
    if (memcmp((actual), (expected), (n)) != 0) {            \
        FAIL("%s:%d: %s bytes differ", __FILE__, __LINE__, #actual); \
        printf("        got     :");                         \
        for (size_t _i = 0; _i < (size_t)(n); _i++)          \
            printf(" %02X", ((const unsigned char *)(actual))[_i]); \
        printf("\n        expected:");                       \
        for (size_t _i = 0; _i < (size_t)(n); _i++)          \
            printf(" %02X", ((const unsigned char *)(expected))[_i]); \
        printf("\n");                                        \
        return;                                              \
    }                                                        \
} while (0)

#define TEST_SUMMARY(suite) ({                               \
    printf("\n%s: %d tests, %d failed\n\n",                  \
           (suite), g_tests_run, g_tests_failed);            \
    g_tests_failed == 0 ? 0 : 1;                             \
})

#endif /* TEST_H */
