# Halo Downmix Decoder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the current `SimpleDecoder` with a configurable, realtime-safe Halo Downmix renderer for the project's fixed ACN/SN3D-to-7.1-to-stereo path, excluding Monofilter.

**Architecture:** A new parameter/mapping module owns the clean-room Halo Downmix contract and precomputed 96 kHz coefficients. `HaloDownmixProcessor` processes an eight-plane logical bed, while `HaloDownmixDecoder` keeps the existing Ambisonics virtual-speaker reconstruction and delegates the resulting bed plus optional LFE sideband to the processor. `IemPipeline` selects the new decoder, accounts for its fixed 3,072-sample latency, and the App persists/dispatches all relevant controls through the existing HIDL path.

**Tech Stack:** C++20, CMake/CTest, Android NDK, Kotlin, Jetpack Compose, MiuiX 0.9.x, JUnit4, Gradle.

## Global Constraints

- Internal IEM DSP sample rate remains exactly 96,000 Hz.
- Decoder base latency is exactly 3,072 samples; relative delay controls clamp to `0..32000` microseconds.
- Delay retarget crossfade is exactly 10,000 samples (`0.0001` per sample).
- Center divergence ramps over 100 samples; M/S and output trims ramp over 1,024 samples.
- Frequency mapping is `20 * 1100^x`; gain mapping is `90*x - 70 dB`.
- Normal stereo routing is `L+C+Ls+Lsr+LFE` and `R+C+Rs+Rsr+LFE` with optional center/LFE division by two.
- Monofilter, arbitrary layout negotiation, Monitor In, plugin presets, solo/mute, phase inversion, and KU100 changes are out of scope.
- Keep the audio callback allocation-free, lock-free, log-free, and free of trigonometric/logarithmic/exponential coefficient work.
- Keep AIDL hidden and the IEM interface version unchanged; extend only the legacy HIDL parameter path.
- Do not edit MiuiX library source; use project-local wrappers and public MiuiX APIs.
- Preserve existing Halo Off and KU100 behavior; only render mode integer `1` changes from Simple to Halo Downmix.

---

### Task 1: Lock The Native Parameter And Mapping Contract

**Files:**
- Create: `IEMDSP/include/iem/HaloDownmixParams.h`
- Create: `IEMDSP/src/HaloDownmixParams.cpp`
- Modify: `IEMDSP/include/iem/IemParams.h`
- Modify: `IEMDSP/src/IemParams.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Create: `tests/IemHaloDownmixParamsTest.cpp`
- Modify: `tests/IemParamsTest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Existing `ParamUpdate`, `SetBool`, clamping conventions, and 96 kHz snapshot update path.
- Produces: `HaloDownmixParams`, `HaloDownmixDerived`, mapping helpers, parameter IDs `0x12090..0x120AC`, and fully precomputed decoder state for later tasks.

- [ ] **Step 1: Write the mapping and default tests**

Create tests that lock these exact contracts:

```cpp
static_assert(iem::kParamHaloDownmixDelayEnable == 0x12090);
static_assert(iem::kParamHaloDownmixOutputRightTrim == 0x120AC);

iem::HaloDownmixParams params{};
CHECK(params.delay_enable);
CHECK(params.ls_delay_us == 0);
CHECK(params.center_trim_millionths == 744444);
CHECK(params.rear_mid_trim_millionths == 711111);
CHECK(params.lfe_lpf_frequency_millionths == 328797);

CHECK_NEAR(iem::HaloDownmixFrequencyHz(0), 20.0F, 1.0e-5F);
CHECK_NEAR(iem::HaloDownmixFrequencyHz(1000000), 22000.0F, 0.02F);
CHECK_NEAR(iem::HaloDownmixGainDb(0), -70.0F, 1.0e-6F);
CHECK_NEAR(iem::HaloDownmixGainDb(1000000), 20.0F, 1.0e-6F);
```

