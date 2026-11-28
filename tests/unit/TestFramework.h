#pragma once
// Minimal self-registering test framework for headless engine unit tests.
//
//   TEST_CASE("Clock consumes whole steps") { CHECK(x == 1); CHECK_NEAR(a, b, 1e-9); }
//
// Each failed CHECK prints file:line and the expression; the process exit code is the number of
// failed test cases (0 = success), so ctest treats any failure as a failed test.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace PhisiksTest {
    struct TestCase { const char* name; std::function<void()> fn; };

    inline std::vector<TestCase>& Registry() { static std::vector<TestCase> tests; return tests; }
    inline int& CurrentFailures() { static int failures = 0; return failures; }

    struct Registrar {
        Registrar(const char* name, std::function<void()> fn) { Registry().push_back({ name, std::move(fn) }); }
    };

    inline void ReportFailure(const char* file, int line, const std::string& message) {
        std::printf("    FAILED %s:%d: %s\n", file, line, message.c_str());
        ++CurrentFailures();
    }

    inline int RunAll(const char* filter = nullptr) {
        int failedCases = 0, ran = 0;
        for (const TestCase& test : Registry()) {
            if (filter && std::string(test.name).find(filter) == std::string::npos) continue;
            CurrentFailures() = 0;
            ++ran;
            try { test.fn(); }
            catch (const std::exception& e) { ReportFailure(__FILE__, __LINE__, std::string("exception: ") + e.what()); }
            catch (...) { ReportFailure(__FILE__, __LINE__, "unknown exception"); }
            std::printf("[%s] %s\n", CurrentFailures() ? "FAIL" : " OK ", test.name);
            if (CurrentFailures()) ++failedCases;
        }
        std::printf("\n%d test case(s), %d failed\n", ran, failedCases);
        return failedCases;
    }
}

#define PHISIKS_TEST_CONCAT_INNER(a, b) a##b
#define PHISIKS_TEST_CONCAT(a, b) PHISIKS_TEST_CONCAT_INNER(a, b)
#define TEST_CASE(name)                                                                        \
    static void PHISIKS_TEST_CONCAT(phisiksTest_, __LINE__)();                                       \
    static ::PhisiksTest::Registrar PHISIKS_TEST_CONCAT(phisiksTestReg_, __LINE__)(name, PHISIKS_TEST_CONCAT(phisiksTest_, __LINE__)); \
    static void PHISIKS_TEST_CONCAT(phisiksTest_, __LINE__)()

#define CHECK(expr) do { if (!(expr)) ::PhisiksTest::ReportFailure(__FILE__, __LINE__, #expr); } while (0)
#define CHECK_NEAR(a, b, eps) do { const double phisiksA = double(a), phisiksB = double(b);                  \
    if (!(std::fabs(phisiksA - phisiksB) <= double(eps)))                                                     \
        ::PhisiksTest::ReportFailure(__FILE__, __LINE__, std::string(#a " ~= " #b "  (") + std::to_string(phisiksA) + " vs " + std::to_string(phisiksB) + ")"); } while (0)
