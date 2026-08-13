#include "viper/ParameterSnapshot.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace {

static_assert(sizeof(viper::ConvolverParams) == 24);
static_assert(offsetof(viper::ConvolverParams, cross_channel) == 4);
static_assert(offsetof(viper::ConvolverParams, wet) == 8);
static_assert(offsetof(viper::ConvolverParams, output_gain_db) == 12);
static_assert(offsetof(viper::ConvolverParams, routing) == 16);
static_assert(offsetof(viper::ConvolverParams, cross_delay_ms) == 20);
static_assert(offsetof(viper::ViPERParams, convolver) == 316);
static_assert(offsetof(viper::ViPERParams, ddc) == 340);
static_assert(offsetof(viper::ViPERParams, dynamic_eq) == 872);
static_assert(sizeof(viper::ViPERParams) == 1160);

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool Near(float actual, float expected, float tolerance = 1.0e-6F) {
    return std::fabs(actual - expected) <= tolerance;
}

bool TestScalarMappings() {
    using namespace viper::params;
    viper::ViPERParams snapshot{};
    if (!Check(
            viper::UpdateParameterSnapshot(
                snapshot, kParamMasterLimiterThreshold, 75, 0, 0, 0, nullptr
            ) == viper::RawParamUpdate::UPDATED,
            "track master limiter threshold"
        )) {
        return false;
    }
    viper::UpdateParameterSnapshot(snapshot, kParamLufsTarget, 140, 0, 0, 0, nullptr);
    viper::UpdateParameterSnapshot(snapshot, kParamBassFrequency, 80, 0, 0, 0, nullptr);
    viper::UpdateParameterSnapshot(snapshot, kParamReverbWet, 35, 0, 0, 0, nullptr);
    viper::UpdateParameterSnapshot(snapshot, kParamTubeSimulatorEnable, 1, 0, 0, 0, nullptr);

    return Check(Near(snapshot.master_limiter.threshold, 0.75F), "master scale")
        && Check(Near(snapshot.lufs.target, -14.0F), "LUFS scale")
        && Check(snapshot.bass.frequency == 80, "bass frequency")
        && Check(Near(snapshot.reverb.wet, 0.35F), "reverb wet scale")
        && Check(snapshot.tube_simulator.enable, "tube enable");
}

bool TestEqualizerMappings() {
    using namespace viper::params;
    viper::ViPERParams snapshot{};
    viper::UpdateParameterSnapshot(snapshot, kParamEqualizerBandCount, 4, 0, 0, 0, nullptr);
    viper::UpdateParameterSnapshot(snapshot, kParamEqualizerBandLevel, 2, -350, 0, 0, nullptr);
    const std::array<float, 4> levels{1.0F, 2.0F, 3.0F, 4.0F};
    viper::UpdateParameterSnapshot(
        snapshot,
        kParamEqualizerBandLevels,
        0,
        0,
        0,
        levels.size(),
        reinterpret_cast<const signed char *>(levels.data())
    );

    return Check(snapshot.equalizer.band_count == 4, "EQ band count")
        && Check(Near(snapshot.equalizer.band_levels[0], 1.0F), "EQ array first")
        && Check(Near(snapshot.equalizer.band_levels[2], 3.0F), "EQ array overwrites band")
        && Check(
            viper::UpdateParameterSnapshot(
                snapshot, kParamEqualizerBandLevel, 31, 100, 0, 0, nullptr
            ) == viper::RawParamUpdate::INVALID,
            "reject invalid EQ band"
        );
}

