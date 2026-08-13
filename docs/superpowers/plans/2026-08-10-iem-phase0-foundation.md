# IEM Phase 0 Portable Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an independently testable `IEMDSP` foundation to `libv4a_re.so` with fixed-capacity planar buffers, a 96 kHz streaming boundary, independent parameters/resources/graph publication, transparent bypass, and telemetry, without adding an audible spatial algorithm or modifying ViPERDSP internals.

**Architecture:** `ViperContext` continues processing stereo through the existing `DspGraph` first, then invokes a separate root-owned `IemGraph`. `IemGraph` owns a portable `iem::IemEngine` from the new `IEMDSP` static library; Phase 0 performs only a transparent host-rate → 96 kHz → host-rate round trip when enabled. IEM scalar state uses its own three-slot mailbox, while rate/topology state uses independent two-slot graph publication.

**Tech Stack:** C++17, CMake 3.16.3, PFFFT-compatible aligned memory conventions, Android AudioEffect legacy ABI, host CTest, remote Android/ARM64 build through SSH.

## Global Constraints

- Keep `ViPERDSP/` source and public parameter layout unchanged.
- Build `IEMDSP` as a separate static library linked into the existing `libv4a_re.so` shared object.
- Internal rate is exactly 96,000 Hz and internal processing quantum is exactly 256 frames.
- Maximum internal channels are exactly 16; Phase 0 processes only two channels but allocates through the same bounded abstraction.
- Host rates remain 8,000 through 384,000 Hz and host blocks remain bounded by `viper::audio::kMaxBlockFrames`.
- No allocation, lock, file I/O, logging, matrix/filter construction, or resource destruction may occur on the audio thread.
- Disabled IEM must leave ViPER output bit-identical.
- IEM failure must restore the post-ViPER dry copy; it must not disable the vendor effect or bypass ViPER.
- Scalar parameter IDs are reserved in `0x12000..0x12FFF` and do not enter `ViPERParams`.
- Phase 0 defaults to disabled and adds no App-facing controls.
- All build and test commands run in remote `~/ViPERFX_RE` through SSH port 8022.
- Preserve unrelated dirty worktree changes and do not commit unless the user explicitly requests it.

---

## File Map

### New `IEMDSP` Library

- `IEMDSP/CMakeLists.txt`: static-library source list and public includes.
- `IEMDSP/include/iem/AlignedPlanarBuffer.h`: aligned fixed-capacity planar storage.
- `IEMDSP/src/AlignedPlanarBuffer.cpp`: control-thread allocation and reset.
- `IEMDSP/include/iem/IemParams.h`: independent scalar state and parameter IDs.
- `IEMDSP/src/IemParams.cpp`: parameter classification, conversion, and clamping.
- `IEMDSP/include/iem/StreamingResampler.h`: allocation-free streaming resampler API.
- `IEMDSP/src/StreamingResampler.cpp`: 64-tap/1024-phase polyphase resampling.
- `IEMDSP/include/iem/PlanarBlockScheduler.h`: fixed 256-frame internal block FIFO API.
- `IEMDSP/src/PlanarBlockScheduler.cpp`: bounded planar ring implementation.
- `IEMDSP/include/iem/IemEngine.h`: stereo transparent round-trip engine API.
- `IEMDSP/src/IemEngine.cpp`: resampling, scheduling, wet/bypass ramp, and latency.
- `IEMDSP/include/iem/IemTelemetry.h`: realtime telemetry snapshot and bypass reason.
- `IEMDSP/src/IemTelemetry.cpp`: lock-free telemetry publication.

### Root Driver Integration

- `src/IemParameterMailbox.h/.cpp`: three-slot `iem::IemParams` mailbox.
- `src/IemResources.h/.cpp`: immutable resource-generation interface; Phase 0 carries only generation and validates empty state.
- `src/IemGraph.h/.cpp`: owns `iem::IemEngine`, configuration, and graph transition.
- `src/IemGraphSlots.h/.cpp`: independent two-slot active/pending/previous state machine.
- `src/IemContext.h/.cpp`: owns all IEM root-driver state, post-ViPER buffers, dispatch, publication, reset, and failure restoration.
- `src/ViperContext.h/.cpp`: routes IEM IDs, prepares/consumes IEM graphs, and processes IEM after ViPER.
- `src/TelemetryProtocol.h`: adds fixed-layout `IemTelemetryWire` without changing existing `TelemetryWire`.
- `CMakeLists.txt`: adds `IEMDSP`, new root sources, host tests, and links.

### Host Tests

- `tests/IemAlignedPlanarBufferTest.cpp`
- `tests/IemParamsTest.cpp`
- `tests/IemStreamingResamplerTest.cpp`
- `tests/IemPlanarBlockSchedulerTest.cpp`
- `tests/IemEngineTest.cpp`
- `tests/IemParameterMailboxTest.cpp`
- `tests/IemGraphSlotsTest.cpp`
- `tests/IemContextTest.cpp`
- `tests/IemTelemetryTest.cpp`

---

### Task 1: Static Library And Aligned Planar Storage

