# Halo Encoder and IEM Render Modes

Date: 2026-08-13

Status: Approved for implementation

## 1. Purpose

This phase extends the existing HIDL-only IEM spatial path with two product changes:

1. Halo becomes a fourth mutually exclusive encoder. It reconstructs a 7.0 surround bed from stereo using the documented NUGEN Halo Upmix 1.7.1.5 dialog-extraction, residual-surround, and time-domain diffusion stages, then encodes that bed into the existing ACN/SN3D bus.
2. KU100 becomes one of three render modes: Off, Simple, and KU100. The default remains KU100 so existing Stereo, Multi, and Granular users keep the current headphone path.

The implementation remains a post-ViPER stereo effect. AIDL transport, RoomEncoder, SimpleDecoder, AllRADecoder, EnergyVisualizer, MultiBandCompressor, head tracking, OSC, LFE, Halo's own final downmix, and overhead/top beds are out of scope.

```text
ViPER stereo output
  -> selected IEM encoder
       Stereo | Multi | Granular | Halo
  -> render-mode branch
       Off:    encoder-native stereo fold, no Ambisonics decode
       Simple: ACN/SN3D bus -> Scene Rotator -> virtual-speaker fold
       KU100:  ACN/SN3D bus -> Scene Rotator -> KU100 Direct -> optional headphone EQ
  -> latency-aligned wet/dry mix and gain
  -> stereo-linked lookahead limiter
  -> host stereo output
```

## 2. Confirmed Product Decisions

- Halo is encoder mode `3`, mutually exclusive with Stereo, Multi, and Granular. Only one encoder processes audio.
- Halo is not a second independent App card and not a replacement for the existing IEM encoders.
- First-phase Halo reconstructs the documented 7.0 bed `L, R, C, Ls, Rs, Lsr, Rsr`. It does not synthesize LFE, does not run Halo's `0x18006972c` stereo/downmix fold, and does not create `Lts/Rts/Trl/Trr`.
- The first-phase Halo control surface is the documented core dialog, fade, diffusion, space, back-boost, and rear-shelf set. DE Ramp/Gate/Stability, C LF Split, vertical/overhead controls, monitor/solo/mute, and host-format parameters are deferred.
- `dialog.net` is embedded as a prepared resource using the same build-time hash-and-emit pattern already used for KU100 and headphone EQ. No runtime file I/O occurs.
- Render Mode is a structural parameter: `0 Off`, `1 Simple`, `2 KU100`. Default is `2 KU100`.
- Headphone EQ remains available only in KU100 mode. Off and Simple skip both KU100 convolution and headphone-EQ convolution.
- Existing Phase 1 IDs, persistence, telemetry, and the HIDL-only App card policy remain unchanged unless this document names a new field.
- Disabled IEM still adds no latency or steady-state CPU.

## 3. Source and Attribution

Halo algorithm authority is the clean-room reconstruction in:

- `/root/HaloMixRE/docs/reverse-engineering/halo-upmix-dsp.md`
- `/root/HaloMixRE/docs/reverse-engineering/halo-upmix-dsp.zh.md`

Pinned model:

- Source notes: HaloMixRE reverse-engineering documents named above
- Vendored resource path in this repo: `IEMDSP/resources/source/halo/dialog.net`
- Format: `FANN_FLO_2.1`, `layer_sizes=38 11 2`, 391 serialized connections
- SHA-256: `652cbd597b9afbd82eb9b39fe80e3e825a381e448c3c2a269c07842f88eb5b72`

The implementation is a project-local reconstruction from those documented equations. It does not vendor, link, or execute the original Halo binary, VST3, or installer payload.

IEM attribution from Phase 1 remains unchanged: IEM Plug-in Suite commit `39de1dd5883f1bd8d65fe1662487f2470a1d7b55`, GPL-3.0-or-later, KU100 and headphone-EQ citations, and the existing App license surface. The Halo encoder does not add a second "Powered by" footer. Settings/About continues to host IEM license text; Halo is documented as a local reconstruction of published reverse-engineering notes, not as an IEM Plug-in Suite component.

## 4. Native Architecture

### 4.1 Encoder selection

