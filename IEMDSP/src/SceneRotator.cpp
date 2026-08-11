#include "iem/SceneRotator.h"

#include <algorithm>
#include <cmath>

namespace iem {

namespace {

constexpr double kDegreesToRadians = 0.017453292519943295769;

} // namespace

bool SceneRotator::Prepare(const EncoderConfig &config) {
    if (config.sample_rate == 0 || config.max_frames == 0
        || config.order == 0 || config.order > kMaxAmbisonicsOrder) {
        return false;
    }
    config_ = config;
    scratch_.assign(
        static_cast<std::size_t>(AmbisonicsChannelCount(config.order)) * config.max_frames,
        0.0F
    );
    params_ = RotationParams{};
    prepared_ = true;
    first_apply_ = true;
    CalculateTarget();
    current_ = target_;
    return true;
}

void SceneRotator::ApplyParams(const IemParams &params) noexcept {
    if (SameRotation(params_, params.rotation)) {
        if (first_apply_) first_apply_ = false;
        return;
    }
    params_ = params.rotation;
    CalculateTarget();
    if (first_apply_) {
        current_ = target_;
        first_apply_ = false;
    }
}

void SceneRotator::ResetAngles() noexcept {
    params_.yaw_centidegrees = 0;
    params_.pitch_centidegrees = 0;
    params_.roll_centidegrees = 0;
    CalculateTarget();
}

void SceneRotator::Reset() noexcept {
    current_ = target_;
}

bool SceneRotator::Process(
    const float *const input[kMaxAmbisonicsChannels],
    float *const output[kMaxAmbisonicsChannels],
    std::size_t frames
) noexcept {
    const uint32_t channels = AmbisonicsChannelCount(config_.order);
    if (!prepared_ || frames > config_.max_frames || input == nullptr || output == nullptr) {
        return false;
    }
    for (uint32_t channel = 0; channel < channels; ++channel) {
        if (input[channel] == nullptr || output[channel] == nullptr) return false;
        std::copy_n(
            input[channel],
            frames,
            scratch_.data() + static_cast<std::size_t>(channel) * config_.max_frames
        );
    }
    for (uint32_t channel = channels; channel < kMaxAmbisonicsChannels; ++channel) {
        if (output[channel] != nullptr) std::fill_n(output[channel], frames, 0.0F);
    }
    if (frames == 0) return true;

    std::copy_n(scratch_.data(), frames, output[0]);
    for (uint32_t degree = 1; degree <= config_.order; ++degree) {
        const uint32_t offset = degree * degree;
        const uint32_t degree_channels = degree * 2U + 1U;
        for (uint32_t row = 0; row < degree_channels; ++row) {
            float *destination = output[offset + row];
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const float amount = static_cast<float>(frame + 1U)
                    / static_cast<float>(frames);
                float sample = 0.0F;
                for (uint32_t column = 0; column < degree_channels; ++column) {
                    const float coefficient = current_[degree][row][column]
                        + (target_[degree][row][column]
                            - current_[degree][row][column]) * amount;
                    sample += scratch_[
                        static_cast<std::size_t>(offset + column) * config_.max_frames + frame
                    ] * coefficient;
                }
                destination[frame] = sample;
            }
        }
    }
    current_ = target_;
    return true;
}

double SceneRotator::P(
    int i,
    int l,
    int a,
    int b,
    const Matrix &order_one,
    const Matrix &previous
) noexcept {
    const double positive = order_one[static_cast<std::size_t>(i + 1)][2];
    const double negative = order_one[static_cast<std::size_t>(i + 1)][0];
    const double zero = order_one[static_cast<std::size_t>(i + 1)][1];
    const std::size_t row = static_cast<std::size_t>(a + l - 1);
    if (b == -l) {
        return positive * previous[row][0]
            + negative * previous[row][static_cast<std::size_t>(2 * l - 2)];
    }
    if (b == l) {
        return positive * previous[row][static_cast<std::size_t>(2 * l - 2)]
            - negative * previous[row][0];
    }
    return zero * previous[row][static_cast<std::size_t>(b + l - 1)];
}

double SceneRotator::U(
    int l,
    int m,
    int n,
    const Matrix &order_one,
    const Matrix &previous
) noexcept {
    return P(0, l, m, n, order_one, previous);
}

double SceneRotator::V(
    int l,
    int m,
    int n,
    const Matrix &order_one,
    const Matrix &previous
) noexcept {
    if (m == 0) {
        return P(1, l, 1, n, order_one, previous)
            + P(-1, l, -1, n, order_one, previous);
    }
    if (m > 0) {
        const double first = P(1, l, m - 1, n, order_one, previous);
        return m == 1
            ? first * std::sqrt(2.0)
            : first - P(-1, l, 1 - m, n, order_one, previous);
    }
    const double first = P(-1, l, -m - 1, n, order_one, previous);
    return m == -1
        ? first * std::sqrt(2.0)
        : first + P(1, l, m + 1, n, order_one, previous);
}