**Files:**
- Create: `IEMDSP/CMakeLists.txt`
- Create: `IEMDSP/include/iem/AlignedPlanarBuffer.h`
- Create: `IEMDSP/src/AlignedPlanarBuffer.cpp`
- Create: `tests/IemAlignedPlanarBufferTest.cpp`
- Modify: `CMakeLists.txt:21-23,115-132`

**Interfaces:**
- Produces: `iem::AlignedPlanarBuffer::Prepare(uint32_t channels, size_t frames) -> bool`
- Produces: `ChannelData(uint32_t) -> float *`, `ChannelData(uint32_t) const -> const float *`
- Produces: `Channels()`, `Frames()`, `Clear()`, `IsPrepared()`

- [ ] **Step 1: Write the storage test**

```cpp
#include "iem/AlignedPlanarBuffer.h"
#include <cstdint>
#include <cstdio>

int main() {
    iem::AlignedPlanarBuffer buffer;
    if (!buffer.Prepare(16, 256)) return 1;
    if (!buffer.IsPrepared() || buffer.Channels() != 16 || buffer.Frames() != 256) return 2;
    for (uint32_t channel = 0; channel < 16; ++channel) {
        float *data = buffer.ChannelData(channel);
        if (data == nullptr || reinterpret_cast<uintptr_t>(data) % 64 != 0) return 3;
        data[17] = static_cast<float>(channel + 1);
    }
    buffer.Clear();
    for (uint32_t channel = 0; channel < 16; ++channel) {
        if (buffer.ChannelData(channel)[17] != 0.0F) return 4;
    }
    if (buffer.Prepare(0, 256) || buffer.Prepare(17, 256) || buffer.Prepare(2, 0)) return 5;
    std::puts("IEM aligned planar buffer tests passed");
    return 0;
}
```

- [ ] **Step 2: Register the missing target and verify RED remotely**

Add a temporary `iem_aligned_planar_buffer_test` target to the root `BUILD_ANALYZER_TESTS` block, sync `CMakeLists.txt` and the test, then run:

```bash
ssh -p 8022 10645@localhost \
  'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-host --target iem_aligned_planar_buffer_test -j2'
```

Expected: configuration or compilation fails because `IEMDSP` and `AlignedPlanarBuffer` do not exist.

- [ ] **Step 3: Implement bounded aligned storage**

Use this class definition:

```cpp
namespace iem {

class AlignedPlanarBuffer final {
public:
    static constexpr uint32_t kMaxChannels = 16;

    AlignedPlanarBuffer() = default;
    ~AlignedPlanarBuffer();
    AlignedPlanarBuffer(const AlignedPlanarBuffer &) = delete;
    AlignedPlanarBuffer &operator=(const AlignedPlanarBuffer &) = delete;

    bool Prepare(uint32_t channels, size_t frames) noexcept;
    void Clear() noexcept;
    float *ChannelData(uint32_t channel) noexcept;
    const float *ChannelData(uint32_t channel) const noexcept;
    uint32_t Channels() const noexcept;
    size_t Frames() const noexcept;
    bool IsPrepared() const noexcept;

private:
    float *storage_ = nullptr;
    uint32_t channels_ = 0;
    size_t frames_ = 0;
    size_t stride_ = 0;
};

} // namespace iem
```

Implementation requirements:

- Round each channel stride up to 16 floats so every channel begins on a 64-byte boundary.
- Reject overflow before multiplication.
- Allocate once with `posix_memalign(..., 64, byte_count)` and zero with `memset`.
- `Prepare` builds replacement storage first and swaps only after successful allocation.
- Destructor frees with `std::free`.
- Accessors return `nullptr` for out-of-range channels.

- [ ] **Step 4: Create and link `IEMDSP`**

```cmake
add_library(IEMDSP STATIC
    src/AlignedPlanarBuffer.cpp
)
target_include_directories(IEMDSP PUBLIC include)
target_compile_options(IEMDSP PRIVATE
    -Wall -Wextra -Werror
    -fno-exceptions -fno-rtti
)
```

Add `add_subdirectory(IEMDSP)` before `add_subdirectory(ViPERDSP)`, link `v4a_re` with `IEMDSP`, and link the host test with `IEMDSP`.

- [ ] **Step 5: Run the focused test remotely**

Run the target and executable. Expected: exit code 0 and `IEM aligned planar buffer tests passed`.

### Task 2: Independent Parameter Snapshot And Mailbox

**Files:**
- Create: `IEMDSP/include/iem/IemParams.h`
- Create: `IEMDSP/src/IemParams.cpp`
- Create: `src/IemParameterMailbox.h`
- Create: `src/IemParameterMailbox.cpp`
- Create: `tests/IemParamsTest.cpp`
- Create: `tests/IemParameterMailboxTest.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `iem::IemParams`
- Produces: `iem::ParamUpdate UpdateIemParameterSnapshot(IemParams &, int param, int val1, int val2, int val3) noexcept`
- Produces: `viper::audio::IemParameterMailbox::Publish` and `ConsumeLatest`

- [ ] **Step 1: Write parameter conversion tests**

Test these exact IDs and values:

```cpp
static_assert(iem::kParamIemEnable == 0x12000);
static_assert(iem::kParamIemWet == 0x12001);
static_assert(iem::kParamIemOutputGain == 0x12002);
static_assert(iem::kParamIemOrder == 0x12003);
static_assert(iem::kParamIemResourceReset == 0x12100);

