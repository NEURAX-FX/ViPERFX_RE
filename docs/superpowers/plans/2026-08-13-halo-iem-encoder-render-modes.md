# Halo Encoder and IEM Render Modes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Halo as a fourth mutually exclusive IEM encoder that reconstructs a 7.0 surround bed from stereo, and make KU100 one of three render modes (Off / Simple / KU100) with KU100 remaining the default.

**Architecture:** Keep the existing 96 kHz IEM graph. HaloEncoder always emits a 7.0 planar bed; `IemPipeline` then either folds that bed to stereo (Off) or encodes it to ACN/SN3D and continues through Scene Rotator plus Simple or KU100. Stereo/Multi/Granular keep emitting ACN/SN3D. New IDs occupy unused `0x12008` and `0x12070..0x1207D`. AIDL mmap format 6 stays unchanged.

**Tech Stack:** C++17, C11 PFFFT, CMake 3.16.3, Android NDK 28.0.13004108, Kotlin, Jetpack Compose, MiuiX 0.9.x, JUnit, CTest, UBSan, ASan compile/link.

**Design Spec:** `docs/superpowers/specs/2026-08-13-halo-iem-encoder-render-modes-design.md`

**Algorithm Authority:** `/root/HaloMixRE/docs/reverse-engineering/halo-upmix-dsp.md`

## Global Constraints

- Read `/root/AndroidIDEProjects/ViPER4Android/Anget.md` before every App, build, or dependency task.
- Do not vendor, link, or execute the original Halo binary, VST3, or installer payload. Reconstruct only from the published HaloMixRE notes.
- Vendor only `dialog.net` at `IEMDSP/resources/source/halo/dialog.net` with SHA-256 `652cbd597b9afbd82eb9b39fe80e3e825a381e448c3c2a269c07842f88eb5b72`.
- Keep IEM after ViPER and before final host output; disabled IEM adds no steady-state latency or CPU work.
- Keep the internal bus fixed at 96 kHz, planar float, ACN/SN3D, orders 1-3, maximum 16 channels.
- Do not modify `ConfigChannel.FORMAT_VERSION`, `ViperParamsSerializer`, or the AIDL mmap slot layout.
- Do not edit MiuiX source or remove `androidx.compose.material.icons.*` imports.
- Do not add Material3 UI components.
- The audio callback performs no allocation, lock, file I/O, logging, plan creation, resource conversion, or OS random call.
- Use TDD for every behavior change: fail the focused test, implement the minimum behavior, then run the focused and affected suites.
- Build and test through SSH port 8022. Sync only files named by the active task.
- ASan remains a compile/link gate on the current 39-bit VA hosts. Run UBSan and normal tests; do not treat pre-existing ASan SIGILL as a Halo regression.
- Commit only the files listed in the active task after its verification passes. Never stage files across both repositories in one commit.
- Do not implement LFE, Halo product downmix `0x18006972c`, overhead beds, C LF Split, DE Ramp/Gate/Stability UI, monitor/solo/mute, or user-editable bed angles.

## File Map

### Driver repository: `/root/AndroidIDEProjects/ViPERFX_RE`

- `IEMDSP/include/iem/IemParams.h`, `IEMDSP/src/IemParams.cpp`: `EncoderMode::HALO`, `RenderMode`, `HaloParams`, IDs `0x12008` and `0x12070..0x1207D`.
- `IEMDSP/include/iem/HaloBed.h`: 7.0 slot table, azimuths, and energy-preserving Off fold.
- `IEMDSP/include/iem/FannDialogNet.h`, `IEMDSP/src/FannDialogNet.cpp`: exact FANN_FLO_2.1 loader and stepwise inference.
- `IEMDSP/include/iem/HaloStft.h`, `IEMDSP/src/HaloStft.cpp`: 1024/512 Hann STFT using shared PFFFT.
- `IEMDSP/include/iem/HaloDialogExtractor.h`, `IEMDSP/src/HaloDialogExtractor.cpp`: 37-feature vector and model-driven centre extraction.
- `IEMDSP/include/iem/HaloSurroundAssigner.h`, `IEMDSP/src/HaloSurroundAssigner.cpp`: residual phase-coherence 7.0 assignment.
- `IEMDSP/include/iem/HaloDiffusion.h`, `IEMDSP/src/HaloDiffusion.cpp`: time-domain diffusion, Space delays, rear shelf.
- `IEMDSP/include/iem/HaloEncoder.h`, `IEMDSP/src/HaloEncoder.cpp`: prepared encoder adapter that always emits the 7.0 bed.
- `IEMDSP/include/iem/SimpleDecoder.h`, `IEMDSP/src/SimpleDecoder.cpp`: virtual-speaker fold used by Off (non-Halo) and Simple.
- `IEMDSP/include/iem/IemPipeline.h`, `IEMDSP/src/IemPipeline.cpp`: render-mode branch, Halo bed encode, skipped KU100/EQ work.
- `IEMDSP/include/iem/IemTelemetry.h`, `IEMDSP/src/IemTelemetry.cpp`, `src/TelemetryProtocol.h`: render mode, Halo prepared/active, STFT hop latency, dialog.net result.
- `tools/IemAssetCompiler.cpp`, `IEMDSP/resources/source/halo/dialog.net`, `IEMDSP/resources/generated/`: hash-checked model embed.
- `tests/IemParamsTest.cpp`, `tests/IemFannDialogNetTest.cpp`, `tests/IemHaloStftTest.cpp`, `tests/IemHaloDialogExtractorTest.cpp`, `tests/IemHaloSurroundAssignerTest.cpp`, `tests/IemHaloDiffusionTest.cpp`, `tests/IemHaloEncoderTest.cpp`, `tests/IemSimpleDecoderTest.cpp`, `tests/IemPipelineTest.cpp`, `tests/IemTelemetryTest.cpp`, `tests/IemAssetCompilerTest.cpp`.

### App repository: `/root/AndroidIDEProjects/ViPER4Android`