Add clamp assertions to `IemParamsTest.cpp` for delay `-1 -> 0`, `50000 -> 32000`, and every normalized field `-1 -> 0`, `1000001 -> 1000000`.

- [ ] **Step 2: Run the focused tests and verify they fail**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON && cmake --build build-host -j2 --target iem_halo_downmix_params_test iem_params_test'
```

Expected: compilation fails because the Halo Downmix types and IDs do not exist.

- [ ] **Step 3: Add the parameter types and exact mappings**

Define these public structures and helpers:

```cpp
struct HaloDownmixBiquadCoefficients {
    float b0 = 1.0F;
    float b1 = 0.0F;
    float b2 = 0.0F;
    float a1 = 0.0F;
    float a2 = 0.0F;
};

struct HaloDownmixBalanceCoefficients {
    float left_a = 1.0F;
    float left_b = 0.0F;
    float right_a = 1.0F;
    float right_b = 0.0F;
};

struct HaloDownmixDerived {
    HaloDownmixBiquadCoefficients side_shelf{};
    HaloDownmixBiquadCoefficients rear_shelf{};
    HaloDownmixBiquadCoefficients lfe_low_pass{};
    HaloDownmixBiquadCoefficients output_high_pass{};
    HaloDownmixBalanceCoefficients balance{};
    float trim_linear[10]{};
};

float HaloDownmixFrequencyHz(int32_t millionths) noexcept;
float HaloDownmixGainDb(int32_t millionths) noexcept;
float HaloDownmixGainLinear(int32_t millionths) noexcept;
HaloDownmixBalanceCoefficients MakeHaloDownmixBalance(
    int32_t left_millionths,
    int32_t right_millionths) noexcept;
void RefreshHaloDownmixDerived(HaloDownmixParams &params) noexcept;
```

Use the confirmed `S=1` high-shelf formula and `Q=1` output high-pass/low-pass formulas from the spec. For the default 200 Hz LFE low-pass at 96 kHz, lock this fixture: `b0=0.0000425576816`, `b1=0.0000851153632`, `b2=0.0000425576816`, `a1=-1.98682529`, `a2=0.986995516`.

- [ ] **Step 4: Wire IDs, clamp rules, and derived refresh**

Append IDs `0x12090..0x120AC` to `IemParams.h`, add `HaloDownmixParams downmix{}` to `DecoderParams`, and add one `UpdateIemParameterSnapshot` case per field. Frequency/gain/pan cases must call `RefreshHaloDownmixDerived`; boolean and delay-only updates must not invoke expensive math unnecessarily.

Use this exact sequence:

```cpp
kParamHaloDownmixDelayEnable = 0x12090,
kParamHaloDownmixLsDelay = 0x12091,
kParamHaloDownmixRsDelay = 0x12092,
kParamHaloDownmixLsrDelay = 0x12093,
kParamHaloDownmixRsrDelay = 0x12094,
kParamHaloDownmixSideShelfEnable = 0x12095,
kParamHaloDownmixSideShelfFrequency = 0x12096,
kParamHaloDownmixSideShelfGain = 0x12097,
kParamHaloDownmixRearShelfEnable = 0x12098,
kParamHaloDownmixRearShelfFrequency = 0x12099,
kParamHaloDownmixRearShelfGain = 0x1209A,
kParamHaloDownmixPanLeft = 0x1209B,
kParamHaloDownmixPanRight = 0x1209C,
kParamHaloDownmixCenterDivergence = 0x1209D,
kParamHaloDownmixFrontMidTrim = 0x1209E,
kParamHaloDownmixFrontSideTrim = 0x1209F,
kParamHaloDownmixCenterTrim = 0x120A0,
kParamHaloDownmixSurroundMidTrim = 0x120A1,
kParamHaloDownmixSurroundSideTrim = 0x120A2,
kParamHaloDownmixRearMidTrim = 0x120A3,
kParamHaloDownmixRearSideTrim = 0x120A4,
kParamHaloDownmixLfeTrim = 0x120A5,
kParamHaloDownmixLfeLpfEnable = 0x120A6,
kParamHaloDownmixLfeLpfFrequency = 0x120A7,
kParamHaloDownmixScaleInputByOutputCount = 0x120A8,
kParamHaloDownmixOutputHpfEnable = 0x120A9,
kParamHaloDownmixOutputHpfFrequency = 0x120AA,
kParamHaloDownmixOutputLeftTrim = 0x120AB,
kParamHaloDownmixOutputRightTrim = 0x120AC,
```

- [ ] **Step 5: Run the focused tests**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_halo_downmix_params_test iem_params_test && ctest --test-dir build-host -R "iem_halo_downmix_params_test|iem_params_test" --output-on-failure'
```

