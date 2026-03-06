#include <gtest/gtest.h>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <vector>

/**
 * Tests for the mic boost (gain) processing logic.
 *
 * These test the same PCM gain math used in CapturePipeline::apply_gain
 * without requiring ALSA or a full pipeline instantiation.
 */

namespace {

// Mirrors the gain application logic from capture_pipeline.cpp
void apply_gain_16(const int16_t* src, int16_t* dst, size_t count, double gain) {
    for (size_t i = 0; i < count; ++i) {
        int32_t s = static_cast<int32_t>(src[i] * gain);
        dst[i] = static_cast<int16_t>(std::clamp(s, (int32_t)-32768, (int32_t)32767));
    }
}

void apply_gain_32(const int32_t* src, int32_t* dst, size_t count, double gain) {
    for (size_t i = 0; i < count; ++i) {
        int64_t s = static_cast<int64_t>(static_cast<int64_t>(src[i]) * gain);
        dst[i] = static_cast<int32_t>(std::clamp(s, (int64_t)INT32_MIN, (int64_t)INT32_MAX));
    }
}

double db_to_linear(double db) {
    return std::pow(10.0, db / 20.0);
}

} // anonymous namespace

TEST(MicBoostTest, UnityGainPassthrough) {
    std::vector<int16_t> src = {0, 1000, -1000, 32767, -32768};
    std::vector<int16_t> dst(src.size());

    apply_gain_16(src.data(), dst.data(), src.size(), 1.0);

    for (size_t i = 0; i < src.size(); ++i) {
        EXPECT_EQ(dst[i], src[i]);
    }
}

TEST(MicBoostTest, SixDbDoublesAmplitude) {
    double gain = db_to_linear(6.0);
    EXPECT_NEAR(gain, 2.0, 0.01);

    std::vector<int16_t> src = {1000, -1000, 100, -100};
    std::vector<int16_t> dst(src.size());

    apply_gain_16(src.data(), dst.data(), src.size(), gain);

    EXPECT_NEAR(dst[0], 2000, 5);
    EXPECT_NEAR(dst[1], -2000, 5);
    EXPECT_NEAR(dst[2], 200, 2);
    EXPECT_NEAR(dst[3], -200, 2);
}

TEST(MicBoostTest, TwentyDbTenXAmplitude) {
    double gain = db_to_linear(20.0);
    EXPECT_NEAR(gain, 10.0, 0.01);

    int16_t src = 3000;
    int16_t dst;
    apply_gain_16(&src, &dst, 1, gain);

    EXPECT_EQ(dst, 30000);
}

TEST(MicBoostTest, ClampingPreventsOverflow16) {
    double gain = db_to_linear(20.0); // 10x

    // 10000 * 10 = 100000 > 32767 → should clamp
    int16_t src_pos = 10000;
    int16_t dst;
    apply_gain_16(&src_pos, &dst, 1, gain);
    EXPECT_EQ(dst, 32767);

    // -10000 * 10 = -100000 < -32768 → should clamp
    int16_t src_neg = -10000;
    apply_gain_16(&src_neg, &dst, 1, gain);
    EXPECT_EQ(dst, -32768);
}

TEST(MicBoostTest, ClampingPreventsOverflow32) {
    double gain = db_to_linear(20.0); // 10x

    int32_t src_pos = 300000000; // * 10 > INT32_MAX
    int32_t dst;
    apply_gain_32(&src_pos, &dst, 1, gain);
    EXPECT_EQ(dst, INT32_MAX);

    int32_t src_neg = -300000000;
    apply_gain_32(&src_neg, &dst, 1, gain);
    EXPECT_EQ(dst, INT32_MIN);
}

TEST(MicBoostTest, NegativeGainAttenuates) {
    double gain = db_to_linear(-6.0);
    EXPECT_NEAR(gain, 0.5, 0.01);

    int16_t src = 20000;
    int16_t dst;
    apply_gain_16(&src, &dst, 1, gain);

    EXPECT_NEAR(dst, 10000, 50);
}

TEST(MicBoostTest, ZeroDbIsUnity) {
    double gain = db_to_linear(0.0);
    EXPECT_DOUBLE_EQ(gain, 1.0);
}

TEST(MicBoostTest, SilenceRemainsZero) {
    std::vector<int16_t> src(100, 0);
    std::vector<int16_t> dst(100);

    apply_gain_16(src.data(), dst.data(), src.size(), db_to_linear(40.0));

    for (auto s : dst) {
        EXPECT_EQ(s, 0);
    }
}