- `app/src/main/java/com/llsl/viper4android/viper/ViperParams.kt`: new IDs.
- `app/src/main/java/com/llsl/viper4android/effect/EffectStates.kt`, `IemEffect.kt`, `EffectGroups.kt`, `EffectPrefs.kt`, `EffectStateStore.kt`: `renderMode`, `IemHaloState`, clamp, persist.
- `app/src/main/java/com/llsl/viper4android/viper/ViperDispatcher.kt`, `DriverTelemetry.kt`: full-state restore and telemetry parse.
- `app/src/main/java/com/llsl/viper4android/ui/screens/main/EffectSectionSummaries.kt`, `EffectSections.kt`: Halo option and `<Encoder> · <Order> · <Render>` summary.
- `app/src/main/java/com/llsl/viper4android/ui/screens/editor/IemEditorScreen.kt`, `EffectEditorViewModel.kt`: Halo controls and Render Mode.
- `app/src/main/res/values*/strings.xml` and existing IEM policy tests.

---

### Task 1: Lock Render Mode, Halo IDs, and Defaults

**Files:**
- Modify: `IEMDSP/include/iem/IemParams.h`
- Modify: `IEMDSP/src/IemParams.cpp`
- Modify: `tests/IemParamsTest.cpp`

**Interfaces:**
- Consumes: existing `UpdateIemParameterSnapshot(IemParams&, int, int, int, int)` and `HasStructuralDifference`.
- Produces: `enum class RenderMode : uint32_t { OFF = 0, SIMPLE = 1, KU100 = 2 }`, `EncoderMode::HALO = 3`, `struct HaloParams`, IDs below, and structural comparison that includes `render_mode`.

```cpp
constexpr int kParamIemRenderMode = 0x12008;
constexpr int kParamHaloDialogIsolate = 0x12070;
constexpr int kParamHaloDialogAggress = 0x12071;
constexpr int kParamHaloDialogAttack = 0x12072;
constexpr int kParamHaloDialogRelease = 0x12073;
constexpr int kParamHaloDialogMixIn = 0x12074;
constexpr int kParamHaloDivergence = 0x12075;
constexpr int kParamHaloFade = 0x12076;
constexpr int kParamHaloFadeRears = 0x12077;
constexpr int kParamHaloDiffusion = 0x12078;
constexpr int kParamHaloSpace = 0x12079;
constexpr int kParamHaloBackBoost = 0x1207A;
constexpr int kParamHaloRearShelfEnable = 0x1207B;
constexpr int kParamHaloRearShelfFreq = 0x1207C;
constexpr int kParamHaloRearShelfGain = 0x1207D;

struct HaloParams {
    int32_t dialog_isolate_thousandths = 0;
    int32_t dialog_aggress_thousandths = 500;
    int32_t dialog_attack_thousandths = 300;
    int32_t dialog_release_thousandths = 750;
    int32_t dialog_mix_in_thousandths = 0;
    int32_t divergence_thousandths = 500;
    int32_t fade_thousandths = 300;
    int32_t fade_rears_thousandths = 200;
    int32_t diffusion_thousandths = 200;
    int32_t space_thousandths = 800;
    bool back_boost = true;
    bool rear_shelf_enable = true;
    int32_t rear_shelf_freq_thousandths = 816;
    int32_t rear_shelf_gain_thousandths = 475;
};
```

- [ ] **Step 1: Write the failing ID, default, clamp, and structural-difference tests**

```cpp
static_assert(iem::kParamIemRenderMode == 0x12008);
static_assert(iem::kParamHaloRearShelfGain == 0x1207D);

iem::IemParams defaults{};
CHECK(defaults.encoder_mode == iem::EncoderMode::STEREO);
CHECK(defaults.render_mode == iem::RenderMode::KU100);
CHECK(defaults.halo.dialog_aggress_thousandths == 500);
CHECK(defaults.halo.space_thousandths == 800);
CHECK(defaults.halo.back_boost);
CHECK(defaults.halo.rear_shelf_freq_thousandths == 816);

CHECK(iem::UpdateIemParameterSnapshot(defaults, iem::kParamIemEncoderMode, 3, 0, 0)
      == iem::ParamUpdate::UPDATED);
CHECK(defaults.encoder_mode == iem::EncoderMode::HALO);
CHECK(iem::UpdateIemParameterSnapshot(defaults, iem::kParamIemEncoderMode, 4, 0, 0)
      == iem::ParamUpdate::INVALID);
CHECK(iem::UpdateIemParameterSnapshot(defaults, iem::kParamIemRenderMode, 0, 0, 0)
      == iem::ParamUpdate::UPDATED);
CHECK(defaults.render_mode == iem::RenderMode::OFF);
CHECK(iem::UpdateIemParameterSnapshot(defaults, iem::kParamIemRenderMode, 3, 0, 0)
      == iem::ParamUpdate::INVALID);
CHECK(iem::UpdateIemParameterSnapshot(defaults, iem::kParamHaloDialogIsolate, 1500, 0, 0)
      == iem::ParamUpdate::UPDATED);
CHECK(defaults.halo.dialog_isolate_thousandths == 1000);

iem::IemParams left{};
iem::IemParams right = left;
right.halo.fade_thousandths = 400;
CHECK(!iem::HasStructuralDifference(left, right));
right.render_mode = iem::RenderMode::OFF;
CHECK(iem::HasStructuralDifference(left, right));
right = left;
right.encoder_mode = iem::EncoderMode::HALO;
CHECK(iem::HasStructuralDifference(left, right));
```

- [ ] **Step 2: Run the focused test and confirm it fails**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_params_test && ctest --test-dir build-host -R iem_params_test --output-on-failure'
```

Expected: FAIL on missing IDs / `render_mode` / `halo`.

- [ ] **Step 3: Implement the minimum parameter contract**

Add `RenderMode`, `EncoderMode::HALO`, `HaloParams halo{}`, and `IemParams.render_mode = RenderMode::KU100`. Extend `HasStructuralDifference` with `left.render_mode != right.render_mode`. Accept encoder mode `0..3` and render mode `0..2`. Clamp every Halo thousandths field to `0..1000`. Keep existing IDs and defaults unchanged.

- [ ] **Step 4: Re-run the focused test**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/include/iem/IemParams.h IEMDSP/src/IemParams.cpp tests/IemParamsTest.cpp
git commit -m "$(cat <<'EOF'
feat(iem): add Halo encoder and render-mode parameter IDs

EOF
)"
```

---

### Task 2: Vendor and Embed `dialog.net`

