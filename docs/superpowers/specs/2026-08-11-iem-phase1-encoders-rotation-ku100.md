# IEM Phase 1: Encoders, Scene Rotation, KU100, and App Controls

Date: 2026-08-11

Status: Approved design

## 1. Purpose

Phase 1 turns the Phase 0 IEM foundation into a usable end-to-end spatial audio path. It adds three encoders, manual scene rotation, first- through third-order KU100 binaural decoding, all upstream headphone compensation filters, output protection, parameter transport, persistence, telemetry, and a minimal MiuiX control surface.

The implementation remains a post-ViPER stereo effect:

```text
ViPER stereo output
  -> selected IEM encoder
  -> ACN/SN3D Ambisonics bus
  -> Scene Rotator
  -> KU100 Direct decoder
  -> optional headphone EQ
  -> latency-aligned wet/dry mix and gain
  -> stereo-linked lookahead limiter
  -> host stereo output
```

Room simulation, head tracking, OSC, RoomEncoder, SimpleDecoder, AllRADecoder, EnergyVisualizer, and MultiBandCompressor are not part of Phase 1.

The target device uses the HIDL/legacy AudioEffect path. AIDL effect transport and its mmap reader are not available in the current source workspaces and are not part of Phase 1.

## 2. Confirmed Product Decisions

- Delivery is end to end: native DSP, parameter protocol, persistence, telemetry, a main-screen card, and a dedicated control page.
- Delivery targets the HIDL/legacy `AudioEffect.setParameter` path. Existing AIDL shared-memory format version 6 remains unchanged.
- The runtime uses a fixed staged pipeline rather than a monolithic processor or three resident complete graphs.
- The internal convention is fixed to ACN/SN3D in Phase 1.
- Supported Ambisonics orders are 1, 2, and 3.
- Stereo input maps to two independent sources in MultiEncoder. No synthetic Mid/Side sources are created.
- GranularEncoder retains its complete audible upstream behavior, including Freeze and all modulation controls. Frozen audio is runtime-only and is not serialized.
- Scene Rotator exposes Yaw/Pitch/Roll, axis inversion, overall inversion, both upstream rotation sequences, and Reset.
- KU100 Direct includes Ord1-Ord3 IRs and all 23 upstream headphone compensation IRs.
- Latency choices are CPU/stability profiles named Low, Balanced, and Stable. The displayed actual latency is measured; the engine does not add silence merely to hit exactly 10, 20, or 40 ms.
- Output protection is a stereo-linked lookahead limiter with a default ceiling of -0.30 dBFS.
- The first-use StereoEncoder width is 60 degrees. MultiEncoder defaults to -30 degrees left and +30 degrees right.
- The first-use global state is StereoEncoder, order 3, Balanced latency, 100 percent Wet, 0 dB Output Gain, limiter On at -0.30 dBFS, and headphone EQ Off. IEM itself remains disabled until the user enables it.
- The App uses a compact main card plus a dedicated four-tab IEM editor.
- The editor footer reads `Powered by the IEM Plug-in Suite` and links to the official project. Full attribution remains in Open Source Licenses.

## 3. Upstream Pinning and Source Reuse

The reference source is:

- Repository: `https://git.iem.at/audioplugins/IEMPluginSuite.git`
- Commit: `39de1dd5883f1bd8d65fe1662487f2470a1d7b55`
- License: GPL-3.0

Phase 1 uses an algorithm-source plus thin-adapter strategy. It does not compile or emulate the upstream JUCE `PluginProcessor` classes.

### 3.1 Directly vendored code

Pure algorithm files such as the efficient spherical-harmonic evaluator and quaternion helpers may be vendored unchanged under an `IEMDSP/upstream/` subtree. Vendored files must retain their original copyright and license headers. A manifest records the upstream path, commit, local path, and whether the file is unchanged or patched.

### 3.2 Adapted code

JUCE-dependent algorithm code may be adapted behind project-local fixed-capacity types. Adaptations must remain visibly separate from unchanged upstream files and must state the upstream file and commit from which they were derived.

The adapter boundary replaces:

