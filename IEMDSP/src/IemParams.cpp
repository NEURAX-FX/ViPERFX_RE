#include "iem/IemParams.h"
#include "iem/HaloLfeSynth.h"

#include <algorithm>

namespace iem {

namespace {

ParamUpdate SetBool(bool &target, int value) noexcept {
    if (value != 0 && value != 1) return ParamUpdate::INVALID;
    target = value != 0;
    return ParamUpdate::UPDATED;
}

bool IsSourceIndex(int value) noexcept {
    return value == 0 || value == 1;
}

template <typename Enum>
ParamUpdate SetEnum(Enum &target, int value, int minimum, int maximum) noexcept {
    if (value < minimum || value > maximum) return ParamUpdate::INVALID;
    target = static_cast<Enum>(value);
    return ParamUpdate::UPDATED;
}

template <typename Value>
void SetClamped(Value &target, int value, int minimum, int maximum) noexcept {
    target = static_cast<Value>(std::clamp(value, minimum, maximum));
}

} // namespace

bool HasStructuralDifference(const IemParams &left, const IemParams &right) noexcept {
    return left.encoder_mode != right.encoder_mode
        || left.render_mode != right.render_mode
        || left.order != right.order
        || left.decoder.headphone_eq != right.decoder.headphone_eq
        || left.latency_profile != right.latency_profile;
}

ParamUpdate UpdateIemParameterSnapshot(
    IemParams &params,
    int param,
    int val1,
    int val2,
    int /*val3*/
) noexcept {
    switch (param) {
    case kParamIemEnable:
        return SetBool(params.enable, val1);
    case kParamIemWet:
        params.wet = static_cast<float>(std::clamp(val1, 0, 100)) / 100.0F;
        return ParamUpdate::UPDATED;
    case kParamIemOutputGain:
        params.output_gain_db = static_cast<float>(std::clamp(val1, -240, 240)) / 10.0F;
        return ParamUpdate::UPDATED;
    case kParamIemOrder:
        params.order = static_cast<uint32_t>(std::clamp(val1, 1, 3));
        return ParamUpdate::UPDATED;
    case kParamIemEncoderMode:
        return SetEnum(params.encoder_mode, val1, 0, 3);
    case kParamIemRenderMode:
        return SetEnum(params.render_mode, val1, 0, 2);
    case kParamIemLatencyProfile:
        return SetEnum(params.latency_profile, val1, 0, 2);
    case kParamIemLimiterEnable:
        return SetBool(params.limiter.enabled, val1);
    case kParamIemLimiterCeiling:
        SetClamped(params.limiter.ceiling_centidb, val1, -1200, 0);
        return ParamUpdate::UPDATED;
    case kParamStereoAzimuth:
        SetClamped(params.stereo.azimuth_centidegrees, val1, -18000, 18000);
        return ParamUpdate::UPDATED;
    case kParamStereoElevation:
        SetClamped(params.stereo.elevation_centidegrees, val1, -18000, 18000);
        return ParamUpdate::UPDATED;
    case kParamStereoRoll:
        SetClamped(params.stereo.roll_centidegrees, val1, -18000, 18000);
        return ParamUpdate::UPDATED;
    case kParamStereoWidth:
        SetClamped(params.stereo.width_centidegrees, val1, -36000, 36000);
        return ParamUpdate::UPDATED;
    case kParamStereoSampleWise:
        return SetBool(params.stereo.sample_wise, val1);
    case kParamMultiAzimuth:
        if (!IsSourceIndex(val1)) return ParamUpdate::INVALID;
        SetClamped(params.multi.azimuth_centidegrees[static_cast<std::size_t>(val1)],
            val2, -18000, 18000);
        return ParamUpdate::UPDATED;
    case kParamMultiElevation:
        if (!IsSourceIndex(val1)) return ParamUpdate::INVALID;
        SetClamped(params.multi.elevation_centidegrees[static_cast<std::size_t>(val1)],
            val2, -18000, 18000);
        return ParamUpdate::UPDATED;
    case kParamMultiGain:
        if (!IsSourceIndex(val1)) return ParamUpdate::INVALID;
        SetClamped(params.multi.gain_decidb[static_cast<std::size_t>(val1)],
            val2, -600, 100);
        return ParamUpdate::UPDATED;
    case kParamMultiMute:
        if (!IsSourceIndex(val1)) return ParamUpdate::INVALID;
        return SetBool(params.multi.mute[static_cast<std::size_t>(val1)], val2);
    case kParamGranularAzimuth:
        SetClamped(params.granular.azimuth_centidegrees, val1, -18000, 18000);
        return ParamUpdate::UPDATED;
    case kParamGranularElevation:
        SetClamped(params.granular.elevation_centidegrees, val1, -18000, 18000);
        return ParamUpdate::UPDATED;
    case kParamGranularShape:
        SetClamped(params.granular.shape_tenths, val1, -100, 100);
        return ParamUpdate::UPDATED;
    case kParamGranularSize:
        SetClamped(params.granular.size_centidegrees, val1, 0, 36000);
        return ParamUpdate::UPDATED;
    case kParamGranularRoll:
        SetClamped(params.granular.roll_centidegrees, val1, -18000, 18000);
        return ParamUpdate::UPDATED;
    case kParamGranularWidth:
        SetClamped(params.granular.width_centidegrees, val1, -36000, 36000);
        return ParamUpdate::UPDATED;
    case kParamGranularDeltaTime:
        SetClamped(params.granular.delta_time_us, val1, 1000, 2000000);
        return ParamUpdate::UPDATED;
    case kParamGranularDeltaTimeMod:
        SetClamped(params.granular.delta_time_mod_tenths_percent, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamGranularGrainLength:
        SetClamped(params.granular.grain_length_us, val1, 1000, 2000000);
        return ParamUpdate::UPDATED;
    case kParamGranularGrainLengthMod:
        SetClamped(params.granular.grain_length_mod_tenths_percent, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamGranularReadPosition:
        SetClamped(params.granular.read_position_us, val1, 0, 4000000);
        return ParamUpdate::UPDATED;
    case kParamGranularPositionMod:
        SetClamped(params.granular.position_mod_us, val1, 0, 4000000);
        return ParamUpdate::UPDATED;
    case kParamGranularPitch:
        SetClamped(params.granular.pitch_millisem, val1, -12000, 12000);
        return ParamUpdate::UPDATED;
    case kParamGranularPitchMod:
        SetClamped(params.granular.pitch_mod_millisem, val1, 0, 12000);
        return ParamUpdate::UPDATED;
    case kParamGranularWindowAttack:
        SetClamped(params.granular.attack_tenths_percent, val1, 0, 500);
        return ParamUpdate::UPDATED;
    case kParamGranularAttackMod:
        SetClamped(params.granular.attack_mod_tenths_percent, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamGranularWindowDecay:
        SetClamped(params.granular.decay_tenths_percent, val1, 0, 500);
        return ParamUpdate::UPDATED;
    case kParamGranularDecayMod:
        SetClamped(params.granular.decay_mod_tenths_percent, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamGranularMix:
        SetClamped(params.granular.mix_tenths_percent, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamGranularSourceProbability:
        SetClamped(params.granular.source_probability_hundredths, val1, -100, 100);
        return ParamUpdate::UPDATED;
    case kParamGranularSpatialMode:
        return SetEnum(params.granular.spatial_mode, val1, 0, 1);
    case kParamGranularSampleWise:
        return SetBool(params.granular.sample_wise, val1);
    case kParamRotationYaw:
        SetClamped(params.rotation.yaw_centidegrees, val1, -18000, 18000);
        return ParamUpdate::UPDATED;
    case kParamRotationPitch:
        SetClamped(params.rotation.pitch_centidegrees, val1, -18000, 18000);
        return ParamUpdate::UPDATED;
    case kParamRotationRoll:
        SetClamped(params.rotation.roll_centidegrees, val1, -18000, 18000);
        return ParamUpdate::UPDATED;
    case kParamRotationInvertYaw:
        return SetBool(params.rotation.invert_yaw, val1);
    case kParamRotationInvertPitch:
        return SetBool(params.rotation.invert_pitch, val1);
    case kParamRotationInvertRoll:
        return SetBool(params.rotation.invert_roll, val1);
    case kParamRotationInvertOverall:
        return SetBool(params.rotation.invert_overall, val1);
    case kParamRotationSequence:
        return SetEnum(params.rotation.sequence, val1, 0, 1);
    case kParamHeadphoneEq:
        return SetEnum(params.decoder.headphone_eq, val1, -1, 22);
    case kParamHaloDialogIsolate:
        SetClamped(params.halo.dialog_isolate_thousandths, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamHaloDialogAggress:
        SetClamped(params.halo.dialog_aggress_thousandths, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamHaloDialogAttack:
        SetClamped(params.halo.dialog_attack_thousandths, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamHaloDialogRelease:
        SetClamped(params.halo.dialog_release_thousandths, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamHaloDialogMixIn:
        SetClamped(params.halo.dialog_mix_in_thousandths, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamHaloDivergence:
        SetClamped(params.halo.divergence_thousandths, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamHaloFade:
        SetClamped(params.halo.fade_thousandths, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamHaloFadeRears:
        SetClamped(params.halo.fade_rears_thousandths, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamHaloDiffusion:
        SetClamped(params.halo.diffusion_thousandths, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamHaloSpace:
        SetClamped(params.halo.space_thousandths, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamHaloBackBoost:
        return SetBool(params.halo.back_boost, val1);
    case kParamHaloRearShelfEnable:
        return SetBool(params.halo.rear_shelf_enable, val1);
    case kParamHaloRearShelfFreq:
        SetClamped(params.halo.rear_shelf_freq_thousandths, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamHaloRearShelfGain:
        SetClamped(params.halo.rear_shelf_gain_thousandths, val1, 0, 1000);
        return ParamUpdate::UPDATED;
    case kParamHaloLfeEnable:
        return SetBool(params.halo.lfe.enabled, val1);
    case kParamHaloLfeFrequency:
        SetClamped(params.halo.lfe.frequency_millionths, val1, 0, 1000000);
        params.halo.lfe.coefficients_96k = MakeHaloLfeLowPass(
            96000, params.halo.lfe.frequency_millionths);
        return ParamUpdate::UPDATED;
    case kParamHaloLfeSplit:
        SetClamped(params.halo.lfe.split_millionths, val1, 0, 1000000);
        return ParamUpdate::UPDATED;
    case kParamHaloLfeGain:
        SetClamped(params.halo.lfe.gain_millionths, val1, 0, 1000000);
        params.halo.lfe.gain_linear = HaloLfeGainLinear(
            params.halo.lfe.gain_millionths);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixDelayEnable:
        return SetBool(params.decoder.downmix.delay_enable, val1);
    case kParamHaloDownmixLsDelay:
        SetClamped(params.decoder.downmix.ls_delay_us, val1, 0, 32000);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixRsDelay:
        SetClamped(params.decoder.downmix.rs_delay_us, val1, 0, 32000);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixLsrDelay:
        SetClamped(params.decoder.downmix.lsr_delay_us, val1, 0, 32000);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixRsrDelay:
        SetClamped(params.decoder.downmix.rsr_delay_us, val1, 0, 32000);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixSideShelfEnable:
        return SetBool(params.decoder.downmix.side_shelf_enable, val1);
    case kParamHaloDownmixSideShelfFrequency:
        SetClamped(params.decoder.downmix.side_shelf_frequency_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixSideShelfGain:
        SetClamped(params.decoder.downmix.side_shelf_gain_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixRearShelfEnable:
        return SetBool(params.decoder.downmix.rear_shelf_enable, val1);
    case kParamHaloDownmixRearShelfFrequency:
        SetClamped(params.decoder.downmix.rear_shelf_frequency_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixRearShelfGain:
        SetClamped(params.decoder.downmix.rear_shelf_gain_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixPanLeft:
        SetClamped(params.decoder.downmix.pan_left_millionths, val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixPanRight:
        SetClamped(params.decoder.downmix.pan_right_millionths, val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixCenterDivergence:
        SetClamped(params.decoder.downmix.center_divergence_millionths,
            val1, 0, 1000000);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixFrontMidTrim:
        SetClamped(params.decoder.downmix.front_mid_trim_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixFrontSideTrim:
        SetClamped(params.decoder.downmix.front_side_trim_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixCenterTrim:
        SetClamped(params.decoder.downmix.center_trim_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixSurroundMidTrim:
        SetClamped(params.decoder.downmix.surround_mid_trim_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixSurroundSideTrim:
        SetClamped(params.decoder.downmix.surround_side_trim_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixRearMidTrim:
        SetClamped(params.decoder.downmix.rear_mid_trim_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixRearSideTrim:
        SetClamped(params.decoder.downmix.rear_side_trim_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixLfeTrim:
        SetClamped(params.decoder.downmix.lfe_trim_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixLfeLpfEnable:
        return SetBool(params.decoder.downmix.lfe_lpf_enable, val1);
    case kParamHaloDownmixLfeLpfFrequency:
        SetClamped(params.decoder.downmix.lfe_lpf_frequency_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixScaleInputByOutputCount:
        return SetBool(params.decoder.downmix.scale_input_by_output_count, val1);
    case kParamHaloDownmixOutputHpfEnable:
        return SetBool(params.decoder.downmix.output_hpf_enable, val1);
    case kParamHaloDownmixOutputHpfFrequency:
        SetClamped(params.decoder.downmix.output_hpf_frequency_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixOutputLeftTrim:
        SetClamped(params.decoder.downmix.output_left_trim_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamHaloDownmixOutputRightTrim:
        SetClamped(params.decoder.downmix.output_right_trim_millionths,
            val1, 0, 1000000);
        RefreshHaloDownmixDerived(params.decoder.downmix);
        return ParamUpdate::UPDATED;
    case kParamIemResourceReset:
    case kCommandResetRotation:
    case kCommandResetIemRuntime:
        return val1 == 1 ? ParamUpdate::COMMAND : ParamUpdate::INVALID;
    case kCommandGranularFreeze:
        return val1 == 0 || val1 == 1 ? ParamUpdate::COMMAND : ParamUpdate::INVALID;
    default:
        return ParamUpdate::NOT_IEM;
    }
}

} // namespace iem