**Files:**
- Create: `IEMDSP/resources/source/halo/dialog.net`
- Create: `IEMDSP/resources/source/halo/README.md`
- Modify: `tools/IemAssetCompiler.cpp`
- Modify: `IEMDSP/resources/generated/IemResourceManifest.h`
- Modify: `IEMDSP/resources/generated/IemResources.cpp`
- Modify: `tests/IemAssetCompilerTest.cpp`

**Interfaces:**
- Consumes: existing SHA-256 compiler and `CompilePinnedAssets(source, output)`.
- Produces: `struct DialogNetResource { uint32_t connection_count; const float *weights; const char *source_sha256; }`, `const DialogNetResource &DialogNet();`, and a build failure if the source hash is not `652cbd597b9afbd82eb9b39fe80e3e825a381e448c3c2a269c07842f88eb5b72`.

Copy the model from `/root/HaloMixRE/Halo Upmix v1.7.1.5 extracted/app/dialog.net`. Do not copy any Halo binary or VST3.

- [ ] **Step 1: Write the failing hash and topology tests**

```cpp
CHECK(Sha256File("IEMDSP/resources/source/halo/dialog.net")
      == "652cbd597b9afbd82eb9b39fe80e3e825a381e448c3c2a269c07842f88eb5b72");
const auto &net = iem::DialogNet();
CHECK(net.connection_count == 391);
CHECK(net.weights != nullptr);
```

Also add a compiler test that a wrong hash fails before any generated file is written.

- [ ] **Step 2: Run the focused compiler test and confirm it fails**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_asset_compiler_test && ctest --test-dir build-host -R iem_asset_compiler_test --output-on-failure'
```

Expected: FAIL on missing Halo resource / `DialogNet()`.

- [ ] **Step 3: Implement hash-checked embedding**

Parse `FANN_FLO_2.1`, require `layer_sizes=38 11 2` and 391 float connections, emit `kDialogNetWeights[391]`. Keep KU100 and headphone-EQ generation unchanged. README must state this is a published FANN model used for clean-room reconstruction, not an IEM Plug-in Suite file.

- [ ] **Step 4: Re-run the focused compiler test**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/resources/source/halo IEMDSP/resources/generated/IemResourceManifest.h IEMDSP/resources/generated/IemResources.cpp tools/IemAssetCompiler.cpp tests/IemAssetCompilerTest.cpp
git commit -m "$(cat <<'EOF'
feat(iem): embed hash-checked Halo dialog.net weights

EOF
)"
```

---

### Task 3: Exact FANN Dialog Net Inference

**Files:**
- Create: `IEMDSP/include/iem/FannDialogNet.h`
- Create: `IEMDSP/src/FannDialogNet.cpp`
- Create: `tests/IemFannDialogNetTest.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `DialogNet()`.
- Produces:

```cpp
class FannDialogNet {
public:
    bool Prepare() noexcept;
    float Infer(const float features[37]) const noexcept;
    bool prepared() const noexcept;
};
```

Use the exact stepwise activations from HaloMixRE section `dialog.net FANN model`. Do not use a smooth sigmoid.

- [ ] **Step 1: Write failing activation and golden-output tests**

```cpp
CHECK(iem::SigmoidSymmetricStepwise(-3.0F) == -1.0F);
CHECK(iem::SigmoidSymmetricStepwise(3.0F) == 1.0F);
CHECK(std::abs(iem::SigmoidSymmetricStepwise(0.0F)) < 1e-6F);
CHECK(iem::LinearPieceSymmetric(0.25F) == 0.25F);
CHECK(iem::LinearPieceSymmetric(2.0F) == 1.0F);

float zeros[37]{};
iem::FannDialogNet net;
CHECK(net.Prepare());
const float out = net.Infer(zeros);
CHECK(std::isfinite(out));
CHECK(out >= -1.0F && out <= 1.0F);
```

Add one golden vector: features all `0.5F` must match a precomputed output stored in the test after the first local host run of the exact formula. Until that fixture is captured, the test should compute the same formula independently in the test file from `DialogNet().weights` so the implementation cannot drift.

- [ ] **Step 2: Run the new test and confirm it fails to compile or fail Prepare**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON && cmake --build build-host -j2 --target iem_fann_dialog_net_test && ctest --test-dir build-host -R iem_fann_dialog_net_test --output-on-failure'
```

Expected: FAIL on missing `FannDialogNet`.

- [ ] **Step 3: Implement the exact 10x38 + 11 forward pass**

```text
for h in 0..9:
    z_h = W_hidden[h][37] + sum(x[i] * W_hidden[h][i] for i in 0..36)
    hidden[h] = sigmoid_symmetric_stepwise(0.5 * z_h)
z_o = W_output[10] + sum(hidden[h] * W_output[h] for h in 0..9)
output = clamp(0.5 * z_o, -1, 1)
```

`Prepare()` copies the 391 weights into a prepared array. `Infer()` does no allocation.

- [ ] **Step 4: Re-run the focused test**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/include/iem/FannDialogNet.h IEMDSP/src/FannDialogNet.cpp tests/IemFannDialogNetTest.cpp IEMDSP/CMakeLists.txt CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(iem): add exact FANN dialog.net inference

EOF
)"
```

---

### Task 4: Halo STFT Front End

**Files:**
- Create: `IEMDSP/include/iem/HaloStft.h`
- Create: `IEMDSP/src/HaloStft.cpp`
- Create: `tests/IemHaloStftTest.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: shared `pffft_core`.
- Produces:

```cpp
class HaloStft {
public:
    static constexpr uint32_t kFftSize = 1024;
    static constexpr uint32_t kHop = 512;
    static constexpr uint32_t kBins = 513;
    static constexpr uint32_t kReportedLatency = 1024;
    bool Prepare(std::size_t max_frames) noexcept;
    bool Process(
        const float *left,
        const float *right,
        std::size_t frames,
        void (*on_frame)(const float left_re[kBins], const float left_im[kBins],
                         const float right_re[kBins], const float right_im[kBins],
                         void *user),
        void *user
    ) noexcept;
    bool InverseAdd(
        const float re[kBins],
        const float im[kBins],
        float *dst,
        std::size_t dst_frames
    ) noexcept;
    uint32_t LatencyFrames() const noexcept { return kReportedLatency; }
};
```

Use Hann `w[n] = 0.5 - 0.5*cos(2*pi*n/1024)` after iFFT. Keep a six-frame L/R spectral history with the documented left-rotating permutation.

