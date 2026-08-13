# Matrix Convolver Replacement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the current ViPER DSP convolver with the EEL-compatible native 2x2 matrix engine and expose Wet, Output Gain, and Routing controls in the Android app.

**Architecture:** `MatrixConvolver` owns shared stereo FFT histories and four immutable kernel banks. `Convolver` remains the effect-facing upload/mix wrapper, while `DspResources` and `DspGraphSlots` continue to publish fully built resources off the audio thread. The app persists and serializes the three new scalar parameters and accepts mono, stereo, and canonical four-channel WAV kernels.

**Tech Stack:** C++17, PFFFT, CMake/CTest, Kotlin, Jetpack Compose, MiuiX 0.9.x, JUnit, Gradle 9.4.1.

## Global Constraints

- Canonical four-channel order is `[H_LL, H_RL, H_LR, H_RR]`.
- Cross branches use `round(crossDelayMs * sampleRate / 1000)` frames of runtime delay.
- Cross Delay ranges from 0 to 10 ms, defaults to 0.3125 ms, and uses 0.0001 ms precision.
- New canonical four-channel IRs swap the two middle tracks but do not bake delay; existing pre-delayed IRs use Cross Delay 0 ms.
- Partition size is exactly 1024 samples and FFT size is 2048.
- The audio thread performs no allocation, locking, file I/O, logging, or destruction.
- Processing supports sample rates from 8 kHz through 384 kHz and arbitrary callback frame counts.
- Keep existing convolver parameter IDs and assign Wet/Gain/Routing/Delay to `0x101B6`, `0x101B7`, `0x101B8`, and `0x101B9`.
- Wet is 0..100 percent, gain is -240..240 centibels, routing is 0=Direct+Cross, 1=Direct Only, 2=Cross Only.
- Do not edit MiuiX source and do not remove Material icons.
- Do not commit unless the user explicitly requests a commit.

---

### Task 1: Add The Matrix FFT Engine

