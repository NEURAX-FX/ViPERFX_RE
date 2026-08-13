# IEM Phase 1 Encoders, Rotation, KU100, and App Controls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a tested HIDL/legacy AudioEffect IEM path with Stereo, two-source Multi, full Granular encoding, manual Scene Rotation, Ord1-Ord3 KU100 decoding, 23 headphone EQs, aligned output controls, telemetry, and MiuiX App controls.

**Architecture:** Extend the Phase 0 fixed 96 kHz IEM engine into a staged `Encoder -> ACN/SN3D bus -> Rotator -> KU100 -> Headphone EQ -> Mix -> Limiter` pipeline. Dynamic parameters use the existing IEM mailbox; Encoder Mode, Order, Headphone EQ, and Latency Profile rebuild a pending graph on the control thread and publish through the existing graph slots. The App persists all controls except Freeze and sends full or incremental state through `AudioEffect.setParameter`; AIDL mmap format 6 is not modified.

**Tech Stack:** C++17, C11 PFFFT, CMake 3.16.3, Android NDK 28.0.13004108, Kotlin, Jetpack Compose, MiuiX 0.9.x, JUnit, CTest, UBSan, ASan.

**Design Spec:** `docs/superpowers/specs/2026-08-11-iem-phase1-encoders-rotation-ku100.md`

## Global Constraints

- Read `/root/AndroidIDEProjects/ViPER4Android/Anget.md` before every App, build, or dependency task.
- Reference IEM Plug-in Suite commit is exactly `39de1dd5883f1bd8d65fe1662487f2470a1d7b55` from `https://git.iem.at/audioplugins/IEMPluginSuite.git`.
- Preserve upstream copyright and GPL-3.0 headers and include the KU100 scientific attributions from the source README files.
- Do not compile JUCE or upstream `PluginProcessor` classes into the driver.
- Keep the internal bus fixed at 96 kHz, planar float, ACN/SN3D, orders 1-3, maximum 16 channels.
- Keep IEM after ViPER and before final host output; disabled IEM adds no steady-state latency or CPU work.
- Do not edit MiuiX source or remove `androidx.compose.material.icons.*` imports.
- Do not add Material3 UI components.
- Do not modify `ConfigChannel.FORMAT_VERSION`, `ViperParamsSerializer`, or the AIDL mmap slot layout in Phase 1.
- The audio callback performs no allocation, lock, file I/O, logging, plan creation, resource conversion, or OS random call.
- Use TDD for every behavior change: fail the focused test, implement the minimum behavior, then run the focused and affected suites.
- Build and test through SSH port 8022. Sync only files named by the active task; do not overwrite unrelated dirty files in either repository.
- The available aarch64 environments expose a 39-bit userspace VA. ASan executables compile and link, but cannot run because the sanitizer shadow mapping requires a larger VA space (Termux reports `SIGILL`; local Linux reports `heap size ... exceeds max user virtual address`). For this execution, run UBSan and normal tests, keep ASan as a compile/link gate, and execute ASan tests later on a 48-bit userspace VA host without changing test code.
- Do not stage `.lcm/`, unrelated App changes, unrelated convolver changes, or user work.
- Commit only the files listed in the active task after its verification passes.

## File Map

### Driver repository: `/root/AndroidIDEProjects/ViPERFX_RE`

- `IEMDSP/include/iem/IemParams.h`, `IEMDSP/src/IemParams.cpp`: complete typed parameter snapshot, fixed IDs, defaults, validation, structural comparison.
- `IEMDSP/upstream/`: pinned pure IEM algorithm files, GPL license, and provenance manifest.
- `IEMDSP/include/iem/SphericalHarmonics.h`, `IEMDSP/src/SphericalHarmonics.cpp`: ACN/SN3D wrapper around pinned efficient SH evaluation.
- `IEMDSP/include/iem/Quaternion.h`: JUCE-free scalar quaternion adapter derived from the pinned upstream reference.
- `IEMDSP/include/iem/IemEncoder.h`: common prepared encoder interface.
- `IEMDSP/include/iem/StereoEncoder.h`, `IEMDSP/src/StereoEncoder.cpp`: stereo direction and width encoder.
- `IEMDSP/include/iem/MultiEncoder.h`, `IEMDSP/src/MultiEncoder.cpp`: two-source L/R encoder.
- `IEMDSP/include/iem/GranularEncoder.h`, `IEMDSP/src/GranularEncoder.cpp`: fixed-pool granular engine and eight-second history.
- `IEMDSP/include/iem/SceneRotator.h`, `IEMDSP/src/SceneRotator.cpp`: order-aware manual Ambisonics rotation.
- `IEMDSP/include/iem/PartitionedMatrixConvolver.h`, `IEMDSP/src/PartitionedMatrixConvolver.cpp`: prepared PFFFT `N -> 2` convolution.
- `IEMDSP/resources/source/`: pinned KU100 and headphone EQ WAV resources plus README attribution.
- `IEMDSP/resources/generated/`: deterministic 96 kHz resource arrays and manifest.
- `tools/IemAssetCompiler.cpp`: host-only resource validator, resampler, and code generator.
- `IEMDSP/include/iem/Ku100Decoder.h`, `IEMDSP/src/Ku100Decoder.cpp`: order-specific resource selection and decode.
- `IEMDSP/include/iem/HeadphoneEq.h`, `IEMDSP/src/HeadphoneEq.cpp`: Off/23-model stereo post-filter.
- `IEMDSP/include/iem/LinkedLookaheadLimiter.h`, `IEMDSP/src/LinkedLookaheadLimiter.cpp`: linked 1 ms lookahead limiter.
- `IEMDSP/include/iem/IemPipeline.h`, `IEMDSP/src/IemPipeline.cpp`: stage ordering, latency accounting, aligned mix, fault isolation.
- `IEMDSP/include/iem/IemTelemetry.h`, `IEMDSP/src/IemTelemetry.cpp`: Phase 1 telemetry fields and consistent publication.
- `IEMDSP/include/iem/IemEngine.h`, `IEMDSP/src/IemEngine.cpp`: host/internal rate bridge around `IemPipeline`.
- `src/IemResources.*`, `src/IemGraph.*`, `src/IemGraphSlots.*`, `src/IemContext.*`: structural graph preparation, publication, transitions, and parameter dispatch.
- `src/TelemetryProtocol.h`, `src/ViperContext.*`: legacy AudioEffect integration and telemetry wire extension.
- `CMakeLists.txt`, `IEMDSP/CMakeLists.txt`, `ViPERDSP/CMakeLists.txt`: common PFFFT target, host tools, tests, and Android linkage.

### App repository: `/root/AndroidIDEProjects/ViPER4Android`

- `app/src/main/java/com/llsl/viper4android/viper/ViperParams.kt`: exact Phase 1 IDs.
- `app/src/main/java/com/llsl/viper4android/effect/EffectStates.kt`: nested persistent `IemState` and transient Freeze field.
- `app/src/main/java/com/llsl/viper4android/effect/EffectGroups.kt`: scalar/list prefs and range normalization, excluding Freeze.
- `app/src/main/java/com/llsl/viper4android/effect/EffectStateStore.kt`: transient command dispatch.
- `app/src/main/java/com/llsl/viper4android/viper/ViperDispatcher.kt`: complete HIDL IEM state publication.
- `app/src/main/java/com/llsl/viper4android/viper/DriverTelemetry.kt`: Phase 1 telemetry parse.
- `app/src/main/java/com/llsl/viper4android/ui/screens/main/EffectSections.kt`, `MainScreen.kt`, `MainViewModel.kt`: HIDL-only main IEM card.
- `app/src/main/java/com/llsl/viper4android/ui/screens/editor/EditorModels.kt`, `EffectEditorScreen.kt`, `EffectEditorViewModel.kt`: IEM route and state actions.
- `app/src/main/java/com/llsl/viper4android/ui/screens/editor/IemEditorScreen.kt`: four-tab IEM editor and attribution footer.
- `app/src/main/res/values*/strings.xml`: English, simplified Chinese, and Russian copy.

---

### Task 1: Lock the Parameter Contract and Defaults

**Files:**
- Modify: `IEMDSP/include/iem/IemParams.h`
- Modify: `IEMDSP/src/IemParams.cpp`
- Modify: `tests/IemParamsTest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Phase 0 `UpdateIemParameterSnapshot(IemParams&, int, int, int, int)`.
- Produces: `EncoderMode`, `LatencyProfile`, `RotationSequence`, `HeadphoneEqId`, complete `IemParams`, `bool HasStructuralDifference(const IemParams&, const IemParams&) noexcept`, and exact IDs from spec section 6.

- [ ] **Step 1: Write failing ID, default, clamp, indexed-source, command, and structural-difference tests**

Add tests with these concrete expectations:

```cpp
static_assert(iem::kParamIemEncoderMode == 0x12004);
static_assert(iem::kParamGranularSampleWise == 0x12045);
static_assert(iem::kParamRotationSequence == 0x12057);
static_assert(iem::kParamHeadphoneEq == 0x12060);
static_assert(iem::kCommandGranularFreeze == 0x12102);