const auto near = [](float left, float right) {
    return std::fabs(left - right) <= 1.0e-6F;
};
iem::IemParams params{};
if (iem::UpdateIemParameterSnapshot(params, iem::kParamIemEnable, 1, 0, 0)
    != iem::ParamUpdate::UPDATED || !params.enable) return 1;
if (iem::UpdateIemParameterSnapshot(params, iem::kParamIemWet, 65, 0, 0)
    != iem::ParamUpdate::UPDATED || !near(params.wet, 0.65F)) return 2;
if (iem::UpdateIemParameterSnapshot(params, iem::kParamIemOutputGain, -35, 0, 0)
    != iem::ParamUpdate::UPDATED || !near(params.output_gain_db, -3.5F)) return 3;
if (iem::UpdateIemParameterSnapshot(params, iem::kParamIemOrder, 8, 0, 0)
    != iem::ParamUpdate::UPDATED || params.order != 3) return 4;
if (iem::UpdateIemParameterSnapshot(params, iem::kParamIemResourceReset, 1, 0, 0)
    != iem::ParamUpdate::COMMAND) return 5;
if (iem::UpdateIemParameterSnapshot(params, 0x101B9, 0, 0, 0)
    != iem::ParamUpdate::NOT_IEM) return 6;
```

- [ ] **Step 2: Write the mailbox race/ordering test**

Verify the latest of 10,000 publications is consumed, generations increase, and the consumer never observes a mixed snapshot:

```cpp
viper::audio::IemParameterMailbox mailbox;
std::atomic<bool> done{false};
std::atomic<int> errors{0};
std::thread producer([&] {
    for (int marker = 1; marker <= 10000; ++marker) {
        iem::IemParams params{};
        params.wet = static_cast<float>(marker % 101) / 100.0F;
        params.output_gain_db = static_cast<float>(marker);
        params.order = static_cast<uint32_t>(marker % 3 + 1);
        mailbox.Publish(params);
    }
    done.store(true, std::memory_order_release);
});
std::thread consumer([&] {
    uint64_t generation = 0;
    iem::IemParams params{};
    while (!done.load(std::memory_order_acquire) || generation < 10000) {
        if (!mailbox.ConsumeLatest(generation, params)) continue;
        const int marker = static_cast<int>(params.output_gain_db);
        const float expected_wet = static_cast<float>(marker % 101) / 100.0F;
        const uint32_t expected_order = static_cast<uint32_t>(marker % 3 + 1);
        if (params.wet != expected_wet || params.order != expected_order) ++errors;
    }
});
producer.join();
consumer.join();
if (errors.load() != 0) return 1;
```

- [ ] **Step 3: Run both new targets and verify RED remotely**

Register `iem_params_test` with `IemParams.cpp`, and register `iem_parameter_mailbox_test` with `IemParameterMailbox.cpp`, `IemParams.cpp`, and `Threads::Threads`. Expected: unresolved parameter and mailbox types.

- [ ] **Step 4: Implement `IemParams`**

```cpp
namespace iem {

constexpr int kParamIemEnable = 0x12000;
constexpr int kParamIemWet = 0x12001;
constexpr int kParamIemOutputGain = 0x12002;
constexpr int kParamIemOrder = 0x12003;
constexpr int kParamIemResourceReset = 0x12100;

struct IemParams {
    bool enable = false;
    float wet = 1.0F;
    float output_gain_db = 0.0F;
    uint32_t order = 1;
};

enum class ParamUpdate { NOT_IEM, UPDATED, COMMAND };

} // namespace iem
```

Clamp Wet to 0..100 before dividing by 100, gain to -240..240 before dividing by 10, and order to 1..3.
Classify only `kParamIemResourceReset` as `COMMAND`; unknown values inside or outside the reserved range return `NOT_IEM`.

- [ ] **Step 5: Implement the independent three-slot mailbox**

Use the existing `ParameterMailbox` memory-ordering algorithm verbatim at the type/structure level, but include only `IemParams`; do not template or modify the ViPER mailbox in Phase 0.

Add `find_package(Threads REQUIRED)` inside the root test block and link `iem_parameter_mailbox_test` with `Threads::Threads` because its producer/consumer test uses `std::thread`.

- [ ] **Step 6: Run both tests remotely**

Expected: parameter conversion and mailbox tests pass.

### Task 3: Allocation-Free Streaming Resampler

**Files:**
- Create: `IEMDSP/include/iem/StreamingResampler.h`
- Create: `IEMDSP/src/StreamingResampler.cpp`
- Create: `tests/IemStreamingResamplerTest.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `StreamingResampler::Prepare(input_rate, output_rate, channels, max_input_frames) -> bool`
- Produces: `Process(input, input_frames, output, output_capacity) -> size_t`
- Produces: `MaxOutputFrames`, `LatencyInputFrames`, `Reset`, `IsPrepared`

