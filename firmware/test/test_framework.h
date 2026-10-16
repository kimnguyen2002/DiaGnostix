#pragma once

// Minimal subset of the Unity test API used by test_dsp.cpp, so the host
// tests need nothing but a C++17 compiler.

#include <cmath>
#include <cstdio>
#include <string>

void setUp();
void tearDown();

namespace testfw {

struct Failure {
  int line;
  std::string message;
};

inline int g_tests = 0;
inline int g_failures = 0;

[[noreturn]] inline void fail(int line, const std::string& message) { throw Failure{line, message}; }

inline std::string format(const char* fmt, double a, double b) {
  char buf[160];
  std::snprintf(buf, sizeof(buf), fmt, a, b);
  return buf;
}

inline void run(const char* name, void (*test)(), const char* file, int line) {
  ++g_tests;
  try {
    setUp();
    test();
    tearDown();
    std::printf("%s:%d:%s:PASS\n", file, line, name);
  } catch (const Failure& f) {
    ++g_failures;
    std::printf("%s:%d:%s:FAIL: %s\n", file, f.line, name, f.message.c_str());
  }
}

inline int finish() {
  std::printf("\n-----------------------\n%d Tests %d Failures\n%s\n", g_tests, g_failures, g_failures ? "FAIL" : "OK");
  return g_failures ? 1 : 0;
}

}  // namespace testfw

#define TEST_ASSERT_TRUE(c) \
  do { if (!(c)) testfw::fail(__LINE__, "Expected TRUE: " #c); } while (0)
#define TEST_ASSERT_FALSE(c) \
  do { if (c) testfw::fail(__LINE__, "Expected FALSE: " #c); } while (0)
#define TEST_ASSERT_EQUAL_INT(e, a)                                                                  \
  do {                                                                                               \
    const double e_ = (e), a_ = (a);                                                                 \
    if (e_ != a_) testfw::fail(__LINE__, testfw::format("Expected %.0f, was %.0f", e_, a_));         \
  } while (0)
#define TEST_ASSERT_FLOAT_WITHIN(d, e, a)                                                            \
  do {                                                                                               \
    const double d_ = (d), e_ = (e), a_ = (a);                                                       \
    if (!(std::fabs(a_ - e_) <= d_))                                                                 \
      testfw::fail(__LINE__, testfw::format("Expected %g, was %g", e_, a_) + " (tolerance " +        \
                                 std::to_string(d_) + ")");                                          \
  } while (0)
#define TEST_ASSERT_EQUAL_FLOAT(e, a) TEST_ASSERT_FLOAT_WITHIN(1e-5 * std::fabs((double)(e)) + 1e-6, e, a)
#define TEST_ASSERT_COMPARE_(op, t, a, msg)                                                          \
  do {                                                                                               \
    const double t_ = (t), a_ = (a);                                                                 \
    if (!(a_ op t_)) testfw::fail(__LINE__, testfw::format(msg, t_, a_));                            \
  } while (0)
#define TEST_ASSERT_GREATER_OR_EQUAL_DOUBLE(t, a) TEST_ASSERT_COMPARE_(>=, t, a, "Expected >= %g, was %g")
#define TEST_ASSERT_GREATER_OR_EQUAL_INT(t, a) TEST_ASSERT_COMPARE_(>=, t, a, "Expected >= %.0f, was %.0f")
#define TEST_ASSERT_LESS_OR_EQUAL_INT(t, a) TEST_ASSERT_COMPARE_(<=, t, a, "Expected <= %.0f, was %.0f")
#define TEST_ASSERT_LESS_THAN_DOUBLE(t, a) TEST_ASSERT_COMPARE_(<, t, a, "Expected < %g, was %g")

#define UNITY_BEGIN() ((void)0)
#define RUN_TEST(fn) testfw::run(#fn, fn, __FILE__, __LINE__)
#define UNITY_END() testfw::finish()