- `juce::AudioBuffer` with aligned planar spans;
- APVTS/raw parameter pointers with immutable `IemParams` snapshots;
- `juce::SmoothedValue` with a fixed local smoother;
- `juce::Random` with a pre-seeded real-time-safe PRNG;
- dynamic grain containers with a fixed 512-entry pool;
- JUCE convolution and WAV loading with prepared project-local resources.

OSC, MIDI, desktop state serialization, editor code, timers, file pickers, RMS analysis controls, and JUCE host negotiation are not ported.

### 3.3 Why the complete PluginProcessor is not wrapped

The upstream processors depend on APVTS, JUCE buffers, `dsp::Convolution`, timers, OSC/MIDI, and runtime resource loading. Recreating enough JUCE API to compile those classes would enlarge the audioserver binary, obscure allocation behavior, and cost more to maintain than the adapters. The adapters reuse the algorithms without importing the desktop plugin runtime.

## 4. Native Architecture

### 4.1 Pipeline and bus

`IemPipeline` owns and orders the processing stages. It does not implement encoder or decoder math itself.

The internal Ambisonics bus is:

- 96 kHz;
- planar float;
- ACN channel order;
- SN3D normalization;
- 4 channels at order 1;
- 9 channels at order 2;
- 16 channels at order 3;
- allocated at the maximum configured internal block size during `Prepare()`.

Inactive higher-order channels are cleared and are never passed as meaningful input to a lower-order decoder.

### 4.2 Encoder interface

All encoders implement one prepared interface with no ownership transfer in `Process()`:

```cpp
class IemEncoder {
public:
    virtual bool Prepare(const EncoderConfig& config) = 0;
    virtual void ApplyParams(const IemParams& params) noexcept = 0;
    virtual void Reset() noexcept = 0;
    virtual bool Process(
        const float* const stereo[2],
        float* const ambisonics[16],
        std::size_t frames
    ) noexcept = 0;
};
```

The concrete adapters are selected structurally; only one encoder processes audio in an active graph.

### 4.3 StereoEncoderAdapter

StereoEncoder uses the upstream center-direction and width model. The two channel directions are derived from center Azimuth/Elevation/Roll and Width, then encoded with the pinned spherical-harmonic evaluator.

Controls:

- Azimuth: -180.00 to +180.00 degrees, default 0;
- Elevation: -180.00 to +180.00 degrees, default 0;
- Roll: -180.00 to +180.00 degrees, default 0;
- Width: -360.00 to +360.00 degrees, first-use default +60;
- Sample-wise Panning: Off/On, default Off.

Block-wise mode smooths coefficient changes across the block. Sample-wise mode updates the interpolated direction and coefficients per sample, matching the upstream high-quality intent without reading mutable parameters per sample.

### 4.4 MultiEncoderAdapter

The host left and right channels become Source 0 and Source 1. Each source has:

- Azimuth: -180.00 to +180.00 degrees;
- Elevation: -180.00 to +180.00 degrees;
- Gain: -60.0 to +10.0 dB;
- Mute: Off/On.

Defaults are Source 0 at -30 degrees and Source 1 at +30 degrees, both at 0 degrees elevation, 0 dB, and unmuted. Upstream Solo, RMS analysis, peak visualization, input-count selection, and master-source rotation are omitted because the Android host has exactly two channels and the shared Scene Rotator supplies global rotation.

### 4.5 GranularEncoderAdapter

GranularEncoder keeps the upstream eight-second stereo circular buffer and 512-grain maximum. It uses a fixed pool and free list prepared outside the audio thread. Failure to obtain a free grain skips that grain and increments telemetry.

Controls and ranges follow upstream:

- Azimuth: -180.00 to +180.00 degrees, default 0;
- Elevation: -180.00 to +180.00 degrees, default 0;
- Shape: -10.0 to +10.0, default 0;
- Size: 0.00 to 360.00 degrees, default 180;
- Roll: -180.00 to +180.00 degrees, default 0;
- Width: -360.00 to +360.00 degrees, default 0;
- Delta Time: 0.001 to 2.000 seconds, default 0.005;
- Delta Time Modulation: 0.0 to 100.0 percent, default 0;
- Grain Length: 0.001 to 2.000 seconds, default 0.250;
- Grain Length Modulation: 0.0 to 100.0 percent, default 0;
- Read Position: 0 to 4.000 seconds, default 0;
- Position Modulation: 0 to 4.000 seconds, default 0.050;
- Pitch: -12.000 to +12.000 semitones, default 0;
- Pitch Modulation: 0 to 12.000 semitones, default 0;
- Window Attack: 0.0 to 50.0 percent, default 50;
- Attack Modulation: 0.0 to 100.0 percent, default 0;
- Window Decay: 0.0 to 50.0 percent, default 50;
- Decay Modulation: 0.0 to 100.0 percent, default 0;
- Mix: 0.0 to 100.0 percent, default 50;
- Source Probability: -1.00 to +1.00, default 0;
- Freeze: Off/On, runtime-only, default Off;
- Spatial Mode: 3D/2D, default 3D;
- Sample-wise Panning: Off/On, default Off.

The production seed is created on the control thread and stored in the prepared graph. Tests may supply a fixed seed. Random generation never calls the OS or allocates in `Process()`.

Turning Freeze on stops circular-buffer writes only after a valid history exists. Reset, discontinuity, graph rebuild, or audioserver restart clears the history and forces Freeze off. The App must not restore a stale On value.

### 4.6 SceneRotatorAdapter

Scene Rotator consumes and produces the same order and normalization. It reuses the pinned quaternion/rotation algorithm and preallocated matrices. A matrix is recomputed only when a rotation parameter generation changes.

Controls:

- Yaw/Pitch/Roll: -180.00 to +180.00 degrees;
- Invert Yaw, Invert Pitch, Invert Roll;
- Invert Quaternion/overall rotation;
- Sequence 0: Yaw -> Pitch -> Roll;
- Sequence 1: Roll -> Pitch -> Yaw, default;
- Reset command: atomically restores all three angles to zero without changing advanced toggles.

Phase 1 accepts manual Euler controls only. Quaternion sensor transport is reserved for Phase 2.

### 4.7 KU100 decoder

`PartitionedMatrixConvolver` is a project-local general `N -> 2` convolver using PFFFT. PFFFT becomes a shared CMake target used by ViPERDSP and IEMDSP; its implementation is compiled once per final binary. IEMDSP does not include or call ViPER effect classes.

The decoder selects exactly one prepared IR matrix:

| Order | Inputs | Source resource | Source format |
|---|---:|---|---|
| 1 | 4 | `irsOrd1.wav` | 44.1 kHz, 8 channels, 236 frames |
| 2 | 9 | `irsOrd2.wav` | 44.1 kHz, 18 channels, 236 frames |
| 3 | 16 | `irsOrd3.wav` | 44.1 kHz, 32 channels, 236 frames |

Pinned source SHA-256 values:

- Ord1: `baf2f8929e739550891cb936750cd6cd434b208d6fed004c3c613734f6c08132`
- Ord2: `f7bdb67f9afb718fbc2185350dd0ec6ef2b53a16c33af4861450a82697c26677`
- Ord3: `a5d585b9523dfda231b7429ee3e694820ddd1a0bdeba4ee25526fb6e1750b643`

The asset generator validates hash, PCM format, sample rate, channel count, and frame count, resamples deterministically to 96 kHz, and emits immutable float resources plus a machine-readable manifest. Decoder channel mapping is covered by per-input impulse tests.

Resource attribution must preserve the upstream references: Benjamin Bernschuetz's Neumann KU100 far-field HRIR/HRTF compilation and the magnitude-least-squares rendering work by Schoerkhuber, Zaunschirm, and Hoeldrich. These citations appear in the generated manifest and the App license surface.

### 4.8 Headphone compensation

All 23 upstream EQ WAV files are embedded. They are 48 kHz mono, 236 frames in the pinned source and are deterministically converted to 96 kHz. The manifest contains every source hash, the displayed model name, and the upstream attribution to Benjamin Bernschuetz.

The selectable models are:

