#include "iem/HaloBed.h"
#include "iem/HaloEncoder.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestCentreEncodingAndFold() {
    constexpr std::size_t kFrames = 1;
    float storage[7][kFrames]{};
    const float *bed[7]{};
    for (int channel = 0; channel < 7; ++channel) bed[channel] = storage[channel];
    storage[static_cast<int>(iem::HaloBedChannel::C)][0] = 1.0F;

    float ambi_storage[iem::kMaxAmbisonicsChannels][kFrames]{};
    float *ambi[iem::kMaxAmbisonicsChannels]{};
    for (uint32_t channel = 0; channel < iem::kMaxAmbisonicsChannels; ++channel) {
        ambi[channel] = ambi_storage[channel];
    }
    iem::EncodeHaloBedToSn3d(1, bed, ambi, kFrames);
    if (!Check(std::fabs(ambi[0][0]) > 0.1F, "centre contributes W")) return false;
    if (!Check(std::fabs(ambi[1][0]) < 1.0e-5F, "centre has no Y")) return false;
    if (!Check(std::fabs(ambi[2][0]) < 1.0e-5F, "centre has no Z")) return false;

    float stereo_storage[2][kFrames]{};
    float *stereo[2]{stereo_storage[0], stereo_storage[1]};
    iem::FoldHaloBedToStereo(bed, stereo, kFrames);
    constexpr float kCentre = 0.7071067811865476F;
    return Check(std::fabs(stereo[0][0] - kCentre) < 1.0e-6F,
            "centre folds equally left")
        && Check(std::fabs(stereo[1][0] - kCentre) < 1.0e-6F,
            "centre folds equally right");
}

bool TestLeftDirection() {
    float storage[7][1]{};
    const float *bed[7]{};
    for (int channel = 0; channel < 7; ++channel) bed[channel] = storage[channel];
    storage[static_cast<int>(iem::HaloBedChannel::L)][0] = 1.0F;
    float ambi_storage[iem::kMaxAmbisonicsChannels][1]{};
    float *ambi[iem::kMaxAmbisonicsChannels]{};
    for (uint32_t channel = 0; channel < iem::kMaxAmbisonicsChannels; ++channel) {
        ambi[channel] = ambi_storage[channel];
    }
    iem::EncodeHaloBedToSn3d(1, bed, ambi, 1);
    return Check(ambi[1][0] < 0.0F, "-30 degree source has negative Y")
        && Check(ambi[3][0] > 0.0F, "-30 degree source has positive X");
}

bool TestPreparedSilence() {
    constexpr std::size_t kFrames = 256;
    iem::HaloEncoder encoder;
    iem::EncoderConfig config{};
    config.max_frames = kFrames;
    if (!Check(encoder.Prepare(config), "prepare HaloEncoder")) return false;
    encoder.ApplyParams(iem::IemParams{});
    if (!Check(encoder.PreparedBytes() > 0, "prepared storage is reported")) return false;

    float input_storage[2][kFrames]{};
    const float *input[2]{input_storage[0], input_storage[1]};
    float bed_storage[7][kFrames]{};
    float *bed[7]{};
    for (int channel = 0; channel < 7; ++channel) bed[channel] = bed_storage[channel];

    for (int block = 0; block < 8; ++block) {
        if (!Check(encoder.ProcessBed(input, bed, kFrames), "process silent block")) return false;
        for (int channel = 0; channel < 7; ++channel) {
            for (float sample : bed_storage[channel]) {
                if (!std::isfinite(sample) || sample != 0.0F) {
                    return Check(false, "silent Halo output is finite zero");
                }
            }
        }
    }
    return Check(encoder.StftLatencyFrames() == 1536, "Halo reports end-to-end encoder latency");
}