iem::IemParams defaults{};
CHECK(defaults.order == 3);
CHECK(defaults.encoder_mode == iem::EncoderMode::STEREO);
CHECK(defaults.stereo.width_centidegrees == 6000);
CHECK(defaults.multi.azimuth_centidegrees[0] == -3000);
CHECK(defaults.multi.azimuth_centidegrees[1] == 3000);
CHECK(defaults.latency_profile == iem::LatencyProfile::BALANCED);
CHECK(defaults.limiter.enabled);
CHECK(defaults.limiter.ceiling_centidb == -30);

CHECK(iem::UpdateIemParameterSnapshot(defaults, iem::kParamMultiGain, 1, -700, 0)
      == iem::ParamUpdate::UPDATED);
CHECK(defaults.multi.gain_decidb[1] == -600);
CHECK(iem::UpdateIemParameterSnapshot(defaults, iem::kParamMultiGain, 2, 0, 0)
      == iem::ParamUpdate::INVALID);
CHECK(iem::UpdateIemParameterSnapshot(defaults, iem::kCommandGranularFreeze, 1, 0, 0)
      == iem::ParamUpdate::COMMAND);

iem::IemParams dynamic = defaults;
dynamic.rotation.yaw_centidegrees = 9000;
CHECK(!iem::HasStructuralDifference(defaults, dynamic));
dynamic.order = 2;
CHECK(iem::HasStructuralDifference(defaults, dynamic));
```

- [ ] **Step 2: Run the focused test and verify failure**

Run:

```bash
rsync -azR -e 'ssh -p 8022' IEMDSP/include/iem/IemParams.h IEMDSP/src/IemParams.cpp tests/IemParamsTest.cpp CMakeLists.txt 10645@localhost:ViPERFX_RE/
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-host --target iem_params_test -j2 && ctest --test-dir build-host -R "^iem_params_test$" --output-on-failure'
```

Expected: compile failure for the new enums/fields/IDs.

- [ ] **Step 3: Implement the complete snapshot and parser**

Use nested trivially copyable structs with integer canonical units. Keep `wet` and `output_gain_db` compatibility fields only if Phase 0 call sites still require them; otherwise migrate all call sites in the same task. Parser rules are exact: invalid enums/indexes return `INVALID`, continuous values clamp, commands do not mutate persistent fields, and structural comparison checks only mode/order/EQ/profile.

```cpp
enum class EncoderMode : uint32_t { STEREO = 0, MULTI = 1, GRANULAR = 2 };
enum class LatencyProfile : uint32_t { LOW = 0, BALANCED = 1, STABLE = 2 };
enum class ParamUpdate { NOT_IEM, UPDATED, COMMAND, INVALID };

inline bool HasStructuralDifference(const IemParams& a, const IemParams& b) noexcept {
    return a.encoder_mode != b.encoder_mode || a.order != b.order
        || a.decoder.headphone_eq != b.decoder.headphone_eq
        || a.latency_profile != b.latency_profile;
}
```

- [ ] **Step 4: Run focused and Phase 0 regression tests**

Run the command from Step 2, then:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host --target iem_engine_test iem_context_test iem_graph_slots_test -j2 && ctest --test-dir build-host -R "^iem_(engine|context|graph_slots)_test$" --output-on-failure'
```

Expected: all four tests pass.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/include/iem/IemParams.h IEMDSP/src/IemParams.cpp tests/IemParamsTest.cpp CMakeLists.txt
git commit -m "feat: define IEM phase 1 parameter contract"
```

### Task 2: Vendor Provenance, SH Math, and Fixed Real-Time Utilities

**Files:**
- Create: `IEMDSP/upstream/LICENSE`
- Create: `IEMDSP/upstream/UPSTREAM.md`
- Create: `IEMDSP/upstream/efficientSHvanilla.h`
- Create: `IEMDSP/upstream/efficientSHvanilla.cpp`
- Create: `IEMDSP/upstream/Quaternion.h`
- Create: `IEMDSP/include/iem/SphericalHarmonics.h`
- Create: `IEMDSP/src/SphericalHarmonics.cpp`
- Create: `IEMDSP/include/iem/Quaternion.h`
- Create: `IEMDSP/include/iem/FixedRandom.h`
- Create: `IEMDSP/include/iem/LinearSmoother.h`
- Create: `tests/IemSphericalHarmonicsTest.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: ACN/SN3D order 1-3 requirement.
- Produces: `EvaluateSn3d(uint32_t order, float azimuth_radians, float elevation_radians, float out[16]) noexcept`, JUCE-free `Quaternion`, `FixedRandom::NextUnit()`, and `LinearSmoother::Reset/SetTarget/Next`.

- [ ] **Step 1: Pin and copy only approved upstream files**

Run:

```bash
test "$(git -C /tmp/opencode/IEMPluginSuite rev-parse HEAD)" = "39de1dd5883f1bd8d65fe1662487f2470a1d7b55"
cp /tmp/opencode/IEMPluginSuite/LICENSE IEMDSP/upstream/LICENSE
cp /tmp/opencode/IEMPluginSuite/resources/efficientSHvanilla.h IEMDSP/upstream/efficientSHvanilla.h
cp /tmp/opencode/IEMPluginSuite/resources/efficientSHvanilla.cpp IEMDSP/upstream/efficientSHvanilla.cpp
cp /tmp/opencode/IEMPluginSuite/resources/Quaternion.h IEMDSP/upstream/Quaternion.h
```

`UPSTREAM.md` must record repository, commit, original paths, unchanged/adapted status, compile status, GPL-3.0, and the rule that JUCE processors are excluded. Mark upstream `Quaternion.h` as reference-only because it includes `JuceHeader.h`; compile only the local `iem/Quaternion.h` adapter.

- [ ] **Step 2: Write failing SH and utility tests**

Test W-channel normalization, cardinal directions, finite output, deterministic PRNG, and exact smoother endpoints:

```cpp
float sh[16]{};
iem::EvaluateSn3d(3, 0.0F, 0.0F, sh);
CHECK_NEAR(sh[0], 1.0F, 1.0e-6F);
CHECK_NEAR(sh[1], 0.0F, 1.0e-6F);
CHECK_NEAR(sh[2], 0.0F, 1.0e-6F);
CHECK_NEAR(sh[3], 1.0F, 1.0e-6F);

iem::FixedRandom a(0x12345678U), b(0x12345678U);
for (int i = 0; i < 128; ++i) CHECK(a.NextU32() == b.NextU32());

iem::LinearSmoother smoother;
smoother.Reset(0.0F);
smoother.SetTarget(1.0F, 4);
CHECK_NEAR(smoother.Next(), 0.25F, 1.0e-6F);
CHECK_NEAR(smoother.Next(), 0.50F, 1.0e-6F);
CHECK_NEAR(smoother.Next(), 0.75F, 1.0e-6F);
CHECK_NEAR(smoother.Next(), 1.00F, 1.0e-6F);

iem::Quaternion quarter_turn = iem::Quaternion::FromYawPitchRoll(
    1.57079632679F, 0.0F, 0.0F, iem::RotationSequence::YAW_PITCH_ROLL);
const iem::Vec3 rotated = quarter_turn.Rotate({1.0F, 0.0F, 0.0F});
CHECK_NEAR(rotated.x, 0.0F, 1.0e-6F);
CHECK_NEAR(rotated.y, 1.0F, 1.0e-6F);
```

- [ ] **Step 3: Run and verify failure**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-host --target iem_spherical_harmonics_test -j2 && ctest --test-dir build-host -R "^iem_spherical_harmonics_test$" --output-on-failure'
```

Expected: compile failure because the wrappers do not exist.

- [ ] **Step 4: Implement wrappers without JUCE dependencies**

`EvaluateSn3d` converts radians to the pinned evaluator inputs, requests order-limited coefficients, applies the pinned N3D-to-SN3D factors once, and clears channels above `(order + 1)^2`. The local quaternion stores only `w/x/y/z`, implements normalized multiplication, conjugate/inverse, Euler construction for both sequences, and scalar `Vec3` rotation; it has no JUCE include. `FixedRandom` uses xorshift32 with a nonzero fallback seed. `LinearSmoother` stores current, target, increment, and remaining frames; `Next()` lands exactly on target when remaining reaches zero.

- [ ] **Step 5: Verify and commit**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host --target iem_spherical_harmonics_test iem_engine_test iem_context_test -j2 && ctest --test-dir build-host -R "^iem_(spherical_harmonics|engine|context)_test$" --output-on-failure'
```

Expected: all three tests pass.

```bash
git add IEMDSP/upstream IEMDSP/include/iem/SphericalHarmonics.h IEMDSP/src/SphericalHarmonics.cpp IEMDSP/include/iem/Quaternion.h IEMDSP/include/iem/FixedRandom.h IEMDSP/include/iem/LinearSmoother.h tests/IemSphericalHarmonicsTest.cpp IEMDSP/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add pinned IEM spatial math"
```

### Task 3: Stereo and Two-Source Multi Encoders

