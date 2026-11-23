#pragma once

#include <gtest/gtest.h>

template<typename TFn>
void RegisterTest(const char* suite, const char* name, TFn fn) {
    class TFnTest : public testing::Test {
    public:
        explicit TFnTest(TFn fn)
            : Fn(std::move(fn))
        { }

        void TestBody() override {
            Fn();
        }

    private:
        TFn Fn;
    };

    testing::RegisterTest(suite, name, nullptr, nullptr, __FILE__, __LINE__, [fn]() -> testing::Test* {
        return new TFnTest(fn);
    });
}

#define ADD_TEST(f, n, ...) f(n, ##__VA_ARGS__)

#define unit_test(f) RegisterTest(TEST_SUITE_NAME, #f, f)
