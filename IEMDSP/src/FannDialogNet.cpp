#include "iem/FannDialogNet.h"

#include "IemResourceManifest.h"

#include <algorithm>
#include <cmath>

namespace iem {

namespace {

constexpr float kStepwiseX[] = {
    -2.6466529369354248F,
    -1.4722193479537964F,
    -0.5493061542510986F,
    0.5493061542510986F,
    1.4722193479537964F,
    2.6466529369354248F,
};

constexpr float kStepwiseY[] = {
    -0.9900000095367432F,
    -0.8999999761581421F,
    -0.5F,
    0.5F,
    0.8999999761581421F,
    0.9900000095367432F,
};

} // namespace

float SigmoidSymmetricStepwise(float value) noexcept {
    if (value < kStepwiseX[0]) return -1.0F;
    if (value >= kStepwiseX[5]) return 1.0F;
    for (int index = 0; index < 5; ++index) {
        if (value < kStepwiseX[index + 1]) {
            const float span = kStepwiseX[index + 1] - kStepwiseX[index];
            const float mix = (value - kStepwiseX[index]) / span;
            return kStepwiseY[index] + (kStepwiseY[index + 1] - kStepwiseY[index]) * mix;
        }
    }
    return 1.0F;
}

float LinearPieceSymmetric(float value) noexcept {
    return std::clamp(value, -1.0F, 1.0F);
}

bool FannDialogNet::Prepare() noexcept {
    const auto &net = resources::DialogNet();
    if (net.connection_count != kDialogNetConnections || net.weights == nullptr) {
        prepared_ = false;
        return false;
    }
    std::copy(net.weights, net.weights + kDialogNetConnections, weights_.begin());
    prepared_ = true;
    return true;
}

float FannDialogNet::Infer(const float features[kDialogNetInputs]) const noexcept {
    if (!prepared_) return 0.0F;
    float hidden[kDialogNetHidden]{};
    for (std::size_t neuron = 0; neuron < kDialogNetHidden; ++neuron) {
        const float *row = weights_.data() + neuron * kDialogNetHiddenWeights;
        float sum = row[kDialogNetInputs];
        for (std::size_t input = 0; input < kDialogNetInputs; ++input) {
            sum += features[input] * row[input];
        }
        hidden[neuron] = SigmoidSymmetricStepwise(0.5F * sum);
    }
    const float *output_row = weights_.data() + kDialogNetHidden * kDialogNetHiddenWeights;
    float output_sum = output_row[kDialogNetHidden];
    for (std::size_t neuron = 0; neuron < kDialogNetHidden; ++neuron) {
        output_sum += hidden[neuron] * output_row[neuron];
    }
    return LinearPieceSymmetric(0.5F * output_sum);
}

} // namespace iem