**Files:**
- Create: `IEMDSP/include/iem/IemEncoder.h`
- Create: `IEMDSP/include/iem/StereoEncoder.h`
- Create: `IEMDSP/src/StereoEncoder.cpp`
- Create: `IEMDSP/include/iem/MultiEncoder.h`
- Create: `IEMDSP/src/MultiEncoder.cpp`
- Create: `tests/IemEncoderTest.cpp`
- Create: `tests/reference/PinnedEncoderReference.h`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `EvaluateSn3d`, `IemParams`, and aligned planar blocks.
- Produces: `IemEncoder`, `StereoEncoder`, and `MultiEncoder` with `Prepare`, `ApplyParams`, `Reset`, and allocation-free `Process`.

- [ ] **Step 1: Write failing reference and behavior tests**

The pinned reference computes source SH gains directly for each test sample. Cover impulse, seeded stereo noise, defaults, mute, gain, order clearing, width, center rotation, block-wise smoothing, and sample-wise mode.

```cpp
iem::StereoEncoder encoder;
CHECK(encoder.Prepare({96000, 256, 3}));
iem::IemParams p{};
p.stereo.width_centidegrees = 6000;
encoder.ApplyParams(p);
ProcessStereoImpulse(encoder, output);
ComparePlanar(output, PinnedStereoReference(p, input), 1.0e-5F);

iem::MultiEncoder multi;
CHECK(multi.Prepare({96000, 256, 3}));
multi.ApplyParams(p);
ProcessStereoImpulse(multi, output);
ComparePlanar(output, PinnedMultiReference(p, input), 1.0e-5F);
p.multi.mute[1] = true;
multi.ApplyParams(p);
CHECK(RightInputContribution(output) == 0.0F);
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-host --target iem_encoder_test -j2'
```

Expected: compile failure for missing encoder headers.

- [ ] **Step 3: Implement the common interface and both encoders**

For Stereo, derive two unit directions from center quaternion and `+/- width / 2`, evaluate one SH vector per source, and accumulate `left * shLeft[ch] + right * shRight[ch]`. For Multi, evaluate each unmuted source independently and apply `pow(10, gain_decidb / 200.0)` before accumulation. Clear all active bus channels before accumulating and clear inactive channels through channel 15.

Sample-wise mode interpolates azimuth/elevation/roll/width over the block and reevaluates coefficients per frame. Block-wise mode evaluates endpoint coefficients once and linearly ramps each coefficient over the block.

- [ ] **Step 4: Verify reference equality and no allocation**

Wrap 1,000 `Process` calls with the Phase 0 allocation counter and assert zero callback allocations. Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host --target iem_encoder_test -j2 && ctest --test-dir build-host -R "^iem_encoder_test$" --output-on-failure && cmake -S . -B build-asan -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" && cmake --build build-asan --target iem_encoder_test -j2 && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan -R "^iem_encoder_test$" --output-on-failure'
```

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/include/iem/IemEncoder.h IEMDSP/include/iem/StereoEncoder.h IEMDSP/src/StereoEncoder.cpp IEMDSP/include/iem/MultiEncoder.h IEMDSP/src/MultiEncoder.cpp tests/IemEncoderTest.cpp tests/reference/PinnedEncoderReference.h IEMDSP/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add stereo and multi IEM encoders"
```

### Task 4: Full Fixed-Pool Granular Encoder

**Files:**
- Create: `IEMDSP/include/iem/GranularEncoder.h`
- Create: `IEMDSP/src/GranularEncoder.cpp`
- Create: `IEMDSP/include/iem/Grain.h`
- Create: `IEMDSP/src/Grain.cpp`
- Create: `tests/IemGranularEncoderTest.cpp`
- Create: `tests/reference/PinnedGranularReference.h`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `IemEncoder`, fixed PRNG, smoother, SH evaluator, and full granular params.
- Produces: `GranularEncoder`, runtime `SetFreeze(bool)`, active/exhausted grain counters, and deterministic prepared seed support.

- [ ] **Step 1: Write failing deterministic, boundary, and Freeze tests**

Use a 440 Hz left/660 Hz right deterministic input and seed `0x6d2b79f5`. Compare production output to the pinned scalar reference for 24,000 frames. Add separate tests for every min/max parameter, eight-second write-head wrap, 512 active grains, pool exhaustion, 2D elevation behavior, block/sample-wise panning, Reset, and Freeze.

```cpp
iem::GranularEncoder granular;
CHECK(granular.Prepare({96000, 256, 3, 0x6d2b79f5U}));
iem::IemParams p{};
p.encoder_mode = iem::EncoderMode::GRANULAR;
granular.ApplyParams(p);
RenderDeterministicInput(granular, 24000, actual);
RenderPinnedGranularReference(p, 0x6d2b79f5U, 24000, expected);
ComparePlanar(actual, expected, 2.0e-5F);

granular.SetFreeze(true);
const uint32_t frozenHead = granular.WriteHeadForTest();
RenderDeterministicInput(granular, 4096, actual);
CHECK(granular.WriteHeadForTest() == frozenHead);
granular.Reset();
CHECK(!granular.IsFrozen());
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-host --target iem_granular_encoder_test -j2'
```

Expected: compile failure for missing implementation.

- [ ] **Step 3: Implement fixed storage and grain scheduling**

Prepare exactly `2 * 8 * 96000` history floats and `std::array<Grain, 512>`. Maintain free and active index arrays with counts, never `std::vector::push_back` in `Process`. Each spawn computes all randomized properties once from the prepared PRNG, clamps the read position into circular history, initializes attack/decay window slopes, pitch increment `exp2(semitones / 12)`, source selection, and SH direction. A finished grain returns its index to the free stack.

Freeze is accepted only after at least one valid history frame. `Reset()` clears history, pools, counters, write head, spawn countdown, smoothers, and Freeze.

- [ ] **Step 4: Run normal, sanitizer, and stress tests**

The test binary itself renders 10 million frames of randomized legal parameters. Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host --target iem_granular_encoder_test -j2 && ctest --test-dir build-host -R "^iem_granular_encoder_test$" --output-on-failure && cmake -S . -B build-ubsan -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all" -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined" && cmake --build build-ubsan --target iem_granular_encoder_test -j2 && ctest --test-dir build-ubsan -R "^iem_granular_encoder_test$" --output-on-failure && cmake -S . -B build-asan -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" && cmake --build build-asan --target iem_granular_encoder_test -j2 && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan -R "^iem_granular_encoder_test$" --output-on-failure'
```

Expected: reference comparisons pass, all samples are finite, no callback allocation, and no sanitizer report.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/include/iem/GranularEncoder.h IEMDSP/src/GranularEncoder.cpp IEMDSP/include/iem/Grain.h IEMDSP/src/Grain.cpp tests/IemGranularEncoderTest.cpp tests/reference/PinnedGranularReference.h IEMDSP/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add fixed-pool granular encoder"
```

### Task 5: Manual Scene Rotator

**Files:**
- Create: `IEMDSP/include/iem/SceneRotator.h`
- Create: `IEMDSP/src/SceneRotator.cpp`
- Create: `tests/IemSceneRotatorTest.cpp`
- Create: `tests/reference/PinnedRotatorReference.h`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: ACN/SN3D planar bus and rotation params.
- Produces: `SceneRotator::Prepare`, `ApplyParams`, `ResetAngles`, `Reset`, and in-place-safe `Process`.

- [ ] **Step 1: Write failing matrix and stream tests**

Cover identity, +/-90 degree yaw, inverse rotations, every axis inversion, overall inversion, both sequences, all three orders, energy preservation, in-place processing, and no matrix recompute when parameter generation is unchanged.

```cpp
for (uint32_t order = 1; order <= 3; ++order) {
    iem::SceneRotator rotator;
    CHECK(rotator.Prepare({96000, 256, order}));
    FillSeededBus(order, input);
    rotator.Process(input, output, 256);
    ComparePlanar(input, output, 1.0e-6F);

    SetYaw(rotator, 9000);
    rotator.Process(input, output, 256);
    ComparePlanar(output, PinnedRotateReference(order, input, 9000, 0, 0), 2.0e-5F);
    CHECK_NEAR(BusEnergy(input), BusEnergy(output), 2.0e-4F);
}
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-host --target iem_scene_rotator_test -j2'
```

Expected: compile failure for the missing class.

- [ ] **Step 3: Implement pinned quaternion and order matrices**

Build the quaternion in the selected sequence, apply axis/overall inversion before matrix construction, and generate order blocks for l=0..order. Cache the last rotation generation and recompute only on change. Use a preallocated scratch bus when input and output alias.

