#pragma once

// Minimal dependency-free test harness for the simulation core.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace summit::test {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& FailureCount() {
  static int failures = 0;
  return failures;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

inline int RunAll() {
  int failedTests = 0;
  for (const auto& t : Registry()) {
    const int before = FailureCount();
    t.fn();
    const bool ok = FailureCount() == before;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
    if (!ok) ++failedTests;
  }
  std::printf("\n%zu tests, %d failed\n", Registry().size(), failedTests);
  return failedTests == 0 ? 0 : 1;
}

}  // namespace summit::test

#define SUMMIT_CAT2(a, b) a##b
#define SUMMIT_CAT(a, b) SUMMIT_CAT2(a, b)

#define TEST_CASE(name)                                                                  \
  static void SUMMIT_CAT(test_fn_, __LINE__)();                                          \
  static ::summit::test::Registrar SUMMIT_CAT(test_reg_, __LINE__)(name, &SUMMIT_CAT(test_fn_, __LINE__)); \
  static void SUMMIT_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      std::printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);             \
      ++::summit::test::FailureCount();                                                  \
    }                                                                                    \
  } while (0)

#define CHECK_NEAR(actual, expected, tol)                                                \
  do {                                                                                   \
    const double a_ = (actual), e_ = (expected), t_ = (tol);                             \
    if (!(std::fabs(a_ - e_) <= t_)) {                                                   \
      std::printf("  %s:%d: CHECK_NEAR(%s) got %.6g expected %.6g +/- %.3g\n", __FILE__, \
                  __LINE__, #actual, a_, e_, t_);                                        \
      ++::summit::test::FailureCount();                                                  \
    }                                                                                    \
  } while (0)

#define CHECK_RANGE(actual, lo, hi)                                                      \
  do {                                                                                   \
    const double a_ = (actual), l_ = (lo), h_ = (hi);                                    \
    if (!(a_ >= l_ && a_ <= h_)) {                                                       \
      std::printf("  %s:%d: CHECK_RANGE(%s) got %.6g expected [%.6g, %.6g]\n", __FILE__,  \
                  __LINE__, #actual, a_, l_, h_);                                        \
      ++::summit::test::FailureCount();                                                  \
    }                                                                                    \
  } while (0)
