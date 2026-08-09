#include "DspGraph.h"
#include "AudioFormat.h"

namespace viper::audio {
namespace {

void ApplySnapshot(ViPER &engine, const viper::ViPERParams &snapshot) {
    engine.ApplyMasterLimiter(snapshot.master_limiter);
    engine.ApplyPlaybackGainControl(snapshot.playback_gain_control);
    engine.ApplyLufs(snapshot.lufs);
    engine.ApplyFetCompressor(snapshot.fet_compressor);
    engine.ApplyBass(snapshot.bass);
    engine.ApplyBassMono(snapshot.bass_mono);
    engine.ApplyPsychoacousticBass(snapshot.psychoacoustic_bass);
    engine.ApplySpectrumExtension(snapshot.spectrum_extension);
    engine.ApplyEqualizer(snapshot.equalizer);
    engine.ApplyConvolver(snapshot.convolver);
    engine.ApplyDdc(snapshot.ddc);
    engine.ApplyFieldSurround(snapshot.field_surround);
    engine.ApplyDiffSurround(snapshot.diff_surround);
    engine.ApplyStereoImager(snapshot.stereo_imager);
    engine.ApplyHeadphoneSurround(snapshot.headphone_surround);
    engine.ApplyReverb(snapshot.reverb);
    engine.ApplyDynamicSystem(snapshot.dynamic_system);
    engine.ApplyClarity(snapshot.clarity);
    engine.ApplyCure(snapshot.cure);
    engine.ApplyTubeSimulator(snapshot.tube_simulator);
    engine.ApplyAnalogX(snapshot.analog_x);
    engine.ApplySpeakerCorrection(snapshot.speaker_correction);
    engine.ApplyMultibandCompressor(snapshot.multiband_compressor);
    engine.ApplyDynamicEq(snapshot.dynamic_eq);
}

} // namespace

bool DspGraph::Prepare(const DspGraphConfig &config) {
    return Prepare(config, viper::ViPERParams{});
}

bool DspGraph::Prepare(
    const DspGraphConfig &config,
    const viper::ViPERParams &snapshot
) {
    if (!IsSupportedSampleRate(config.sample_rate)
        || config.max_block_frames == 0
        || config.max_block_frames > kMaxBlockFrames) {
        prepared_ = false;
        return false;
    }
    engine_.SetSamplingRate(config.sample_rate);
    engine_.ResetAllEffects();
    ApplySnapshot(engine_, snapshot);
    config_ = config;
    prepared_ = true;
    return true;
}

void DspGraph::Reset() noexcept {
    engine_.ResetAllEffects();
}

bool DspGraph::Process(float *interleaved, size_t frame_count) noexcept {
    if (!prepared_ || interleaved == nullptr || frame_count == 0
        || frame_count > config_.max_block_frames) {
        return false;
    }
    engine_.Process(interleaved, static_cast<uint32_t>(frame_count));
    return true;
}

ViPER &DspGraph::Engine() noexcept {
    return engine_;
}

const ViPER &DspGraph::Engine() const noexcept {
    return engine_;
}

const DspGraphConfig &DspGraph::Config() const noexcept {
    return config_;
}

bool DspGraph::IsPrepared() const noexcept {
    return prepared_;
}

} // namespace viper::audio