- [ ] **Step 1: Write failing latency, energy, and hop tests**

```cpp
iem::HaloStft stft;
CHECK(stft.Prepare(256));
CHECK(stft.LatencyFrames() == 1024);
// Impulse at frame 0 appears in the overlap-add output at frame 1024.
// A unit-energy sine at bin 8 survives a round trip within 1e-4 RMS.
```

- [ ] **Step 2: Run the focused test and confirm it fails**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_halo_stft_test && ctest --test-dir build-host -R iem_halo_stft_test --output-on-failure'
```

Expected: FAIL on missing `HaloStft`.

- [ ] **Step 3: Implement the prepared PFFFT STFT**

Allocate FFT plans, Hann table, overlap buffers, and six-frame history in `Prepare()`. `Process()` and `InverseAdd()` only write prepared storage.

- [ ] **Step 4: Re-run the focused test**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/include/iem/HaloStft.h IEMDSP/src/HaloStft.cpp tests/IemHaloStftTest.cpp IEMDSP/CMakeLists.txt CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(iem): add Halo 1024/512 Hann STFT

EOF
)"
```

---

### Task 5: Dialog Feature Vector and Centre Extraction

**Files:**
- Create: `IEMDSP/include/iem/HaloDialogExtractor.h`
- Create: `IEMDSP/src/HaloDialogExtractor.cpp`
- Create: `tests/IemHaloDialogExtractorTest.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `FannDialogNet`, HaloMixRE feature equations.
- Produces:

```cpp
struct HaloDialogFrame {
    float centre_re[HaloStft::kBins];
    float centre_im[HaloStft::kBins];
    float residual_l_re[HaloStft::kBins];
    float residual_l_im[HaloStft::kBins];
    float residual_r_re[HaloStft::kBins];
    float residual_r_im[HaloStft::kBins];
};

class HaloDialogExtractor {
public:
    bool Prepare() noexcept;
    void ApplyParams(const HaloParams &params) noexcept;
    void Reset() noexcept;
    void ProcessFrame(
        const float left_re[HaloStft::kBins],
        const float left_im[HaloStft::kBins],
        const float right_re[HaloStft::kBins],
        const float right_im[HaloStft::kBins],
        HaloDialogFrame &out
    ) noexcept;
};
```

Use:

```text
DialogExtractionAggression' = min(2 * DialogExtractionAggression, 1)
DialogIsolate'              = DialogIsolate * DialogExtractionAggression'
```

`DE_RampS`, `DE_Stability`, and the three `DD Gate` parameters stay at documented numeric defaults and are not persisted.

- [ ] **Step 1: Write failing feature and extraction tests**

```cpp
float features[37];
iem::BuildHaloDialogFeatures(/* documented bin fixture */, features);
CHECK(features[36] >= 0.0F && features[36] <= 1.0F);
// Bin 0 log-frequency feature is 0 after ln(0) clamp.
CHECK(features_for_bin0[36] == 0.0F);

iem::HaloDialogExtractor extractor;
CHECK(extractor.Prepare());
extractor.ApplyParams(iem::IemParams{}.halo);
// Identical L/R dialog-like bin sends energy to centre and reduces residual L/R.
```

- [ ] **Step 2: Run the focused test and confirm it fails**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_halo_dialog_extractor_test && ctest --test-dir build-host -R iem_halo_dialog_extractor_test --output-on-failure'
```

Expected: FAIL on missing extractor.

- [ ] **Step 3: Implement the 37-feature builder and model-driven centre path**

Follow HaloMixRE `0x180090408` and `0x180090a24` exactly. Keep all histories in prepared storage.

- [ ] **Step 4: Re-run the focused test**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/include/iem/HaloDialogExtractor.h IEMDSP/src/HaloDialogExtractor.cpp tests/IemHaloDialogExtractorTest.cpp IEMDSP/CMakeLists.txt CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(iem): add Halo dialog feature vector and centre extraction

EOF
)"
```

---

### Task 6: Residual Surround Assignment

**Files:**
- Create: `IEMDSP/include/iem/HaloSurroundAssigner.h`
- Create: `IEMDSP/src/HaloSurroundAssigner.cpp`
- Create: `tests/IemHaloSurroundAssignerTest.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: residual L/R spectra and `HaloParams`.
- Produces:

```cpp
enum class HaloBedChannel : uint32_t { L=0, R=1, C=2, Ls=3, Rs=4, Lsr=5, Rsr=6, kCount=7 };

class HaloSurroundAssigner {
public:
    bool Prepare() noexcept;
    void ApplyParams(const HaloParams &params) noexcept;
    void Reset() noexcept;
    void ProcessFrame(const HaloDialogFrame &in, float bed_re[7][HaloStft::kBins],
                      float bed_im[7][HaloStft::kBins]) noexcept;
};
```

Use the documented equations:

```text
c_raw = clamp(2*(1-Divergence)*(p-Divergence), 0, 1)
fade_fs = 2*Fade - 1
fade_sb = 2*FadeRears - 1
front_gain = f
rear_gain  = (1-f) * (BackBoost ? 1 : b)
side_gain  = (1-f) - rear_gain
```

Keep `DE_RampS` at its documented default so `ramp_fs = 0.5/DE_RampS`, with the `1e8` endpoint sentinel.

- [ ] **Step 1: Write failing assignment tests**

```cpp
// Divergence=1, Fade=1, BackBoost=1: residual energy stays in L/R, rear gains are 0.
// Divergence=0, Fade=0, BackBoost=0: residual energy moves to Lsr/Rsr.
// BackBoost=1 vs 0 changes only rear_gain for the same p.
```

- [ ] **Step 2: Run the focused test and confirm it fails**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_halo_surround_assigner_test && ctest --test-dir build-host -R iem_halo_surround_assigner_test --output-on-failure'
```

Expected: FAIL on missing assigner.

- [ ] **Step 3: Implement the per-bin residual split**

Do not call LFE, C LF Split, or the product downmix.

- [ ] **Step 4: Re-run the focused test**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/include/iem/HaloSurroundAssigner.h IEMDSP/src/HaloSurroundAssigner.cpp tests/IemHaloSurroundAssignerTest.cpp IEMDSP/CMakeLists.txt CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(iem): add Halo residual surround assignment

EOF
)"
```

