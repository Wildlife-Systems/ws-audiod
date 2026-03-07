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

// ---------------------------------------------------------------------------
// DC offset removal tests
// ---------------------------------------------------------------------------

namespace {

// Mirrors the DC removal IIR from capture_pipeline.cpp
// y[n] = x[n] - x[n-1] + alpha * y[n-1]
constexpr float DC_ALPHA = 0.999f;

void apply_dc_remove_16(int16_t* data, size_t frame_count,
                         uint16_t channels,
                         std::vector<float>& prev_x,
                         std::vector<float>& prev_y) {
    for (size_t f = 0; f < frame_count; ++f) {
        for (uint16_t ch = 0; ch < channels; ++ch) {
            size_t idx = f * channels + ch;
            float x = data[idx];
            float y = x - prev_x[ch] + DC_ALPHA * prev_y[ch];
            prev_x[ch] = x;
            prev_y[ch] = y;
            data[idx] = static_cast<int16_t>(
                std::clamp(static_cast<int32_t>(y), (int32_t)-32768, (int32_t)32767));
        }
    }
}

} // anonymous namespace

TEST(DcRemoveTest, RemovesDcOffset) {
    // Signal: constant DC of 1000 — need enough samples for IIR to converge
    const size_t N = 8000;
    std::vector<int16_t> data(N, 1000);
    std::vector<float> px(1, 0.0f), py(1, 0.0f);

    apply_dc_remove_16(data.data(), N, 1, px, py);

    // After convergence, output should be near zero
    // Check last 100 samples are close to 0
    for (size_t i = N - 100; i < N; ++i) {
        EXPECT_NEAR(data[i], 0, 5);
    }
}

TEST(DcRemoveTest, PreservesAcSignal) {
    // Generate a 1 kHz sine at 48 kHz, 1 second
    const size_t N = 48000;
    std::vector<int16_t> original(N);
    for (size_t i = 0; i < N; ++i) {
        original[i] = static_cast<int16_t>(
            16000.0 * std::sin(2.0 * M_PI * 1000.0 * i / 48000.0));
    }

    std::vector<int16_t> filtered = original;
    std::vector<float> px(1, 0.0f), py(1, 0.0f);
    apply_dc_remove_16(filtered.data(), N, 1, px, py);

    // After settling (skip first 500 samples), amplitude should be preserved
    double sum_orig = 0, sum_filt = 0;
    for (size_t i = 500; i < N; ++i) {
        sum_orig += static_cast<double>(original[i]) * original[i];
        sum_filt += static_cast<double>(filtered[i]) * filtered[i];
    }
    double rms_orig = std::sqrt(sum_orig / (N - 500));
    double rms_filt = std::sqrt(sum_filt / (N - 500));

    // RMS should be within 1% — filter barely affects 1 kHz
    EXPECT_NEAR(rms_filt / rms_orig, 1.0, 0.01);
}

TEST(DcRemoveTest, RemovesOffsetFromAcSignal) {
    // Sine + DC offset
    const size_t N = 48000;
    std::vector<int16_t> data(N);
    for (size_t i = 0; i < N; ++i) {
        data[i] = static_cast<int16_t>(
            5000 + 10000.0 * std::sin(2.0 * M_PI * 500.0 * i / 48000.0));
    }

    std::vector<float> px(1, 0.0f), py(1, 0.0f);
    apply_dc_remove_16(data.data(), N, 1, px, py);

    // Mean of last half should be near zero (DC removed)
    double sum = 0;
    for (size_t i = N / 2; i < N; ++i) {
        sum += data[i];
    }
    double mean = sum / (N / 2);
    EXPECT_NEAR(mean, 0.0, 20.0);
}

TEST(DcRemoveTest, SilencePassesThrough) {
    std::vector<int16_t> data(1000, 0);
    std::vector<float> px(1, 0.0f), py(1, 0.0f);

    apply_dc_remove_16(data.data(), 1000, 1, px, py);

    for (auto s : data) {
        EXPECT_EQ(s, 0);
    }
}

TEST(DcRemoveTest, StereoIndependentChannels) {
    // Ch0: DC offset 2000, Ch1: zero — need enough frames for IIR convergence
    const size_t frames = 8000;
    std::vector<int16_t> data(frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        data[f * 2 + 0] = 2000; // ch0
        data[f * 2 + 1] = 0;    // ch1
    }

    std::vector<float> px(2, 0.0f), py(2, 0.0f);
    apply_dc_remove_16(data.data(), frames, 2, px, py);

    // Ch0 should converge toward 0, ch1 should stay 0
    for (size_t f = frames - 50; f < frames; ++f) {
        EXPECT_NEAR(data[f * 2 + 0], 0, 10);
        EXPECT_EQ(data[f * 2 + 1], 0);
    }
}