- [ ] **Step 1: Write deterministic resampler tests**

Cover:

- 48,000 → 96,000 stereo constant input: produced frames are within the documented filter priming bound and settled samples equal 0.25 within `1e-4`.
- 384,000 → 96,000 stereo 1 kHz sine: settled RMS differs by less than 0.15 dB.
- 8,000 → 96,000 mono impulse: all produced samples are finite and reported count never exceeds `MaxOutputFrames`.
- Chunk invariance: one 4,096-frame call and calls split as 17/31/257/509/remainder produce equal output within `2e-5` after matching priming.
- `Reset` reproduces the same impulse output.
- Calls after `Prepare` do not allocate, measured by a test-only global `operator new` counter.

Representative test body:

```cpp
iem::StreamingResampler resampler;
if (!resampler.Prepare(48000, 96000, 2, 4096)) return 1;
std::array<std::vector<float>, 2> input{
    std::vector<float>(4096, 0.25F),
    std::vector<float>(4096, 0.25F),
};
const size_t capacity = resampler.MaxOutputFrames(input[0].size());
std::array<std::vector<float>, 2> output{
    std::vector<float>(capacity),
    std::vector<float>(capacity),
};
const float *input_ptrs[2]{input[0].data(), input[1].data()};
float *output_ptrs[2]{output[0].data(), output[1].data()};
const size_t produced = resampler.Process(
    input_ptrs, input[0].size(), output_ptrs, capacity
);
if (produced == 0 || produced > capacity) return 2;
for (size_t i = 256; i < produced; ++i) {
    if (std::fabs(output[0][i] - 0.25F) > 1.0e-4F) return 3;
}
```

- [ ] **Step 2: Register and run the missing target remotely**

Register `iem_streaming_resampler_test` with `IemStreamingResamplerTest.cpp` and link it to `IEMDSP`. Expected: compile failure because `StreamingResampler` is absent.

- [ ] **Step 3: Implement the API and coefficient bank**

```cpp
class StreamingResampler final {
public:
    static constexpr uint32_t kTapCount = 64;
    static constexpr uint32_t kPhaseCount = 1024;

    bool Prepare(
        uint32_t input_rate,
        uint32_t output_rate,
        uint32_t channels,
        size_t max_input_frames
    ) noexcept;
    size_t Process(
        const float *const *input,
        size_t input_frames,
        float *const *output,
        size_t output_capacity
    ) noexcept;
    size_t MaxOutputFrames(size_t input_frames) const noexcept;
    uint32_t LatencyInputFrames() const noexcept;
    void Reset() noexcept;
    bool IsPrepared() const noexcept;
};
```

Implementation rules:

- Build 1,024 phases × 64 taps during `Prepare` using a Kaiser-windowed sinc with beta 8.6.
- Cutoff is `0.475 * min(1.0, output_rate / input_rate)` in input-normalized units.
- Normalize every phase to unity DC gain.
- Keep a 64-frame history ring per channel and a 64-bit fixed-point source position.
- Store all coefficients, history, and scratch in `AlignedPlanarBuffer` or aligned owned arrays prepared off-thread.
- Clamp denormal output to zero; reject non-finite input by returning zero produced frames and setting an internal failure flag.
- `Process` must not resize any container.

- [ ] **Step 4: Run the focused test remotely**

Expected: all rate, response, chunk, reset, and allocation assertions pass.

### Task 4: Fixed 256-Frame Scheduler And Transparent Engine

