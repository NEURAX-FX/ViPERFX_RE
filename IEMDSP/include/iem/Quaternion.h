#pragma once

#include "iem/IemParams.h"

#include <cmath>

namespace iem {

struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

class Quaternion {
public:
    constexpr Quaternion() noexcept = default;
    constexpr Quaternion(float w, float x, float y, float z) noexcept
        : w(w), x(x), y(y), z(z) {}

    static Quaternion FromAxisAngle(Vec3 axis, float radians) noexcept {
        const float half_angle = radians * 0.5F;
        const float sine = std::sin(half_angle);
        return Quaternion{
            std::cos(half_angle), axis.x * sine, axis.y * sine, axis.z * sine
        };
    }

    static Quaternion FromYawPitchRoll(
        float yaw,
        float pitch,
        float roll,
        RotationSequence sequence
    ) noexcept {
        const Quaternion yaw_rotation = FromAxisAngle({0.0F, 0.0F, 1.0F}, yaw);
        const Quaternion pitch_rotation = FromAxisAngle({0.0F, 1.0F, 0.0F}, pitch);
        const Quaternion roll_rotation = FromAxisAngle({1.0F, 0.0F, 0.0F}, roll);
        const Quaternion result = sequence == RotationSequence::ROLL_PITCH_YAW
            ? yaw_rotation * pitch_rotation * roll_rotation
            : roll_rotation * pitch_rotation * yaw_rotation;
        return result.Normalized();
    }

    Quaternion operator*(const Quaternion &right) const noexcept {
        return {
            w * right.w - x * right.x - y * right.y - z * right.z,
            w * right.x + x * right.w + y * right.z - z * right.y,
            w * right.y - x * right.z + y * right.w + z * right.x,
            w * right.z + x * right.y - y * right.x + z * right.w,
        };
    }

    Quaternion Conjugate() const noexcept {
        return {w, -x, -y, -z};
    }

    Quaternion Normalized() const noexcept {
        const float magnitude_squared = w * w + x * x + y * y + z * z;
        if (!(magnitude_squared > 0.0F) || !std::isfinite(magnitude_squared)) {
            return {};
        }
        const float inverse_magnitude = 1.0F / std::sqrt(magnitude_squared);
        return {
            w * inverse_magnitude,
            x * inverse_magnitude,
            y * inverse_magnitude,
            z * inverse_magnitude,
        };
    }

    Quaternion Inverse() const noexcept {
        const float magnitude_squared = w * w + x * x + y * y + z * z;
        if (!(magnitude_squared > 0.0F) || !std::isfinite(magnitude_squared)) {
            return {};
        }
        const Quaternion conjugate = Conjugate();
        return {
            conjugate.w / magnitude_squared,
            conjugate.x / magnitude_squared,
            conjugate.y / magnitude_squared,
            conjugate.z / magnitude_squared,
        };
    }

    Vec3 Rotate(Vec3 vector) const noexcept {
        const Quaternion normalized = Normalized();
        const Quaternion rotated = normalized
            * Quaternion{0.0F, vector.x, vector.y, vector.z}
            * normalized.Conjugate();
        return {rotated.x, rotated.y, rotated.z};
    }

    float w = 1.0F;
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

} // namespace iem
