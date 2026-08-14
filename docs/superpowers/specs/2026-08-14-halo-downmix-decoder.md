# Halo Downmix Decoder Design

## Status

Approved scope for replacing the current `SimpleDecoder` with a clean-room Halo Downmix renderer. The FFT Monofilter is explicitly excluded.

## Goals

- Replace the current direct Ambisonics-to-stereo `SimpleDecoder` with `HaloDownmixDecoder`.
- Preserve scene rotation by decoding the rotated ACN/SN3D signal to a logical 7.0 speaker bed before downmix processing.
- Accept the project-local Halo LFE sideband as an eighth logical input when the Halo encoder is active.
- Reproduce the confirmed Halo Downmix 1.5.x processing order that is relevant to fixed 7.1-to-stereo rendering.
- Expose the relevant Downmix controls through the existing HIDL-only IEM parameter path and App editor.
- Keep the audio callback allocation-free and free of trigonometric, logarithmic, and exponential coefficient work.

## Non-Goals

- No FFT Monofilter. Its exact window/overlap kernel remains unresolved, and the project does not want its sound.
- No arbitrary host layout negotiation or the complete 55-layout table. The project renderer has one fixed logical input layout and one fixed stereo output layout.
- No Monitor In / In + Downmix comparison buffer or configured-standard-coefficient monitor pass.
- No AmbiX/FuMa metadata switch. The project already has a fixed ACN/SN3D contract.
- No plugin preset/XML compatibility, solo/mute matrix, phase-invert bank, metering UI, color state, or NUGEN wrapper behavior.
- No changes to KU100 rendering or headphone EQ.

## Signal Flow

`HaloDownmixDecoder` processes one block in this order:

1. Decode rotated ACN/SN3D channels to the seven existing virtual speakers: `L, R, C, Ls, Rs, Lsr, Rsr`.
2. Add an optional external LFE plane. Non-Halo encoders supply silence.
3. Apply the common 3,072-sample delay and relative side/rear un-delay targets.
4. Apply enabled side and rear high-shelf filters.
5. Apply the confirmed balance rotation to `L/R`, `Ls/Rs`, and `Lsr/Rsr`.
6. Apply center divergence with a 100-sample ramp.
7. Optionally low-pass the LFE plane.
8. Convert symmetric pairs to normalized mid/side, apply 1,024-sample gain ramps, then decode the pairs.
9. Route the processed logical bed to stereo with the confirmed normal-output unity fallback rules.
10. Optionally high-pass the stereo output.
11. Apply smoothed left and right output trims.

The normal stereo route is:

```text
L' = L + C + Ls + Lsr + LFE
R' = R + C + Rs + Rsr + LFE
```

`ScaleIPByOPCount` optionally divides center and LFE by two because each source targets both stereo outputs. Single-sided surround and rear sources are not divided.

Balance uses the confirmed asymmetric pair transform independently for its left and right controls:

```text
a(p) = (sin(pi*p/4) + cos(pi*p/4)) / sqrt(2)
b(p) = (sin(pi*p/4) - cos(pi*p/4)) / sqrt(2)

L' = a(pL)*L + b(pR)*R
R' = b(pL)*L + a(pR)*R
```

`pL=pR=1` is exactly transparent. Center divergence uses `L += D*C/2`, `R += D*C/2`, and `C *= 1-D`.

## Decoder Integration

- Rename `SimpleDecoder` to `HaloDownmixDecoder`; remove the old implementation after all call sites and tests migrate.
- `Prepare` accepts Ambisonics order, sample rate, and maximum block size.
- Internally allocate an eight-plane logical bed during `Prepare`; no process-time allocation is allowed.
- `Process` accepts ACN/SN3D inputs, an optional LFE pointer, and stereo outputs.
- Render mode `SIMPLE` always uses `HaloDownmixDecoder` after scene rotation.
- Non-Halo render mode `OFF` continues to use the decoder without scene rotation because an Ambisonics signal still requires stereo rendering.
- Halo render mode `OFF` remains the direct Halo-bed fold and is not changed by this work.
- Halo render mode `SIMPLE` passes the explicit LFE sideband into `HaloDownmixDecoder`; the pipeline must not mix that LFE a second time.
- KU100 keeps the existing decoder-aligned LFE path.

## Delay And Latency

- The decoder reports a fixed base latency of 3,072 samples.
- At the project's fixed 96 kHz internal rate this is 32 ms.
- Side/rear delay controls represent relative advance against that common delay.
- The original UI maps delay to 0..100 ms, but a causal 3,072-sample ring at 96 kHz can represent only 0..32 ms. Project controls therefore clamp to 0..32,000 microseconds rather than allowing an invalid negative target.
- Disabling relative delay returns every logical role, including LFE, to the common 3,072-sample target; it does not remove the decoder's base latency.
- Delay target changes crossfade over 10,000 samples using the confirmed `0.0001` per-sample step.
- `IemPipeline::WetLatencyFrames()` adds 3,072 samples whenever `HaloDownmixDecoder` is active.
- Latency-profile ceilings become 50 ms for Low, 64 ms for Balanced, and 96 ms for Stable so the longest supported path, Halo STFT plus Halo Downmix plus limiter, prepares successfully. Partition sizes and scheduler waterlines do not change.
- Dry-path compensation and telemetry use the updated reported latency.

