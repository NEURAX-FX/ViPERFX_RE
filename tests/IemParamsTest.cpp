#include "iem/IemParams.h"

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

bool Updated(iem::IemParams &params, int id, int val1, int val2 = 0) {
    return iem::UpdateIemParameterSnapshot(params, id, val1, val2, 0)
        == iem::ParamUpdate::UPDATED;
}

bool TestIdsAndDefaults() {
    static_assert(iem::kParamIemEnable == 0x12000);
    static_assert(iem::kParamIemWet == 0x12001);
    static_assert(iem::kParamIemOutputGain == 0x12002);
    static_assert(iem::kParamIemOrder == 0x12003);
    static_assert(iem::kParamIemEncoderMode == 0x12004);
    static_assert(iem::kParamIemLatencyProfile == 0x12005);
    static_assert(iem::kParamIemLimiterEnable == 0x12006);
    static_assert(iem::kParamIemLimiterCeiling == 0x12007);
    static_assert(iem::kParamStereoAzimuth == 0x12010);
    static_assert(iem::kParamStereoSampleWise == 0x12014);
    static_assert(iem::kParamMultiAzimuth == 0x12020);
    static_assert(iem::kParamMultiMute == 0x12023);
    static_assert(iem::kParamGranularAzimuth == 0x12030);
    static_assert(iem::kParamGranularSampleWise == 0x12045);
    static_assert(iem::kParamRotationYaw == 0x12050);
    static_assert(iem::kParamRotationSequence == 0x12057);
    static_assert(iem::kParamHeadphoneEq == 0x12060);
    static_assert(iem::kParamIemResourceReset == 0x12100);
    static_assert(iem::kCommandResetRotation == 0x12101);
    static_assert(iem::kCommandGranularFreeze == 0x12102);
    static_assert(iem::kCommandResetIemRuntime == 0x12103);

    const iem::IemParams params{};
    return Check(!params.enable, "IEM defaults disabled")
        && Check(Near(params.wet, 1.0F), "wet defaults to 100 percent")
        && Check(Near(params.output_gain_db, 0.0F), "output gain defaults to zero")
        && Check(params.order == 3, "order defaults to third order")
        && Check(params.encoder_mode == iem::EncoderMode::STEREO,
            "encoder defaults to stereo")
        && Check(params.latency_profile == iem::LatencyProfile::BALANCED,
            "latency defaults to balanced")
        && Check(params.limiter.enabled && params.limiter.ceiling_centidb == -30,
            "limiter defaults")
        && Check(params.stereo.width_centidegrees == 6000,
            "stereo defaults to 60 degree width")
        && Check(params.multi.azimuth_centidegrees[0] == -3000
                && params.multi.azimuth_centidegrees[1] == 3000,
            "multi defaults to left and right sources")
        && Check(params.granular.delta_time_us == 5000
                && params.granular.grain_length_us == 250000
                && params.granular.position_mod_us == 50000,
            "granular timing defaults")
        && Check(params.granular.size_centidegrees == 18000
                && params.granular.attack_tenths_percent == 500
                && params.granular.decay_tenths_percent == 500
                && params.granular.mix_tenths_percent == 500,
            "granular shape defaults")
        && Check(params.rotation.sequence == iem::RotationSequence::ROLL_PITCH_YAW,
            "rotation sequence default")
        && Check(params.decoder.headphone_eq == iem::HeadphoneEqId::OFF,
            "headphone EQ defaults off");
}