bool TestIndexedDynamicsMappings() {
    using namespace viper::params;
    viper::ViPERParams snapshot{};
    viper::UpdateParameterSnapshot(
        snapshot, kParamMultibandCompressorBandCount, 5, 0, 0, 0, nullptr
    );
    viper::UpdateParameterSnapshot(
        snapshot, kParamMultibandCompressorCrossoverFrequency, 3, 8000, 0, 0, nullptr
    );
    viper::UpdateParameterSnapshot(
        snapshot, kParamMultibandCompressorBandThreshold, 4, -1800, 0, 0, nullptr
    );
    viper::UpdateParameterSnapshot(snapshot, kParamDynamicEqBandCount, 3, 0, 0, 0, nullptr);
    viper::UpdateParameterSnapshot(snapshot, kParamDynamicEqBandQ, 2, 125, 0, 0, nullptr);
    viper::UpdateParameterSnapshot(
        snapshot, kParamDynamicEqBandThreshold, 2, -240, 0, 0, nullptr
    );

    return Check(snapshot.multiband_compressor.band_count == 5, "MBC band count")
        && Check(
            Near(snapshot.multiband_compressor.crossover_frequencies[3], 8000.0F),
            "MBC crossover"
        )
        && Check(
            Near(snapshot.multiband_compressor.bands[4].threshold, -18.0F),
            "MBC threshold"
        )
        && Check(snapshot.dynamic_eq.band_count == 3, "dynamic EQ count")
        && Check(Near(snapshot.dynamic_eq.bands[2].q, 1.25F), "dynamic EQ Q")
        && Check(
            Near(snapshot.dynamic_eq.bands[2].threshold, -24.0F),
            "dynamic EQ threshold"
        );
}

bool TestCommandClassification() {
    using namespace viper::params;
    viper::ViPERParams snapshot{};
    return Check(
               viper::UpdateParameterSnapshot(
                   snapshot, kParamConvolverSetBuffer, 0, 0, 0, 0, nullptr
               ) == viper::RawParamUpdate::RESOURCE_COMMAND,
               "classify convolver resource command"
           )
        && Check(
            viper::UpdateParameterSnapshot(
                snapshot, kParamDdcCoefficients, 0, 0, 0, 0, nullptr
            ) == viper::RawParamUpdate::RESOURCE_COMMAND,
            "classify DDC resource command"
        )
        && Check(
            viper::UpdateParameterSnapshot(
                snapshot, kParamResetAllEffects, 0, 0, 0, 0, nullptr
            ) == viper::RawParamUpdate::RESET_COMMAND,
            "classify reset command"
        )
        && Check(
            viper::UpdateParameterSnapshot(snapshot, 0x7FFFFFFF, 0, 0, 0, 0, nullptr)
                == viper::RawParamUpdate::UNKNOWN,
            "classify unknown command"
        );
}

bool TestConvolverMappingsAndClamping() {
    using namespace viper::params;
    viper::ViPERParams snapshot{};
    const auto apply = [&](int param, int value) {
        return viper::UpdateParameterSnapshot(
            snapshot, param, value, 0, 0, 0, nullptr
        ) == viper::RawParamUpdate::UPDATED;
    };

    if (!Check(apply(kParamConvolverCrossChannel, 35), "convolver cross update")) {
        return false;
    }
    if (!Check(apply(kParamConvolverWet, 65), "convolver wet update")) return false;
    if (!Check(apply(kParamConvolverOutputGain, -35), "convolver gain update")) {
        return false;
    }
    if (!Check(apply(kParamConvolverRouting, 2), "convolver routing update")) {
        return false;
    }
    if (!Check(apply(kParamConvolverCrossDelay, 3125), "convolver delay update")) {
        return false;
    }
    if (!Check(Near(snapshot.convolver.cross_channel, 0.35F), "convolver cross scale")) {
        return false;
    }
    if (!Check(Near(snapshot.convolver.wet, 0.65F), "convolver wet scale")) return false;
    if (!Check(Near(snapshot.convolver.output_gain_db, -3.5F), "convolver gain scale")) {
        return false;
    }
    if (!Check(snapshot.convolver.routing == 2, "convolver routing value")) return false;
    if (!Check(Near(snapshot.convolver.cross_delay_ms, 0.3125F), "convolver delay scale")) {
        return false;
    }

    apply(kParamConvolverCrossChannel, 101);
    apply(kParamConvolverWet, -1);
    apply(kParamConvolverOutputGain, 241);
    apply(kParamConvolverRouting, 9);
    apply(kParamConvolverCrossDelay, 100001);
    return Check(Near(snapshot.convolver.cross_channel, 1.0F), "clamp convolver cross")
        && Check(Near(snapshot.convolver.wet, 0.0F), "clamp convolver wet")
        && Check(Near(snapshot.convolver.output_gain_db, 24.0F), "clamp convolver gain")
        && Check(snapshot.convolver.routing == 2, "clamp convolver routing")
        && Check(Near(snapshot.convolver.cross_delay_ms, 10.0F), "clamp convolver delay");
}