bool TestIdentityLatencyAndContinuity() {
    constexpr std::size_t kBlock = 256;
    constexpr std::size_t kFrames = 32768;
    constexpr std::size_t kImpulseFrame = 733;

    iem::HaloEncoder encoder;
    iem::EncoderConfig config{};
    config.max_frames = kBlock;
    if (!Check(encoder.Prepare(config), "prepare identity HaloEncoder")) return false;

    iem::IemParams params{};
    params.halo.dialog_isolate_thousandths = 0;
    params.halo.divergence_thousandths = 1000;
    params.halo.fade_thousandths = 0;
    params.halo.fade_rears_thousandths = 0;
    params.halo.diffusion_thousandths = 0;
    params.halo.space_thousandths = 0;
    params.halo.rear_shelf_enable = false;
    encoder.ApplyParams(params);

    std::vector<float> input(kFrames, 0.0F);
    std::vector<float> output(kFrames);
    float input_storage[2][kBlock]{};
    const float *input_block[2]{input_storage[0], input_storage[1]};
    float bed_storage[7][kBlock]{};
    float *bed[7]{};
    const float *bed_const[7]{};
    for (int channel = 0; channel < 7; ++channel) {
        bed[channel] = bed_storage[channel];
        bed_const[channel] = bed_storage[channel];
    }
    float stereo_storage[2][kBlock]{};
    float *stereo[2]{stereo_storage[0], stereo_storage[1]};

    for (std::size_t offset = 0; offset < kFrames; offset += kBlock) {
        for (std::size_t frame = 0; frame < kBlock; ++frame) {
            const float sample = offset + frame == kImpulseFrame ? 0.1F : 0.0F;
            input[offset + frame] = sample;
            input_storage[0][frame] = sample;
            input_storage[1][frame] = sample;
        }
        if (!Check(encoder.ProcessBed(input_block, bed, kBlock), "process identity block")) {
            return false;
        }
        iem::FoldHaloBedToStereo(bed_const, stereo, kBlock);
        for (std::size_t frame = 0; frame < kBlock; ++frame) {
            output[offset + frame] = stereo_storage[0][frame];
        }
    }

    std::size_t peak_frame = 0;
    float peak = 0.0F;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        if (std::fabs(output[frame]) > peak) {
            peak = std::fabs(output[frame]);
            peak_frame = frame;
        }
    }
    const std::size_t best_lag = peak_frame - kImpulseFrame;

    std::printf("Halo identity diagnostic: best_lag=%zu reported=%u peak=%g\n",
        best_lag, encoder.StftLatencyFrames(), peak);

    return Check(best_lag == encoder.StftLatencyFrames(),
            "reported Halo latency matches measured identity latency")
        && Check(peak > 1.0e-3F, "identity path preserves impulse energy");
}

bool TestToneHasNoHopClicks() {
    constexpr std::size_t kBlock = 256;
    constexpr std::size_t kFrames = 32768;
    constexpr float kTwoPi = 6.283185307179586F;

    iem::HaloEncoder encoder;
    iem::EncoderConfig config{};
    config.max_frames = kBlock;
    if (!Check(encoder.Prepare(config), "prepare tone HaloEncoder")) return false;

    iem::IemParams params{};
    params.halo.dialog_isolate_thousandths = 0;
    params.halo.divergence_thousandths = 1000;
    params.halo.fade_thousandths = 0;
    params.halo.fade_rears_thousandths = 0;
    params.halo.diffusion_thousandths = 0;
    params.halo.space_thousandths = 0;
    params.halo.rear_shelf_enable = false;
    encoder.ApplyParams(params);

    float input_storage[2][kBlock]{};
    const float *input_block[2]{input_storage[0], input_storage[1]};
    float bed_storage[7][kBlock]{};
    float *bed[7]{};
    const float *bed_const[7]{};
    for (int channel = 0; channel < 7; ++channel) {
        bed[channel] = bed_storage[channel];
        bed_const[channel] = bed_storage[channel];
    }
    float stereo_storage[2][kBlock]{};
    float *stereo[2]{stereo_storage[0], stereo_storage[1]};
    std::vector<float> output(kFrames);

    for (std::size_t offset = 0; offset < kFrames; offset += kBlock) {
        for (std::size_t frame = 0; frame < kBlock; ++frame) {
            const float sample = 0.1F * std::sin(
                kTwoPi * 997.0F * static_cast<float>(offset + frame) / 96000.0F);
            input_storage[0][frame] = sample;
            input_storage[1][frame] = sample;
        }
        if (!Check(encoder.ProcessBed(input_block, bed, kBlock), "process tone block")) {
            return false;
        }
        iem::FoldHaloBedToStereo(bed_const, stereo, kBlock);
        for (std::size_t frame = 0; frame < kBlock; ++frame) {
            output[offset + frame] = stereo_storage[0][frame];
        }
    }

    double typical_step = 0.0;
    double maximum_hop_step = 0.0;
    std::size_t step_count = 0;
    for (std::size_t frame = 8193; frame < kFrames; ++frame) {
        const double step = std::fabs(output[frame] - output[frame - 1]);
        typical_step += step;
        ++step_count;
        if (frame % iem::HaloStft::kHop == 0) {
            maximum_hop_step = std::max(maximum_hop_step, step);
        }
    }
    typical_step /= static_cast<double>(step_count);
    const double ratio = maximum_hop_step / std::max(typical_step, 1.0e-12);
    std::printf("Halo tone diagnostic: hop_step_ratio=%g\n", ratio);
    return Check(ratio < 4.0, "continuous tone has no STFT hop clicks");
}