- [ ] **Step 4: Verify and commit**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host --target iem_scene_rotator_test -j2 && ctest --test-dir build-host -R "^iem_scene_rotator_test$" --output-on-failure && cmake --build build-ubsan --target iem_scene_rotator_test -j2 && ctest --test-dir build-ubsan -R "^iem_scene_rotator_test$" --output-on-failure && cmake --build build-asan --target iem_scene_rotator_test -j2 && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan -R "^iem_scene_rotator_test$" --output-on-failure'
```

Expected: PASS and zero callback allocations.

```bash
git add IEMDSP/include/iem/SceneRotator.h IEMDSP/src/SceneRotator.cpp tests/IemSceneRotatorTest.cpp tests/reference/PinnedRotatorReference.h IEMDSP/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add manual IEM scene rotation"
```

### Task 6: Shared PFFFT Target and General N-to-2 Convolver

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `ViPERDSP/CMakeLists.txt`
- Modify: `IEMDSP/CMakeLists.txt`
- Create: `IEMDSP/include/iem/PartitionedMatrixConvolver.h`
- Create: `IEMDSP/src/PartitionedMatrixConvolver.cpp`
- Create: `tests/IemPartitionedMatrixConvolverTest.cpp`

**Interfaces:**
- Consumes: existing `ViPERDSP/viper/utils/pffft.c` exactly once.
- Produces: `pffft_core` CMake target and prepared `PartitionedMatrixConvolver::Prepare(input_channels, ir_frames, partition_frames, output_input_frame_ir)` / `Process(planar_in, stereo_out, frames)`. IR layout is `[output 0..1][input 0..N-1][frame 0..ir_frames-1]`.

- [ ] **Step 1: Write failing convolution tests**

Generate deterministic IR matrices for N=4, 9, and 16 and lengths 1, 236, and 514. Compare random callback sizes against direct time-domain convolution. Verify Reset, arbitrary `1..384` Process frames, finite output, latency reporting, and zero allocation.

```cpp
iem::PartitionedMatrixConvolver convolver;
CHECK(convolver.Prepare(16, 236, 256, ir.data()));
for (std::size_t frames : {1U, 7U, 64U, 255U, 256U, 384U}) {
    ProcessNext(convolver, input, frames, actual);
}
DirectMatrixConvolution(16, 236, input, ir, expected);
CompareDelayedStereo(actual, expected, convolver.LatencyFrames(), 2.0e-4F);
```

- [ ] **Step 2: Refactor PFFFT and verify existing tests before adding the convolver**

Before `add_subdirectory(IEMDSP)` and `add_subdirectory(ViPERDSP)`, create the root target:

```cmake
add_library(pffft_core STATIC ViPERDSP/viper/utils/pffft.c)
target_include_directories(pffft_core PUBLIC ViPERDSP/viper/utils)
```

After each library target exists, add `target_link_libraries(ViPERDSP PUBLIC pffft_core)` in `ViPERDSP/CMakeLists.txt` and `target_link_libraries(IEMDSP PUBLIC pffft_core)` in `IEMDSP/CMakeLists.txt`. Remove direct `pffft.c` compilation from `ViPERDSP` and host tests. Build and run `audio_analyzer_test`, `matrix_convolver_test`, and `convolver_test`; all must pass before continuing.

- [ ] **Step 3: Implement uniform partitioned convolution**

Prepare one forward plan, one inverse plan, input history spectra for every input/partition, and two output accumulators. Reuse one forward transform per input partition across both ears. Normalize inverse PFFFT output exactly once. Buffer partial Process calls without changing stream output. `Reset()` zeroes overlap, spectra history, and partial buffers but keeps plans and IR spectra.

- [ ] **Step 4: Run focused, legacy, sanitizer, and symbol checks**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host --target iem_partitioned_matrix_convolver_test audio_analyzer_test matrix_convolver_test convolver_test v4a_re -j2 && ctest --test-dir build-host -R "^(iem_partitioned_matrix_convolver|audio_analyzer|matrix_convolver|convolver)_test$" --output-on-failure && cmake -S . -B build-ubsan -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all" -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined" && cmake --build build-ubsan --target iem_partitioned_matrix_convolver_test -j2 && ctest --test-dir build-ubsan -R "^iem_partitioned_matrix_convolver_test$" --output-on-failure && cmake -S . -B build-asan -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" && cmake --build build-asan --target iem_partitioned_matrix_convolver_test -j2 && test "$(llvm-nm build-host/libpffft_core.a | grep -c " T pffft_transform$")" = 1'
```

Expected: tests pass; final `v4a_re` link has no duplicate PFFFT definition.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt ViPERDSP/CMakeLists.txt IEMDSP/CMakeLists.txt IEMDSP/include/iem/PartitionedMatrixConvolver.h IEMDSP/src/PartitionedMatrixConvolver.cpp tests/IemPartitionedMatrixConvolverTest.cpp
git commit -m "feat: add shared PFFFT matrix convolution"
```

### Task 7: Reproducible KU100 and Headphone EQ Assets

**Files:**
- Create: `IEMDSP/resources/source/IRs/irsOrd1.wav`
- Create: `IEMDSP/resources/source/IRs/irsOrd2.wav`
- Create: `IEMDSP/resources/source/IRs/irsOrd3.wav`
- Create: `IEMDSP/resources/source/IRs/README.txt`
- Create: `IEMDSP/resources/source/EQ/AKG-K1000-Closed.wav`
- Create: `IEMDSP/resources/source/EQ/AKG-K1000-Open.wav`
- Create: `IEMDSP/resources/source/EQ/AKG-K141MK2.wav`
- Create: `IEMDSP/resources/source/EQ/AKG-K240DF.wav`
- Create: `IEMDSP/resources/source/EQ/AKG-K240MK2.wav`
- Create: `IEMDSP/resources/source/EQ/AKG-K271MK2.wav`
- Create: `IEMDSP/resources/source/EQ/AKG-K271STUDIO.wav`
- Create: `IEMDSP/resources/source/EQ/AKG-K601.wav`
- Create: `IEMDSP/resources/source/EQ/AKG-K701.wav`
- Create: `IEMDSP/resources/source/EQ/AKG-K702.wav`
- Create: `IEMDSP/resources/source/EQ/AudioTechnica-ATH-M50.wav`
- Create: `IEMDSP/resources/source/EQ/Beyerdynamic-DT250.wav`
- Create: `IEMDSP/resources/source/EQ/Beyerdynamic-DT770PRO-250Ohms.wav`
- Create: `IEMDSP/resources/source/EQ/Beyerdynamic-DT880.wav`
- Create: `IEMDSP/resources/source/EQ/Beyerdynamic-DT990PRO.wav`
- Create: `IEMDSP/resources/source/EQ/Presonus-HD7.wav`
- Create: `IEMDSP/resources/source/EQ/Sennheiser-HD430.wav`
- Create: `IEMDSP/resources/source/EQ/Sennheiser-HD480.wav`
- Create: `IEMDSP/resources/source/EQ/Sennheiser-HD560ovationII.wav`
- Create: `IEMDSP/resources/source/EQ/Sennheiser-HD565ovation.wav`
- Create: `IEMDSP/resources/source/EQ/Sennheiser-HD600.wav`
- Create: `IEMDSP/resources/source/EQ/Sennheiser-HD650.wav`
- Create: `IEMDSP/resources/source/EQ/SHURE-SRH940.wav`
- Create: `IEMDSP/resources/source/EQ/README.txt`
- Create: `IEMDSP/resources/source/SHA256SUMS`
- Create: `tools/IemAssetCompiler.cpp`
- Create: `tools/Sha256.h`
- Create: `tools/Sha256.cpp`
- Create: `IEMDSP/resources/generated/IemResourceManifest.h`
- Create: `IEMDSP/resources/generated/IemResources.cpp`
- Create: `tests/IemAssetCompilerTest.cpp`
- Modify: `CMakeLists.txt`
- Modify: `IEMDSP/CMakeLists.txt`

**Interfaces:**
- Consumes: pinned source WAV hash/metadata and Phase 0 streaming resampler kernel.
- Produces: host-only `Sha256File(path)`, immutable 96 kHz float arrays, stable EQ enum order 0-22, source/output hashes, channel/frame metadata, and `FindKu100(order)` / `FindHeadphoneEq(id)`.

- [ ] **Step 1: Copy and hash-check pinned binary resources**

Copy Ord1-3, both README files, and all 23 EQ WAVs from the pinned clone. Create `SHA256SUMS` containing the 26 exact hashes printed by `sha256sum` from the pinned checkout, commit that file, and verify the complete set before generation. The three decoder hard gates are:

```bash
printf '%s  %s\n' \
  baf2f8929e739550891cb936750cd6cd434b208d6fed004c3c613734f6c08132 IEMDSP/resources/source/IRs/irsOrd1.wav \
  f7bdb67f9afb718fbc2185350dd0ec6ef2b53a16c33af4861450a82697c26677 IEMDSP/resources/source/IRs/irsOrd2.wav \
  a5d585b9523dfda231b7429ee3e694820ddd1a0bdeba4ee25526fb6e1750b643 IEMDSP/resources/source/IRs/irsOrd3.wav | sha256sum -c -