**Files:**
- Create: `IEMDSP/include/iem/PlanarBlockScheduler.h`
- Create: `IEMDSP/src/PlanarBlockScheduler.cpp`
- Create: `IEMDSP/include/iem/IemEngine.h`
- Create: `IEMDSP/src/IemEngine.cpp`
- Create: `tests/IemPlanarBlockSchedulerTest.cpp`
- Create: `tests/IemEngineTest.cpp`
- Modify: `IEMDSP/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: bounded planar FIFO push/peek/consume APIs.
- Produces: `IemEngine::Prepare`, `ApplyParams`, `Process`, `Reset`, `LatencyFrames`, `IsPrepared`.

- [ ] **Step 1: Write scheduler tests**

Verify a two-channel ramp pushed in 17/300/1/511-frame chunks is emitted only in exact 256-frame blocks, wraps without corruption, rejects capacity overflow without modifying existing data, and resets to empty.

```cpp
iem::PlanarBlockScheduler scheduler;
if (!scheduler.Prepare(2, 1024)) return 1;
// Push the same monotonically increasing fixture in 17/300/1/511 frame calls.
if (!scheduler.HasBlock()) return 2;
for (size_t frame = 0; frame < 256; ++frame) {
    if (scheduler.ChannelBlock(0)[frame] != static_cast<float>(frame)) return 3;
}
scheduler.ConsumeBlock();
if (scheduler.AvailableFrames() != 573) return 4;
```

- [ ] **Step 2: Implement `PlanarBlockScheduler`**

```cpp
class PlanarBlockScheduler final {
public:
    static constexpr size_t kBlockFrames = 256;
    bool Prepare(uint32_t channels, size_t capacity_frames) noexcept;
    bool Push(const float *const *input, size_t frames) noexcept;
    bool HasBlock() const noexcept;
    const float *ChannelBlock(uint32_t channel) const noexcept;
    void ConsumeBlock() noexcept;
    size_t AvailableFrames() const noexcept;
    void Reset() noexcept;
};
```

Use one aligned ring per channel and copy wrapped regions in at most two `memcpy` operations.

- [ ] **Step 3: Write transparent engine tests**

For host rates 8,000, 44,100, 48,000, 96,000, 192,000, and 384,000:

- disabled engine leaves every interleaved sample bit-identical;
- enabled engine always returns exactly the input host frame count;
- after `LatencyFrames()` and filter settling, a 997 Hz stereo sine round trip stays within 0.2 dB and contains no NaN/Inf;
- arbitrary callback sizes 1/7/63/256/1,023/8,192 remain bounded;
- setting Wet 0 produces latency-aligned dry when enabled;
- Output Gain +6.0 dB produces the expected linear gain within `1e-3` after settling;
- no allocation occurs in `Process` after `Prepare`.

```cpp
for (const uint32_t rate : {8000U, 44100U, 48000U, 96000U, 192000U, 384000U}) {
    iem::IemEngine engine;
    iem::IemParams params{};
    if (!engine.Prepare({rate, 8192, 96000, 256, 16}, params)) return 1;
    std::vector<float> audio(8192 * 2, 0.125F);
    const auto original = audio;
    if (!engine.Process(audio.data(), 8192) || audio != original) return 2;
    params.enable = true;
    engine.ApplyParams(params);
    if (!engine.Process(audio.data(), 8192)) return 3;
    for (float sample : audio) if (!std::isfinite(sample)) return 4;
}
```

- [ ] **Step 4: Implement `IemEngine`**

```cpp
struct IemEngineConfig {
    uint32_t host_sample_rate = 0;
    size_t max_host_block_frames = 0;
    uint32_t internal_sample_rate = 96000;
    size_t internal_block_frames = 256;
    uint32_t max_channels = 16;
};

class IemEngine final {
public:
    bool Prepare(const IemEngineConfig &config, const IemParams &params) noexcept;
    void ApplyParams(const IemParams &params) noexcept;
    bool Process(float *stereo_interleaved, size_t host_frames) noexcept;
    void Reset() noexcept;
    uint32_t LatencyFrames() const noexcept;
    bool IsPrepared() const noexcept;
};
```

Processing order in Phase 0:

1. If disabled, return true without touching input.
2. Deinterleave stereo into fixed host scratch.
3. Push host→96 kHz resampler output into the input scheduler.
4. For every complete 256-frame block, copy both channels unchanged into the internal output scheduler.
5. Feed internal output through 96 kHz→host resampler and an exact-count host output FIFO.
6. Emit exactly `host_frames`, using zeros only during initial priming.
7. Delay dry by the same reported latency, apply Wet, apply precomputed gain, and use a 256-host-frame enable/bypass ramp.

All FIFO capacities are computed during `Prepare` from the worst supported ratio for the configured host rate plus two internal blocks. Reject a configuration if any bound overflows.

- [ ] **Step 5: Run scheduler and engine tests remotely**

Register `iem_planar_block_scheduler_test` and `iem_engine_test`, link both to `IEMDSP`, then run them remotely. Expected: both pass at all listed rates and callback sizes.

### Task 5: Independent Resources, Graph, And Two-Slot Publication

**Files:**
- Create: `src/IemResources.h`
- Create: `src/IemResources.cpp`
- Create: `src/IemGraph.h`
- Create: `src/IemGraph.cpp`
- Create: `src/IemGraphSlots.h`
- Create: `src/IemGraphSlots.cpp`
- Create: `tests/IemGraphSlotsTest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `viper::audio::IemGraphConfig`
- Produces: `IemGraph::Prepare`, `Process`, `ApplyParams`, `Reset`, `Engine`, `Config`, `Transition`, `SetResourceGeneration`, `ResourceGeneration`.
- Produces: `IemGraphSlots::PrepareInitial`, `PreparePending`, `ConsumePending`, `ReleasePrevious`.
- Produces: `IemResources::Generation`, `CaptureRaw`, `ApplyTo`.

- [ ] **Step 1: Write graph-slot state tests**

Write `IemGraphSlotsTest` with the following state assertions:

- initial generation 1 publishes graph 0;
- generation 2 prepares graph 1 without replacing active early;
- audio-side `ConsumePending` returns active generation 2 and previous generation 1;
- a second pending graph is rejected until `ReleasePrevious`;
- equal/older generations are rejected;
- invalid host rates and blocks preserve the active graph;
- scalar params applied to active and previous graphs stay equal during transition.