bool TestExplicitLfeSideband() {
    constexpr std::size_t kBlock = 256;
    constexpr std::size_t kFrames = 8192;
    constexpr std::size_t kImpulseFrame = 733;

    iem::EncoderConfig config{};
    config.max_frames = kBlock;
    iem::HaloEncoder enabled;
    iem::HaloEncoder disabled;
    if (!Check(enabled.Prepare(config) && disabled.Prepare(config),
            "prepare LFE comparison encoders")) return false;

    iem::IemParams enabled_params{};
    enabled_params.halo.dialog_isolate_thousandths = 0;
    enabled_params.halo.divergence_thousandths = 1000;
    enabled_params.halo.fade_thousandths = 0;
    enabled_params.halo.fade_rears_thousandths = 0;
    enabled_params.halo.diffusion_thousandths = 0;
    enabled_params.halo.space_thousandths = 0;
    enabled_params.halo.rear_shelf_enable = false;
    enabled_params.halo.lfe.enabled = true;
    enabled_params.halo.lfe.split_millionths = 0;
    iem::IemParams disabled_params = enabled_params;
    disabled_params.halo.lfe.enabled = false;
    enabled.ApplyParams(enabled_params);
    disabled.ApplyParams(disabled_params);

    float input_storage[2][kBlock]{};
    const float *input[2]{input_storage[0], input_storage[1]};
    float enabled_storage[iem::kHaloBedChannels][kBlock]{};
    float disabled_storage[iem::kHaloBedChannels][kBlock]{};
    float enabled_lfe[kBlock]{};
    float disabled_lfe[kBlock]{};
    iem::HaloBedView enabled_view{};
    iem::HaloBedView disabled_view{};
    for (uint32_t channel = 0; channel < iem::kHaloDirectionalChannels; ++channel) {
        enabled_view.directional[channel] = enabled_storage[channel];
        disabled_view.directional[channel] = disabled_storage[channel];
    }
    enabled_view.lfe = enabled_lfe;
    disabled_view.lfe = disabled_lfe;

    float lfe_peak = 0.0F;
    std::size_t lfe_peak_frame = 0;
    for (std::size_t offset = 0; offset < kFrames; offset += kBlock) {
        for (std::size_t frame = 0; frame < kBlock; ++frame) {
            const float sample = offset + frame == kImpulseFrame ? 0.1F : 0.0F;
            input_storage[0][frame] = sample;
            input_storage[1][frame] = sample;
        }
        if (!Check(enabled.ProcessBed(input, enabled_view, kBlock)
                && disabled.ProcessBed(input, disabled_view, kBlock),
                "process LFE comparison block")) return false;
        for (uint32_t channel = 0; channel < iem::kHaloDirectionalChannels; ++channel) {
            for (std::size_t frame = 0; frame < kBlock; ++frame) {
                if (enabled_storage[channel][frame] != disabled_storage[channel][frame]) {
                    return Check(false, "split zero leaves directional bed unchanged");
                }
            }
        }
        for (std::size_t frame = 0; frame < kBlock; ++frame) {
            if (disabled_lfe[frame] != 0.0F) {
                return Check(false, "disabled encoder LFE remains zero");
            }
            if (std::fabs(enabled_lfe[frame]) > lfe_peak) {
                lfe_peak = std::fabs(enabled_lfe[frame]);
                lfe_peak_frame = offset + frame;
            }
        }
    }
    return Check(lfe_peak > 1.0e-8F, "enabled encoder produces an LFE sideband")
        && Check(lfe_peak_frame >= kImpulseFrame + iem::HaloStft::kReportedLatency,
            "LFE peak starts inside the Halo encoder latency window")
        && Check(lfe_peak_frame <= kImpulseFrame + enabled.StftLatencyFrames()
                + iem::HaloStft::kHop,
            "LFE peak does not drift beyond the Halo encoder latency window");
}

} // namespace

int main() {
    if (!TestCentreEncodingAndFold()) return 1;
    if (!TestLeftDirection()) return 1;
    if (!TestPreparedSilence()) return 1;
    if (!TestIdentityLatencyAndContinuity()) return 1;
    if (!TestToneHasNoHopClicks()) return 1;
    if (!TestExplicitLfeSideband()) return 1;
    std::puts("IEM Halo encoder tests passed");
    return 0;
}
