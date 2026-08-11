#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace iem {

constexpr int kParamIemEnable = 0x12000;
constexpr int kParamIemWet = 0x12001;
constexpr int kParamIemOutputGain = 0x12002;
constexpr int kParamIemOrder = 0x12003;
constexpr int kParamIemEncoderMode = 0x12004;
constexpr int kParamIemLatencyProfile = 0x12005;
constexpr int kParamIemLimiterEnable = 0x12006;
constexpr int kParamIemLimiterCeiling = 0x12007;

constexpr int kParamStereoAzimuth = 0x12010;
constexpr int kParamStereoElevation = 0x12011;
constexpr int kParamStereoRoll = 0x12012;
constexpr int kParamStereoWidth = 0x12013;
constexpr int kParamStereoSampleWise = 0x12014;

constexpr int kParamMultiAzimuth = 0x12020;
constexpr int kParamMultiElevation = 0x12021;
constexpr int kParamMultiGain = 0x12022;
constexpr int kParamMultiMute = 0x12023;

constexpr int kParamGranularAzimuth = 0x12030;
constexpr int kParamGranularElevation = 0x12031;
constexpr int kParamGranularShape = 0x12032;
constexpr int kParamGranularSize = 0x12033;
constexpr int kParamGranularRoll = 0x12034;
constexpr int kParamGranularWidth = 0x12035;
constexpr int kParamGranularDeltaTime = 0x12036;
constexpr int kParamGranularDeltaTimeMod = 0x12037;
constexpr int kParamGranularGrainLength = 0x12038;
constexpr int kParamGranularGrainLengthMod = 0x12039;
constexpr int kParamGranularReadPosition = 0x1203A;
constexpr int kParamGranularPositionMod = 0x1203B;
constexpr int kParamGranularPitch = 0x1203C;
constexpr int kParamGranularPitchMod = 0x1203D;
constexpr int kParamGranularWindowAttack = 0x1203E;
constexpr int kParamGranularAttackMod = 0x1203F;
constexpr int kParamGranularWindowDecay = 0x12040;
constexpr int kParamGranularDecayMod = 0x12041;
constexpr int kParamGranularMix = 0x12042;
constexpr int kParamGranularSourceProbability = 0x12043;
constexpr int kParamGranularSpatialMode = 0x12044;
constexpr int kParamGranularSampleWise = 0x12045;

constexpr int kParamRotationYaw = 0x12050;
constexpr int kParamRotationPitch = 0x12051;
constexpr int kParamRotationRoll = 0x12052;
constexpr int kParamRotationInvertYaw = 0x12053;
constexpr int kParamRotationInvertPitch = 0x12054;
constexpr int kParamRotationInvertRoll = 0x12055;
constexpr int kParamRotationInvertOverall = 0x12056;
constexpr int kParamRotationSequence = 0x12057;

constexpr int kParamHeadphoneEq = 0x12060;

constexpr int kParamIemResourceReset = 0x12100;
constexpr int kCommandResetRotation = 0x12101;
constexpr int kCommandGranularFreeze = 0x12102;
constexpr int kCommandResetIemRuntime = 0x12103;

enum class EncoderMode : uint32_t {
    STEREO = 0,
    MULTI = 1,
    GRANULAR = 2,
};

enum class LatencyProfile : uint32_t {
    LOW = 0,
    BALANCED = 1,
    STABLE = 2,
};

enum class GranularSpatialMode : uint32_t {
    THREE_D = 0,
    TWO_D = 1,
};

enum class RotationSequence : uint32_t {
    YAW_PITCH_ROLL = 0,
    ROLL_PITCH_YAW = 1,
};