test "$(ls IEMDSP/resources/source/EQ/*.wav | wc -l)" = 23
cd IEMDSP/resources/source && sha256sum -c SHA256SUMS
```

- [ ] **Step 2: Write failing compiler validation and reproducibility tests**

Test wrong hash, wrong channel count, truncation, source metadata, exact zero-based EQ ordering, deterministic conversion, and generated lookup:

```cpp
CHECK(CompilePinnedAssets(source_dir, output_a).ok());
CHECK(CompilePinnedAssets(source_dir, output_b).ok());
CHECK(ReadBytes(output_a) == ReadBytes(output_b));
CHECK(GeneratedManifest().ku100[2].input_channels == 16);
CHECK(GeneratedManifest().headphone_eq_count == 23);
CHECK(std::string(GeneratedManifest().headphone_eq[0].display_name) == "AKG K1000 Closed");
CHECK(std::string(GeneratedManifest().headphone_eq[22].display_name) == "Shure SRH940");
```

- [ ] **Step 3: Implement host-only compiler and generate committed outputs**

Implement a compact host-only SHA-256 transform in `tools/Sha256.cpp`; test it against the empty-string and `abc` standard vectors before using it for files. Parse the pinned little-endian PCM16 KU100 WAVs and stereo float32 headphone EQ WAVs. Validate source SHA-256 and exact metadata. For KU100, read all `N` source filters, resample to 96 kHz, multiply channel `(n,m)` by `0.3 * sqrt(2n + 1) * (44100.0 / 96000.0)`, and emit left/right matrices with right sign negative only when `m < 0`. For headphone EQ, preserve both channel-specific filters while resampling 48 kHz/2048-frame stereo to 96 kHz/4096-frame stereo without KU100 gain/normalization factors. Emit hexadecimal float literals and source/output SHA-256 plus scientific attribution strings.

Run the compiler twice into separate temporary directories, compare recursively, then replace `IEMDSP/resources/generated/` with one output.

- [ ] **Step 4: Build generated resources without runtime file I/O**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-host --target iem_asset_compiler_test IEMDSP -j2 && ctest --test-dir build-host -R "^iem_asset_compiler_test$" --output-on-failure && if strings build-host/IEMDSP/libIEMDSP.a | grep -q "resources/source/.*\.wav"; then exit 1; fi'
```

Expected: test passes and Android runtime objects contain no source WAV path.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/resources/source IEMDSP/resources/generated tools/IemAssetCompiler.cpp tools/Sha256.h tools/Sha256.cpp tests/IemAssetCompilerTest.cpp CMakeLists.txt IEMDSP/CMakeLists.txt
git commit -m "feat: embed pinned KU100 spatial resources"
```

### Task 8: KU100 Decoder and 23 Headphone EQs

**Files:**
- Create: `IEMDSP/include/iem/Ku100Decoder.h`
- Create: `IEMDSP/src/Ku100Decoder.cpp`
- Create: `IEMDSP/include/iem/HeadphoneEq.h`
- Create: `IEMDSP/src/HeadphoneEq.cpp`
- Create: `tests/IemKu100DecoderTest.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: generated resources and `PartitionedMatrixConvolver`.
- Produces: prepared order-specific KU100 decode, prepared Off/23 headphone EQ selection, exact latency, and resource error codes.

- [ ] **Step 1: Write failing per-channel impulse and model tests**

For each order and every input channel, send one impulse and compare both ears to an independently generated reference that applies `0.3 * sqrt(2n + 1) * (44100 / 96000)` and the Mid/Side sign rule (`m >= 0`: L=R, `m < 0`: L=-R). This catches channel-order, normalization, gain, and resampling mistakes rather than comparing the decoder to its own generated table. For every EQ ID -1..22, impulse-test lookup and output; Off must copy exactly after the same matched delay reported by enabled EQ choices and must execute no EQ FFT.

```cpp
for (uint32_t order : {1U, 2U, 3U}) {
    iem::Ku100Decoder decoder;
    CHECK(decoder.Prepare({order, 256}));
    for (uint32_t channel = 0; channel < (order + 1) * (order + 1); ++channel) {
        RenderChannelImpulse(decoder, channel, actual);
        CompareStereoIr(actual, GeneratedKu100(order, channel), decoder.LatencyFrames(), 2.0e-4F);
    }
}
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-host --target iem_ku100_decoder_test -j2'
```

Expected: compile failure for missing decoder types.

- [ ] **Step 3: Implement prepared resource selection**

Map order to 4/9/16 inputs and reject every other order. Map EQ -1 to bypass and 0-22 to manifest entries. Create all plans and IR spectra in `Prepare`; `Process` only calls prepared convolvers/copy. Keep error enum values stable for telemetry: `NONE`, `INVALID_ORDER`, `INVALID_EQ`, `RESOURCE_METADATA`, `CONVOLVER_PREPARE`, `PROCESS_NONFINITE`.

- [ ] **Step 4: Verify and commit**

The focused test loops over all three profiles, randomized callback partitions, and Reset. Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host --target iem_ku100_decoder_test -j2 && ctest --test-dir build-host -R "^iem_ku100_decoder_test$" --output-on-failure && cmake --build build-ubsan --target iem_ku100_decoder_test -j2 && ctest --test-dir build-ubsan -R "^iem_ku100_decoder_test$" --output-on-failure && cmake --build build-asan --target iem_ku100_decoder_test -j2 && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan -R "^iem_ku100_decoder_test$" --output-on-failure'
```

```bash
git add IEMDSP/include/iem/Ku100Decoder.h IEMDSP/src/Ku100Decoder.cpp IEMDSP/include/iem/HeadphoneEq.h IEMDSP/src/HeadphoneEq.cpp tests/IemKu100DecoderTest.cpp IEMDSP/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add KU100 and headphone EQ decoding"
```

### Task 9: Linked Limiter, Aligned Mix, and Complete IEM Pipeline

**Files:**
- Create: `IEMDSP/include/iem/LinkedLookaheadLimiter.h`
- Create: `IEMDSP/src/LinkedLookaheadLimiter.cpp`
- Create: `IEMDSP/include/iem/IemPipeline.h`
- Create: `IEMDSP/src/IemPipeline.cpp`
- Create: `tests/IemLimiterTest.cpp`
- Create: `tests/IemPipelineTest.cpp`
- Modify: `IEMDSP/include/iem/IemEngine.h`
- Modify: `IEMDSP/src/IemEngine.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: all prepared encoders, rotator, decoder/EQ, and Phase 0 scheduler/resamplers.
- Produces: complete `IemPipeline::Prepare/ApplyParams/SetFreeze/Reset/Process`, measured latency, limiter reduction, and selected-profile constants.

- [ ] **Step 1: Write failing limiter tests**

At 96 kHz assert 96-frame lookahead, shared gain for one-channel overload, exact ceiling tolerance, immediate attack, 50 ms release, no makeup, matched-delay Off mode, Reset, silence, and non-finite rejection.

```cpp
iem::LinkedLookaheadLimiter limiter;
CHECK(limiter.Prepare(96000, 256));
limiter.SetEnabled(true);
limiter.SetCeilingCentidb(-30);
RenderLeftOnlyOverload(limiter, output);
CHECK(Peak(output.left) <= DbToLinear(-0.30F) + 1.0e-6F);
CHECK(GainTrace(output.left) == GainTrace(output.right));
CHECK(limiter.LatencyFrames() == 96);
limiter.SetEnabled(false);
RenderImpulse(limiter, output);
CHECK(ImpulseIndex(output.left) == 96);
```

- [ ] **Step 2: Write failing pipeline tests**

Cover all mode/order/EQ/profile combinations, Wet 0/100, aligned Dry, output gain, stage order, Freeze routing, Reset, non-finite fallback, profile latency <=10/20/40 ms, and bypass identity/no added latency.

- [ ] **Step 3: Implement limiter and pipeline**

The limiter scans the delayed lookahead window for the linked absolute peak, applies `min(1, ceiling / peak)` immediately, and releases with `exp(-1 / (0.050 * 96000))`. `IemPipeline` owns one selected encoder, rotator, decoder, EQ, a dry delay sized to exact wet latency, gain smoother, and limiter. Process in the design order and validate final samples with `std::isfinite` before publishing them.

Use fixed profile tables whose measured total latency remains within the spec caps. Put constants in one `constexpr LatencyProfileConfig kLatencyProfiles[3]` and assert monotonic partition/waterline values in tests.

- [ ] **Step 4: Replace transparent Phase 0 internal processing**

Update `IemEngine::ProcessInternalBlock` to call `IemPipeline` and publish its latency/counters. Preserve Phase 0 host-rate streaming behavior and arbitrary callback support. Disabled engine still returns direct host-rate input without entering internal DSP.