---

### Task 7: Time-Domain Diffusion, Space, and Rear Shelf

**Files:**
- Create: `IEMDSP/include/iem/HaloDiffusion.h`
- Create: `IEMDSP/src/HaloDiffusion.cpp`
- Create: `tests/IemHaloDiffusionTest.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: 7.0 time-domain bed and `HaloParams`.
- Produces:

```cpp
class HaloDiffusion {
public:
    bool Prepare(uint32_t sample_rate) noexcept;
    void ApplyParams(const HaloParams &params) noexcept;
    void Reset() noexcept;
    void Process(float *const bed[7], std::size_t frames) noexcept;
};
```

Use:

```text
diff = 0                             if Diffusion == 0
     = 1                             if Diffusion == 1
     = db_to_lin(30 * Diffusion - 30) otherwise
space_delay_a = trunc(2500 * Space)
space_delay_b = trunc( 625 * Space)
```

Prepare circular delay lines for the 96 kHz graph. Rear Shelf Enable forces shelf gain to 0 dB when Off. If the reconstruction notes do not independently pin rear-shelf frequency endpoints, extract and lock them in this test before claiming equation match.

- [ ] **Step 1: Write failing mapping and delay tests**

```cpp
CHECK(iem::HaloDiffusionGain(0.0F) == 0.0F);
CHECK(iem::HaloDiffusionGain(1.0F) == 1.0F);
CHECK(std::abs(iem::HaloDiffusionGain(0.5F) - iem::DbToLin(-15.0F)) < 1e-6F);
CHECK(iem::HaloSpaceDelayA(0.8F) == 2000);
CHECK(iem::HaloSpaceDelayB(0.8F) == 500);
// Rear shelf enable=false yields identical output to 0 dB shelf.
```

- [ ] **Step 2: Run the focused test and confirm it fails**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_halo_diffusion_test && ctest --test-dir build-host -R iem_halo_diffusion_test --output-on-failure'
```

Expected: FAIL on missing diffusion helpers.

- [ ] **Step 3: Implement the documented 7.0 helper and four-branch decorrelation/rear-shelf helper**

Do not call `0x180067544`, `0x180069318`, `0x180068548`, or `0x18006972c`.

- [ ] **Step 4: Re-run the focused test**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/include/iem/HaloDiffusion.h IEMDSP/src/HaloDiffusion.cpp tests/IemHaloDiffusionTest.cpp IEMDSP/CMakeLists.txt CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(iem): add Halo diffusion, space delay, and rear shelf

EOF
)"
```

---

### Task 8: HaloEncoder Bed Output and Ambisonics Encode Helper

**Files:**
- Create: `IEMDSP/include/iem/HaloBed.h`
- Create: `IEMDSP/include/iem/HaloEncoder.h`
- Create: `IEMDSP/src/HaloEncoder.cpp`
- Create: `tests/IemHaloEncoderTest.cpp`
- Modify: `IEMDSP/include/iem/IemEncoder.h`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 3-7.
- Produces:

```cpp
constexpr float kHaloBedAzimuthDegrees[7] = {-30.F, 30.F, 0.F, -90.F, 90.F, -135.F, 135.F};

void EncodeHaloBedToSn3d(
    uint32_t order,
    const float *const bed[7],
    float *const ambisonics[kMaxAmbisonicsChannels],
    std::size_t frames
) noexcept;

void FoldHaloBedToStereo(
    const float *const bed[7],
    float *const stereo[2],
    std::size_t frames
) noexcept;

class HaloEncoder {
public:
    bool Prepare(const EncoderConfig &config) noexcept;
    void ApplyParams(const IemParams &params) noexcept;
    void Reset() noexcept;
    bool ProcessBed(
        const float *const stereo[2],
        float *const bed[7],
        std::size_t frames
    ) noexcept;
    uint32_t StftLatencyFrames() const noexcept;
    bool Prepared() const noexcept;
};
```

`HaloEncoder` does not inherit `IemEncoder` if that interface still requires Ambisonics output. Keep `IemEncoder` for Stereo/Multi/Granular. Pipeline owns the bed-to-SN3D step.

Off fold coefficients: `C = 1/sqrt(2)`, `Ls/Rs = 1/sqrt(2)`, `Lsr/Rsr = 1/2`, `L/R = 1`.

- [ ] **Step 1: Write failing bed, fold, and encode tests**

```cpp
// Centre-only bed at order 1 has energy in W and none in Y/Z beyond tolerance.
// L-only bed encodes near azimuth -30 degrees.
// Fold of centre-only bed yields equal L/R = C/sqrt(2).
// ProcessBed of silence is finite zeros after latency flush.
// Prepared Halo storage estimate is printed and is separate from Granular's 6.1 MiB history.
CHECK(encoder.PreparedBytes() > 0);
```

- [ ] **Step 2: Run the focused test and confirm it fails**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_halo_encoder_test && ctest --test-dir build-host -R iem_halo_encoder_test --output-on-failure'
```

Expected: FAIL on missing `HaloEncoder`.

- [ ] **Step 3: Implement HaloEncoder and the two helpers**

`ProcessBed()` runs STFT -> dialog -> surround -> iFFT -> diffusion. All working buffers come from `Prepare()`.

- [ ] **Step 4: Re-run the focused test**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/include/iem/HaloBed.h IEMDSP/include/iem/HaloEncoder.h IEMDSP/src/HaloEncoder.cpp IEMDSP/include/iem/IemEncoder.h tests/IemHaloEncoderTest.cpp IEMDSP/CMakeLists.txt CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(iem): add HaloEncoder 7.0 bed and SN3D encode helper

EOF
)"
```

---

### Task 9: Simple Virtual-Speaker Decoder

**Files:**
- Create: `IEMDSP/include/iem/SimpleDecoder.h`
- Create: `IEMDSP/src/SimpleDecoder.cpp`
- Create: `tests/IemSimpleDecoderTest.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `EvaluateSn3d` and `kHaloBedAzimuthDegrees`.
- Produces:

```cpp
class SimpleDecoder {
public:
    bool Prepare(uint32_t order) noexcept;
    bool Process(
        const float *const ambisonics[kMaxAmbisonicsChannels],
        float *const stereo[2],
        std::size_t frames
    ) noexcept;
};
```