```text
AKG K1000 Closed, AKG K1000 Open, AKG K141 MK2, AKG K240 DF,
AKG K240 MK2, AKG K271 MK2, AKG K271 Studio, AKG K601,
AKG K701, AKG K702, Audio-Technica ATH-M50, Beyerdynamic DT250,
Beyerdynamic DT770 Pro 250 Ohms, Beyerdynamic DT880,
Beyerdynamic DT990 Pro, Presonus HD7, Sennheiser HD430,
Sennheiser HD480, Sennheiser HD560 Ovation II,
Sennheiser HD565 Ovation, Sennheiser HD600, Sennheiser HD650,
Shure SRH940
```

Off is the default and bypasses the EQ stage at zero processing cost.

### 4.9 Wet/dry, output gain, and limiter

When IEM is enabled, the dry branch is delayed by the measured wet-path latency before mixing. Wet is equal-power mixed from 0 to 100 percent. Output gain follows the mix and ranges from -24.0 to +24.0 dB.

The limiter is stereo-linked and uses a fixed 1.0 ms lookahead at 96 kHz, instantaneous peak attack, and a 50 ms exponential release. Both channels share one gain envelope to preserve image direction. It is a sample-peak limiter with no automatic makeup gain. Ceiling ranges from -12.00 to 0.00 dBFS and defaults to -0.30 dBFS. Limiter latency is included in telemetry.

When IEM is disabled, the entire IEM pipeline is bypassed and adds no latency or steady-state CPU use.

## 5. Latency Profiles and Structural Changes

Profiles are user-facing CPU/stability goals:

| Profile | UI target | Intent |
|---|---:|---|
| Low | 10 ms | smallest practical partitions and queue waterline |
| Balanced | 20 ms | default compromise |
| Stable | 40 ms | larger partitions and waterline for constrained devices |

The selected profile chooses convolution partitioning and scheduler waterlines. Low, Balanced, and Stable must use monotonically nondecreasing partition sizes and waterlines, and their measured total IEM latency must not exceed 10, 20, and 40 ms respectively. Within those limits, the implementation selects the smallest settings that satisfy the p99 CPU gate on the target device. The selected constants are fixed in source and covered by latency tests; they are not tuned dynamically on the audio thread. Actual algorithmic latency is measured from prepared stage delays and reported, and no delay is inserted solely to make the number equal the UI target.

Structural fields are Encoder Mode, Order, Headphone EQ, and Latency Profile. A structural change prepares one pending graph on the control thread. Rapid changes coalesce to the newest desired snapshot; there is never an unbounded graph-build queue.

Mode, Order, and EQ changes with equal latency use an equal-power graph crossfade. A latency-profile change uses a short fade-out, graph switch and prefill, then fade-in; two streams with different time origins are not mixed directly.

All other fields are dynamic and arrive through the Phase 0 IEM mailbox. They apply on an internal block boundary and use stage-local smoothing where discontinuities would click.

## 6. Parameter Protocol

Existing Phase 0 IDs remain unchanged:

| ID | Meaning | Wire value |
|---|---|---|
| `0x12000` | Enable | 0/1 |
| `0x12001` | Wet | integer percent, 0-100 |
| `0x12002` | Output Gain | 0.1 dB, -240 to +240 |
| `0x12003` | Order | 1-3 |
| `0x12100` | Resource Reset | command value 1 |

New IDs are fixed as follows:

| ID | Meaning | Wire value |
|---|---|---|
| `0x12004` | Encoder Mode | 0 Stereo, 1 Multi, 2 Granular |
| `0x12005` | Latency Profile | 0 Low, 1 Balanced, 2 Stable |
| `0x12006` | Limiter Enable | 0/1 |
| `0x12007` | Limiter Ceiling | 0.01 dB, -1200 to 0 |
| `0x12010` | Stereo Azimuth | 0.01 degree, -18000 to 18000 |
| `0x12011` | Stereo Elevation | 0.01 degree, -18000 to 18000 |
| `0x12012` | Stereo Roll | 0.01 degree, -18000 to 18000 |
| `0x12013` | Stereo Width | 0.01 degree, -36000 to 36000 |
| `0x12014` | Stereo Sample-wise Panning | 0/1 |
| `0x12020` | Multi Source Azimuth | `val1` source 0/1, `val2` 0.01 degree |
| `0x12021` | Multi Source Elevation | `val1` source 0/1, `val2` 0.01 degree |
| `0x12022` | Multi Source Gain | `val1` source 0/1, `val2` 0.1 dB, -600 to 100 |
| `0x12023` | Multi Source Mute | `val1` source 0/1, `val2` 0/1 |
| `0x12030` | Granular Azimuth | 0.01 degree, -18000 to 18000 |
| `0x12031` | Granular Elevation | 0.01 degree, -18000 to 18000 |
| `0x12032` | Granular Shape | 0.1 unit, -100 to 100 |
| `0x12033` | Granular Size | 0.01 degree, 0 to 36000 |
| `0x12034` | Granular Roll | 0.01 degree, -18000 to 18000 |
| `0x12035` | Granular Width | 0.01 degree, -36000 to 36000 |
| `0x12036` | Granular Delta Time | microseconds, 1000 to 2000000 |
| `0x12037` | Delta Time Modulation | 0.1 percent, 0 to 1000 |
| `0x12038` | Granular Grain Length | microseconds, 1000 to 2000000 |
| `0x12039` | Grain Length Modulation | 0.1 percent, 0 to 1000 |
| `0x1203A` | Granular Read Position | microseconds, 0 to 4000000 |
| `0x1203B` | Position Modulation | microseconds, 0 to 4000000 |
| `0x1203C` | Granular Pitch | 0.001 semitone, -12000 to 12000 |
| `0x1203D` | Pitch Modulation | 0.001 semitone, 0 to 12000 |
| `0x1203E` | Window Attack | 0.1 percent, 0 to 500 |
| `0x1203F` | Attack Modulation | 0.1 percent, 0 to 1000 |
| `0x12040` | Window Decay | 0.1 percent, 0 to 500 |
| `0x12041` | Decay Modulation | 0.1 percent, 0 to 1000 |
| `0x12042` | Granular Mix | 0.1 percent, 0 to 1000 |
| `0x12043` | Source Probability | 0.01 unit, -100 to 100 |
| `0x12044` | Granular Spatial Mode | 0 3D, 1 2D |
| `0x12045` | Granular Sample-wise Panning | 0/1 |
| `0x12050` | Rotation Yaw | 0.01 degree, -18000 to 18000 |
| `0x12051` | Rotation Pitch | 0.01 degree, -18000 to 18000 |
| `0x12052` | Rotation Roll | 0.01 degree, -18000 to 18000 |
| `0x12053` | Invert Yaw | 0/1 |
| `0x12054` | Invert Pitch | 0/1 |
| `0x12055` | Invert Roll | 0/1 |
| `0x12056` | Invert Overall Rotation | 0/1 |
| `0x12057` | Rotation Sequence | 0 Yaw-Pitch-Roll, 1 Roll-Pitch-Yaw |
| `0x12060` | Headphone EQ | -1 Off, 0-22 in the manifest order listed in section 4.8 |
| `0x12101` | Reset Rotation | command value 1 |
| `0x12102` | Granular Freeze | command value 0/1 |
| `0x12103` | Reset IEM Runtime State | command value 1 |

Wire scaling is explicit:

- angles: hundredths of a degree;
- ordinary gain: tenths of a dB, preserving the Phase 0 Output Gain contract;
- limiter ceiling: hundredths of a dB;
- granular seconds: integer microseconds;
- granular pitch: thousandths of a semitone;
- percentages requiring upstream 0.1 precision: tenths of a percent;
- Source Probability: hundredths;
- enums and booleans: integer values defined by the protocol table.

Multi array parameters use `val1 = source index` and `val2 = raw value`, matching the existing indexed-band transport. Only indices 0 and 1 are valid.

Reset Rotation, Reset IEM, Freeze transitions, and Resource Reset have dedicated command semantics. An invalid enum, source index, or command value is rejected. Continuous values are clamped in both App normalization and the driver parser, with the driver remaining authoritative. IDs not listed above remain reserved and must not be interpreted by Phase 1.

## 7. State, Persistence, and Dispatch