`IemPipeline` continues to own stage order. `EncoderMode` gains `HALO = 3`. The encoder variant becomes:

```cpp
std::variant<std::monostate, StereoEncoder, MultiEncoder, GranularEncoder, HaloEncoder>
```

`IemEncoder::Process()` remains the only audio-thread encoder entry. Halo must satisfy the same no-allocation, no-lock, no-I/O contract as Granular.

### 4.2 HaloEncoder

HaloEncoder is a two-domain adapter:

```text
host stereo
  -> resample/block into the existing 96 kHz IEM graph
  -> STFT: 1024-point FFT, hop 512, Hann window, 1024-sample reported latency
  -> per-bin dialog.net inference
  -> model-driven centre/dialog extraction
  -> residual phase-coherence surround assignment
  -> iFFT / overlap-add to 7 planar bed channels
  -> time-domain diffusion / decorrelation / rear-shelf
  -> 7.0 planar bed output
```

Internal working layout is fixed and does not use Halo's 17-slot host-format mapper:

| Slot | Bed | Azimuth | Elevation |
|---|---|---:|---:|
| 0 | L | -30.00 | 0.00 |
| 1 | R | +30.00 | 0.00 |
| 2 | C | 0.00 | 0.00 |
| 3 | Ls | -90.00 | 0.00 |
| 4 | Rs | +90.00 | 0.00 |
| 5 | Lsr | -135.00 | 0.00 |
| 6 | Rsr | +135.00 | 0.00 |

These angles are compile-time constants. They are not user-editable in this phase.

All STFT, overlap-add, delay-line, IIR, and FANN working buffers are allocated in `Prepare()`. `Process()` may only write into those prepared buffers. FFT work uses the existing shared PFFFT target; HaloEncoder must not introduce a second FFT implementation.

### 4.3 Frequency-domain front end

The STFT/iFFT pair follows the reconstructed Halo front end:

- 1024-point real FFT, hop 512, 50 percent overlap, 513 complex bins;
- symmetric Hann window `w[n] = 0.5 - 0.5*cos(2*pi*n/1024)` for `0 <= n < 1024`, applied after the inverse FFT;
- reported end-to-end encoder latency 1536 samples at the internal 96 kHz rate: the 1024-sample STFT window plus one 512-sample feature/output hop;
- six-frame L/R spectral history with the documented left-rotating permutation;
- seven complex planes after extraction, locally ordered `[L, R, Ls, Rs, Lsr, Rsr, C]`;
- per-bin 37-feature vector plus the documented FANN bias node;
- exact `FANN_SIGMOID_SYMMETRIC_STEPWISE` hidden activation and `FANN_LINEAR_PIECE_SYMMETRIC` output activation from the reconstruction notes.

Dialog extraction uses the documented transforms:

```text
DialogExtractionAggression' = min(2 * DialogExtractionAggression, 1)
DialogIsolate'              = DialogIsolate * DialogExtractionAggression'
```

Attack, Release, and Mix In feed the documented per-bin envelope and frequency-smoothed mask. Residual L/R then receive the documented triangular phase-coherence coordinate, energy smoother, transient countdown, eight-item alpha averager, and front/side/rear gain split. `BackBoost` changes only the rear assignment, matching the reconstruction.

`DE_RampS`, `DE_Stability`, and the three `DD Gate` parameters are not exposed. Their documented default numeric behavior is used internally where the reconstruction requires a value to keep the published equations well-defined. Those internals are not persisted as user state.

### 4.4 Time-domain diffusion and rear shelf

After overlap-add, HaloEncoder runs the documented 7.0 time-domain diffusion/upmix helper and the four-branch decorrelation/rear-shelf helper. It does not call:

- LFE synthesis `0x180067544`;
- multi-input fade/routing `0x18006799c` beyond the already-applied residual fade gains;
- centre LF divergence `0x180069318`;
- vertical/overhead matrix `0x180068548`;
- final stereo fold `0x18006972c`.

`Diffusion` uses the documented piecewise mapping:

```text
diff = 0                             if Diffusion == 0
     = 1                             if Diffusion == 1
     = db_to_lin(30 * Diffusion - 30) otherwise
```