## Parameters

Add `HaloDownmixParams` under `IemParams::decoder`. Parameter IDs occupy the unused `0x12090..0x120AC` range.

| ID | Field | Storage | Default |
|---|---|---|---|
| `0x12090` | Delay enable | bool | on |
| `0x12091..0x12094` | Ls, Rs, Lsr, Rsr relative delay | microseconds, `0..32000` | `0` |
| `0x12095..0x12097` | Side shelf enable, cutoff, gain | bool, normalized millionths, normalized millionths | off, 100 Hz, 0 dB |
| `0x12098..0x1209A` | Rear shelf enable, cutoff, gain | bool, normalized millionths, normalized millionths | off, 100 Hz, 0 dB |
| `0x1209B..0x1209C` | Pan left/right | millionths `0..1000000` | `1000000` |
| `0x1209D` | Center divergence | millionths `0..1000000` | `0` |
| `0x1209E..0x120A5` | Front M/S, center, side M/S, rear M/S, LFE trims | normalized millionths | binary constructor defaults |
| `0x120A6` | LFE LPF enable | bool | off |
| `0x120A7` | LFE LPF cutoff | normalized millionths | 200 Hz |
| `0x120A8` | Scale input by output count | bool | off |
| `0x120A9` | Output HPF enable | bool | off |
| `0x120AA` | Output HPF cutoff | normalized millionths | 30 Hz |
| `0x120AB..0x120AC` | Output left/right trim | normalized millionths | 0 dB |

Frequency normalization follows the binary mapping:

```text
frequency_hz = 20 * 1100^x
```

Gain normalization follows:

```text
gain_db = 90*x - 70
```

Relevant trim defaults are:

- front mid and side: `0 dB`
- center: `-3 dB`
- side-surround mid and side: `-3 dB`
- rear-surround mid and side: `-6 dB`
- LFE: `0 dB`
- output left and right: `0 dB`

Control-thread snapshot updates precompute shelf, LFE low-pass, output high-pass, balance, and linear-gain values for 96 kHz. `ApplyParams` only copies derived values and schedules ramps.

The side/rear shelves use the confirmed RBJ high-shelf design with slope `S=1`. The output high-pass and LFE low-pass use RBJ designs with `Q=1`; this matches the confirmed recursive-biquad family while keeping the project implementation deterministic.

There is no second Downmix LFE-enable parameter. The existing Halo LFE enable controls source generation; the Downmix LFE trim and optional LPF control only the supplied sideband. This prevents conflicting state and preserves existing profiles.

## App UI

The Decoder tab keeps the three render choices but renames the user-facing `Simple` label to `Halo Downmix`.

When `Halo Downmix` is selected, show project-local MiuiX rows grouped as:

- Timing: delay enable and four relative-delay sliders.
- Tone: side shelf and rear shelf enable/frequency/gain.
- Image: pan left, pan right, and center divergence.
- Levels: front M/S, center, side M/S, rear M/S, LFE, and left/right output trims.
- Filters: LFE LPF, output HPF, and input-count scaling.

Frequency and gain sliders display physical Hz and dB while persisting normalized millionths. Controls remain available for every encoder because the decoder processes the reconstructed virtual bed, not only Halo encoder output.

## State And Compatibility

- Existing profiles gain the new defaults through normal preference fallback; no migration shim is needed because the fields are newly appended.
- Existing render mode integer `1` remains valid and now selects Halo Downmix.
- HIDL parameter dispatch is extended; AIDL visibility and interface version remain unchanged.
- Telemetry continues to report render mode `1`; only the App label changes.
- Parameter updates do not rebuild the pipeline unless the existing structural fields change.

## Realtime And Error Handling

- All bed, delay, and filter storage is allocated in `Prepare`.
- No locks, heap allocation, file access, logging, or expensive coefficient math occurs in `Process`.
- Reset clears delay rings, filter histories, crossfades, and smoothers deterministically.
- Invalid sample rate, order, block size, or null channel pointers fail preparation/processing using the existing pipeline error model.
- Non-finite output remains caught by the pipeline's existing final validation.

## Verification

- Mapping tests lock the exact frequency/gain endpoints and default physical values.
- Matrix tests cover impulse routing for every logical role, unity fallback, and target-count scaling.
- Decoder tests cover ACN/SN3D virtual-speaker reconstruction, LFE routing, chunk invariance, reset determinism, and no process-time allocation.
- Delay tests cover the 3,072-sample base latency, per-role relative advance, and 10,000-sample retarget crossfade.
- Filter tests cover shelf, LFE LPF, and output HPF fixtures at 96 kHz.
- Pipeline tests cover latency accounting, dry/wet alignment, no duplicate LFE mix, rotation before downmix, and unchanged KU100 behavior.
- App contract tests cover defaults, normalization, persistence, parameter IDs, dispatch, labels, and localization.
- Final gates include host tests, realtime audit, UBSan, Android arm64/armv7 release builds, App unit tests, lint, and debug assembly.