Expected: both tests pass.

- [ ] **Step 6: Commit**

```bash
git add IEMDSP/include/iem/HaloDownmixParams.h IEMDSP/src/HaloDownmixParams.cpp IEMDSP/include/iem/IemParams.h IEMDSP/src/IemParams.cpp IEMDSP/CMakeLists.txt tests/IemHaloDownmixParamsTest.cpp tests/IemParamsTest.cpp CMakeLists.txt
git commit -m "feat(iem): add Halo Downmix parameter contract"
```

---

### Task 2: Implement The Fixed 7.1-To-Stereo Downmix Processor

**Files:**
- Create: `IEMDSP/include/iem/HaloDownmixProcessor.h`
- Create: `IEMDSP/src/HaloDownmixProcessor.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Create: `tests/IemHaloDownmixProcessorTest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `HaloDownmixParams` and precomputed `HaloDownmixDerived` from Task 1.
- Produces: A reusable eight-plane logical-bed processor with exact routing, filtering, smoothing, reset, and fixed latency.

- [ ] **Step 1: Write role-routing and scaling tests**

Use one-sample impulses after the 3,072-sample base delay. Assert:

```cpp
// L routes only left; C and LFE route to both; Lsr routes only left.
CHECK_NEAR(output_left[3072], 1.0F, 1.0e-6F);
CHECK_NEAR(output_right[3072], 0.0F, 1.0e-6F);

// ScaleIPByOPCount divides C and LFE by two, not Ls/Lsr.
params.scale_input_by_output_count = true;
CHECK_NEAR(center_left, 0.5F, 1.0e-6F);
CHECK_NEAR(center_right, 0.5F, 1.0e-6F);
```

Cover all eight logical roles: `L, R, C, LFE, Ls, Rs, Lsr, Rsr`.

- [ ] **Step 2: Write image and trim tests**

Lock the confirmed formulas:

```cpp
params.pan_left_millionths = 1000000;
params.pan_right_millionths = 1000000;
// Identity.

params.divergence_millionths = 1000000;
// Center becomes zero; each front channel receives C/2.

params.front_mid_trim_millionths = 711111; // -6 dB
params.front_side_trim_millionths = 777778; // 0 dB
// Equal L/R input changes only through the mid gain.
```

Assert 100-sample divergence ramps and 1,024-sample trim ramps complete exactly at their specified lengths.

- [ ] **Step 3: Write delay and filter tests**

Assert:

- delay disabled puts every role at sample 3,072;
- `LsDelay=10000 us` at 96 kHz advances Ls by 960 samples to sample 2,112;
- a delay-target update crossfades for exactly 10,000 samples;
- side shelf affects only Ls/Rs;
- rear shelf affects only Lsr/Rsr;
- LFE LPF affects only LFE;
- output HPF affects both final outputs;
- `Reset()` reproduces the first impulse response exactly.