double SceneRotator::W(
    int l,
    int m,
    int n,
    const Matrix &order_one,
    const Matrix &previous
) noexcept {
    if (m > 0) {
        return P(1, l, m + 1, n, order_one, previous)
            + P(-1, l, -m - 1, n, order_one, previous);
    }
    if (m < 0) {
        return P(1, l, m - 1, n, order_one, previous)
            - P(-1, l, 1 - m, n, order_one, previous);
    }
    return 0.0;
}

void SceneRotator::CalculateTarget() noexcept {
    const double yaw = static_cast<double>(params_.yaw_centidegrees)
        * 0.01 * kDegreesToRadians * (params_.invert_yaw ? -1.0 : 1.0);
    const double pitch = static_cast<double>(params_.pitch_centidegrees)
        * 0.01 * kDegreesToRadians * (params_.invert_pitch ? -1.0 : 1.0);
    const double roll = static_cast<double>(params_.roll_centidegrees)
        * 0.01 * kDegreesToRadians * (params_.invert_roll ? -1.0 : 1.0);
    const double ca = std::cos(yaw);
    const double cb = std::cos(pitch);
    const double cy = std::cos(roll);
    const double sa = std::sin(yaw);
    const double sb = std::sin(pitch);
    const double sy = std::sin(roll);
    double cartesian[3][3]{};

    if (params_.sequence == RotationSequence::ROLL_PITCH_YAW) {
        cartesian[0][0] = ca * cb;
        cartesian[1][0] = sa * cb;
        cartesian[2][0] = -sb;
        cartesian[0][1] = ca * sb * sy - sa * cy;
        cartesian[1][1] = sa * sb * sy + ca * cy;
        cartesian[2][1] = cb * sy;
        cartesian[0][2] = ca * sb * cy + sa * sy;
        cartesian[1][2] = sa * sb * cy - ca * sy;
        cartesian[2][2] = cb * cy;
    } else {
        cartesian[0][0] = ca * cb;
        cartesian[1][0] = sa * cy + ca * sb * sy;
        cartesian[2][0] = sa * sy - ca * sb * cy;
        cartesian[0][1] = -sa * cb;
        cartesian[1][1] = ca * cy - sa * sb * sy;
        cartesian[2][1] = ca * sy + sa * sb * cy;
        cartesian[0][2] = sb;
        cartesian[1][2] = -cb * sy;
        cartesian[2][2] = cb * cy;
    }
    if (params_.invert_overall) {
        for (int row = 0; row < 3; ++row) {
            for (int column = row + 1; column < 3; ++column) {
                std::swap(cartesian[row][column], cartesian[column][row]);
            }
        }
    }

    target_ = {};
    target_[0][0][0] = 1.0F;
    Matrix &order_one = target_[1];
    order_one[0][0] = static_cast<float>(cartesian[1][1]);
    order_one[0][1] = static_cast<float>(cartesian[1][2]);
    order_one[0][2] = static_cast<float>(cartesian[1][0]);
    order_one[1][0] = static_cast<float>(cartesian[2][1]);
    order_one[1][1] = static_cast<float>(cartesian[2][2]);
    order_one[1][2] = static_cast<float>(cartesian[2][0]);
    order_one[2][0] = static_cast<float>(cartesian[0][1]);
    order_one[2][1] = static_cast<float>(cartesian[0][2]);
    order_one[2][2] = static_cast<float>(cartesian[0][0]);

    for (int l = 2; l <= static_cast<int>(config_.order); ++l) {
        Matrix &matrix = target_[static_cast<std::size_t>(l)];
        const Matrix &previous = target_[static_cast<std::size_t>(l - 1)];
        for (int m = -l; m <= l; ++m) {
            for (int n = -l; n <= l; ++n) {
                const int delta = m == 0 ? 1 : 0;
                const double denominator = std::abs(n) == l
                    ? static_cast<double>((2 * l) * (2 * l - 1))
                    : static_cast<double>(l * l - n * n);
                double u = std::sqrt(static_cast<double>(l * l - m * m) / denominator);
                double v = std::sqrt(
                    (1.0 + delta) * (l + std::abs(m) - 1.0)
                        * (l + std::abs(m)) / denominator
                ) * (1.0 - 2.0 * delta) * 0.5;
                double w = std::sqrt(
                    (l - std::abs(m) - 1.0) * (l - std::abs(m)) / denominator
                ) * (1.0 - delta) * -0.5;
                if (u != 0.0) u *= U(l, m, n, order_one, previous);
                if (v != 0.0) v *= V(l, m, n, order_one, previous);
                if (w != 0.0) w *= W(l, m, n, order_one, previous);
                matrix[static_cast<std::size_t>(m + l)][static_cast<std::size_t>(n + l)]
                    = static_cast<float>(u + v + w);
            }
        }
    }
    ++matrix_recompute_count_;
}

bool SceneRotator::SameRotation(
    const RotationParams &left,
    const RotationParams &right
) noexcept {
    return left.yaw_centidegrees == right.yaw_centidegrees
        && left.pitch_centidegrees == right.pitch_centidegrees
        && left.roll_centidegrees == right.roll_centidegrees
        && left.invert_yaw == right.invert_yaw
        && left.invert_pitch == right.invert_pitch
        && left.invert_roll == right.invert_roll
        && left.invert_overall == right.invert_overall
        && left.sequence == right.sequence;
}

} // namespace iem
