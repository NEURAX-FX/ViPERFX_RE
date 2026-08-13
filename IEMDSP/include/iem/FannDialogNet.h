#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace iem {

constexpr std::size_t kDialogNetInputs = 37;
constexpr std::size_t kDialogNetHidden = 10;
constexpr std::size_t kDialogNetHiddenWeights = 38;
constexpr std::size_t kDialogNetOutputWeights = 11;
constexpr std::size_t kDialogNetConnections = 391;

float SigmoidSymmetricStepwise(float value) noexcept;
float LinearPieceSymmetric(float value) noexcept;

class FannDialogNet {
public:
    bool Prepare() noexcept;
    float Infer(const float features[kDialogNetInputs]) const noexcept;
    bool prepared() const noexcept { return prepared_; }

private:
    bool prepared_ = false;
    std::array<float, kDialogNetConnections> weights_{};
};

} // namespace iem