- [ ] **Step 5: Verify and commit**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host --target iem_limiter_test iem_pipeline_test iem_engine_test iem_streaming_resampler_test iem_planar_block_scheduler_test -j2 && ctest --test-dir build-host -R "^iem_(limiter|pipeline|engine|streaming_resampler|planar_block_scheduler)_test$" --output-on-failure && cmake --build build-ubsan --target iem_limiter_test iem_pipeline_test -j2 && ctest --test-dir build-ubsan -R "^iem_(limiter|pipeline)_test$" --output-on-failure && cmake --build build-asan --target iem_limiter_test iem_pipeline_test -j2 && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan -R "^iem_(limiter|pipeline)_test$" --output-on-failure'
```

```bash
git add IEMDSP/include/iem/LinkedLookaheadLimiter.h IEMDSP/src/LinkedLookaheadLimiter.cpp IEMDSP/include/iem/IemPipeline.h IEMDSP/src/IemPipeline.cpp tests/IemLimiterTest.cpp tests/IemPipelineTest.cpp IEMDSP/include/iem/IemEngine.h IEMDSP/src/IemEngine.cpp IEMDSP/CMakeLists.txt CMakeLists.txt
git commit -m "feat: build complete IEM spatial pipeline"
```

### Task 10: Structural Graph Rebuilds, Commands, Telemetry, and Fault Isolation

**Files:**
- Modify: `src/IemResources.h`
- Modify: `src/IemResources.cpp`
- Modify: `src/IemGraph.h`
- Modify: `src/IemGraph.cpp`
- Modify: `src/IemGraphSlots.h`
- Modify: `src/IemGraphSlots.cpp`
- Modify: `src/IemContext.h`
- Modify: `src/IemContext.cpp`
- Modify: `IEMDSP/include/iem/IemTelemetry.h`
- Modify: `IEMDSP/src/IemTelemetry.cpp`
- Modify: `src/TelemetryProtocol.h`
- Modify: `src/ViperContext.h`
- Modify: `src/ViperContext.cpp`
- Modify: `tests/IemGraphSlotsTest.cpp`
- Modify: `tests/IemContextTest.cpp`
- Modify: `tests/IemTelemetryTest.cpp`
- Create: `tests/IemRealtimeAuditTest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: structural comparison, complete pipeline, graph slots, and legacy AudioEffect dispatch.
- Produces: latest-wins pending build, Freeze/Reset commands, equal-latency crossfade, latency-profile fade-through, dry fallback, and telemetry wire fields.

- [ ] **Step 1: Write failing structural/coalescing and command tests**

Tests must prove dynamic Yaw does not rebuild, Mode/Order/EQ/Profile do rebuild while enabled, structural fields only update the snapshot while disabled, enabling prepares once from all accumulated fields, rapid structural updates retain the newest generation, failed pending preparation keeps active audio, Freeze is runtime-only, Reset clears runtime state, and non-finite output falls back to the original post-ViPER dry buffer.

```cpp
const auto generation = context.GraphGeneration();
CHECK(context.DispatchRawParam(iem::kParamRotationYaw, 9000, 0, 0));
CHECK(context.GraphGeneration() == generation);
CHECK(context.DispatchRawParam(iem::kParamIemOrder, 2, 0, 0));
CHECK(context.GraphGeneration() == generation + 1);
const iem::IemParams beforeFreeze = context.Params();
CHECK(context.DispatchRawParam(iem::kCommandGranularFreeze, 1, 0, 0));
CHECK(PersistentParamsEqual(context.Params(), beforeFreeze));
CHECK(context.ActiveGraphForTest().Engine().PipelineForTest().IsFrozen());
```

- [ ] **Step 2: Extend telemetry tests and wire contract**

Add active mode/order, graph generation, latency frames/ms, active/exhausted grains, queue counters, fault code, limiter reduction, and preparation result. Keep consistent generation read semantics and update `WIRE_SIZE` consumers in the App task.

- [ ] **Step 3: Implement control-thread rebuild and transitions**

`DispatchRawParam` updates the persistent snapshot first. If IEM is disabled, a structural field only marks the snapshot dirty. Enable=1 prepares one pending graph from that complete snapshot. If already enabled and structural fields differ, prepare at most one pending graph from the newest snapshot. If a newer structural generation arrives during preparation, discard the stale pending graph and prepare the newest on the next control call. Equal-latency changes use existing `GraphCrossfade`; profile changes use fade-out, slot swap/prefill, fade-in state.

Commands call active and previous graphs where required but never enter the persistent snapshot. On process failure copy `dry_buffer_`, release previous, reset active runtime state, increment fault telemetry, and return `false` without touching ViPER state.

- [ ] **Step 4: Add real-time audit and benchmark assertions**

Instrument allocation and lock hooks around 10,000 randomized callbacks for the maximum configuration. Record callback nanoseconds and assert p99 < 50 percent of callback period in Release on the target arm64 device. Host CI records timing but does not apply the device threshold.

- [ ] **Step 5: Verify and commit**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host --target iem_graph_slots_test iem_context_test iem_telemetry_test iem_realtime_audit_test dsp_graph_test -j2 && ctest --test-dir build-host -R "^(iem_(graph_slots|context|telemetry|realtime_audit)|dsp_graph)_test$" --output-on-failure && cmake --build build-ubsan --target iem_graph_slots_test iem_context_test iem_telemetry_test iem_realtime_audit_test -j2 && ctest --test-dir build-ubsan -R "^iem_(graph_slots|context|telemetry|realtime_audit)_test$" --output-on-failure && cmake --build build-asan --target iem_graph_slots_test iem_context_test iem_telemetry_test iem_realtime_audit_test -j2 && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan -R "^iem_(graph_slots|context|telemetry|realtime_audit)_test$" --output-on-failure'
```

```bash
git add src/IemResources.h src/IemResources.cpp src/IemGraph.h src/IemGraph.cpp src/IemGraphSlots.h src/IemGraphSlots.cpp src/IemContext.h src/IemContext.cpp IEMDSP/include/iem/IemTelemetry.h IEMDSP/src/IemTelemetry.cpp src/TelemetryProtocol.h src/ViperContext.h src/ViperContext.cpp tests/IemGraphSlotsTest.cpp tests/IemContextTest.cpp tests/IemTelemetryTest.cpp tests/IemRealtimeAuditTest.cpp CMakeLists.txt
git commit -m "feat: publish and monitor IEM spatial graphs"
```

### Task 11: App Parameter State, Persistence, HIDL Dispatch, and Telemetry

**Files:**
- Modify: `app/src/main/java/com/llsl/viper4android/viper/ViperParams.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/effect/EffectStates.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/effect/EffectGroups.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/effect/EffectStateStore.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/viper/ViperDispatcher.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/viper/DriverTelemetry.kt`
- Create: `app/src/test/java/com/llsl/viper4android/effect/IemStateContractTest.kt`
- Create: `app/src/test/java/com/llsl/viper4android/viper/IemDispatchTest.kt`
- Modify: `app/src/test/java/com/llsl/viper4android/viper/DriverTelemetryTest.kt`

**Interfaces:**
- Consumes: exact driver IDs/scales and telemetry wire size from Task 10.
- Produces: nested `IemState`, `Effects.iem`, transient `dispatchIemCommand`, complete `dispatchIemState`, and parsed telemetry.

- [ ] **Step 1: Write failing state/default/preset tests**

Assert the spec defaults, all ranges, list length two, JSON round trip, preference round trip, and Freeze exclusion:

```kotlin
val state = EffectState().iem
assertEquals(3, state.general.order)
assertEquals(6000, state.stereo.widthCentidegrees)
assertEquals(listOf(-3000, 3000), state.multi.azimuthCentidegrees)
assertEquals(-30, state.output.limiterCeilingCentidb)

val frozen = EffectState().copy(iem = EffectState().iem.copy(freeze = true))
val restored = deserializeEffectPrefs(serializeEffectPrefs(frozen))
assertFalse(restored.iem.freeze)
```

- [ ] **Step 2: Write failing dispatch tests**

Use a recording `ViperEffect`/dispatch target. Assert a complete enabled state emits every persistent scalar and both indexed Multi values exactly once, Freeze is absent, incremental source edit uses `(param, source, raw)`, and `dispatchIemCommand(0x12102, 1)` does not write preferences.

- [ ] **Step 3: Implement App state and exact parameter registry**

Store canonical integer wire units in these exact state types:

```kotlin
data class IemGeneralState(
    val enable: Boolean = false,
    val encoderMode: Int = 0,
    val order: Int = 3,
)

data class IemStereoState(
    val azimuthCentidegrees: Int = 0,
    val elevationCentidegrees: Int = 0,
    val rollCentidegrees: Int = 0,
    val widthCentidegrees: Int = 6000,
    val sampleWise: Boolean = false,
)

data class IemMultiState(
    val azimuthCentidegrees: List<Int> = listOf(-3000, 3000),
    val elevationCentidegrees: List<Int> = listOf(0, 0),
    val gainDecidb: List<Int> = listOf(0, 0),
    val mute: List<Boolean> = listOf(false, false),
)

data class IemGranularState(
    val azimuthCentidegrees: Int = 0,
    val elevationCentidegrees: Int = 0,
    val shapeTenths: Int = 0,
    val sizeCentidegrees: Int = 18000,
    val rollCentidegrees: Int = 0,
    val widthCentidegrees: Int = 0,
    val deltaTimeUs: Int = 5000,
    val deltaTimeModTenthsPercent: Int = 0,
    val grainLengthUs: Int = 250000,
    val grainLengthModTenthsPercent: Int = 0,
    val readPositionUs: Int = 0,
    val positionModUs: Int = 50000,
    val pitchMilliSemitones: Int = 0,
    val pitchModMilliSemitones: Int = 0,
    val attackTenthsPercent: Int = 500,
    val attackModTenthsPercent: Int = 0,
    val decayTenthsPercent: Int = 500,
    val decayModTenthsPercent: Int = 0,
    val mixTenthsPercent: Int = 500,
    val sourceProbabilityHundredths: Int = 0,
    val spatialMode: Int = 0,
    val sampleWise: Boolean = false,
)