- [ ] **Step 4: Run the processor test and verify it fails**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_halo_downmix_processor_test'
```

Expected: compilation fails because `HaloDownmixProcessor` does not exist.

- [ ] **Step 5: Implement the processor without process-time allocation**

Expose this interface:

```cpp
class HaloDownmixProcessor {
public:
    static constexpr uint32_t kSampleRate = 96000;
    static constexpr uint32_t kLatencyFrames = 3072;
    static constexpr uint32_t kRoleCount = 8;

    bool Prepare(std::size_t max_frames) noexcept;
    void ApplyParams(const HaloDownmixParams &params) noexcept;
    void Reset() noexcept;
    bool Process(
        const float *const inputs[kRoleCount],
        float *const outputs[2],
        std::size_t frames) noexcept;
    std::size_t PreparedBytes() const noexcept;
};
```

Allocate role delay rings and scratch planes in `Prepare`. Implement small project-local `BiquadState`, `LinearRamp`, and delay-crossfade structs privately in the `.cpp`; do not create general framework classes.

- [ ] **Step 6: Run processor and realtime tests**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_halo_downmix_processor_test iem_realtime_audit_test && ctest --test-dir build-host -R "iem_halo_downmix_processor_test|iem_realtime_audit_test" --output-on-failure'
```

Expected: routing/filter tests pass and realtime audit reports no allocation.

- [ ] **Step 7: Commit**

```bash
git add IEMDSP/include/iem/HaloDownmixProcessor.h IEMDSP/src/HaloDownmixProcessor.cpp IEMDSP/CMakeLists.txt tests/IemHaloDownmixProcessorTest.cpp CMakeLists.txt
git commit -m "feat(iem): implement Halo Downmix bed processor"
```

---

### Task 3: Replace SimpleDecoder With HaloDownmixDecoder

**Files:**
- Create: `IEMDSP/include/iem/HaloDownmixDecoder.h`
- Create: `IEMDSP/src/HaloDownmixDecoder.cpp`
- Delete: `IEMDSP/include/iem/SimpleDecoder.h`
- Delete: `IEMDSP/src/SimpleDecoder.cpp`
- Delete: `tests/IemSimpleDecoderTest.cpp`
- Create: `tests/IemHaloDownmixDecoderTest.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Existing ACN/SN3D spherical-harmonic evaluation and `HaloDownmixProcessor`.
- Produces: `HaloDownmixDecoder`, which reconstructs the seven virtual speakers and processes optional LFE.

- [ ] **Step 1: Port existing decoder invariants into the new test**

Retain order validation and virtual-speaker geometry tests from `IemSimpleDecoderTest.cpp`, then add LFE and latency assertions:

```cpp
iem::HaloDownmixDecoder decoder;
CHECK(decoder.Prepare(3, 96000, 256));
CHECK(decoder.LatencyFrames() == 3072);

const float *lfe = lfe_storage.data();
CHECK(decoder.Process(ambi, lfe, stereo, frames));
CHECK_NEAR(left[3072], right[3072], 1.0e-6F);
```

Add chunk-invariance coverage comparing one 4,096-frame call with sixteen 256-frame calls.

- [ ] **Step 2: Run the new test and verify it fails**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_halo_downmix_decoder_test'
```

Expected: target or header is missing.

- [ ] **Step 3: Implement the decoder wrapper**

Expose:

```cpp
class HaloDownmixDecoder {
public:
    bool Prepare(uint32_t order, uint32_t sample_rate,
        std::size_t max_frames) noexcept;
    void ApplyParams(const HaloDownmixParams &params) noexcept;
    void Reset() noexcept;
    bool Process(const float *const *ambi, const float *lfe,
        float *const stereo[2], std::size_t frames) noexcept;
    uint32_t LatencyFrames() const noexcept { return 3072U; }
    std::size_t PreparedBytes() const noexcept;
};
```

Reuse the seven azimuths and direct spherical-harmonic matrix from `SimpleDecoder`. Decode into seven preallocated planes, provide a preallocated zero LFE plane when `lfe == nullptr`, and call `HaloDownmixProcessor`.

