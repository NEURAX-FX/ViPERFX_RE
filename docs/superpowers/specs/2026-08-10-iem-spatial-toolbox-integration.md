# IEM Spatial Toolbox Integration Design

## 1. Purpose

Integrate the audio-DSP portion of IEM Plugin Suite 1.15.0 into the existing Android vendor audio effect as an independent `IEMDSP` engine while preserving `ViPERDSP` as-is.

The delivered vendor binary remains `libv4a_re.so`, but its runtime pipeline becomes:

```text
Stereo input
  -> ViPERDSP
  -> IEMDSP input resampler (fixed internal 96 kHz)
  -> Ambisonics encoder (1st/2nd/3rd order: 4/9/16 channels)
  -> fixed spatial processing rack
  -> binaural or custom virtual-speaker decoder
  -> IEM output protection and output resampler
  -> Stereo output
```

`ViPERDSP` and `IEMDSP` have independent parameters, resources, graph publication, telemetry, reset, bypass, and failure handling. IEM failures must never disable or corrupt ViPER processing.

## 2. Source Baseline

The design references IEM Plugin Suite 1.15.0 from:

- Repository: `https://git.iem.at/audioplugins/IEMPluginSuite/`
- License in upstream repository: GPLv3
- JUCE baseline: 8.0.7
- Supported upstream Ambisonics order: up to seventh order
- Relevant bundled resources: KU100 binaural decoder IRs Ord1 through Ord5 and headphone compensation EQ impulses

Licensing resolution is explicitly deferred for the private prototype, as requested. The implementation must nevertheless remain isolated under `IEMDSP/`, preserve upstream copyright and attribution files, and avoid mixing imported files into `ViPERDSP/`. Distribution must not proceed without resolving the GPLv3 and existing ViPERDSP rights boundary.

## 3. Scope

### 3.1 Included Audio DSP

The Android toolbox includes audio processing and corresponding control views for:

- StereoEncoder, MultiEncoder, and GranularEncoder modes
- SceneRotator
- RoomEncoder
- FdnReverb
- DualDelay
- DirectivityShaper
- DirectionalCompressor
- OmniCompressor
- MultibandCompressor
- MultiEQ
- DistanceCompensator
- MatrixMultiplier
- SimpleDecoder and AllRADecoder matrix generation
- ProbeDecoder
- BinauralDecoder

### 3.2 App-Only Tools

- CoordinateConverter becomes an App coordinate utility.
- EnergyVisualizer becomes an App visualization fed by bounded driver telemetry.
- ToolBox convention and normalization controls become engine settings rather than a separate audio module.

### 3.3 Excluded Host Features

- JUCE AudioProcessor wrappers
- VST/VST3/LV2/AU/AAX and standalone host code
- OSC and MIDI control
- DAW bus negotiation and arbitrary host channel routing
- Upstream editors and desktop OpenGL UI

## 4. Repository And Build Architecture

Add a new top-level `IEMDSP/` C++17 static library and link it into the existing vendor effect target.

```text
ViPERFX_RE/
  IEMDSP/
    include/iem/
    src/core/
    src/encoders/
    src/spatial/
    src/dynamics/
    src/decoders/
    src/resources/
    third_party/
    tests/
  ViPERDSP/                  # unchanged internals
  src/
    IemContext.*
    IemGraph.*
    IemGraphSlots.*
    IemParameterMailbox.*
    IemResources.*
```

JUCE runtime types are replaced at the IEM boundary:

| Upstream type | Android implementation |
|---|---|
| `juce::AudioBuffer<float>` | aligned fixed-capacity planar float buffers |
| APVTS parameters | immutable `IemParams` plus scalar mailbox |
| `juce::SmoothedValue` | preallocated linear/exponential ramps |
| JUCE/FFTW convolution | existing PFFFT infrastructure and partitioned convolution |
| JUCE IIR filters | project-local Biquad/filter primitives |
| Timer/OSC/MIDI callbacks | removed |

No IEM module may allocate, lock, log, parse files, build matrices, or destroy resources on the audio thread.

## 5. Internal Audio Format

- Maximum order: third order.
- Internal channels: 4, 9, or 16 using ACN channel order and SN3D normalization by default.
- Internal sample rate: fixed 96 kHz.
- Internal processing quantum: 256 frames.
- Storage: planar aligned buffers allocated for the maximum 16 channels.
- Host rates: 8 kHz through 384 kHz.
- Boundary resampling: stateful band-limited polyphase resamplers with preallocated history.
- Dry/wet: dry samples are delayed by the exact IEM pipeline latency before mixing.
- Disabled IEM: zero-IEM-latency direct path after ViPERDSP.

The engine exposes measured total latency in host frames for diagnostics. Enable, disable, decoder replacement, and graph publication transitions use bounded crossfades to prevent discontinuities.