enum class HeadphoneEqId : int32_t {
    OFF = -1,
    AKG_K1000_CLOSED = 0,
    AKG_K1000_OPEN = 1,
    AKG_K141_MK2 = 2,
    AKG_K240_DF = 3,
    AKG_K240_MK2 = 4,
    AKG_K271_MK2 = 5,
    AKG_K271_STUDIO = 6,
    AKG_K601 = 7,
    AKG_K701 = 8,
    AKG_K702 = 9,
    AUDIO_TECHNICA_ATH_M50 = 10,
    BEYERDYNAMIC_DT250 = 11,
    BEYERDYNAMIC_DT770_PRO_250_OHM = 12,
    BEYERDYNAMIC_DT880 = 13,
    BEYERDYNAMIC_DT990_PRO = 14,
    PRESONUS_HD7 = 15,
    SENNHEISER_HD430 = 16,
    SENNHEISER_HD480 = 17,
    SENNHEISER_HD560_OVATION_II = 18,
    SENNHEISER_HD565_OVATION = 19,
    SENNHEISER_HD600 = 20,
    SENNHEISER_HD650 = 21,
    SHURE_SRH940 = 22,
};

struct StereoParams {
    int32_t azimuth_centidegrees = 0;
    int32_t elevation_centidegrees = 0;
    int32_t roll_centidegrees = 0;
    int32_t width_centidegrees = 6000;
    bool sample_wise = false;
};

struct MultiParams {
    std::array<int32_t, 2> azimuth_centidegrees{-3000, 3000};
    std::array<int32_t, 2> elevation_centidegrees{0, 0};
    std::array<int32_t, 2> gain_decidb{0, 0};
    std::array<bool, 2> mute{false, false};
};

struct GranularParams {
    int32_t azimuth_centidegrees = 0;
    int32_t elevation_centidegrees = 0;
    int32_t shape_tenths = 0;
    int32_t size_centidegrees = 18000;
    int32_t roll_centidegrees = 0;
    int32_t width_centidegrees = 0;
    int32_t delta_time_us = 5000;
    int32_t delta_time_mod_tenths_percent = 0;
    int32_t grain_length_us = 250000;
    int32_t grain_length_mod_tenths_percent = 0;
    int32_t read_position_us = 0;
    int32_t position_mod_us = 50000;
    int32_t pitch_millisem = 0;
    int32_t pitch_mod_millisem = 0;
    int32_t attack_tenths_percent = 500;
    int32_t attack_mod_tenths_percent = 0;
    int32_t decay_tenths_percent = 500;
    int32_t decay_mod_tenths_percent = 0;
    int32_t mix_tenths_percent = 500;
    int32_t source_probability_hundredths = 0;
    GranularSpatialMode spatial_mode = GranularSpatialMode::THREE_D;
    bool sample_wise = false;
};

struct RotationParams {
    int32_t yaw_centidegrees = 0;
    int32_t pitch_centidegrees = 0;
    int32_t roll_centidegrees = 0;
    bool invert_yaw = false;
    bool invert_pitch = false;
    bool invert_roll = false;
    bool invert_overall = false;
    RotationSequence sequence = RotationSequence::ROLL_PITCH_YAW;
};

struct DecoderParams {
    HeadphoneEqId headphone_eq = HeadphoneEqId::OFF;
};

struct LimiterParams {
    bool enabled = true;
    int32_t ceiling_centidb = -30;
};

struct IemParams {
    bool enable = false;
    float wet = 1.0F;
    float output_gain_db = 0.0F;
    uint32_t order = 3;
    EncoderMode encoder_mode = EncoderMode::STEREO;
    LatencyProfile latency_profile = LatencyProfile::BALANCED;
    LimiterParams limiter{};
    StereoParams stereo{};
    MultiParams multi{};
    GranularParams granular{};
    RotationParams rotation{};
    DecoderParams decoder{};
};

enum class ParamUpdate {
    NOT_IEM,
    UPDATED,
    COMMAND,
    INVALID,
};

static_assert(std::is_trivially_copyable_v<IemParams>);

bool HasStructuralDifference(const IemParams &left, const IemParams &right) noexcept;

ParamUpdate UpdateIemParameterSnapshot(
    IemParams &params,
    int param,
    int val1,
    int val2,
    int val3
) noexcept;

} // namespace iem