- [ ] **Step 4: Remove SimpleDecoder and update build targets**

Delete the old files, replace `iem_simple_decoder_test` with `iem_halo_downmix_decoder_test`, and ensure no source or CMake reference to `SimpleDecoder` remains:

```bash
rg 'SimpleDecoder|iem_simple_decoder_test' IEMDSP tests CMakeLists.txt
```

Expected: no matches.

- [ ] **Step 5: Run decoder tests**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON && cmake --build build-host -j2 --target iem_halo_downmix_decoder_test && ctest --test-dir build-host -R iem_halo_downmix_decoder_test --output-on-failure'
```

Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add IEMDSP/include/iem/HaloDownmixDecoder.h IEMDSP/src/HaloDownmixDecoder.cpp IEMDSP/include/iem/SimpleDecoder.h IEMDSP/src/SimpleDecoder.cpp IEMDSP/CMakeLists.txt tests/IemSimpleDecoderTest.cpp tests/IemHaloDownmixDecoderTest.cpp CMakeLists.txt
git commit -m "feat(iem): replace Simple decoder with Halo Downmix"
```

---

### Task 4: Integrate Halo Downmix Into IemPipeline

**Files:**
- Modify: `IEMDSP/include/iem/IemPipeline.h`
- Modify: `IEMDSP/src/IemPipeline.cpp`
- Modify: `tests/IemPipelineTest.cpp`
- Modify: `tests/IemRealtimeAuditTest.cpp`

**Interfaces:**
- Consumes: `HaloDownmixDecoder` from Task 3 and existing explicit Halo LFE sideband.
- Produces: Correct SIMPLE/OFF selection, no duplicate LFE, updated dry/wet alignment, and profile ceilings `40/64/96 ms`.

- [ ] **Step 1: Add failing pipeline latency tests**

Lock these equations:

```cpp
// Non-Halo SIMPLE and non-Halo OFF.
CHECK(pipeline.WetLatencyFrames() == 3072U);

// Halo SIMPLE.
CHECK(pipeline.WetLatencyFrames()
    == halo_encoder.StftLatencyFrames() + 3072U);

// Halo OFF remains only the Halo encoder latency.
CHECK(halo_off.WetLatencyFrames() == halo_encoder.StftLatencyFrames());
```

Add impulse tests proving Halo SIMPLE LFE appears exactly once, is invariant under scene rotation/order, and is delayed by the same 3,072 samples as the directional bed.

- [ ] **Step 2: Run pipeline tests and verify they fail**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_pipeline_test'
```

Expected: build fails on old `SimpleDecoder` references or latency assertions fail.

- [ ] **Step 3: Replace the pipeline member and prepare/apply/reset calls**

Replace `SimpleDecoder simple_decoder_` with `HaloDownmixDecoder halo_downmix_decoder_`. Prepare with `(order, 96000, max_frames)`, apply `params.decoder.downmix`, and include `LatencyFrames()` only for paths that use the decoder.

Set:

```cpp
constexpr LatencyProfileConfig kLatencyProfiles[3]{
    {64, 1, 50},
    {128, 2, 64},
    {256, 4, 96},
};
```

- [ ] **Step 4: Route LFE through the decoder exactly once**

For render mode `SIMPLE`, pass `halo_lfe` to `HaloDownmixDecoder::Process` and skip `MixDelayedLfe`. For non-Halo OFF, pass `nullptr`. Keep Halo OFF's direct fold and KU100's decoder-aligned `MixDelayedLfe` unchanged.

- [ ] **Step 5: Run pipeline and realtime tests**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_pipeline_test iem_realtime_audit_test && ctest --test-dir build-host -R "iem_pipeline_test|iem_realtime_audit_test" --output-on-failure'
```

Expected: both pass; realtime audit reports no allocation or lock use.

- [ ] **Step 6: Commit**