Each of the seven speakers uses the SN3D encoding of its azimuth at elevation 0. Reconstruct speaker feeds by mode matching, then fold with `FoldHaloBedToStereo`.

- [ ] **Step 1: Write failing decode tests**

```cpp
// Encoding a unit source at +30 degrees then Simple-decoding yields more R than L.
// Energy of a unit W-only input is finite and non-zero.
// Prepare(0) fails.
```

- [ ] **Step 2: Run the focused test and confirm it fails**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_simple_decoder_test && ctest --test-dir build-host -R iem_simple_decoder_test --output-on-failure'
```

Expected: FAIL on missing `SimpleDecoder`.

- [ ] **Step 3: Implement the fixed virtual-speaker matrix**

No convolution, no headphone EQ.

- [ ] **Step 4: Re-run the focused test**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/include/iem/SimpleDecoder.h IEMDSP/src/SimpleDecoder.cpp tests/IemSimpleDecoderTest.cpp IEMDSP/CMakeLists.txt CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(iem): add Simple virtual-speaker decoder

EOF
)"
```

---

### Task 10: Pipeline Render-Mode Branch

**Files:**
- Modify: `IEMDSP/include/iem/IemPipeline.h`
- Modify: `IEMDSP/src/IemPipeline.cpp`
- Modify: `tests/IemPipelineTest.cpp`
- Modify: `tests/IemRealtimeAuditTest.cpp`

**Interfaces:**
- Consumes: `HaloEncoder`, `EncodeHaloBedToSn3d`, `FoldHaloBedToStereo`, `SimpleDecoder`.
- Produces: pipeline behavior:

```text
if encoder == HALO:
    HaloEncoder.ProcessBed -> bed
    if render == OFF: FoldHaloBedToStereo(bed)
    else: EncodeHaloBedToSn3d(bed) -> rotator -> (Simple or KU100+EQ)
else:
    existing IemEncoder.Process -> ACN/SN3D
    if render == OFF or SIMPLE: rotator only if SIMPLE; then SimpleDecoder
    if render == KU100: existing rotator -> KU100 -> EQ
```

`HasStructuralDifference` already includes `render_mode`. `Prepare()` must skip `Ku100Decoder` and `HeadphoneEq` when render mode is not KU100. Those objects stay unprepared and add no latency or CPU. Halo STFT latency is included in `wet_latency_frames_`.

Encoder variant becomes:

```cpp
std::variant<std::monostate, StereoEncoder, MultiEncoder, GranularEncoder, HaloEncoder>
```

- [ ] **Step 1: Write failing pipeline tests**

```cpp
// Halo + Off and Halo + KU100 produce distinct stereo for the same input.
// Halo + Simple with yaw=90 differs from yaw=0.
// Stereo + Off does not require KU100 resources and has no KU100 latency.
// Disabled IEM Process path is unchanged: no STFT work when enable=false at engine/graph level.
// Switching KU100 -> Off reduces wet latency by the KU100+EQ amount.
```

- [ ] **Step 2: Run the focused pipeline tests and confirm the new cases fail**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_pipeline_test && ctest --test-dir build-host -R iem_pipeline_test --output-on-failure'
```

Expected: FAIL on missing render-mode / Halo branch.

- [ ] **Step 3: Implement the branch and latency accounting**

Keep existing equal-latency crossfade / fade-out-switch-fade-in rules in `IemGraph` consumers. `IemPipeline::Prepare` itself just builds one graph for the requested mode.

- [ ] **Step 4: Re-run pipeline and realtime-audit tests**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_pipeline_test iem_realtime_audit_test && ctest --test-dir build-host -R "iem_pipeline_test|iem_realtime_audit_test" --output-on-failure'
```

Expected: PASS. Realtime audit still forbids allocation/locks/I/O in `Process`.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/include/iem/IemPipeline.h IEMDSP/src/IemPipeline.cpp tests/IemPipelineTest.cpp tests/IemRealtimeAuditTest.cpp
git commit -m "$(cat <<'EOF'
feat(iem): branch pipeline on Halo bed and render mode

EOF
)"
```

---

### Task 11: Telemetry and Graph Publication

**Files:**
- Modify: `IEMDSP/include/iem/IemTelemetry.h`
- Modify: `IEMDSP/src/IemTelemetry.cpp`
- Modify: `src/TelemetryProtocol.h`
- Modify: `tests/IemTelemetryTest.cpp`
- Modify: `tests/IemGraphSlotsTest.cpp`
- Modify: `tests/IemContextTest.cpp`

**Interfaces:**
- Consumes: pipeline render/encoder state.
- Produces: added snapshot fields, still packed into an extended but versioned IEM telemetry wire. Keep `IemTelemetryWire.version` at 2 only if the extra fields fit the existing reserved word and trailing space without breaking `WIRE_SIZE`. If they do not fit, bump to version 3 and update App parse in Task 12 in the same logical change set, but commit driver first with the new version documented.

Preferred layout: reuse `reserved` as `render_mode` and add no size change if possible. If four new fields are required, bump version to 3, set `kIemTelemetryWireSize` accordingly, and document:

```cpp
uint32_t render_mode;
uint32_t halo_prepared;
uint32_t halo_stft_latency_frames;
uint32_t dialog_net_result;
```

- [ ] **Step 1: Write failing telemetry tests**

```cpp
// After a Halo+Off prepare/process, snapshot.encoder_mode == 3
// snapshot.render_mode == 0
// snapshot.halo_stft_latency_frames == 1024
// snapshot.dialog_net_result reports SUCCESS
```

- [ ] **Step 2: Run focused telemetry tests and confirm they fail**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_telemetry_test && ctest --test-dir build-host -R iem_telemetry_test --output-on-failure'
```

Expected: FAIL on missing fields.

- [ ] **Step 3: Implement publication**

`RecordSpatialState` gains render-mode and Halo fields. Graph prepare failures for `dialog.net` remain fatal for a new Halo graph and never silently skip dialog extraction.

- [ ] **Step 4: Re-run telemetry, graph-slot, and context tests**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add IEMDSP/include/iem/IemTelemetry.h IEMDSP/src/IemTelemetry.cpp src/TelemetryProtocol.h tests/IemTelemetryTest.cpp tests/IemGraphSlotsTest.cpp tests/IemContextTest.cpp
git commit -m "$(cat <<'EOF'
feat(iem): publish Halo render-mode telemetry