## 6. Fixed Spatial Rack

The processing order is fixed and not user-reorderable:

```text
Input Adapter
  -> selected Encoder
  -> Scene Rotator
  -> Directivity Shaper
  -> Room Encoder / early reflections
  -> MultiEQ
  -> Directional / Omni / Multiband dynamics
  -> Distance Compensation
  -> Dual Delay
  -> FDN Reverb
  -> selected Decoder
  -> Output gain and protection limiter
```

Every module has an explicit enable state and reset method. Alternative encoders and decoders are exclusive selections, not simultaneously active modules.

## 7. Decoder Modes

### 7.1 IEM KU100 Direct

- Embed the complete upstream Ord1 through Ord5 KU100 impulse set and headphone compensation EQ resources.
- Current runtime selects Ord1 through Ord3 according to the active engine order.
- Ord4 and Ord5 remain packaged for future higher-order support.
- Resource decoding, 96 kHz resampling, partitioning, and FFT preparation happen on the control thread.

### 7.2 Custom Virtual Speakers

- Maximum 16 enabled nodes.
- Each node stores stable ID, azimuth, elevation, distance, linear gain, mute, and enable state.
- Effective order is limited by enabled node count:
  - fewer than 4 nodes: custom decoder unavailable
  - 4 to 8 nodes: first order
  - 9 to 15 nodes: second order
  - 16 nodes: third order
- AllRAD/SimpleDecoder matrices are built off-thread and published as immutable resources.
- During graph construction, each node direction selects or interpolates direction-specific left/right HRIRs from the selected SOFA-derived resource. Spherical-neighborhood interpolation is preferred; nearest-direction fallback is permitted only when the SOFA sampling grid cannot form a valid neighborhood and must be reported in diagnostics.
- Node outputs are rendered through the prepared HRIRs and summed to stereo.
- HRTF rendering uses 256-frame uniform partitioned convolution and shares each node's input spectrum between its left and right filters.
- Without a valid SOFA-derived resource, Custom Virtual Speakers is disabled with an explicit reason. It never silently falls back to panning or KU100 Direct.

## 8. SOFA And HRTF Resources

SOFA parsing is not performed inside `audioserver`.

The App adds a small native SOFA preprocessing library, using a dedicated parser such as libmysofa, to convert a selected SOFA file into an engine-specific immutable `.ihr` package.

`.ihr` contains:

- magic and format version
- coordinate convention and units
- original and target sample rates
- direction count and HRIR frame count
- direction vectors
- interleaved left/right HRIR samples
- source metadata hash
- payload CRC

The App uploads `.ihr` through a dedicated IEM bulk channel. The driver validates all dimensions, finite values, upper bounds, version, and CRC before constructing replacement HRTF resources. Invalid replacement data leaves the currently published HRTF and graph active.

Direction interpolation, target-rate conversion, HRIR windowing, partitioning, and FFT preparation happen during replacement graph construction, never during audio processing.

Resource management provides visible import, replace, inspect, and delete operations. Important operations never depend on long press.

## 9. Parameter And Resource Publication

Reserve scalar parameter IDs `0x12000` through `0x12FFF` for IEM.

Add root-driver types independent of `ViPERDSP`:

- `IemParams`
- `IemParameterSnapshot`
- `IemParameterMailbox`
- `IemResources`
- `IemGraph`
- `IemGraphSlots`

Scalar changes use a three-slot immutable mailbox. Topology and resource changes build a replacement `IemGraph` in the inactive slot, then switch only at a host audio-buffer boundary. The control thread destroys retired graphs only after audio-thread acknowledgement.

Parameter groups:

- Engine: enable, order, wet, output gain, limiter, convention, normalization
- Encoder: selected mode, width, azimuth, elevation, source positions
- Rotator: quaternion, manual yaw/pitch/roll offsets, inversion, lock, reset
- Room: dimensions, source/listener position, wall coefficients, reflection order
- Spatial rack: Directivity, EQ, dynamics, delay, FDN, distance parameters
- Decoder: mode, KU100 order, layout ID, SOFA resource ID
- Layout: up to 16 immutable node records

HIDL-style scalar controls use the reserved IDs and chunked bulk commands. AIDL/shared-memory transport uses a separate IEM state region so `ViPERParams` and ViPERDSP layout remain unchanged.

## 10. Head Tracking

The existing foreground `ViperService` owns the sensor in the background. Tracking must not depend on an Activity remaining visible.