```bash
git add IEMDSP/include/iem/IemPipeline.h IEMDSP/src/IemPipeline.cpp tests/IemPipelineTest.cpp tests/IemRealtimeAuditTest.cpp
git commit -m "feat(iem): route Halo Downmix through the pipeline"
```

---

### Task 5: Persist And Dispatch App Downmix State

**Files:**
- Modify: `app/src/main/java/com/llsl/viper4android/effect/EffectStates.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/effect/IemEffect.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/viper/ViperParams.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/viper/ViperDispatcher.kt`
- Modify: `app/src/test/java/com/llsl/viper4android/effect/IemStateContractTest.kt`
- Modify: `app/src/test/java/com/llsl/viper4android/viper/IemDispatchTest.kt`

**Interfaces:**
- Consumes: Native IDs and defaults from Task 1.
- Produces: `IemDownmixState`, preference definitions, normalization, profile round-trip, and 29 HIDL scalar writes.

- [ ] **Step 1: Add failing App state/default tests**

Define expected defaults in tests:

```kotlin
val downmix = IemState().decoder.downmix
assertTrue(downmix.delayEnabled)
assertEquals(744444, downmix.centerTrimMillionths)
assertEquals(711111, downmix.rearMidTrimMillionths)
assertFalse(downmix.lfeLpfEnabled)
assertFalse(downmix.outputHpfEnabled)
```

Add normalization assertions for `-1`, `32_001`, and `1_000_001`, plus JSON/profile round-trip coverage for every field.

- [ ] **Step 2: Add failing ID and dispatch tests**

Assert exact IDs `0x12090..0x120AC`, write count increase by 29, and representative values for delay, pan, center trim, LFE LPF, HPF, and output trim.

- [ ] **Step 3: Run focused App tests and verify they fail**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && bash ./gradlew :app:testDebugUnitTest --tests "com.llsl.viper4android.effect.IemStateContractTest" --tests "com.llsl.viper4android.viper.IemDispatchTest" --no-daemon'
```

Expected: compilation fails because `IemDownmixState` and IDs are missing.

- [ ] **Step 4: Implement state, preferences, and dispatch**

Add:

```kotlin
data class IemDownmixState(
    val delayEnabled: Boolean = true,
    val lsDelayUs: Int = 0,
    val rsDelayUs: Int = 0,
    val lsrDelayUs: Int = 0,
    val rsrDelayUs: Int = 0,
    val sideShelfEnabled: Boolean = false,
    val sideShelfFrequencyMillionths: Int = 229819,
    val sideShelfGainMillionths: Int = 777778,
    val rearShelfEnabled: Boolean = false,
    val rearShelfFrequencyMillionths: Int = 229819,
    val rearShelfGainMillionths: Int = 777778,
    val panLeftMillionths: Int = 1000000,
    val panRightMillionths: Int = 1000000,
    val centerDivergenceMillionths: Int = 0,
    val frontMidTrimMillionths: Int = 777778,
    val frontSideTrimMillionths: Int = 777778,
    val centerTrimMillionths: Int = 744444,
    val surroundMidTrimMillionths: Int = 744444,
    val surroundSideTrimMillionths: Int = 744444,
    val rearMidTrimMillionths: Int = 711111,
    val rearSideTrimMillionths: Int = 711111,
    val lfeTrimMillionths: Int = 777778,
    val lfeLpfEnabled: Boolean = false,
    val lfeLpfFrequencyMillionths: Int = 328797,
    val scaleInputByOutputCount: Boolean = false,
    val outputHpfEnabled: Boolean = false,
    val outputHpfFrequencyMillionths: Int = 57898,
    val outputLeftTrimMillionths: Int = 777778,
    val outputRightTrimMillionths: Int = 777778,
)