data class IemRotationState(
    val yawCentidegrees: Int = 0,
    val pitchCentidegrees: Int = 0,
    val rollCentidegrees: Int = 0,
    val invertYaw: Boolean = false,
    val invertPitch: Boolean = false,
    val invertRoll: Boolean = false,
    val invertOverall: Boolean = false,
    val sequence: Int = 1,
)

data class IemDecoderState(val headphoneEq: Int = -1)

data class IemOutputState(
    val wetPercent: Int = 100,
    val gainDecidb: Int = 0,
    val latencyProfile: Int = 1,
    val limiterEnabled: Boolean = true,
    val limiterCeilingCentidb: Int = -30,
)

data class IemState(
    val general: IemGeneralState = IemGeneralState(),
    val stereo: IemStereoState = IemStereoState(),
    val multi: IemMultiState = IemMultiState(),
    val granular: IemGranularState = IemGranularState(),
    val rotation: IemRotationState = IemRotationState(),
    val decoder: IemDecoderState = IemDecoderState(),
    val output: IemOutputState = IemOutputState(),
    val freeze: Boolean = false,
)
```

Add one scalar pref per scalar ID and two-element `IntListPref`/`BoolListPref` fields for Multi. Keep `freeze` in state for UI only and exclude it from the group pref list. Add `EffectStateStore.dispatchTransientIemCommand(paramId, value)` that updates only `iem.freeze` for `0x12102` and dispatches a Scalar command without scheduling a repository write.

`ViperDispatcher.dispatchState()` sends IEM Enable=0 first, then every persistent field and both sources, then the target Enable value last. Driver tests from Task 10 prove that this sequence prepares one graph, not one graph per structural field. Do not alter `ConfigChannel` or `ViperParamsSerializer`.

- [ ] **Step 4: Parse extended telemetry and run tests**

Update wire size and offsets exactly once, reject shorter or wrong-version payloads, and retain existing ViPER analyzer fields. Run focused tests and all existing App unit tests.

Run remotely:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && ./gradlew :app:testDebugUnitTest --tests "*IemStateContractTest" --tests "*IemDispatchTest" --tests "*DriverTelemetryTest" --no-daemon && ./gradlew :app:testDebugUnitTest --no-daemon'
```

- [ ] **Step 5: Commit in the App repository**

```bash
git add app/src/main/java/com/llsl/viper4android/viper/ViperParams.kt app/src/main/java/com/llsl/viper4android/effect/EffectStates.kt app/src/main/java/com/llsl/viper4android/effect/EffectGroups.kt app/src/main/java/com/llsl/viper4android/effect/EffectStateStore.kt app/src/main/java/com/llsl/viper4android/viper/ViperDispatcher.kt app/src/main/java/com/llsl/viper4android/viper/DriverTelemetry.kt app/src/test/java/com/llsl/viper4android/effect/IemStateContractTest.kt app/src/test/java/com/llsl/viper4android/viper/IemDispatchTest.kt app/src/test/java/com/llsl/viper4android/viper/DriverTelemetryTest.kt
git commit -m "feat: add persistent IEM control state"
```

### Task 12: HIDL-Only Main IEM Card and Editor Route

**Files:**
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/main/MainScreen.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/main/EffectSections.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/main/EffectSectionSummaries.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/main/MainViewModel.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/editor/EditorModels.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/editor/EffectEditorScreen.kt`
- Create: `app/src/test/java/com/llsl/viper4android/ui/screens/main/IemCardPolicyTest.kt`
- Modify: `app/src/test/java/com/llsl/viper4android/ui/screens/editor/EffectEditorRoutingTest.kt`
- Modify: `app/src/main/res/values/strings.xml`
- Modify: `app/src/main/res/values-zh-rCN/strings.xml`
- Modify: `app/src/main/res/values-ru/strings.xml`

**Interfaces:**
- Consumes: `IemState`, `Effects.iem`, `EditorKind.IEM`, and existing `ViperEffectCard`/MiuiX controls.
- Produces: main card after Convolver, HIDL visibility policy, high-frequency edits, and IEM editor launch.

- [ ] **Step 1: Write failing visibility, order, summary, and route tests**

```kotlin
assertTrue(shouldShowIemCard(aidlModeActive = false))
assertFalse(shouldShowIemCard(aidlModeActive = true))
assertEquals("Stereo · 3rd order · KU100", iemSummary(EffectState().iem))
assertEquals(EditorKind.IEM, editorKindFromRoute("iem"))
assertTrue(effectSectionOrder().indexOf("convolver") < effectSectionOrder().indexOf("iem"))
assertTrue(effectSectionOrder().indexOf("iem") < effectSectionOrder().indexOf("fieldSurround"))
```

- [ ] **Step 2: Run focused tests and verify failure**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && ./gradlew :app:testDebugUnitTest --tests "*IemCardPolicyTest" --tests "*EffectEditorRoutingTest" --no-daemon'
```

Expected: compile failure for missing IEM symbols.

- [ ] **Step 3: Implement the compact main card**

Use `Icons.Default.SurroundSound`, existing `ViperEffectCard`, `LabeledDropdown` for Mode, a compact three-choice segmented control for Order using existing `ViperTabs`, `LabeledSlider` for Wet, and an icon/text command to open `EditorKind.IEM`. Do not add nested cards, Material3, a graph, or explanatory feature copy.

Pass `aidlModeActive` into `EffectList` and omit the IEM item when true. ViewModel methods clamp to the same ranges as prefs and use `EffectStateStore.updatePref`.

- [ ] **Step 4: Run policy tests, unit suite, lint, and compile**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && ./gradlew :app:testDebugUnitTest :app:lintDebug :app:assembleDebug --no-daemon'
```

Expected: PASS; no Material3 import policy regression.

- [ ] **Step 5: Commit in the App repository**

```bash
git add app/src/main/java/com/llsl/viper4android/ui/screens/main/MainScreen.kt app/src/main/java/com/llsl/viper4android/ui/screens/main/EffectSections.kt app/src/main/java/com/llsl/viper4android/ui/screens/main/EffectSectionSummaries.kt app/src/main/java/com/llsl/viper4android/ui/screens/main/MainViewModel.kt app/src/main/java/com/llsl/viper4android/ui/screens/editor/EditorModels.kt app/src/main/java/com/llsl/viper4android/ui/screens/editor/EffectEditorScreen.kt app/src/test/java/com/llsl/viper4android/ui/screens/main/IemCardPolicyTest.kt app/src/test/java/com/llsl/viper4android/ui/screens/editor/EffectEditorRoutingTest.kt app/src/main/res/values/strings.xml app/src/main/res/values-zh-rCN/strings.xml app/src/main/res/values-ru/strings.xml
git commit -m "feat: add IEM spatial audio entry point"
```

### Task 13: Four-Tab MiuiX IEM Editor and Attribution

**Files:**
- Create: `app/src/main/java/com/llsl/viper4android/ui/screens/editor/IemEditorScreen.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/editor/EffectEditorScreen.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/editor/EffectEditorViewModel.kt`
- Modify: `app/src/main/res/values/strings.xml`
- Modify: `app/src/main/res/values-zh-rCN/strings.xml`
- Modify: `app/src/main/res/values-ru/strings.xml`
- Create: `app/src/test/java/com/llsl/viper4android/ui/screens/editor/IemEditorContractTest.kt`
- Modify: `app/src/test/java/com/llsl/viper4android/ui/screens/editor/EffectEditorViewModelDispatchTest.kt`

**Interfaces:**
- Consumes: all IEM prefs, transient command dispatch, telemetry, and existing editor scaffold.
- Produces: Encoder/Rotation/Decoder/Output tabs, explicit resets/Freeze, 23-model dropdown, telemetry status, and clickable attribution.

- [ ] **Step 1: Write failing editor contract and dispatch tests**

Test exact tab order, mode-specific section lists, EQ list count/order, external URL, Reset command, Freeze command/non-persistence, source index dispatch, and telemetry labels.

```kotlin
assertEquals(listOf("encoder", "rotation", "decoder", "output"), iemEditorTabs())
assertEquals(23, headphoneEqOptions().count { it.id >= 0 })
assertEquals("AKG K1000 Closed", headphoneEqOptions().first { it.id == 0 }.label)
assertEquals("Shure SRH940", headphoneEqOptions().first { it.id == 22 }.label)
assertEquals("https://plugins.iem.at", IEM_PROJECT_URL)

viewModel.setIemFreeze(true)
assertEquals(EffectDispatchCommand.Scalar(0x12102, 1, true), target.lastCommand)
viewModel.resetIemRotation()
assertEquals(EffectDispatchCommand.Scalar(0x12101, 1, true), target.lastCommand)
```

- [ ] **Step 2: Run focused tests and verify failure**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && ./gradlew :app:testDebugUnitTest --tests "*IemEditorContractTest" --tests "*EffectEditorViewModelDispatchTest" --no-daemon'
```