`Space` (`UpmixPsdWidth`) sets the two documented integer delay taps:

```text
space_delay_a = trunc(2500 * Space)
space_delay_b = trunc( 625 * Space)
```

Those delay lines are fixed-capacity circular buffers prepared for the 96 kHz graph. Rear Shelf Enable forces shelf gain to 0 dB when Off. Rear Shelf Frequency uses the same logarithmic interpolation family already documented for Halo frequency controls; Rear Shelf Gain uses the documented signed mapping. If the reconstruction notes do not yet pin the exact rear-shelf frequency endpoints independently of the vertical shelf, the implementation must extract and lock those endpoints in a driver unit test before claiming bit-exactness against the published equations. The user-facing control remains a 0.000 to 1.000 normalized slider.

### 4.5 Render modes

Render Mode is consumed by `IemPipeline`. HaloEncoder always emits the 7.0 bed; it does not itself choose Off/Simple/KU100. Stereo/Multi/Granular continue to emit ACN/SN3D.

**Off**

- Halo: `IemPipeline` folds the 7.0 bed with a fixed energy-preserving stereo matrix. Default coefficients are `C = 1/sqrt(2)`, `Ls/Rs = 1/sqrt(2)`, `Lsr/Rsr = 1/2`, `L/R = 1`. LFE is absent. This is not Halo's product downmix object.
- Stereo/Multi/Granular: `IemPipeline` folds the encoder's current ACN/SN3D bus with the same Simple virtual-speaker matrix described below, but skip Scene Rotator, KU100, and headphone EQ. Off therefore still has a defined stereo result for non-Halo encoders.
- KU100 convolution, headphone-EQ convolution, and Ambisonics rotation are skipped and add no CPU.

**Simple**

- Halo: `IemPipeline` encodes the 7.0 bed into the existing ACN/SN3D bus using the fixed azimuth table, then continues as below.
- Stereo/Multi/Granular: already emit that bus.
- Scene Rotator remains in path.
- Decode is a fixed virtual-speaker matrix, not HRTF:
  - speakers at the same seven azimuths used by the Halo bed;
  - each speaker gain is the SN3D spherical-harmonic encoding of that direction, conjugated/decoded by mode matching;
  - left/right stereo fold then uses the Off matrix above.
- Headphone EQ is skipped.

**KU100**

- Halo: encode the 7.0 bed to ACN/SN3D using the fixed azimuth table, then follow the existing Phase 1 path.
- Stereo/Multi/Granular: existing Phase 1 path.
- Unchanged IR hashes, Mid/Side baking, and EQ delay matching.

Render Mode, Encoder Mode, Order, Headphone EQ, and Latency Profile remain structural. Changing Render Mode while IEM is enabled prepares one pending graph. Equal-latency mode changes still crossfade; a change that adds or removes KU100/EQ latency uses the existing fade-out / switch / fade-in rule.

### 4.6 Existing stages

Scene Rotator, KU100, headphone EQ, wet/dry alignment, output gain, limiter, and latency profiles keep their Phase 1 contracts. Halo STFT hop and overlap-add latency are included in measured IEM latency and therefore in dry-path alignment.

When IEM is disabled, HaloEncoder is not prepared as a live graph and performs no STFT work.

## 5. Parameter Protocol

Existing Phase 1 IDs are unchanged. `0x12004` Encoder Mode accepts `3 = Halo`. New IDs occupy the unused `0x12008` and `0x12070` blocks.