data class IemDecoderState(
    val headphoneEq: Int = -1,
    val downmix: IemDownmixState = IemDownmixState(),
)
```

Use explicit `int`/`bool` preferences with delay range `0..32000` and normalized range `0..1000000`. Append writes after `PARAM_IEM_HEADPHONE_EQ` in stable ascending-ID order.

- [ ] **Step 5: Run focused App tests**

Run the command from Step 3. Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add app/src/main/java/com/llsl/viper4android/effect/EffectStates.kt app/src/main/java/com/llsl/viper4android/effect/IemEffect.kt app/src/main/java/com/llsl/viper4android/viper/ViperParams.kt app/src/main/java/com/llsl/viper4android/viper/ViperDispatcher.kt app/src/test/java/com/llsl/viper4android/effect/IemStateContractTest.kt app/src/test/java/com/llsl/viper4android/viper/IemDispatchTest.kt
git commit -m "feat(iem): persist Halo Downmix controls"
```

---

### Task 6: Add The Halo Downmix Decoder UI

**Files:**
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/editor/IemEditorScreen.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/main/EffectSectionSummaries.kt`
- Modify: `app/src/main/res/values/strings.xml`
- Modify: `app/src/main/res/values-zh-rCN/strings.xml`
- Modify: `app/src/main/res/values-ru/strings.xml`
- Modify: `app/src/test/java/com/llsl/viper4android/ui/screens/editor/IemEditorContractTest.kt`
- Modify: `app/src/test/java/com/llsl/viper4android/ui/screens/editor/IemEditorLocalizationPolicyTest.kt`
- Modify: `app/src/test/java/com/llsl/viper4android/ui/screens/main/IemCardPolicyTest.kt`

**Interfaces:**
- Consumes: `IemDownmixState` and `Effects.iem` preferences from Task 5.
- Produces: User-facing `Halo Downmix` render label and grouped MiuiX controls shown only for render mode `1`.

- [ ] **Step 1: Re-read MiuiX docs and source**

Read the local Slider, Switch, Card, and TopAppBar docs/source required by `Anget.md`; do not guess package names or edit MiuiX.

- [ ] **Step 2: Add failing UI contract tests**

Assert:

```kotlin
assertEquals(listOf("off", "haloDownmix", "ku100"), iemRenderModes())
assertEquals(29, iemDownmixControls().size)
assertEquals(100.0, haloDownmixFrequencyHz(229819), 0.1)
assertEquals(0.0, haloDownmixGainDb(777778), 0.001)
assertEquals("Halo Downmix", iemSummary(...renderMode = 1...))
```

Require every new `iem_editor_downmix_*` key in all existing locale files.

- [ ] **Step 3: Run focused UI tests and verify they fail**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && bash ./gradlew :app:testDebugUnitTest --tests "com.llsl.viper4android.ui.screens.editor.IemEditorContractTest" --tests "com.llsl.viper4android.ui.screens.editor.IemEditorLocalizationPolicyTest" --tests "com.llsl.viper4android.ui.screens.main.IemCardPolicyTest" --no-daemon'
```

Expected: label/mapping/control assertions fail.

- [ ] **Step 4: Rename the mode and add nonlinear display helpers**

Change the render label from `Simple` to `Halo Downmix`. Add pure mapping helpers mirroring native formulas:

```kotlin
fun haloDownmixFrequencyHz(millionths: Int): Double =
    20.0 * 1100.0.pow(millionths.coerceIn(0, 1_000_000) / 1_000_000.0)

fun haloDownmixGainDb(millionths: Int): Double =
    90.0 * millionths.coerceIn(0, 1_000_000) / 1_000_000.0 - 70.0
```

- [ ] **Step 5: Add grouped decoder controls**

When render mode is `1`, render sections in this order:

1. Timing: enable plus Ls/Rs/Lsr/Rsr `0..32 ms`.
2. Tone: side and rear shelf enable, Hz, dB.
3. Image: Pan L/R and center divergence as percentages.
4. Levels: front M/S, center, side M/S, rear M/S, LFE, output L/R in dB.
5. Filters: LFE LPF, output HPF, input-count scaling.