Expected: compile failure for the missing editor contract.

- [ ] **Step 3: Implement editor structure and actions**

`EffectEditorScreen` delegates IEM to `IemEditorScreen` instead of putting its controls in the graph workspace. Use `ViperScaffold`/existing top bar, `ViperTabs`, `LabeledSlider`, `LabeledDropdown`, MiuiX switches, and `ViperIconButton` with `RestartAlt` for Reset. Use unframed section headings and dividers; never place cards inside cards.

Encoder tab:

- Stereo: Azimuth, Elevation, Roll, Width, Sample-wise switch.
- Multi: L/R segmented source selector, Azimuth, Elevation, Gain, Mute.
- Granular: Spatial, Timing, Pitch, Window, Mix sections with every spec control and explicit Freeze.

Rotation tab exposes three angles, explicit Reset, axis/overall inversion, and sequence. Decoder exposes read-only KU100 order plus Off/23 EQ dropdown. Output exposes Wet, Gain, profile, limiter/Ceiling, and read-only telemetry.

At scroll end render clickable low-emphasis `Powered by the IEM Plug-in Suite`; launch `Intent.ACTION_VIEW` with `https://plugins.iem.at`. Keep all user-facing copy in resources.

- [ ] **Step 4: Verify layout and interaction on two viewport classes**

Run unit/lint/build. Install the debug APK and inspect portrait phone plus landscape/large-width device: no overlap, clipped labels, nested cards, hidden Reset, or controls shifting size. Verify every slider's precision dialog uses canonical units and every icon has a content description.

- [ ] **Step 5: Commit in the App repository**

```bash
git add app/src/main/java/com/llsl/viper4android/ui/screens/editor/IemEditorScreen.kt app/src/main/java/com/llsl/viper4android/ui/screens/editor/EffectEditorScreen.kt app/src/main/java/com/llsl/viper4android/ui/screens/editor/EffectEditorViewModel.kt app/src/main/res/values/strings.xml app/src/main/res/values-zh-rCN/strings.xml app/src/main/res/values-ru/strings.xml app/src/test/java/com/llsl/viper4android/ui/screens/editor/IemEditorContractTest.kt app/src/test/java/com/llsl/viper4android/ui/screens/editor/EffectEditorViewModelDispatchTest.kt
git commit -m "feat: add complete IEM control editor"
```

### Task 14: Full Build, Device Performance, Attribution, and Release Gate

**Files:**
- Create: `module/IEM_ATTRIBUTION.md`
- Modify: `Makefile`
- Modify: `README.md`
- Create: `docs/iem-upstream-attribution.md`
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/settings/SettingsDialog.kt`
- Create: `app/src/test/java/com/llsl/viper4android/ui/screens/settings/IemAttributionPolicyTest.kt`
- Modify: `app/src/main/res/values/strings.xml`
- Modify: `app/src/main/res/values-zh-rCN/strings.xml`
- Modify: `app/src/main/res/values-ru/strings.xml`

**Interfaces:**
- Consumes: all prior tasks.
- Produces: reproducible release evidence, complete attribution, and a deployable HIDL build.

- [ ] **Step 1: Add complete attribution and operating documentation**

Document the pinned repository/commit, unchanged/adapted source list, GPL-3.0, KU100 Bernschuetz citation, magnitude-least-squares Schoerkhuber/Zaunschirm/Hoeldrich citation, 23 EQ provenance, build-time resampling, parameter ID block, HIDL-only Phase 1 support, and diagnostic telemetry fields. Add `cp $(MODULE_DIR)/IEM_ATTRIBUTION.md $(MODULE_OUT)/` beside the existing LICENSE copy in `Makefile`. Add an explicit IEM Plug-in Suite entry to the App Settings/About license surface and test that it exposes the project URL, GPL-3.0, pinned commit, and both scientific citations; the editor footer alone is not sufficient.

- [ ] **Step 2: Run the complete host and sanitizer gates**

```bash
ssh -p 8022 10645@localhost '
  set -e
  cd "$HOME/ViPERFX_RE"
  cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
  cmake --build build-host -j2
  ctest --test-dir build-host --output-on-failure
  cmake -S . -B build-ubsan -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all" -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined"
  cmake --build build-ubsan -j2
  ctest --test-dir build-ubsan --output-on-failure
  cmake -S . -B build-asan -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
  cmake --build build-asan -j2
  ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
'
```

Expected: every test passes with no sanitizer report.

- [ ] **Step 3: Run both Android ABI Release builds and binary checks**

```bash
ssh -p 8022 10645@localhost '
  set -e
  cd "$HOME/ViPERFX_RE"
  cmake -S . -B build/arm64-v8a -DCMAKE_TOOLCHAIN_FILE="$HOME/android-sdk/ndk/28.0.13004108/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 -DANDROID_ARM_NEON=TRUE -DCMAKE_BUILD_TYPE=Release
  cmake --build build/arm64-v8a -j2
  cmake -S . -B build/armeabi-v7a -DCMAKE_TOOLCHAIN_FILE="$HOME/android-sdk/ndk/28.0.13004108/build/cmake/android.toolchain.cmake" -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-21 -DANDROID_ARM_NEON=TRUE -DCMAKE_BUILD_TYPE=Release
  cmake --build build/armeabi-v7a -j2
  if readelf -d build/arm64-v8a/libv4a_re.so | grep -q JUCE; then exit 1; fi
  if nm -D build/arm64-v8a/libv4a_re.so | grep -q " U pffft_"; then exit 1; fi
'
```

Expected: both libraries build; no JUCE dependency, unresolved PFFFT symbol, or duplicate-symbol link error.

- [ ] **Step 4: Run complete App gates**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && ./gradlew :app:testDebugUnitTest :app:lintDebug :app:assembleDebug --stacktrace --no-daemon'
```

Expected: PASS and debug APK produced.

- [ ] **Step 5: Perform device smoke and performance acceptance**

Install matched driver/App builds on the HIDL target and verify:

1. Music and voice remain clean with IEM disabled.
2. Stereo/Multi/Granular work at orders 1, 2, and 3.
3. Every mode/order/EQ/profile switch is click-free under continuous playback.
4. Freeze captures valid history, Reset clears it, and audioserver restart returns Freeze Off.
5. Wet 0/100, output gain, limiter ceiling, and stereo image behave as specified.
6. Lock screen, output-device switch, App process death, and audioserver restart restore persistent state.
7. Ord3 Granular + KU100 + EQ + limiter p99 callback time is below 50 percent of callback period.
8. Low/Balanced/Stable measured latency is <=10/20/40 ms.
9. Thirty minutes of continuous playback produces no increasing fault, underflow, overflow, or grain-exhaustion counter under default settings.
10. Editor phone/large/landscape layouts contain no overlap or clipped controls and the IEM attribution link opens the official site.

- [ ] **Step 6: Commit documentation only after all gates pass**

Driver repository:

```bash
git add module/IEM_ATTRIBUTION.md Makefile README.md docs/iem-upstream-attribution.md
git commit -m "docs: document IEM spatial integration"
```

App repository, only when strings/license UI changed in this task:

```bash
git add app/src/main/res/values/strings.xml app/src/main/res/values-zh-rCN/strings.xml app/src/main/res/values-ru/strings.xml app/src/main/java/com/llsl/viper4android/ui/screens/settings/SettingsDialog.kt app/src/test/java/com/llsl/viper4android/ui/screens/settings/IemAttributionPolicyTest.kt
git commit -m "docs: add IEM open source attribution"
```

## Spec Coverage Index

- Spec sections 3 and 4.1-4.2: Tasks 2 and 9.
- Stereo and Multi encoders, sections 4.3-4.4: Task 3.
- Full Granular behavior, section 4.5: Task 4.
- Manual Scene Rotator, section 4.6: Task 5.
- KU100 mapping/resources and 23 EQs, sections 4.7-4.8: Tasks 6-8.
- Wet/dry, output gain, limiter, and latency profiles, sections 4.9 and 5: Task 9.
- Exact IDs, state, HIDL dispatch, and Freeze semantics, sections 6-7: Tasks 1, 10, and 11.
- Main card, four-tab editor, attribution footer, and AIDL visibility policy, section 8: Tasks 12-14.
- Telemetry, faults, transitions, and real-time bounds, sections 9-11: Tasks 9-10 and 14.
- Algorithm, convolution, protocol, App, sanitizer, ABI, performance, and device acceptance, sections 12-13: focused gates in Tasks 1-13 and the complete gate in Task 14.

## Execution Notes

- Start execution from the current shared workspaces because Phase 0 files are not fully committed. Before each commit, inspect `git status`, `git diff`, and the staged diff; stage only the active task paths.
- Do not create a clean worktree from `HEAD` until the Phase 0 working tree has been captured, because such a worktree would omit the foundation this plan consumes.
- If a task encounters concurrent edits in one of its listed files, preserve those edits and integrate around them. Stop only when the concurrent change makes the specified interface impossible.
- The App and driver repositories have separate commit histories. Never stage files across both repositories in one commit.