```cpp
viper::audio::IemGraphSlots slots;
viper::audio::IemResources resources;
iem::IemParams params{};
if (!slots.PrepareInitial({48000, 8192, 1}, params, resources)) return 1;
if (!slots.PreparePending({96000, 8192, 2}, params, resources)) return 2;
const auto swap = slots.ConsumePending();
if (!swap.changed || swap.active == nullptr || swap.previous == nullptr) return 3;
if (swap.active->Config().generation != 2) return 4;
if (slots.PreparePending({96000, 8192, 3}, params, resources)) return 5;
slots.ReleasePrevious();
if (!slots.PreparePending({96000, 8192, 3}, params, resources)) return 6;
```

- [ ] **Step 2: Run target and verify RED remotely**

Register `iem_graph_slots_test` with `IemGraph.cpp`, `IemGraphSlots.cpp`, `IemResources.cpp`, `GraphCrossfade.cpp`, and `AudioFormat.cpp`, and link `IEMDSP`. Expected: missing graph/resource types.

- [ ] **Step 3: Implement Phase 0 `IemResources`**

```cpp
class IemResources final {
public:
    uint64_t Generation() const noexcept;
    bool CaptureRaw(int param, int val1) noexcept;
    bool ApplyTo(IemGraph &graph) const noexcept;
private:
    uint64_t generation_ = 0;
};
```

`CaptureRaw` accepts only `kParamIemResourceReset` with `val1 == 1`, increments generation, and rejects every other value without changing state. Phase 0 `ApplyTo` validates that the graph is prepared and calls `graph.SetResourceGeneration(generation_)`; no payload or heap allocation is added. This locks the interface used by later immutable HRTF/layout resources.

- [ ] **Step 4: Implement `IemGraph` and slots**

Use a two-element `std::array<IemGraph, 2>` and the same packed active/pending/previous atomic state representation as `DspGraphSlots`, implemented in new files rather than modifying or templating the ViPER slots.

`IemGraph::Process` calls only `IemEngine::Process`. `IemGraphConfig` contains host sample rate, max block frames, and generation. `Prepare` validates with `IsSupportedSampleRate` and `kMaxBlockFrames` before preparing the engine. The graph owns a root `GraphCrossfade`, exposes it through `Transition()`, and stores the last applied resource generation for publication tests.

- [ ] **Step 5: Run graph-slot tests remotely**

Expected: all state, generation, and invalid-replacement cases pass.

### Task 6: ViperContext Post-ViPER Integration And Failure Isolation

**Files:**
- Create: `src/IemContext.h`
- Create: `src/IemContext.cpp`
- Modify: `src/ViperContext.h:3-59`
- Modify: `src/ViperContext.cpp:22-31,174-245,247-265,604-755`
- Modify: `CMakeLists.txt:115-132`
- Create: `tests/IemContextTest.cpp`

**Interfaces:**
- Consumes: `IemParameterMailbox`, `IemResources`, and `IemGraphSlots`.
- Produces: `IemContext::Prepare`, `DispatchRawParam`, `Process`, `Reset`.
- Produces: post-ViPER IEM processing in the vendor effect.

- [ ] **Step 1: Write a chain-level test harness**

Build a host test from `DspGraph` plus `IemContext`. Verify:

- IEM disabled output equals ViPER-only output exactly;
- IEM enabled receives ViPER output, not original dry input;
- passing a frame count above the prepared capacity makes IEM processing report failure while restoring the saved ViPER output copy;
- an IEM parameter ID updates only `IemParams`;
- a ViPER parameter ID still updates only `ViPERParams`;
- reset clears IEM resampler/FIFO state independently of ViPER effect state;
- impulse, seeded random, silence, and maximum finite PCM inputs remain bit-identical at 44.1/48/96/192/384 kHz while default IEM params are disabled.

```cpp
viper::audio::DspGraph viper_graph;
IemContext iem_context;
if (!viper_graph.Prepare({48000, 8192, 1}) || !iem_context.Prepare(48000, 8192)) {
    return 1;
}
std::vector<float> audio(1024 * 2, 0.0F);
audio[0] = 1.0F;
if (!viper_graph.Process(audio.data(), 1024)) return 2;
const auto post_viper = audio;
if (!iem_context.Process(audio.data(), 1024) || audio != post_viper) return 3;
```

- [ ] **Step 2: Implement the independent root `IemContext`**

```cpp
class IemContext final {
public:
    bool Prepare(uint32_t host_sample_rate, size_t max_block_frames);
    bool DispatchRawParam(int param, int val1, int val2, int val3) noexcept;
    bool Process(float *post_viper_interleaved, size_t frame_count) noexcept;
    void Reset() noexcept;

private:
    std::vector<float> dry_buffer_;
    std::vector<float> previous_buffer_;
    viper::audio::IemGraphSlots graph_slots_;
    viper::audio::IemResources resources_;
    viper::audio::IemParameterMailbox parameter_mailbox_;
    iem::IemParams parameter_snapshot_{};
    uint64_t applied_parameter_generation_ = 0;
    uint64_t graph_generation_ = 0;
    uint32_t host_sample_rate_ = 0;
    size_t max_block_frames_ = 0;
};
```

Allocate both vectors during `Prepare` with the configured maximum stereo buffer size. `Process` owns graph swaps, scalar consumption, previous-graph crossfade, post-ViPER dry copying, and restoration on every failure path.