**Files:**
- Create: `ViPERDSP/viper/utils/MatrixConvolver.h`
- Create: `ViPERDSP/viper/utils/MatrixConvolver.cpp`
- Create: `tests/MatrixConvolverTest.cpp`
- Modify: `ViPERDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `enum class MatrixRouting : uint32_t { DIRECT_AND_CROSS, DIRECT_ONLY, CROSS_ONLY }`.
- Produces: `struct MatrixConvolverMix { float cross; float wet; float output_gain; MatrixRouting routing; }`.
- Produces: `bool MatrixConvolver::LoadInterleaved(const float *, uint32_t frames, uint32_t channels, uint32_t sample_rate)`.
- Produces: `uint32_t MatrixConvolver::Process(const float *source, float *dest, uint32_t frames, const MatrixConvolverMix &mix)`.
- Produces: `void Reset()`, `void Unload()`, `bool IsUsable() const`, and `uint32_t LatencyFrames() const`.

- [ ] **Step 1: Register a failing host test target**

Add `matrix_convolver_test` to the root `BUILD_ANALYZER_TESTS` block and link it with `MatrixConvolver.cpp` and `pffft.c`.

```cmake
add_executable(matrix_convolver_test
    tests/MatrixConvolverTest.cpp
    ViPERDSP/viper/utils/MatrixConvolver.cpp
    ViPERDSP/viper/utils/pffft.c
)
target_include_directories(matrix_convolver_test PRIVATE ViPERDSP)
target_link_libraries(matrix_convolver_test PRIVATE m)
```

- [ ] **Step 2: Write failing impulse-routing tests**

Use a 2048-frame input/output fixture. For canonical four-channel frame zero `{1, 2, 3, 4}`, verify a left impulse produces `outL=1` and `outR=2` at the engine latency, while a right impulse produces `outL=3` and `outR=4`.

```cpp
std::array<float, 16 * 4> ir{};
ir[0] = 1.0F; // H_LL
ir[1] = 2.0F; // H_RL
ir[2] = 3.0F; // H_LR
ir[3] = 4.0F; // H_RR
CHECK(engine.LoadInterleaved(ir.data(), 16, 4, 48000));
```

Add mono/stereo tests that verify the generated cross impulse appears 15 samples after the direct impulse at 48 kHz and 30 samples after it at 96 kHz.

- [ ] **Step 3: Run the new target and confirm RED**

Run:

```bash
cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host --target matrix_convolver_test -j2
```

Expected: compilation fails because `MatrixConvolver` is not implemented.

- [ ] **Step 4: Implement immutable kernel preparation**

Allocate all PFFFT setup, work, history, kernel, FIFO, dry-delay, and 10 ms cross-delay storage in `LoadInterleaved`. Expand 1/2-channel input into four deinterleaved time-domain kernels before partitioning. For 4-channel input, copy channels directly using the canonical order.

Use flattened aligned allocations so cleanup is bounded and partial allocation failure can call `Unload()` safely:

```cpp
float *kernel_spectra_[4];
float *input_history_[2];
float *fft_work_;
float *fft_input_[2];
float *fft_accum_[2];
```

- [ ] **Step 5: Implement streaming overlap-save processing**

Buffer arbitrary callback sizes into 1024-sample L/R input blocks. For each full block, perform two forward FFTs, four partition multiply-accumulates into four branch spectra, four inverse FFTs, and enqueue the valid second halves. Delay dry L/R by the same 1024-frame FIFO latency before Wet interpolation, then apply the variable cross-delay taps.

Apply routing weights in the frequency-domain accumulation:

```cpp
const float direct = routing == MatrixRouting::CROSS_ONLY ? 0.0F : 1.0F;
const float cross = routing == MatrixRouting::DIRECT_ONLY ? 0.0F : clamped_cross;
const float denominator = direct + cross;
const float norm = denominator > 0.0F ? 1.0F / denominator : 1.0F;
```

Apply Wet and output gain after inverse FFT with `powf(10.0F, gain_db / 20.0F)` precomputed once per `Process` call.

- [ ] **Step 6: Run matrix tests and confirm GREEN**

Run:

```bash
cmake --build build-host --target matrix_convolver_test -j2
./build-host/matrix_convolver_test
```

Expected: impulse mapping, delay scaling, routing, Wet, gain, reset, and callback-boundary tests pass.

### Task 2: Replace The Convolver Effect Wrapper

**Files:**
- Modify: `ViPERDSP/viper/effects/Convolver.h`
- Modify: `ViPERDSP/viper/effects/Convolver.cpp`
- Modify: `ViPERDSP/viper/ViPER.cpp`
- Modify: `ViPERDSP/CMakeLists.txt`
- Test: `tests/MatrixConvolverTest.cpp`

**Interfaces:**
- Consumes: `MatrixConvolver` from Task 1.
- Produces: `Convolver::SetWet(float)`, `SetOutputGain(float db)`, and `SetRouting(MatrixRouting)`.
- Preserves: upload methods and `GetKernelID()` used by `DspResources`.

- [ ] **Step 1: Add failing wrapper tests**

Test that a committed four-channel buffer becomes active, a malformed replacement does not change the kernel ID, and disabling/re-enabling clears convolution history.

- [ ] **Step 2: Run the wrapper test and confirm RED**

Run `cmake --build build-host --target matrix_convolver_test -j2 && ./build-host/matrix_convolver_test`.

Expected: failures for four-channel commit and new parameter methods.

- [ ] **Step 3: Replace `PConvSingle` and `WaveBuffer` ownership**

Remove `kernel_ch1_`, `kernel_ch2_`, the old post-output cross mix, and the two wrapper `WaveBuffer` objects. Keep the upload buffer and CRC protocol, but validate channel count with:

```cpp
const bool supported = ch_count == 1 || ch_count == 2 || ch_count == 4;
```

Commit by calling `matrix_.LoadInterleaved(kernel_buffer_, frames, channel_count_, sampling_rate_)`. Set the new kernel ID only after successful engine construction.

- [ ] **Step 4: Route all scalar setters into `MatrixConvolverMix`**

Clamp Cross/Wet to 0..1, output gain to -24..24 dB, routing to the three defined enum values, and Cross Delay to 0..10 ms. `Process` converts delay to frames at the active sample rate, returns `frame_size`, and writes delayed output whenever enabled and usable; otherwise it copies/preserves dry input according to the existing in-place contract.

- [ ] **Step 5: Run the wrapper tests and confirm GREEN**

Run the matrix target and `ctest --test-dir build-host --output-on-failure`.

### Task 3: Extend The Native Parameter Snapshot And Resource Protocol

**Files:**
- Modify: `ViPERDSP/include/ViPERParams.h`
- Modify: `ViPERDSP/viper/ParameterSnapshot.cpp`
- Modify: `ViPERDSP/viper/ViPER.cpp`
- Modify: `src/DspResources.cpp`
- Modify: `tests/ParameterSnapshotTest.cpp`
- Modify: `tests/DspResourcesTest.cpp`

**Interfaces:**
- Produces IDs `kParamConvolverWet`, `kParamConvolverOutputGain`, and `kParamConvolverRouting`.
- Extends `ConvolverParams` with `wet`, `output_gain_db`, and `routing`.

- [ ] **Step 1: Add failing snapshot tests**

```cpp
apply(kParamConvolverWet, 65);
apply(kParamConvolverOutputGain, -35);
apply(kParamConvolverRouting, 2);
CHECK(Near(snapshot.convolver.wet, 0.65F));
CHECK(Near(snapshot.convolver.output_gain_db, -3.5F));
CHECK(snapshot.convolver.routing == 2);
```

Also verify clamping for Wet `[-1, 101]`, gain `[-241, 241]`, and routing outside `0..2`.

- [ ] **Step 2: Add a failing four-channel resource replay test**

Upload 64 floats as 16 canonical four-channel frames, commit a valid CRC, prepare a replacement graph, and verify its kernel ID is published. Add a five-channel prepare attempt and verify the previous committed resource remains active.

- [ ] **Step 3: Run snapshot/resource tests and confirm RED**

Run:

```bash
cmake --build build-host --target parameter_snapshot_test dsp_resources_test -j2
./build-host/parameter_snapshot_test
./build-host/dsp_resources_test
```

- [ ] **Step 4: Add IDs, fields, clamping, equality, and engine application**

Use defaults `wet=1.0F`, `output_gain_db=0.0F`, and `routing=0`. Update both raw `SetParam` handling and typed `ApplyConvolver` handling so HIDL and AIDL paths produce identical state.

- [ ] **Step 5: Accept four-channel immutable resources**

Update `CaptureRaw` and `HasConvolverKernel` to accept only 1, 2, or 4 channels. Preserve the current committed vector when prepare/CRC/shape validation fails.

- [ ] **Step 6: Run the tests and confirm GREEN**

Run the two targets and full CTest suite.

### Task 4: Support Canonical Four-Channel WAV Files

**Files:**
- Modify: `ViPERDSP/viper/utils/WavReader.h`
- Modify: `/root/AndroidIDEProjects/ViPER4Android/app/src/main/java/com/llsl/viper4android/utils/WavDecoder.kt`
- Create: `/root/AndroidIDEProjects/ViPER4Android/app/src/test/java/com/llsl/viper4android/utils/WavDecoderTest.kt`
- Create: `tests/WavReaderTest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces decoded channel counts restricted to 1, 2, or 4.
- Supports PCM 16/24/32, IEEE float 32, and WAVE_FORMAT_EXTENSIBLE whose subformat GUID is PCM or IEEE float.