EOF
)"
```

---

### Task 12: App State, Persistence, and Dispatch

**Files:**
- Modify: `app/src/main/java/com/llsl/viper4android/viper/ViperParams.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/effect/EffectStates.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/effect/IemEffect.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/effect/EffectGroups.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/effect/EffectPrefs.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/viper/ViperDispatcher.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/viper/DriverTelemetry.kt`
- Modify: `app/src/test/java/com/llsl/viper4android/effect/IemStateContractTest.kt`
- Modify: `app/src/test/java/com/llsl/viper4android/viper/IemDispatchTest.kt`
- Modify: `app/src/test/java/com/llsl/viper4android/viper/DriverTelemetryTest.kt`

**Interfaces:**
- Consumes: Task 1 IDs and Task 11 wire.
- Produces:

```kotlin
data class IemGeneralState(
    val enable: Boolean = false,
    val encoderMode: Int = 0,
    val order: Int = 3,
    val renderMode: Int = 2,
)

data class IemHaloState(
    val dialogIsolateThousandths: Int = 0,
    val dialogAggressThousandths: Int = 500,
    val dialogAttackThousandths: Int = 300,
    val dialogReleaseThousandths: Int = 750,
    val dialogMixInThousandths: Int = 0,
    val divergenceThousandths: Int = 500,
    val fadeThousandths: Int = 300,
    val fadeRearsThousandths: Int = 200,
    val diffusionThousandths: Int = 200,
    val spaceThousandths: Int = 800,
    val backBoost: Boolean = true,
    val rearShelfEnable: Boolean = true,
    val rearShelfFreqThousandths: Int = 816,
    val rearShelfGainThousandths: Int = 475,
)
```

`encoderMode` range becomes `0..3`. `normalizeIemState()` clamps `renderMode` to `0..2` and every Halo thousandths field to `0..1000`. Pref count becomes `48 + 1 renderMode + 14 Halo = 63`.

Full-state HIDL restore keeps: Enable=0, every persistent IEM field including Render Mode and Halo, then Enable with the target value.

- [ ] **Step 1: Write failing App contract and dispatch tests**

```kotlin
assertEquals(2, EffectState().iem.general.renderMode)
assertEquals(500, EffectState().iem.halo.dialogAggressThousandths)
assertEquals(3, Effects.iem.encoderMode.range.last)
assertEquals(63, EFFECT_GROUPS.first { it.effectKey == "iem" }.prefs.size)
// preset JSON round-trip persists renderMode=1 and halo.spaceThousandths=123
// full restore writes 0x12008 and 0x12070..0x1207D between Enable=0 and Enable=1
// telemetry parse accepts encoderMode=3 and renderMode=0
```

- [ ] **Step 2: Sync only these App files and run the focused tests**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && bash gradlew :app:testDebugUnitTest --tests "*IemStateContractTest" --tests "*IemDispatchTest" --tests "*DriverTelemetryTest" --no-daemon'
```

Expected: FAIL on missing fields / IDs.

- [ ] **Step 3: Implement state, prefs, dispatcher, and telemetry parse**

Do not change AIDL format 6. Keep Freeze excluded from persistence.

- [ ] **Step 4: Re-run the focused App tests**

Expected: PASS.

- [ ] **Step 5: Commit in the App repository**

```bash
git add app/src/main/java/com/llsl/viper4android/viper/ViperParams.kt app/src/main/java/com/llsl/viper4android/effect/EffectStates.kt app/src/main/java/com/llsl/viper4android/effect/IemEffect.kt app/src/main/java/com/llsl/viper4android/effect/EffectGroups.kt app/src/main/java/com/llsl/viper4android/effect/EffectPrefs.kt app/src/main/java/com/llsl/viper4android/viper/ViperDispatcher.kt app/src/main/java/com/llsl/viper4android/viper/DriverTelemetry.kt app/src/test/java/com/llsl/viper4android/effect/IemStateContractTest.kt app/src/test/java/com/llsl/viper4android/viper/IemDispatchTest.kt app/src/test/java/com/llsl/viper4android/viper/DriverTelemetryTest.kt
git commit -m "$(cat <<'EOF'
feat(iem): persist Halo encoder and render-mode state

EOF
)"
```

---

### Task 13: App Card, Editor, and Localization

**Files:**
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/main/EffectSectionSummaries.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/main/EffectSections.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/editor/IemEditorScreen.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/editor/EffectEditorViewModel.kt`
- Modify: `app/src/main/res/values/strings.xml`
- Modify: `app/src/main/res/values-zh-rCN/strings.xml`
- Modify: `app/src/main/res/values-ru/strings.xml`
- Modify: `app/src/test/java/com/llsl/viper4android/ui/screens/main/IemCardPolicyTest.kt`
- Modify: `app/src/test/java/com/llsl/viper4android/ui/screens/editor/IemEditorContractTest.kt`
- Modify: `app/src/test/java/com/llsl/viper4android/ui/screens/editor/IemEditorLocalizationPolicyTest.kt`

**Interfaces:**
- Consumes: Task 12 state.
- Produces: Encoder Mode options Stereo / Multi / Granular / Halo; summary `"<Encoder> · <Order> · <Render>"`; Decoder-tab Render Mode Off / Simple / KU100; Halo encoder controls listed in the spec; Headphone EQ enabled only when `renderMode == 2`.

- [ ] **Step 1: Write failing policy tests**

```kotlin
assertEquals("Halo · 3rd order · Off", iemSummary(stateWithHaloAndOff))
assertEquals("Stereo · 3rd order · KU100", iemSummary(EffectState().iem))
assertTrue(iemEditorTabs() == listOf("encoder", "rotation", "decoder", "output"))
assertFalse(shouldEnableHeadphoneEq(renderMode = 0))
assertFalse(shouldEnableHeadphoneEq(renderMode = 1))
assertTrue(shouldEnableHeadphoneEq(renderMode = 2))
// localization policy still forbids hardcoded English Halo/render labels
```

- [ ] **Step 2: Run the focused UI tests and confirm they fail**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && bash gradlew :app:testDebugUnitTest --tests "*IemCardPolicyTest" --tests "*IemEditorContractTest" --tests "*IemEditorLocalizationPolicyTest" --no-daemon'
```