| ID | Meaning | Wire value | Default |
|---|---|---|---|
| `0x12004` | Encoder Mode | 0 Stereo, 1 Multi, 2 Granular, 3 Halo | 0 |
| `0x12008` | Render Mode | 0 Off, 1 Simple, 2 KU100 | 2 |
| `0x12070` | Dialog Isolate | 0.001 unit, 0 to 1000 | 0 |
| `0x12071` | Dialog Aggress. | 0.001 unit, 0 to 1000 | 500 |
| `0x12072` | Dialog Attack | 0.001 unit, 0 to 1000 | 300 |
| `0x12073` | Dialog Release | 0.001 unit, 0 to 1000 | 750 |
| `0x12074` | Dialog Mix In | 0.001 unit, 0 to 1000 | 0 |
| `0x12075` | Divergence | 0.001 unit, 0 to 1000 | 500 |
| `0x12076` | Fade | 0.001 unit, 0 to 1000 | 300 |
| `0x12077` | Fade Rears | 0.001 unit, 0 to 1000 | 200 |
| `0x12078` | Diffusion | 0.001 unit, 0 to 1000 | 200 |
| `0x12079` | Space | 0.001 unit, 0 to 1000 | 800 |
| `0x1207A` | Back Boost | 0/1 | 1 |
| `0x1207B` | Rear Shelf Enable | 0/1 | 1 |
| `0x1207C` | Rear Shelf Freq | 0.001 unit, 0 to 1000 | 816 |
| `0x1207D` | Rear Shelf Gain | 0.001 unit, 0 to 1000 | 475 |

Normalized Halo sliders use thousandths so the documented constructor defaults `0.300000012`, `0.800000012`, and `0.815500021` round to the nearest persisted integer. App and driver both clamp; the driver remains authoritative.

`0x12008` is structural. `0x12070..0x1207D` are dynamic and apply on the next internal block boundary with stage-local smoothing where a step would click.

Invalid Encoder Mode or Render Mode values are rejected. Continuous Halo values are clamped. IDs not listed here or in the Phase 1 spec remain reserved.

## 6. State, Persistence, and Dispatch

App state gains:

```text
IemGeneralState.renderMode: Int = 2
IemHaloState {
    dialogIsolateThousandths = 0
    dialogAggressThousandths = 500
    dialogAttackThousandths = 300
    dialogReleaseThousandths = 750
    dialogMixInThousandths = 0
    divergenceThousandths = 500
    fadeThousandths = 300
    fadeRearsThousandths = 200
    diffusionThousandths = 200
    spaceThousandths = 800
    backBoost = true
    rearShelfEnable = true
    rearShelfFreqThousandths = 816
    rearShelfGainThousandths = 475
}
```

`encoderMode` range becomes `0..3`. `normalizeIemState()` continues to force Freeze off and now also clamps `renderMode` to `0..2` and every Halo thousandths field to `0..1000`.

All new fields except transient runtime diagnostics persist in device settings and preset JSON. Full-state HIDL restore keeps the Phase 1 order: Enable=0, every persistent IEM field including Render Mode and Halo, then Enable with the target value. Incremental edits still send one ID.

AIDL mode still hides the IEM card. The AIDL shared-memory format stays at version 6.

## 7. App UX

### 7.1 Main card

The existing HIDL-only IEM card remains after Convolver. Encoder Mode options become Stereo, Multi, Granular, Halo. The one-line summary becomes:

```text
<Encoder> · <Order> · <Render>
```

Examples: `Stereo · 3rd order · KU100`, `Halo · 3rd order · Off`.

Expanded content still contains only Encoder Mode, Order, Wet, and the editor command. Render Mode is not added to the compact card.

### 7.2 Editor

The four tabs remain Encoder, Rotation, Decoder, Output.

Encoder tab:

- Mode dropdown includes Halo.
- Halo content is a flat control list: Dialog Isolate, Dialog Aggress., Dialog Attack, Dialog Release, Dialog Mix In, Divergence, Fade, Fade Rears, Diffusion, Space, Back Boost, Rear Shelf Enable, Rear Shelf Freq, Rear Shelf Gain.
- No nested cards. No bed-angle editors.

Decoder tab:

- Render Mode segmented control: Off / Simple / KU100.
- Effective Order remains visible.
- Headphone EQ is enabled only when Render Mode is KU100. In Off or Simple it is shown disabled with the current stored value preserved.
- KU100 resource errors remain inline and apply only in KU100 mode.

Rotation and Output tabs are unchanged, except Output diagnostics also remain valid for Halo. Actual latency must include Halo STFT/overlap-add delay when Halo is selected.

All new visible strings are added to English, simplified Chinese, and Russian. Product names `Halo` and `KU100` stay as published.

## 8. Telemetry

Existing IEM telemetry remains. Add:

- active Render Mode;
- Halo prepared/active flag;
- Halo STFT hop latency in frames;
- last dialog.net preparation result.

UI telemetry remains read-only and never writes `IemState`.

## 9. Error and Transition Semantics

Phase 1 fault rules still apply. Additional Halo rules:

- `dialog.net` hash or topology mismatch is a build failure. Runtime must not silently disable dialog extraction and continue as if the model were present.
- A Halo prepare failure keeps the current active graph if one exists; otherwise IEM returns dry and reports the fault.
- NaN/Inf from the Halo path trips the existing IEM fault reset. It never writes non-finite samples into the host buffer.
- Render Mode Off/Simple must not execute KU100 or headphone-EQ convolution, even if a previous KU100 graph is still in memory during a fade-out.
- Switching away from Halo may destroy prepared STFT/FANN state only on the control thread after the old graph has been unpublished.

## 10. Real-Time and Resource Bounds

Audio-thread bans from Phase 1 remain in force.

Prepared Halo storage includes the STFT window, seven complex planes, overlap-add histories, FANN weights, delay lines, and IIR histories. Tests must report the Halo prepared-memory estimate separately from the Granular 6.1 MiB circular buffer.

The new largest configuration is Ord3 Halo + KU100 + headphone EQ + limiter. The existing p99 gate still applies: processing time below 50 percent of the callback period on the target arm64 device. If that configuration cannot meet the gate, the implementation must fail the benchmark rather than silently drop Halo stages.

## 11. Verification

In addition to unchanged Phase 1 gates:

- `dialog.net` hash, layer sizes, and 391-weight load.
- Golden dialog-feature vector and FANN output for a documented bin fixture.
- Dialog extraction and residual-surround assignment against reconstructed per-bin equations.
- Time-domain diffusion/delay/shelf against reconstructed sample equations.
- Bed-to-Ambisonics encoding at the seven fixed azimuths for orders 1-3.
- Off fold, Simple rotate-then-fold, and KU100 path produce distinct, energy-sane stereo for the same Halo bed.
- Render Mode and Encoder Mode rebuild/coalescing, including Halo <-> Granular and KU100 <-> Off latency transitions.
- Persistence and full-state/incremental dispatch for every new ID.
- App policy: Encoder Mode 3 is accepted, Render Mode summary strings are correct, headphone EQ is disabled outside KU100, and AIDL still hides the card.
- Host CTest, UBSan, Android Release arm64-v8a and armeabi-v7a, App unit tests, lint, and `assembleDebug`.
- ASan remains best-effort on the current host if the existing runtime SIGILL limitation persists; that limitation is documented, not treated as a Halo regression.

Device-accessible smoke:

- enable Halo on HIDL, change dialog/fade/diffusion, switch Off/Simple/KU100, switch back to Stereo;
- confirm no audioserver crash after bind-mounting the new driver;
- document any remaining physical listening items that this environment cannot perform.

## 12. Completion Criteria

This phase is complete only when:

1. HaloEncoder produces a validated 7.0 bed from stereo using the documented dialog, residual-surround, and diffusion stages, with `dialog.net` embedded and hash-checked.
2. That bed encodes to ACN/SN3D at orders 1-3 using the fixed azimuth table.
3. Off, Simple, and KU100 render modes are selectable, default to KU100, and skip unused convolution work.
4. Existing Stereo, Multi, and Granular paths remain available and keep KU100 as their default render.
5. HIDL persistence and incremental dispatch cover Render Mode and the Halo control set; AIDL still hides IEM.
6. The MiuiX card and editor expose every in-scope control without hidden important actions, in English, simplified Chinese, and Russian.
7. Real-time, sanitizer, Android Release, and App gates pass, with any pre-existing ASan host limitation explicitly restated.

## 13. Explicit Non-Goals

- Independent Halo effect card on the main screen.
- Replacing Stereo/Multi/Granular.
- LFE, Halo product downmix, overhead beds, C LF Split, DE Ramp/Gate/Stability, monitor/solo/mute, or AmbisonicFormat from the original plugin.
- Shipping or linking the original Halo binary.
- AIDL IEM transport.
- User-editable bed angles or additional HRTF sets.