- [ ] **Step 1: Build failing in-memory WAV fixtures**

Create little-endian RIFF fixtures for plain four-channel PCM16 and extensible four-channel float32. Assert decoded sample order remains interleaved and unchanged.

- [ ] **Step 2: Run decoder tests and confirm RED**

Run `./gradlew :app:testDebugUnitTest --tests '*WavDecoderTest' --no-daemon` and the host `wav_reader_test` target.

- [ ] **Step 3: Parse extensible format safely**

Validate chunk bounds before every position/seek update. For format `0xFFFE`, require a `fmt` chunk of at least 40 bytes, read valid bits/channel mask, and inspect the first two bytes of the subformat GUID for PCM `1` or float `3`.

- [ ] **Step 4: Run decoder tests and confirm GREEN**

Run both decoder targets and reject 3/5-channel fixtures explicitly.

### Task 5: Extend Android State, Persistence, And Shared-Memory Layout

**Files:**
- Modify: `/root/AndroidIDEProjects/ViPER4Android/app/src/main/java/com/llsl/viper4android/viper/ViperParams.kt`
- Modify: `/root/AndroidIDEProjects/ViPER4Android/app/src/main/java/com/llsl/viper4android/effect/EffectStates.kt`
- Modify: `/root/AndroidIDEProjects/ViPER4Android/app/src/main/java/com/llsl/viper4android/effect/EffectGroups.kt`
- Modify: `/root/AndroidIDEProjects/ViPER4Android/app/src/main/java/com/llsl/viper4android/viper/ViperParamsLayout.kt`
- Modify: `/root/AndroidIDEProjects/ViPER4Android/app/src/main/java/com/llsl/viper4android/viper/ViperParamsSerializer.kt`
- Create: `/root/AndroidIDEProjects/ViPER4Android/app/src/test/java/com/llsl/viper4android/viper/ConvolverParamsSerializerTest.kt`