bool TestRemainingEffectMappings() {
    using namespace viper::params;
    viper::ViPERParams snapshot{};
    const auto apply = [&](int param, int val1, int val2 = 0) {
        return viper::UpdateParameterSnapshot(
            snapshot, param, val1, val2, 0, 0, nullptr
        ) == viper::RawParamUpdate::UPDATED;
    };

    if (!Check(apply(kParamPlaybackGainControlEnable, 1), "playback enable")) return false;
    apply(kParamPlaybackGainControlStrength, 45);
    apply(kParamPlaybackGainControlMaxGain, 60);
    apply(kParamPlaybackGainControlOutputThreshold, 90);
    apply(kParamLufsEnable, 1);
    apply(kParamLufsMaxGain, 120);
    apply(kParamLufsSpeed, 3);
    apply(kParamFetCompressorEnable, 1);
    apply(kParamFetCompressorThreshold, 25);
    apply(kParamFetCompressorNoClip, 1);
    apply(kParamBassMode, 2);
    apply(kParamBassGain, 55);
    apply(kParamBassAntiPop, 1);
    apply(kParamBassMonoEnable, 1);
    apply(kParamBassMonoFrequency, 120);
    apply(kParamPsychoacousticBassEnable, 1);
    apply(kParamPsychoacousticBassHarmonicOrder, 3);
    apply(kParamSpectrumExtensionEnable, 1);
    apply(kParamSpectrumExtensionExciter, 40);
    apply(kParamEqualizerEnable, 1);
    apply(kParamConvolverEnable, 1);
    apply(kParamConvolverCrossChannel, 25);
    apply(kParamDdcEnable, 1);
    apply(kParamFieldSurroundEnable, 1);
    apply(kParamFieldSurroundWidening, 35);
    apply(kParamFieldSurroundDepth, 18);
    apply(kParamDiffSurroundEnable, 1);
    apply(kParamDiffSurroundWetDryMix, 30);
    apply(kParamStereoImagerEnable, 1);
    apply(kParamStereoImagerMidWidth, 125);
    apply(kParamHeadphoneSurroundEnable, 1);
    apply(kParamHeadphoneSurroundQuality, 2);
    apply(kParamReverbRoomSize, 70);
    apply(kParamReverbWidth, 80);
    apply(kParamReverbDamp, 20);
    apply(kParamReverbDry, 65);
    apply(kParamDynamicSystemEnable, 1);
    apply(kParamDynamicSystemXLow, 10);
    apply(kParamDynamicSystemSideGainHigh, 75);
    apply(kParamClarityEnable, 1);
    apply(kParamClarityMode, 1);
    apply(kParamClarityGain, 45);
    apply(kParamCureEnable, 1);
    apply(kParamCureCrossfeedPreset, 2);
    apply(kParamAnalogXEnable, 1);
    apply(kParamAnalogXMode, 3);
    apply(kParamSpeakerCorrectionEnable, 1);

    return Check(snapshot.playback_gain_control.enable, "playback tracked")
        && Check(Near(snapshot.playback_gain_control.strength, 0.45F), "playback strength")
        && Check(Near(snapshot.lufs.max_gain, 12.0F), "LUFS max gain")
        && Check(snapshot.fet_compressor.no_clip, "FET no clip")
        && Check(snapshot.bass.mode == 2, "bass mode")
        && Check(snapshot.bass_mono.frequency == 120, "bass mono frequency")
        && Check(snapshot.psychoacoustic_bass.harmonic_order == 3, "psycho harmonic")
        && Check(Near(snapshot.spectrum_extension.exciter, 0.4F), "spectrum exciter")
        && Check(snapshot.equalizer.enable, "EQ enable")
        && Check(Near(snapshot.convolver.cross_channel, 0.25F), "convolver cross")
        && Check(snapshot.ddc.enable, "DDC enable")
        && Check(snapshot.field_surround.depth == 18, "field depth")
        && Check(Near(snapshot.diff_surround.wet_dry_mix, 0.3F), "diff mix")
        && Check(Near(snapshot.stereo_imager.mid_width, 125.0F), "stereo width")
        && Check(snapshot.headphone_surround.quality == 2, "VHE quality")
        && Check(Near(snapshot.reverb.dry, 0.65F), "reverb dry")
        && Check(Near(snapshot.dynamic_system.side_gain_high, 0.75F), "dynamic side gain")
        && Check(Near(snapshot.clarity.gain, 0.45F), "clarity gain")
        && Check(snapshot.cure.crossfeed_preset == 2, "cure preset")
        && Check(snapshot.analog_x.mode == 3, "analog mode")
        && Check(snapshot.speaker_correction.enable, "speaker correction enable");
}