- [ ] **Step 3: Route independent parameter IDs**

Inside `IemContext::DispatchRawParam`, call `UpdateIemParameterSnapshot`. For `UPDATED`, publish only to `parameter_mailbox_` and return true. For `COMMAND`, call `resources_.CaptureRaw`; when it succeeds, prepare a pending IEM graph using the active host configuration, current IEM snapshot, incremented graph generation, and updated resources, then return true. A rejected IEM command still returns true but leaves the active graph unchanged. For `NOT_IEM`, return false so `ViperContext` executes existing ViPER dispatch unchanged.

- [ ] **Step 4: Prepare and reset the IEM graph independently**

Add one `IemContext iem_context_` member to `ViperContext`. During `HandleSetConfig`, call `iem_context_.Prepare` after the ViPER graph succeeds. If IEM preparation fails, leave IEM bypassed and do not set the global `disable_reason_`. At the start of `ViperContext::DispatchRawParam`, return immediately when `iem_context_.DispatchRawParam(...)` returns true.

Extend reset/enable/disable/discontinuity handling to reset IEM graphs and clear only IEM buffers without changing the existing ViPER reset order.

- [ ] **Step 5: Process IEM after ViPER**

Immediately after the existing ViPER graph transition and before `analyzer_.Push`, call:

```cpp
iem_context_.Process(buffer_.data(), frame_count);
```

`IemContext::Process` leaves valid post-IEM output in the buffer on success and restores the post-ViPER input on failure. Analyzer and PCM conversion always consume that guaranteed buffer.

- [ ] **Step 6: Run chain and existing graph tests remotely**

Register `iem_context_test` with `IemContext.cpp`, IEM root graph/resource/mailbox sources, ViPER graph sources, `GraphCrossfade.cpp`, and `AudioFormat.cpp`; link `IEMDSP`, `ViPERDSP`, and `m`. Run it with `dsp_graph_test`, `dsp_graph_slots_test`, `parameter_mailbox_test`, and `dsp_resources_test`. Expected: all pass.

### Task 7: Telemetry, Deadline Accounting, And Bypass Reasons

**Files:**
- Create: `IEMDSP/include/iem/IemTelemetry.h`
- Create: `IEMDSP/src/IemTelemetry.cpp`
- Create: `tests/IemTelemetryTest.cpp`
- Modify: `IEMDSP/src/IemEngine.cpp`
- Modify: `src/IemContext.h`
- Modify: `src/IemContext.cpp`
- Modify: `src/TelemetryProtocol.h`
- Modify: `src/ViperContext.cpp:12-20,408-424`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `iem::IemTelemetrySnapshot`
- Produces: `iem::IemTelemetryPublisher::RecordBlock`, `RecordFailure`, `Read`
- Produces: `viper::IemTelemetryWire`
- Produces: `IemContext::ReadTelemetry(iem::IemTelemetrySnapshot &) const noexcept`
- Produces query ID `kParamGetIemTelemetry = 10`

- [ ] **Step 1: Write telemetry snapshot tests**

Verify concurrent reads never mix sequence generations and these values round-trip exactly:

- enabled/prepared flags
- bypass reason
- host and internal rate
- reported latency
- processed frames
- latest, average, and maximum process nanoseconds
- deadline miss count
- resampler underflow count

```cpp
iem::IemTelemetryPublisher publisher;
publisher.Configure(48000, 96000);
publisher.RecordBlock(256, 1200000, 5333333, 1024, true);
iem::IemTelemetrySnapshot snapshot{};
if (!publisher.Read(snapshot)) return 1;
if (snapshot.processed_frames != 256 || snapshot.latest_process_ns != 1200000) return 2;
if (snapshot.host_sample_rate != 48000 || snapshot.internal_sample_rate != 96000) return 3;
publisher.RecordFailure(iem::IemBypassReason::OUTPUT_UNDERFLOW);
if (!publisher.Read(snapshot)
    || snapshot.bypass_reason != iem::IemBypassReason::OUTPUT_UNDERFLOW) return 4;
```

- [ ] **Step 2: Implement telemetry types**

```cpp
enum class IemBypassReason : uint32_t {
    NONE = 0,
    DISABLED,
    NOT_PREPARED,
    INVALID_BLOCK,
    RESAMPLER_FAILURE,
    OUTPUT_UNDERFLOW,
    NON_FINITE,
};

struct IemTelemetrySnapshot {
    uint64_t sequence = 0;
    uint64_t processed_frames = 0;
    uint64_t latest_process_ns = 0;
    uint64_t average_process_ns = 0;
    uint64_t max_process_ns = 0;
    uint64_t deadline_misses = 0;
    uint64_t output_underflows = 0;
    uint32_t host_sample_rate = 0;
    uint32_t internal_sample_rate = 96000;
    uint32_t latency_frames = 0;
    IemBypassReason bypass_reason = IemBypassReason::DISABLED;
    bool enabled = false;
    bool prepared = false;
};
```

Use a sequence-lock publisher with fixed atomics. Update average using an integer running total/count bounded by periodic renormalization; do not use a mutex.