**Interfaces:**
- Extends `ConvolverState` with `wet: Int = 100`, `outputGain: Int = 0`, and `routing: Int = 0`.
- Persists keys `wet`, `outputGain`, and `routing` through `ConvolverEffect`.

- [ ] **Step 1: Add a failing serializer test**

Serialize a state with Cross 35, Wet 65, Gain -35, Routing 2 and verify native values at the Convolver offsets are `0.35F`, `0.65F`, `-3.5F`, and integer `2`.

- [ ] **Step 2: Update constants and persisted state**

Add app parameter IDs `0x101B6..0x101B9`; define ranges Wet `0..100`, Output Gain `-240..240`, Routing `0..2`, and Cross Delay `0..100000` in 100 ns units.

- [ ] **Step 3: Update the native mirror layout**

Set `ViperParamsLayout.SIZE` to 1160. Set Convolver size to 24 with offsets Enable 0, Cross 4, Wet 8, Output Gain 12, Routing 16, and Cross Delay 20. Add 16 to every original root effect offset after Convolver; field layouts after it do not otherwise change.

- [ ] **Step 4: Update shared-memory serialization**

Write Wet as `wet / 100F`, gain as `outputGain / 10F`, routing as an integer, and Cross Delay as `crossDelay100Ns / 10000F` milliseconds. Keep `effectiveEnable` dependent on a selected kernel.

- [ ] **Step 5: Run serializer/persistence tests and confirm GREEN**

Run `./gradlew :app:testDebugUnitTest --tests '*ConvolverParamsSerializerTest' --no-daemon` and existing effect preference tests.

### Task 6: Add MiuiX Convolver Controls

**Files:**
- Modify: `/root/AndroidIDEProjects/ViPER4Android/app/src/main/java/com/llsl/viper4android/ui/screens/main/EffectSections.kt`
- Modify: `/root/AndroidIDEProjects/ViPER4Android/app/src/main/res/values/strings.xml`
- Modify: `/root/AndroidIDEProjects/ViPER4Android/app/src/main/res/values-zh-rCN/strings.xml`
- Modify: `/root/AndroidIDEProjects/ViPER4Android/app/src/main/res/values-ru/strings.xml`

**Interfaces:**
- Consumes: `Effects.convolver.wet`, `.outputGain`, and `.routing` from Task 5.

- [ ] **Step 1: Add Wet and Gain sliders**

Use the existing project-local `LabeledSlider` wrapper. Wet range is 0..100 with `%`; gain range is -240..240 state units displayed as `value / 10.0` dB with one decimal place.

- [ ] **Step 2: Add the three-option routing dropdown**

Use the existing project-local dropdown wrapper with localized labels Direct + Cross, Direct Only, and Cross Only. Persist the selected index directly.

- [ ] **Step 3: Verify Compose compilation**

Run `./gradlew :app:compileDebugKotlin --no-daemon` on the supported remote Android environment after syncing changed files.

### Task 7: Full Native And Android Verification

**Files:**
- No source files unless a verification failure exposes a defect.

**Interfaces:**
- Consumes all previous tasks.

- [ ] **Step 1: Run fresh host tests**

```bash
cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host -j2
ctest --test-dir build-host --output-on-failure
```

- [ ] **Step 2: Run sanitizer coverage**

Configure a separate `build-asan` with `-fsanitize=address,undefined -fno-omit-frame-pointer`, build the matrix/resource targets, and run them with leak detection enabled.

- [ ] **Step 3: Build arm64 release driver**

```bash
ssh -p 8022 10645@localhost "cd ~/ViPERFX_RE && make clean && make ARCH=arm64 MODE=Release -j2"
```

- [ ] **Step 4: Build the Android app**

```bash
ssh -p 8022 10645@localhost "cd ~/ViPER4Android && ./gradlew assembleDebug --stacktrace --no-daemon 2>&1"
```

- [ ] **Step 5: Install and validate live playback**

Back up the current module library, bind the new arm64 library inside PID 1's mount namespace, restart the relevant audio services, and verify:

- loaded and built library SHA-256 values match;
- AudioFlinger creates the ViPER effect;
- mono, stereo, and four-channel kernels commit with the expected kernel ID;
- 48/96 kHz playback remains stable with the convolver active;
- no crash-buffer, tombstone, underrun, NaN, or non-finite telemetry appears;
- process-time maxima remain below the active callback budget.

- [ ] **Step 6: Inspect final diffs without committing**

Run `git status --short`, `git diff --check`, and focused diffs in both repositories. Leave unrelated user changes untouched and report any unverified 192/384 kHz hardware routes explicitly.