bool TestGeneralAndStereoMappings() {
    iem::IemParams params{};
    if (!Check(Updated(params, iem::kParamIemEnable, 1) && params.enable,
            "map enable")) return false;
    if (!Check(Updated(params, iem::kParamIemWet, 65) && Near(params.wet, 0.65F),
            "map wet")) return false;
    if (!Check(Updated(params, iem::kParamIemOutputGain, -35)
            && Near(params.output_gain_db, -3.5F), "map output gain")) return false;
    if (!Check(Updated(params, iem::kParamIemOrder, 2) && params.order == 2,
            "map order")) return false;
    if (!Check(Updated(params, iem::kParamIemEncoderMode, 2)
            && params.encoder_mode == iem::EncoderMode::GRANULAR,
            "map encoder mode")) return false;
    if (!Check(Updated(params, iem::kParamIemLatencyProfile, 0)
            && params.latency_profile == iem::LatencyProfile::LOW,
            "map latency profile")) return false;
    if (!Check(Updated(params, iem::kParamIemLimiterEnable, 0)
            && !params.limiter.enabled, "map limiter enable")) return false;
    if (!Check(Updated(params, iem::kParamIemLimiterCeiling, -1800)
            && params.limiter.ceiling_centidb == -1200, "clamp limiter ceiling")) {
        return false;
    }

    return Check(Updated(params, iem::kParamStereoAzimuth, 20000)
            && params.stereo.azimuth_centidegrees == 18000, "clamp stereo azimuth")
        && Check(Updated(params, iem::kParamStereoElevation, -1200)
            && params.stereo.elevation_centidegrees == -1200, "map stereo elevation")
        && Check(Updated(params, iem::kParamStereoRoll, 4500)
            && params.stereo.roll_centidegrees == 4500, "map stereo roll")
        && Check(Updated(params, iem::kParamStereoWidth, -40000)
            && params.stereo.width_centidegrees == -36000, "clamp stereo width")
        && Check(Updated(params, iem::kParamStereoSampleWise, 1)
            && params.stereo.sample_wise, "map stereo sample-wise mode");
}

bool TestMultiMappings() {
    iem::IemParams params{};
    if (!Check(Updated(params, iem::kParamMultiAzimuth, 1, 9000)
            && params.multi.azimuth_centidegrees[1] == 9000, "map multi azimuth")) {
        return false;
    }
    if (!Check(Updated(params, iem::kParamMultiElevation, 0, -20000)
            && params.multi.elevation_centidegrees[0] == -18000,
            "clamp multi elevation")) return false;
    if (!Check(Updated(params, iem::kParamMultiGain, 1, -700)
            && params.multi.gain_decidb[1] == -600, "clamp multi gain")) return false;
    if (!Check(Updated(params, iem::kParamMultiMute, 0, 1) && params.multi.mute[0],
            "map multi mute")) return false;
    return Check(iem::UpdateIemParameterSnapshot(
            params, iem::kParamMultiGain, 2, 0, 0) == iem::ParamUpdate::INVALID,
            "reject invalid multi source")
        && Check(iem::UpdateIemParameterSnapshot(
            params, iem::kParamMultiMute, 0, 2, 0) == iem::ParamUpdate::INVALID,
            "reject invalid multi boolean");
}

bool TestGranularMappings() {
    iem::IemParams params{};
    return Check(Updated(params, iem::kParamGranularAzimuth, -20000)
            && params.granular.azimuth_centidegrees == -18000, "granular azimuth")
        && Check(Updated(params, iem::kParamGranularElevation, 9000)
            && params.granular.elevation_centidegrees == 9000, "granular elevation")
        && Check(Updated(params, iem::kParamGranularShape, 150)
            && params.granular.shape_tenths == 100, "granular shape")
        && Check(Updated(params, iem::kParamGranularSize, -1)
            && params.granular.size_centidegrees == 0, "granular size")
        && Check(Updated(params, iem::kParamGranularRoll, 5000)
            && params.granular.roll_centidegrees == 5000, "granular roll")
        && Check(Updated(params, iem::kParamGranularWidth, 40000)
            && params.granular.width_centidegrees == 36000, "granular width")
        && Check(Updated(params, iem::kParamGranularDeltaTime, 0)
            && params.granular.delta_time_us == 1000, "granular delta time")
        && Check(Updated(params, iem::kParamGranularDeltaTimeMod, 1200)
            && params.granular.delta_time_mod_tenths_percent == 1000,
            "granular delta time modulation")
        && Check(Updated(params, iem::kParamGranularGrainLength, 3000000)
            && params.granular.grain_length_us == 2000000, "granular grain length")
        && Check(Updated(params, iem::kParamGranularGrainLengthMod, 250)
            && params.granular.grain_length_mod_tenths_percent == 250,
            "granular grain modulation")
        && Check(Updated(params, iem::kParamGranularReadPosition, 5000000)
            && params.granular.read_position_us == 4000000, "granular read position")
        && Check(Updated(params, iem::kParamGranularPositionMod, -1)
            && params.granular.position_mod_us == 0, "granular position modulation")
        && Check(Updated(params, iem::kParamGranularPitch, -13000)
            && params.granular.pitch_millisem == -12000, "granular pitch")
        && Check(Updated(params, iem::kParamGranularPitchMod, 13000)
            && params.granular.pitch_mod_millisem == 12000, "granular pitch modulation")
        && Check(Updated(params, iem::kParamGranularWindowAttack, 600)
            && params.granular.attack_tenths_percent == 500, "granular attack")
        && Check(Updated(params, iem::kParamGranularAttackMod, 800)
            && params.granular.attack_mod_tenths_percent == 800,
            "granular attack modulation")
        && Check(Updated(params, iem::kParamGranularWindowDecay, -1)
            && params.granular.decay_tenths_percent == 0, "granular decay")
        && Check(Updated(params, iem::kParamGranularDecayMod, 1200)
            && params.granular.decay_mod_tenths_percent == 1000,
            "granular decay modulation")
        && Check(Updated(params, iem::kParamGranularMix, 750)
            && params.granular.mix_tenths_percent == 750, "granular mix")
        && Check(Updated(params, iem::kParamGranularSourceProbability, -150)
            && params.granular.source_probability_hundredths == -100,
            "granular source probability")
        && Check(Updated(params, iem::kParamGranularSpatialMode, 1)
            && params.granular.spatial_mode == iem::GranularSpatialMode::TWO_D,
            "granular spatial mode")
        && Check(Updated(params, iem::kParamGranularSampleWise, 1)
            && params.granular.sample_wise, "granular sample-wise mode");
}