- [ ] **Step 3: Instrument `IemEngine::Process`**

Measure with `steady_clock`, compute callback budget as `host_frames * 1e9 / host_sample_rate`, increment a miss when process duration exceeds budget, and record every failure/underflow reason. Disabled calls record `DISABLED` without touching samples.

- [ ] **Step 4: Add a separate telemetry wire query**

Append `IemTelemetryWire` to `TelemetryProtocol.h` with fixed-width integer fields and a wire version. Implement `IemContext::ReadTelemetry` by reading the active IEM engine publisher. Add GET query ID 10 in `ViperContext`; preserve query ID 9 and `TelemetryWire` byte layout unchanged.

- [ ] **Step 5: Run telemetry and chain tests remotely**

Register `iem_telemetry_test` with `IemTelemetry.cpp` and link `IEMDSP` plus `Threads::Threads`. Run it with `iem_context_test` and `audio_analyzer_test`. Expected: telemetry concurrency, wire conversion, deadline, failure isolation, and existing analyzer telemetry tests pass.

### Task 8: Full Phase 0 Remote Verification

**Files:**
- Verify all files created or modified in Tasks 1-7.

**Interfaces:**
- Consumes the complete Phase 0 foundation.

- [ ] **Step 1: Sync only intended Phase 0 files**

Use `rsync -azR -e 'ssh -p 8022'` with `IEMDSP/`, the listed root sources, listed tests, and `CMakeLists.txt`. Do not sync unrelated dirty files.

- [ ] **Step 2: Configure and run the complete host suite**

```bash
ssh -p 8022 10645@localhost '
  cd "$HOME/ViPERFX_RE" &&
  cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug &&
  cmake --build build-host --target \
    audio_analyzer_test audio_format_test graph_crossfade_test \
    dsp_graph_test parameter_snapshot_test dsp_resources_test \
    dsp_graph_slots_test parameter_mailbox_test matrix_convolver_test \
    convolver_test wav_reader_test \
    iem_aligned_planar_buffer_test iem_params_test \
    iem_streaming_resampler_test iem_planar_block_scheduler_test \
    iem_engine_test iem_parameter_mailbox_test iem_graph_slots_test \
    iem_context_test iem_telemetry_test -j2 &&
  ctest --test-dir build-host --output-on-failure
'
```

Expected: all existing and new host tests pass.

- [ ] **Step 3: Run UBSan host coverage**

```bash
ssh -p 8022 10645@localhost '
  cd "$HOME/ViPERFX_RE" &&
  cmake --fresh -S . -B build-ubsan \
    -DBUILD_ANALYZER_TESTS=ON \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined" &&
  cmake --build build-ubsan --target \
    iem_aligned_planar_buffer_test iem_params_test \
    iem_streaming_resampler_test iem_planar_block_scheduler_test \
    iem_engine_test iem_parameter_mailbox_test iem_graph_slots_test \
    iem_context_test iem_telemetry_test \
    dsp_graph_test dsp_graph_slots_test parameter_mailbox_test -j2 &&
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    ctest --test-dir build-ubsan --output-on-failure \
      -R "iem_|dsp_graph|parameter_mailbox"
'
```

Expected: all selected tests pass with no UBSan diagnostics. ASan is not a completion gate because the current Termux ASan runtime exits with SIGILL even for a trivial sanitized program.

- [ ] **Step 4: Build both release architectures remotely**

```bash
ssh -p 8022 10645@localhost \
  'cd "$HOME/ViPERFX_RE" && make clean && make -j2'
```

Expected:

- `out/libv4a_re_arm64-v8a.so`
- `out/libv4a_re_armeabi-v7a.so`

- [ ] **Step 5: Verify disabled-IEM binary behavior before installation**

`IemContextTest` from Task 6 must include impulse, seeded random, silence, and maximum finite PCM vectors at 44.1/48/96/192/384 kHz. Run:

```bash
ssh -p 8022 10645@localhost '
  cd "$HOME/ViPERFX_RE" &&
  sha256sum out/libv4a_re_arm64-v8a.so out/libv4a_re_armeabi-v7a.so &&
  ./build-host/iem_context_test
'
```

Expected: both hashes are printed and the fixture reports bit-identical disabled-IEM output.

- [ ] **Step 6: Inspect final source state without committing**

Run:

```bash
git diff --check
git status --short
```

Expected: no whitespace errors, only intended Phase 0 files differ, and all unrelated existing changes remain untouched.

## Self-Review

- Spec coverage: Phase 0 library boundary, fixed buffers, fixed 96 kHz rate, 256-frame scheduling, independent params/resources/graphs, post-ViPER integration, failure isolation, bypass, latency, and telemetry each map to a task.
- Deferred-work scan: no unspecified implementation markers remain.
- Type consistency: `IemParams`, `IemEngineConfig`, `IemEngine`, `IemResources`, `IemGraph`, `IemGraphSlots`, `IemParameterMailbox`, and telemetry names are stable across producer and consumer tasks.
- Scope: no encoder, rotator, HRTF, room, UI, service, App protocol, or ViPERDSP internal change is included in Phase 0.
