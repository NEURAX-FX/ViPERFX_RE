#include "iem/FixedRandom.h"
#include "iem/LinearSmoother.h"
#include "iem/Quaternion.h"
#include "iem/SphericalHarmonics.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace {

constexpr float kHalfPi = 1.5707963267948966F;

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Near(float left, float right, float tolerance = 1.0e-6F) {
    return std::fabs(left - right) <= tolerance;
}

bool TestSphericalHarmonics() {
    std::array<float, iem::kMaxAmbisonicsChannels> coefficients{};
    iem::EvaluateSn3d(3, 0.0F, 0.0F, coefficients.data());
    if (!Check(Near(coefficients[0], 1.0F), "front W coefficient")) return false;
    if (!Check(Near(coefficients[1], 0.0F), "front Y coefficient")) return false;
    if (!Check(Near(coefficients[2], 0.0F), "front Z coefficient")) return false;
    if (!Check(Near(coefficients[3], 1.0F), "front X coefficient")) return false;
    if (!Check(Near(coefficients[6], -0.5F), "front second-order zonal coefficient")) {
        return false;
    }
    if (!Check(Near(coefficients[8], 0.8660254038F),
            "front second-order horizontal coefficient")) return false;
    if (!Check(Near(coefficients[13], -0.6123724357F),
            "front third-order first horizontal coefficient")) return false;
    if (!Check(Near(coefficients[15], 0.7905694150F),
            "front third-order outer coefficient")) return false;

    iem::EvaluateSn3d(1, kHalfPi, 0.0F, coefficients.data());
    if (!Check(Near(coefficients[1], 1.0F) && Near(coefficients[3], 0.0F),
            "left cardinal direction")) return false;
    for (uint32_t channel = 4; channel < iem::kMaxAmbisonicsChannels; ++channel) {
        if (!Check(coefficients[channel] == 0.0F, "clear inactive order channels")) {
            return false;
        }
    }

    iem::EvaluateSn3d(3, -0.73F, 0.41F, coefficients.data());
    for (float coefficient : coefficients) {
        if (!Check(std::isfinite(coefficient), "finite arbitrary SH coefficient")) {
            return false;
        }
    }

    coefficients.fill(1.0F);
    iem::EvaluateSn3d(4, 0.0F, 0.0F, coefficients.data());
    for (float coefficient : coefficients) {
        if (!Check(coefficient == 0.0F, "invalid order clears output")) return false;
    }
    return true;
}

bool TestFixedRandom() {
    iem::FixedRandom left(0x12345678U);
    iem::FixedRandom right(0x12345678U);
    bool observed_nonzero = false;
    for (int index = 0; index < 128; ++index) {
        const uint32_t left_value = left.NextU32();
        if (!Check(left_value == right.NextU32(), "deterministic PRNG sequence")) {
            return false;
        }
        observed_nonzero = observed_nonzero || left_value != 0;
    }
    if (!Check(observed_nonzero, "nonzero PRNG sequence")) return false;

    iem::FixedRandom zero_seed(0);
    for (int index = 0; index < 128; ++index) {
        const float value = zero_seed.NextUnit();
        if (!Check(value >= 0.0F && value < 1.0F, "unit random range")) return false;
    }
    return true;
}

bool TestLinearSmoother() {
    iem::LinearSmoother smoother;
    smoother.Reset(0.0F);
    smoother.SetTarget(1.0F, 4);
    if (!Check(Near(smoother.Next(), 0.25F), "smoother quarter")) return false;
    if (!Check(Near(smoother.Next(), 0.50F), "smoother half")) return false;
    if (!Check(Near(smoother.Next(), 0.75F), "smoother three quarters")) return false;
    if (!Check(Near(smoother.Next(), 1.00F), "smoother target")) return false;
    if (!Check(!smoother.IsSmoothing(), "smoother completion")) return false;
    smoother.SetTarget(-2.0F, 0);
    return Check(Near(smoother.Current(), -2.0F), "zero-frame target is immediate");
}

bool TestQuaternion() {
    const iem::Quaternion quarter_turn = iem::Quaternion::FromYawPitchRoll(
        kHalfPi, 0.0F, 0.0F, iem::RotationSequence::YAW_PITCH_ROLL
    );
    const iem::Vec3 rotated = quarter_turn.Rotate({1.0F, 0.0F, 0.0F});
    if (!Check(Near(rotated.x, 0.0F) && Near(rotated.y, 1.0F)
            && Near(rotated.z, 0.0F), "quarter-turn yaw")) return false;
    const iem::Vec3 restored = quarter_turn.Inverse().Rotate(rotated);
    if (!Check(Near(restored.x, 1.0F) && Near(restored.y, 0.0F)
            && Near(restored.z, 0.0F), "quaternion inverse")) return false;

    const iem::Quaternion first = iem::Quaternion::FromYawPitchRoll(
        0.3F, -0.4F, 0.7F, iem::RotationSequence::YAW_PITCH_ROLL
    );
    const iem::Quaternion second = iem::Quaternion::FromYawPitchRoll(
        0.3F, -0.4F, 0.7F, iem::RotationSequence::ROLL_PITCH_YAW
    );
    const iem::Vec3 first_vector = first.Rotate({1.0F, 0.0F, 0.0F});
    const iem::Vec3 second_vector = second.Rotate({1.0F, 0.0F, 0.0F});
    return Check(!Near(first_vector.x, second_vector.x, 1.0e-4F)
            || !Near(first_vector.y, second_vector.y, 1.0e-4F)
            || !Near(first_vector.z, second_vector.z, 1.0e-4F),
        "rotation sequences differ");
}

} // namespace

int main() {
    if (!TestSphericalHarmonics()) return 1;
    if (!TestFixedRandom()) return 1;
    if (!TestLinearSmoother()) return 1;
    if (!TestQuaternion()) return 1;
    std::puts("IEM spherical harmonics and utility tests passed");
    return 0;
}