bool TestRotationDecoderAndStructuralChanges() {
    iem::IemParams params{};
    if (!Check(Updated(params, iem::kParamRotationYaw, 19000)
            && params.rotation.yaw_centidegrees == 18000, "rotation yaw")) return false;
    if (!Check(Updated(params, iem::kParamRotationPitch, -9000)
            && params.rotation.pitch_centidegrees == -9000, "rotation pitch")) return false;
    if (!Check(Updated(params, iem::kParamRotationRoll, 4500)
            && params.rotation.roll_centidegrees == 4500, "rotation roll")) return false;
    if (!Check(Updated(params, iem::kParamRotationInvertYaw, 1)
            && params.rotation.invert_yaw, "invert yaw")) return false;
    if (!Check(Updated(params, iem::kParamRotationInvertPitch, 1)
            && params.rotation.invert_pitch, "invert pitch")) return false;
    if (!Check(Updated(params, iem::kParamRotationInvertRoll, 1)
            && params.rotation.invert_roll, "invert roll")) return false;
    if (!Check(Updated(params, iem::kParamRotationInvertOverall, 1)
            && params.rotation.invert_overall, "invert overall")) return false;
    if (!Check(Updated(params, iem::kParamRotationSequence, 0)
            && params.rotation.sequence == iem::RotationSequence::YAW_PITCH_ROLL,
            "rotation sequence")) return false;
    if (!Check(Updated(params, iem::kParamHeadphoneEq, 22)
            && params.decoder.headphone_eq == iem::HeadphoneEqId::SHURE_SRH940,
            "headphone EQ")) return false;

    iem::IemParams dynamic = params;
    dynamic.rotation.yaw_centidegrees = 0;
    if (!Check(!iem::HasStructuralDifference(params, dynamic),
            "rotation is dynamic")) return false;
    dynamic = params;
    dynamic.encoder_mode = iem::EncoderMode::MULTI;
    if (!Check(iem::HasStructuralDifference(params, dynamic),
            "encoder mode is structural")) return false;
    dynamic = params;
    dynamic.order = 2;
    if (!Check(iem::HasStructuralDifference(params, dynamic),
            "order is structural")) return false;
    dynamic = params;
    dynamic.latency_profile = iem::LatencyProfile::STABLE;
    if (!Check(iem::HasStructuralDifference(params, dynamic),
            "latency profile is structural")) return false;
    dynamic = params;
    dynamic.decoder.headphone_eq = iem::HeadphoneEqId::OFF;
    return Check(iem::HasStructuralDifference(params, dynamic),
        "headphone EQ is structural");
}

