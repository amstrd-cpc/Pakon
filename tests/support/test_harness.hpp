#pragma once

// Minimal test harness: no external dependencies, registered test cases,
// failure collection, CTest-friendly exit code.

#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <type_traits>
#include <vector>

namespace pakon::test {

// Best-effort value rendering for failure messages; universal fallback
// keeps EXPECT_EQ usable with types that have no std::formatter.
template <typename T>
std::string dbg(const T& value) {
    if constexpr (std::is_integral_v<T>) {
        if constexpr (sizeof(T) == 1) {
            return std::format("0x{:02x}", static_cast<unsigned>(value));
        } else {
            return std::format("{}", value);
        }
    } else if constexpr (std::is_same_v<T, std::string>) {
        return value;
    } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
        std::string out;
        for (const auto b : value) {
            out += std::format("{:02x}", b);
        }
        return out;
    } else {
        return "<value>";
    }
}

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failure_count() {
    static int count = 0;
    return count;
}

inline const char*& current_test() {
    static const char* name = "";
    return name;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report_failure(const char* file, int line, const std::string& what) {
    ++failure_count();
    std::fprintf(stderr, "FAIL %s (%s:%d): %s\n", current_test(), file, line,
                 what.c_str());
}

inline int run_all() {
    int failed_tests = 0;
    for (const auto& test : registry()) {
        current_test() = test.name;
        const int before = failure_count();
        test.fn();
        if (failure_count() != before) {
            ++failed_tests;
        } else {
            std::printf("PASS %s\n", test.name);
        }
    }
    std::printf("%zu tests, %d failed\n", registry().size(), failed_tests);
    return failed_tests == 0 ? 0 : 1;
}

} // namespace pakon::test

#define PAKON_TEST(name)                                              \
    static void pakon_test_##name();                                  \
    static ::pakon::test::Registrar pakon_registrar_##name{#name,     \
                                                           &pakon_test_##name}; \
    static void pakon_test_##name()

#define EXPECT(cond)                                                              \
    do {                                                                          \
        if (!(cond)) {                                                            \
            ::pakon::test::report_failure(__FILE__, __LINE__,                     \
                                          "expected: " #cond);                    \
        }                                                                         \
    } while (0)

#define EXPECT_EQ(a, b)                                                           \
    do {                                                                          \
        const auto& va = (a);                                                     \
        const auto& vb = (b);                                                     \
        if (!(va == vb)) {                                                        \
            ::pakon::test::report_failure(                                        \
                __FILE__, __LINE__,                                               \
                std::format("{} == {} (got {} vs {})", #a, #b,                    \
                            ::pakon::test::dbg(va), ::pakon::test::dbg(vb)));     \
        }                                                                         \
    } while (0)