Use `LabeledSlider`/`LabeledSwitch` project wrappers, disable dependent sliders when their filter/shelf toggle is off, and keep headphone EQ enabled only for KU100.

- [ ] **Step 6: Add translations and run UI tests**

Add English, Simplified Chinese, and Russian labels/descriptions for every row and section. Run the Step 3 command; expected: pass.

- [ ] **Step 7: Build the App**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && bash ./gradlew :app:assembleDebug --no-daemon'
```

Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 8: Commit**

```bash
git add app/src/main/java/com/llsl/viper4android/ui/screens/editor/IemEditorScreen.kt app/src/main/java/com/llsl/viper4android/ui/screens/main/EffectSectionSummaries.kt app/src/main/res/values/strings.xml app/src/main/res/values-zh-rCN/strings.xml app/src/main/res/values-ru/strings.xml app/src/test/java/com/llsl/viper4android/ui/screens/editor/IemEditorContractTest.kt app/src/test/java/com/llsl/viper4android/ui/screens/editor/IemEditorLocalizationPolicyTest.kt app/src/test/java/com/llsl/viper4android/ui/screens/main/IemCardPolicyTest.kt
git commit -m "feat(iem): add Halo Downmix editor controls"
```

---

### Task 7: Document, Verify, Build, And Install

**Files:**
- Modify: `README.md`
- Modify: `docs/iem-upstream-attribution.md`
- Modify: `docs/superpowers/specs/2026-08-14-halo-downmix-decoder.md`
- Modify: `docs/superpowers/plans/2026-08-14-halo-downmix-decoder.md`

**Interfaces:**
- Consumes: Completed native and App implementations.
- Produces: Verified host/Android artifacts, updated attribution, installed App, and persistent/live driver copies.

- [ ] **Step 1: Update documentation**

Document that render mode `1` is Halo Downmix, list the included fixed-layout DSP stages, state the 3,072-sample latency, list parameter range `0x12090..0x120AC`, and explicitly state that Monofilter and arbitrary layout negotiation are excluded.

- [ ] **Step 2: Run the complete host suite**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON && cmake --build build-host -j2 && ctest --test-dir build-host --output-on-failure'
```

Expected: all tests pass.

- [ ] **Step 3: Run UBSan and ASan gates**

Configure/build/test the existing UBSan tree and compile/link the existing ASan tree. Expected: UBSan tests pass; ASan binaries build successfully, with the known AndroidIDE runtime limitation documented if execution is unavailable.

- [ ] **Step 4: Build both Android ABIs**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && make libs'
```

Expected: `out/libv4a_re_arm64-v8a.so` and `out/libv4a_re_armeabi-v7a.so` are rebuilt.

- [ ] **Step 5: Run full App gates**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && bash ./gradlew :app:testDebugUnitTest :app:lintDebug :app:assembleDebug --no-daemon'
```

Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 6: Install without restarting audio services**

Install the debug APK with `pm install -r`. Copy both driver ABIs into `/data/adb/modules/ViPER4Android-RE/system/vendor/...`, update the existing `ainur_jamesdsp/system/effect_mount` backing files in place, and verify live `/vendor/lib*/soundfx/libv4a_re.so` hashes match the new artifacts. Do not restart `audioserver` or other services unless the user explicitly asks.

- [ ] **Step 7: Commit documentation**

```bash
git add README.md docs/iem-upstream-attribution.md docs/superpowers/specs/2026-08-14-halo-downmix-decoder.md docs/superpowers/plans/2026-08-14-halo-downmix-decoder.md
git commit -m "docs(iem): document Halo Downmix decoder"
```

- [ ] **Step 8: Inspect final status**

Run `git status --short` in both repositories and report any pre-existing or unrelated changes without reverting them.