bool TestInvalidValuesAndCommands() {
    iem::IemParams params{};
    return Check(iem::UpdateIemParameterSnapshot(
            params, iem::kParamIemEnable, 2, 0, 0) == iem::ParamUpdate::INVALID,
            "reject invalid enable")
        && Check(iem::UpdateIemParameterSnapshot(
            params, iem::kParamIemEncoderMode, 4, 0, 0) == iem::ParamUpdate::INVALID,
            "reject invalid encoder mode")
        && Check(iem::UpdateIemParameterSnapshot(
            params, iem::kParamIemLatencyProfile, -1, 0, 0) == iem::ParamUpdate::INVALID,
            "reject invalid latency profile")
        && Check(iem::UpdateIemParameterSnapshot(
            params, iem::kParamGranularSpatialMode, 2, 0, 0) == iem::ParamUpdate::INVALID,
            "reject invalid spatial mode")
        && Check(iem::UpdateIemParameterSnapshot(
            params, iem::kParamRotationSequence, 2, 0, 0) == iem::ParamUpdate::INVALID,
            "reject invalid rotation sequence")
        && Check(iem::UpdateIemParameterSnapshot(
            params, iem::kParamHeadphoneEq, 23, 0, 0) == iem::ParamUpdate::INVALID,
            "reject invalid headphone EQ")
        && Check(iem::UpdateIemParameterSnapshot(
            params, iem::kParamIemResourceReset, 1, 0, 0) == iem::ParamUpdate::COMMAND,
            "resource reset command")
        && Check(iem::UpdateIemParameterSnapshot(
            params, iem::kCommandResetRotation, 1, 0, 0) == iem::ParamUpdate::COMMAND,
            "rotation reset command")
        && Check(iem::UpdateIemParameterSnapshot(
            params, iem::kCommandGranularFreeze, 0, 0, 0) == iem::ParamUpdate::COMMAND,
            "freeze off command")
        && Check(iem::UpdateIemParameterSnapshot(
            params, iem::kCommandGranularFreeze, 1, 0, 0) == iem::ParamUpdate::COMMAND,
            "freeze on command")
        && Check(iem::UpdateIemParameterSnapshot(
            params, iem::kCommandResetIemRuntime, 1, 0, 0) == iem::ParamUpdate::COMMAND,
            "runtime reset command")
        && Check(iem::UpdateIemParameterSnapshot(
            params, iem::kCommandGranularFreeze, 2, 0, 0) == iem::ParamUpdate::INVALID,
            "reject invalid freeze command")
        && Check(iem::UpdateIemParameterSnapshot(
            params, iem::kCommandResetRotation, 0, 0, 0) == iem::ParamUpdate::INVALID,
            "reject invalid reset command")
        && Check(iem::UpdateIemParameterSnapshot(
            params, 0x101B9, 0, 0, 0) == iem::ParamUpdate::NOT_IEM,
            "leave ViPER parameter untouched")
        && Check(iem::UpdateIemParameterSnapshot(
            params, 0x12FFE, 0, 0, 0) == iem::ParamUpdate::NOT_IEM,
            "ignore unknown reserved parameter");
}

