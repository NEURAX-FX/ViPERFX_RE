#include "iem/FannDialogNet.h"
#include "IemResourceManifest.h"

#include <cmath>
#include <cstdio>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Near(float left, float right) {
    return std::fabs(left - right) <= 1.0e-6F;
}

float IndependentInfer(const float features[37]) {
    const float *weights = iem::resources::DialogNet().weights;
    float hidden[10]{};
    for (int neuron = 0; neuron < 10; ++neuron) {
        const float *row = weights + neuron * 38;
        float sum = row[37];
        for (int input = 0; input < 37; ++input) {
            sum += features[input] * row[input];
        }
        hidden[neuron] = iem::SigmoidSymmetricStepwise(0.5F * sum);
    }
    const float *output_row = weights + 10 * 38;
    float output_sum = output_row[10];
    for (int neuron = 0; neuron < 10; ++neuron) {
        output_sum += hidden[neuron] * output_row[neuron];
    }
    return iem::LinearPieceSymmetric(0.5F * output_sum);
}

bool TestActivations() {
    return Check(iem::SigmoidSymmetricStepwise(-3.0F) == -1.0F, "stepwise lower clip")
        && Check(iem::SigmoidSymmetricStepwise(3.0F) == 1.0F, "stepwise upper clip")
        && Check(std::abs(iem::SigmoidSymmetricStepwise(0.0F)) < 1.0e-6F,
            "stepwise zero")
        && Check(iem::LinearPieceSymmetric(0.25F) == 0.25F, "linear interior")
        && Check(iem::LinearPieceSymmetric(2.0F) == 1.0F, "linear upper clip")
        && Check(iem::LinearPieceSymmetric(-2.0F) == -1.0F, "linear lower clip");
}

bool TestPreparedInference() {
    float zeros[37]{};
    float halves[37]{};
    for (float &value : halves) value = 0.5F;

    iem::FannDialogNet net;
    if (!Check(net.Prepare() && net.prepared(), "prepare dialog.net")) return false;

    const float zero_out = net.Infer(zeros);
    const float half_out = net.Infer(halves);
    return Check(std::isfinite(zero_out) && zero_out >= -1.0F && zero_out <= 1.0F,
            "zero vector is finite and clipped")
        && Check(Near(zero_out, IndependentInfer(zeros)),
            "zero vector matches independent formula")
        && Check(Near(half_out, IndependentInfer(halves)),
            "half vector matches independent formula");
}

} // namespace

int main() {
    if (!TestActivations()) return 1;
    if (!TestPreparedInference()) return 1;
    std::puts("IEM FANN dialog.net tests passed");
    return 0;
}