The App adds `IemState` as a member of `EffectState`, with nested state for General, Stereo, Multi, Granular, Rotation, Decoder, and Output. All user parameters except Freeze are stored in device settings and preset JSON.

The native `IemParams` remains separate from `ViPERParams`. The HIDL/legacy AudioEffect path sends the scalar and indexed IDs from section 6. `ViperDispatcher.dispatchState()` sends every persistent IEM field when attaching the service or restoring a preset, and `EffectStateStore` sends one incremental ID for an ordinary edit. Command IDs are never persisted or included in a full-state restore.

The existing AIDL `ConfigChannel`, `ViperParamsSerializer`, shared-memory slot size, and `FORMAT_VERSION = 6` are not modified. When the App is operating in AIDL mode, the Phase 1 IEM card is not shown because the corresponding native reader is outside this phase.

On service attach or state restore:

1. App normalizes persisted values.
2. Freeze is forced Off.
3. App publishes one complete snapshot.
4. The control side computes structural differences and prepares a graph if needed.
5. The audio thread consumes only complete mailbox generations.

Full-state and incremental dispatch must converge on field-equivalent native `IemParams` snapshots.

## 8. App UX

### 8.1 Main card

Add an `IEM Spatial Audio` `ViperEffectCard` after Convolver and before the legacy spatial effects. The header contains its own Enable switch and a one-line summary:

```text
Stereo | 3rd order | KU100
```

Expanded content contains only:

- Encoder Mode dropdown;
- Order segmented selector;
- Wet slider;
- icon plus text command to open the IEM editor.

Changing Mode, Order, EQ, or Latency dispatches the structural value only after a completed selection/gesture. Dynamic sliders may dispatch intermediate values using the existing `last` convention.

### 8.2 Dedicated editor

Extend `EffectEditorActivity` with `EditorKind.IEM`; do not introduce another navigation host. Use `ViperScaffold`, `ViperTopBar`, and existing MiuiX wrappers.

The editor has four tabs:

- Encoder;
- Rotation;
- Decoder;
- Output.

The Encoder tab switches among Stereo, Multi, and Granular content. Multi uses an L/R segmented selector. Granular uses unframed Spatial, Timing, Pitch, Window, and Mix sections. It does not nest cards.

Rotation provides three primary angle sliders, an explicit Reset icon button, and an Advanced section for inversion and sequence.

Decoder shows KU100 Direct, the effective order, headphone EQ Off/model selection, and inline resource errors.

Output contains Wet, Output Gain, latency profile, limiter toggle and ceiling. It also shows read-only actual latency, active grains, queue underflow/overflow, and limiter gain reduction.

No 3D scene, response graph, room editor, OSC control, or head-tracking page is added in Phase 1.

### 8.3 Attribution

At the bottom of the editor scroll content, show a low-emphasis clickable line:

```text
Powered by the IEM Plug-in Suite
```

It opens the official IEM Plug-in Suite project page at `https://plugins.iem.at`. The App Open Source Licenses/About surface separately includes the project URL, pinned commit, GPL-3.0 license text, resource provenance, and local modification notice.

Visible strings are added to base English, simplified Chinese, and the existing Russian locale. Product/model names remain as published.

## 9. Telemetry

Extend IEM telemetry without changing parameters by polling in reverse. Required fields are:

- enabled/active state;
- active Encoder Mode and Order;
- graph generation;
- measured total IEM latency in frames and milliseconds;
- active grain count and grain-pool exhaustion count;
- input/output queue underflow and overflow counts;
- graph/resource fault code;
- limiter gain reduction;
- last structural preparation result.

Telemetry is single-writer/single-reader and read consistently by generation as in Phase 0. UI telemetry is informational and never overwrites `IemState`.

## 10. Error and Transition Semantics

- Pending graph preparation failure keeps the current active graph and reports an error.
- Failure before any valid graph leaves post-ViPER dry audio untouched.
- NaN/Inf generated by the IEM path trips a fault, resets IEM state, and returns to dry; it never contaminates the host buffer.
- Grain-pool exhaustion drops only the new grain.
- Scheduler underflow zero-fills only the missing wet samples and increments telemetry. A single underflow does not permanently disable IEM.
- Resource metadata or hash mismatch is a build failure. Runtime resource lookup cannot silently choose another order or EQ.
- Reset and discontinuity clear convolution history, granular history, smoothers, queue state, and limiter delay.
- Important operations have explicit controls. None rely on long press.