Expected: FAIL on old summary / missing Halo strings.

- [ ] **Step 3: Implement card, editor, and translations**

Keep the compact card limited to Encoder Mode, Order, Wet, and Open Editor. Put Render Mode only on the Decoder tab. Do not add a second "Powered by" footer. Product names `Halo` and `KU100` stay as published in all locales.

- [ ] **Step 4: Re-run the focused UI tests plus `:app:lintDebug`**

Expected: PASS.

- [ ] **Step 5: Commit in the App repository**

```bash
git add app/src/main/java/com/llsl/viper4android/ui/screens/main/EffectSectionSummaries.kt app/src/main/java/com/llsl/viper4android/ui/screens/main/EffectSections.kt app/src/main/java/com/llsl/viper4android/ui/screens/editor/IemEditorScreen.kt app/src/main/java/com/llsl/viper4android/ui/screens/editor/EffectEditorViewModel.kt app/src/main/res/values/strings.xml app/src/main/res/values-zh-rCN/strings.xml app/src/main/res/values-ru/strings.xml app/src/test/java/com/llsl/viper4android/ui/screens/main/IemCardPolicyTest.kt app/src/test/java/com/llsl/viper4android/ui/screens/editor/IemEditorContractTest.kt app/src/test/java/com/llsl/viper4android/ui/screens/editor/IemEditorLocalizationPolicyTest.kt
git commit -m "$(cat <<'EOF'
feat(iem): expose Halo encoder and render modes in the editor

EOF
)"
```

---

### Task 14: Documentation, Host/Android Gates, and Device Smoke

**Files:**
- Modify: `docs/iem-upstream-attribution.md`
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-08-13-halo-iem-encoder-render-modes-design.md`

**Interfaces:**
- Consumes: all prior tasks.
- Produces: operating notes for Halo and render modes, plus release evidence.

- [ ] **Step 1: Document Halo as a local reconstruction**

State that Halo is not an IEM Plug-in Suite component, name `dialog.net` hash and path, list IDs `0x12008` and `0x12070..0x1207D`, describe Off/Simple/KU100, and restate that the original Halo binary is not shipped. Change the spec status from draft to approved-for-implementation after this task's gates pass.

- [ ] **Step 2: Run host and UBSan gates; compile ASan**

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
'
```

Expected: host and UBSan tests pass. ASan links. If ASan executables die with SIGILL before `main`, document it as the known 39-bit VA limitation.

- [ ] **Step 3: Run both Android ABI Release builds**

```bash
ssh -p 8022 10645@localhost '
  set -e
  cd "$HOME/ViPERFX_RE"
  cmake -S . -B build/arm64-v8a -DCMAKE_TOOLCHAIN_FILE="$HOME/android-sdk/ndk/28.0.13004108/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 -DANDROID_ARM_NEON=TRUE -DCMAKE_BUILD_TYPE=Release
  cmake --build build/arm64-v8a -j2
  cmake -S . -B build/armeabi-v7a -DCMAKE_TOOLCHAIN_FILE="$HOME/android-sdk/ndk/28.0.13004108/build/cmake/android.toolchain.cmake" -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-21 -DANDROID_ARM_NEON=TRUE -DCMAKE_BUILD_TYPE=Release
  cmake --build build/armeabi-v7a -j2
  if llvm-readelf -d build/arm64-v8a/libv4a_re.so | grep -q JUCE; then exit 1; fi
  if llvm-nm -D build/arm64-v8a/libv4a_re.so | grep -q " U pffft_"; then exit 1; fi
'
```

Expected: both libraries build; no JUCE dependency or unresolved PFFFT symbol.

- [ ] **Step 4: Run complete App gates**

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && bash gradlew :app:testDebugUnitTest :app:lintDebug :app:assembleDebug --stacktrace --no-daemon'
```

Expected: PASS and debug APK produced.

- [ ] **Step 5: Perform device-accessible smoke and list remaining listening items**

If root/bind-mount is available:

1. Bind-mount the new `arm64-v8a` `libv4a_re.so` and restart `audioserver`.
2. Enable Halo on HIDL, change dialog/fade/diffusion, switch Off/Simple/KU100, then switch back to Stereo.
3. Confirm no audioserver crash and telemetry remains finite.

Document, do not fake, any physical listening item this environment cannot perform: long-form surround image, voice-centre lock, p99 callback on a music stream, and 30-minute soak.

- [ ] **Step 6: Commit documentation only after the automated gates pass**

```bash
git add docs/iem-upstream-attribution.md README.md docs/superpowers/specs/2026-08-13-halo-iem-encoder-render-modes-design.md
git commit -m "$(cat <<'EOF'
docs(iem): document Halo encoder and render modes

EOF
)"
```

---

## Spec Coverage

- Purpose and confirmed decisions, sections 1-2: Tasks 1, 8, 10, 13.
- Source, `dialog.net` hash, no Halo binary, IEM attribution unchanged, section 3: Tasks 2 and 14.
- Encoder variant and HaloEncoder contract, sections 4.1-4.2: Tasks 8 and 10.
- STFT, dialog.net, residual surround, diffusion/shelf, sections 4.3-4.4: Tasks 3-7.
- Render modes Off/Simple/KU100 and skipped convolution, section 4.5: Tasks 8-10.
- Existing stages and disabled-IEM CPU rule, section 4.6: Task 10.
- Parameter protocol, section 5: Task 1 and Task 12.
- State, persistence, HIDL restore, AIDL hide, section 6: Task 12.
- App card/editor/localization, section 7: Task 13.
- Telemetry, section 8: Task 11 and Task 12.
- Faults and transitions, section 9: Tasks 10-11.
- Real-time bounds, section 10: Tasks 10 and 14.
- Verification and completion criteria, sections 11-12: Task 14 plus focused gates in Tasks 1-13.
- Non-goals, section 13: Global Constraints and the explicit omissions in Tasks 6-7.

## Execution Notes

- Start from the current shared workspaces. Phase 1 IEM files may still be dirty; never revert unrelated work.
- If a listed file already contains concurrent edits, preserve them and integrate around them.
- The App and driver repositories have separate commit histories.
- Rear-shelf frequency endpoints are the only reconstruction detail that may need a fixture captured during Task 7. Capture that fixture in the test; do not leave a placeholder in production code.