- Register Android rotation-vector/game-rotation sensor only when IEM and tracking are both enabled.
- Publish timestamped orientation at 60 Hz.
- Convert Android coordinates to the IEM right-handed coordinate convention in the service.
- Store calibration as a reference quaternion; publish normalized relative quaternions.
- Interpolate orientation inside each audio buffer.
- Persist tracking enable and calibration policy; re-register after service reconstruction.
- Unregister immediately when IEM, tracking, driver, or service is disabled.
- Show an ongoing notification with a visible pause action.
- Do not hold a permanent WakeLock initially. Add an opt-in tracking WakeLock only if device testing proves that the vendor freezes sensors during screen-off playback.
- If samples are stale for 250 ms, freeze the last valid pose and warn. If stale for 2 seconds, switch to manual orientation without disabling the IEM engine.

## 11. App UX

Keep all existing ViPER spatial effects unchanged.

Add an independent `IEM Spatial Engine` card in the Spatial category with:

- enable switch
- summary: active order, decoder, tracking state
- quick wet, gain, order, and decoder controls
- explicit `Open IEM Spatial Workbench` action

The workbench contains:

1. Overview and Music/Cinema/Game/Room state presets
2. Encoder and Scene Rotator
3. Room, Early Reflections, DualDelay, and FDN
4. Directivity, MultiEQ, and dynamics
5. Decoder and 16-node virtual-speaker editor
6. KU100 and SOFA/HRTF resource management
7. Energy direction, node level, peak, latency, resampler, and process-time diagnostics

The virtual-speaker editor provides top and elevation views plus an accessible list. Add, edit, duplicate, enable, mute, and delete are explicit visible actions.

## 12. Realtime Safety And Failure Handling

- Invalid SOFA, CRC, matrix, room, or filter replacement preserves the current valid graph.
- NaN/Inf clears the failing IEM module state and bypasses IEM while ViPER continues.
- Missing sensors switch to manual pose with visible status.
- Insufficient virtual nodes clamp effective order and report the reason.
- Deadline misses increment telemetry and warn the App.
- Repeated deadline misses bypass IEM rather than silently changing user order or quality.
- IEM bypass and recovery use a bounded crossfade.
- Reset, rate change, decoder change, and resource replacement clear stale histories off-thread or at a safe graph boundary.

## 13. Verification

### Mathematical And Golden Tests

- Generate 96 kHz reference impulses and sweeps from upstream IEMPluginSuite.
- Compare Encoder, Rotator, Room, FDN, delay, filter, dynamics, and Decoder outputs with defined tolerances.
- Validate ACN/SN3D channel ordering and normalization at every order.
- Validate quaternion transforms, calibration, inversion, and sensor coordinate conversion.

### Resource Tests

- Built-in Ord1 through Ord5 KU100 integrity and metadata.
- Valid, truncated, oversized, non-finite, wrong-coordinate, duplicate-direction, and CRC-invalid `.ihr` packages.
- AllRAD/SimpleDecoder singular and underdetermined layouts.
- Replacement failure preserving the active resource and graph.

### Realtime And Performance Tests

- Host rates from 8 through 384 kHz.
- Orders 1/2/3 and layouts with 4/9/16 nodes.
- Zero allocations, locks, file I/O, and destruction after setup.
- Resampler response, latency, dry alignment, and crossfade continuity.
- ARM64 processing average and maximum against callback budget.
- Screen-off tracking, service restart, AudioFlinger restart, stale sensor, and IEM-only bypass.

## 14. Delivery Phases

Each phase must build, test, and remain independently bypassable.

### Phase 0: Portable Foundation

- `IEMDSP` library target
- fixed 16-channel planar buffers
- 96 kHz boundary resampler and block scheduler
- independent params, resources, graph slots, bypass, telemetry
- no audible spatial algorithm yet

### Phase 1: Core Ambisonics And KU100

- Stereo/Multi/Granular encoders
- Scene Rotator with manual pose
- KU100 Direct Ord1 through Ord3
- wet, latency alignment, output gain, limiter

### Phase 2: Background Head Tracking

- service sensor ownership
- calibration and coordinate conversion
- quaternion publication and interpolation
- screen-off and service-restart behavior

### Phase 3: SOFA And Custom Virtual Speakers

- SOFA preprocessor and `.ihr`
- App resource management
- AllRAD/SimpleDecoder
- 16-node editor and HRTF renderer

### Phase 4: Room And Time Effects

- RoomEncoder
- DistanceCompensator
- DualDelay
- FdnReverb

### Phase 5: Directional Processing And Dynamics

- DirectivityShaper
- MultiEQ
- Directional, Omni, and Multiband compressors
- MatrixMultiplier and ProbeDecoder

### Phase 6: Complete Workbench And Diagnostics

- presets
- energy visualization
- process-time and resource diagnostics
- localization, accessibility, and final device validation

## 15. Non-Goals

- Changing ViPERDSP internals
- Replacing current ViPER effects
- Supporting seventh-order runtime in the first Android implementation
- Producing multichannel hardware output from the stereo vendor effect
- Shipping OSC, MIDI, DAW routing, or desktop plugin UI
- Silently approximating custom HRTF rendering when no SOFA resource exists