bool TestRemainingIndexedMappings() {
    using namespace viper::params;
    viper::ViPERParams snapshot{};
    const auto apply = [&](int param, int band, int value) {
        return viper::UpdateParameterSnapshot(
            snapshot, param, band, value, 0, 0, nullptr
        ) == viper::RawParamUpdate::UPDATED;
    };

    if (!Check(apply(kParamMultibandCompressorBandEnable, 1, 1), "MBC band enable")) {
        return false;
    }
    apply(kParamMultibandCompressorBandRatio, 1, 400);
    apply(kParamMultibandCompressorBandKneeAuto, 1, 1);
    apply(kParamMultibandCompressorBandAttack, 1, 25);
    apply(kParamMultibandCompressorBandReleaseAuto, 1, 1);
    apply(kParamMultibandCompressorBandNoClip, 1, 1);
    if (!Check(apply(kParamDynamicEqBandFrequency, 4, 8000), "dynamic EQ frequency")) {
        return false;
    }
    apply(kParamDynamicEqBandGain, 4, 35);
    apply(kParamDynamicEqBandAttack, 4, 15);
    apply(kParamDynamicEqBandFilterType, 4, 2);

    const auto &mbc = snapshot.multiband_compressor.bands[1];
    const auto &dynamic = snapshot.dynamic_eq.bands[4];
    return Check(mbc.enable, "MBC enable value")
        && Check(Near(mbc.ratio, 4.0F), "MBC ratio")
        && Check(mbc.knee_auto, "MBC knee auto")
        && Check(Near(mbc.attack, 0.25F), "MBC attack")
        && Check(mbc.release_auto, "MBC release auto")
        && Check(mbc.no_clip, "MBC no clip")
        && Check(Near(dynamic.frequency, 8000.0F), "dynamic EQ frequency value")
        && Check(Near(dynamic.gain, 3.5F), "dynamic EQ gain")
        && Check(Near(dynamic.attack, 15.0F), "dynamic EQ attack")
        && Check(dynamic.filter_type == 2, "dynamic EQ filter type");
}

} // namespace

int main() {
    if (!TestScalarMappings()) return 1;
    if (!TestEqualizerMappings()) return 1;
    if (!TestIndexedDynamicsMappings()) return 1;
    if (!TestCommandClassification()) return 1;
    if (!TestConvolverMappingsAndClamping()) return 1;
    if (!TestRemainingEffectMappings()) return 1;
    if (!TestRemainingIndexedMappings()) return 1;
    std::puts("Parameter snapshot tests passed");
    return 0;
}