## 11. Real-Time and Resource Bounds

The audio thread must perform no heap allocation, lock acquisition, file I/O, logging, plan creation, resource conversion, or OS random calls.

All fixed-capacity storage is prepared before publication. The eight-second stereo granular history consumes about 6.1 MiB at 96 kHz. The design must report the prepared graph memory estimate in tests and account for the transient active plus pending/previous graph overlap during transitions.

The largest normal configuration is Ord3 Granular + KU100 + headphone EQ + limiter. Release benchmarks on the target arm64 device require p99 processing time below 50 percent of the corresponding callback period. A benchmark failure blocks release even if average time passes.

## 12. Verification

### 12.1 Algorithm tests

- Golden spherical-harmonic coefficients for cardinal and arbitrary directions at orders 1-3.
- Stereo output against pinned-upstream fixtures, including width and rotated center.
- Multi linear superposition, per-source gain/mute, and default -30/+30 mapping.
- Granular fixed-seed output fixtures, every modulation extreme, Freeze transition, circular-buffer wrap, 512-grain exhaustion, reset, and both panning modes.
- Scene Rotator identity, known rotations, inverse, both sequences, and energy preservation per order.

Golden fixture metadata records upstream commit, parameters, sample rate, input hash, and generator version. The normal test build does not require JUCE.

### 12.2 Convolution and output tests

- Per-input impulses reproduce each KU100 left/right IR channel mapping.
- Partitioned output matches a direct time-domain reference within a documented tolerance.
- Random callback partitioning produces the same stream as contiguous processing.
- Low/Balanced/Stable profiles are output-equivalent apart from latency.
- Every headphone EQ impulse matches its converted resource.
- Wet 0/100, dry alignment, output gain, limiter stereo linking, ceiling, release, silence, and non-finite input.

### 12.3 Pipeline and protocol tests

- Encoder Mode, Order, EQ, and latency rebuild/coalescing.
- Equal-latency crossfade and latency-profile fade-through transition.
- Enable/bypass, reset/discontinuity, 8-384 kHz host rates, and random callback sizes from 8 to the configured maximum.
- Every parameter ID, range, scaling, enum, command, and indexed source.
- Preference and preset round trip; Freeze exclusion.
- Full-state and incremental dispatch equivalence.
- A UI policy test proves the IEM card is shown on the HIDL path and omitted in AIDL mode.
- Telemetry consistency and fault reporting.

### 12.4 Real-time and build gates

- Callback allocation/lock instrumentation reports zero violations.
- Host CTest passes in normal, UBSan, and ASan builds.
- Android Release builds pass for arm64-v8a and armeabi-v7a.
- App unit tests, lint, and `assembleDebug` pass.
- The final shared library has no JUCE dependency or unresolved duplicate PFFFT symbols.
- Asset conversion is byte-for-byte reproducible from the pinned sources.
- Device smoke tests cover music, voice, enable/disable, all modes and orders, EQ switching, lock screen, output-device change, audioserver restart, and 30 minutes of continuous playback.

## 13. Completion Criteria

Phase 1 is complete only when:

1. Stereo, Multi, and full Granular encoders produce validated Ord1-Ord3 ACN/SN3D buses.
2. Manual Scene Rotator and KU100 Ord1-Ord3 decoding pass upstream golden and impulse tests.
3. All 23 headphone EQ choices are reproducibly embedded and selectable.
4. Wet/dry alignment, latency profiles, output gain, and the linked limiter meet tests and device performance gates.
5. The HIDL/legacy AudioEffect path restores and incrementally updates the same IEM state, while AIDL mode does not expose unsupported controls.
6. The main card and dedicated MiuiX editor expose every Phase 1 control without hidden important actions.
7. Attribution and GPL obligations are present in both the editor footer and license surface.
8. Disabled IEM preserves the existing ViPER output and adds no steady-state latency or CPU work.