bool TestHaloAndRenderModeContract() {
    static_assert(iem::kParamIemRenderMode == 0x12008);
    static_assert(iem::kParamHaloDialogIsolate == 0x12070);
    static_assert(iem::kParamHaloRearShelfGain == 0x1207D);

    iem::IemParams defaults{};
    if (!Check(defaults.encoder_mode == iem::EncoderMode::STEREO,
            "encoder still defaults to stereo")) return false;
    if (!Check(defaults.render_mode == iem::RenderMode::KU100,
            "render mode defaults to KU100")) return false;
    if (!Check(defaults.halo.dialog_aggress_thousandths == 500,
            "dialog aggress default")) return false;
    if (!Check(defaults.halo.space_thousandths == 800, "space default")) return false;
    if (!Check(defaults.halo.back_boost, "back boost default")) return false;
    if (!Check(defaults.halo.rear_shelf_freq_thousandths == 816,
            "rear shelf frequency default")) return false;

    if (!Check(Updated(defaults, iem::kParamIemEncoderMode, 3)
            && defaults.encoder_mode == iem::EncoderMode::HALO,
            "accept Halo encoder mode")) return false;
    if (!Check(iem::UpdateIemParameterSnapshot(
            defaults, iem::kParamIemEncoderMode, 4, 0, 0) == iem::ParamUpdate::INVALID,
            "reject encoder mode 4")) return false;
    if (!Check(Updated(defaults, iem::kParamIemRenderMode, 0)
            && defaults.render_mode == iem::RenderMode::OFF,
            "accept Off render mode")) return false;
    if (!Check(iem::UpdateIemParameterSnapshot(
            defaults, iem::kParamIemRenderMode, 3, 0, 0) == iem::ParamUpdate::INVALID,
            "reject render mode 3")) return false;
    if (!Check(Updated(defaults, iem::kParamHaloDialogIsolate, 1500)
            && defaults.halo.dialog_isolate_thousandths == 1000,
            "clamp Halo thousandths")) return false;

    iem::IemParams left{};
    iem::IemParams right = left;
    right.halo.fade_thousandths = 400;
    if (!Check(!iem::HasStructuralDifference(left, right),
            "Halo fade is dynamic")) return false;
    right.render_mode = iem::RenderMode::OFF;
    if (!Check(iem::HasStructuralDifference(left, right),
            "render mode is structural")) return false;
    right = left;
    right.encoder_mode = iem::EncoderMode::HALO;
    return Check(iem::HasStructuralDifference(left, right),
        "Halo encoder mode is structural");
}

bool TestHaloLfeContract() {
    static_assert(iem::kParamHaloLfeEnable == 0x1207E);
    static_assert(iem::kParamHaloLfeFrequency == 0x1207F);
    static_assert(iem::kParamHaloLfeSplit == 0x12080);
    static_assert(iem::kParamHaloLfeGain == 0x12081);

    iem::IemParams defaults{};
    if (!Check(defaults.halo.lfe.enabled, "Halo LFE defaults enabled")) return false;
    if (!Check(defaults.halo.lfe.frequency_millionths == 750000,
            "Halo LFE frequency default")) return false;
    if (!Check(defaults.halo.lfe.split_millionths == 0,
            "Halo LFE split default")) return false;
    if (!Check(defaults.halo.lfe.gain_millionths == 272727,
            "Halo LFE gain default")) return false;
    if (!Check(Near(defaults.halo.lfe.coefficients_96k.b0, 0.00000953702965F)
            && Near(defaults.halo.lfe.coefficients_96k.fb1, 1.99379110F),
            "Halo LFE default 96 kHz coefficients")) return false;

    if (!Check(Updated(defaults, iem::kParamHaloLfeEnable, 0)
            && !defaults.halo.lfe.enabled,
            "disable Halo LFE")) return false;
    if (!Check(Updated(defaults, iem::kParamHaloLfeFrequency, 1500000)
            && defaults.halo.lfe.frequency_millionths == 1000000,
            "clamp Halo LFE frequency")) return false;
    if (!Check(Updated(defaults, iem::kParamHaloLfeSplit, -1)
            && defaults.halo.lfe.split_millionths == 0,
            "clamp Halo LFE split")) return false;
    if (!Check(Updated(defaults, iem::kParamHaloLfeGain, 1000001)
            && defaults.halo.lfe.gain_millionths == 1000000,
            "clamp Halo LFE gain")) return false;

    iem::IemParams right{};
    right.halo.lfe.split_millionths = 1000000;
    return Check(!iem::HasStructuralDifference(iem::IemParams{}, right),
        "Halo LFE controls are dynamic");
}

} // namespace

int main() {
    if (!TestIdsAndDefaults()) return 1;
    if (!TestGeneralAndStereoMappings()) return 1;
    if (!TestMultiMappings()) return 1;
    if (!TestGranularMappings()) return 1;
    if (!TestRotationDecoderAndStructuralChanges()) return 1;
    if (!TestInvalidValuesAndCommands()) return 1;
    if (!TestHaloAndRenderModeContract()) return 1;
    if (!TestHaloLfeContract()) return 1;
    std::puts("IEM parameter tests passed");
    return 0;
}
