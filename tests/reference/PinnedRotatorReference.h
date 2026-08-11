#pragma once

#include "iem/IemParams.h"

#include <array>
#include <cmath>

namespace iem_test {

inline std::array<std::array<float, 3>, 3> FirstOrderRotation(
    const iem::RotationParams &params
) {
    constexpr double kDegreesToRadians = 0.017453292519943295769;
    const double yaw = params.yaw_centidegrees * 0.01 * kDegreesToRadians
        * (params.invert_yaw ? -1.0 : 1.0);
    const double pitch = params.pitch_centidegrees * 0.01 * kDegreesToRadians
        * (params.invert_pitch ? -1.0 : 1.0);
    const double roll = params.roll_centidegrees * 0.01 * kDegreesToRadians
        * (params.invert_roll ? -1.0 : 1.0);
    const double ca = std::cos(yaw), cb = std::cos(pitch), cy = std::cos(roll);
    const double sa = std::sin(yaw), sb = std::sin(pitch), sy = std::sin(roll);
    double cartesian[3][3]{};
    if (params.sequence == iem::RotationSequence::ROLL_PITCH_YAW) {
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
    if (params.invert_overall) {
        for (int row = 0; row < 3; ++row) {
            for (int column = row + 1; column < 3; ++column) {
                const double value = cartesian[row][column];
                cartesian[row][column] = cartesian[column][row];
                cartesian[column][row] = value;
            }
        }
    }
    return {{
        {{static_cast<float>(cartesian[1][1]), static_cast<float>(cartesian[1][2]),
            static_cast<float>(cartesian[1][0])}},
        {{static_cast<float>(cartesian[2][1]), static_cast<float>(cartesian[2][2]),
            static_cast<float>(cartesian[2][0])}},
        {{static_cast<float>(cartesian[0][1]), static_cast<float>(cartesian[0][2]),
            static_cast<float>(cartesian[0][0])}},
    }};
}

} // namespace iem_test
