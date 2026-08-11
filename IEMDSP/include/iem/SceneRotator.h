#pragma once

#include "iem/IemEncoder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace iem {

class SceneRotator {
public:
    bool Prepare(const EncoderConfig &config);
    void ApplyParams(const IemParams &params) noexcept;
    void ResetAngles() noexcept;
    void Reset() noexcept;
    bool Process(
        const float *const input[kMaxAmbisonicsChannels],
        float *const output[kMaxAmbisonicsChannels],
        std::size_t frames
    ) noexcept;

    uint64_t MatrixRecomputeCountForTest() const noexcept {
        return matrix_recompute_count_;
    }

private:
    using Matrix = std::array<std::array<float, 7>, 7>;
    using OrderMatrices = std::array<Matrix, 4>;

    static double P(
        int i,
        int l,
        int a,
        int b,
        const Matrix &order_one,
        const Matrix &previous
    ) noexcept;
    static double U(
        int l,
        int m,
        int n,
        const Matrix &order_one,
        const Matrix &previous
    ) noexcept;
    static double V(
        int l,
        int m,
        int n,
        const Matrix &order_one,
        const Matrix &previous
    ) noexcept;
    static double W(
        int l,
        int m,
        int n,
        const Matrix &order_one,
        const Matrix &previous
    ) noexcept;

    void CalculateTarget() noexcept;
    static bool SameRotation(const RotationParams &left, const RotationParams &right) noexcept;

    EncoderConfig config_{};
    RotationParams params_{};
    OrderMatrices current_{};
    OrderMatrices target_{};
    std::vector<float> scratch_{};
    uint64_t matrix_recompute_count_ = 0;
    bool prepared_ = false;
    bool first_apply_ = true;
};

} // namespace iem
