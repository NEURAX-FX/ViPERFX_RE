# IEM Spatial Integration Provenance and Operation

## Upstream and License

- Official project: https://plugins.iem.at
- Repository: `https://git.iem.at/audioplugins/IEMPluginSuite.git`
- Pinned commit: `39de1dd5883f1bd8d65fe1662487f2470a1d7b55`
- License: GPL-3.0-or-later

Original copyright and license headers are retained in vendored source files. The complete provenance manifest is `IEMDSP/upstream/UPSTREAM.md`.

## Source Integration

Unchanged upstream material:

- `resources/efficientSHvanilla.cpp`, compiled as the spherical-harmonic evaluator.
- `resources/efficientSHvanilla.h`, retained as an uncompiled reference because it includes JUCE helpers.
- `resources/Quaternion.h`, retained as an uncompiled reference because it includes JUCE vector types.
- KU100 Ord1-3 impulse responses, their attribution README, and 23 stereo headphone-EQ impulse responses.

Project-local adaptations derived from the pinned implementation:

- `iem/SphericalHarmonics.*`: fixed ACN/SN3D wrapper.
- `iem/Quaternion.*`: JUCE-free scalar quaternion and vector rotation.
- Stereo, Multi, and Granular encoders; manual scene rotation; KU100 decoder; headphone EQ; latency scheduling; wet/output/limiter pipeline.

JUCE processors, editors, OSC/MIDI support, desktop state, file pickers, and runtime resource loading are excluded. Phase 1 is available only through the legacy non-AIDL (HIDL-era) Android effect path; the App hides the feature on AIDL.

## Resource Processing

The pinned source assets are SHA-256 checked before generation. KU100 PCM16 filters and 48 kHz stereo float32 headphone-EQ filters are deterministically resampled to the internal 96 kHz representation at build time and emitted as compiled C++ resources. All 23 headphone models preserve independent left and right correction filters. No filesystem I/O, decoding, allocation, or resampling occurs in the audio callback.

Resource citations:

1. Benjamin Bernschuetz, "A Spherical Far Field HRIR/HRTF Compilation of the Neumann KU 100," AIA-DAGA, 2013. http://audiogroup.web.th-koeln.de/ku100hrir.html
2. Christian Schoerkhuber, Markus Zaunschirm, and Robert Hoeldrich, "Binaural Rendering of Ambisonic Signals via Magnitude Least Squares," Fortschritte der Akustik, DAGA, 2018.

## Parameter Protocol

The IEM scalar parameter block is reserved at `0x12000..0x12FFF`:

- `0x12000..0x12007`: enable, wet, output gain, order, encoder mode, latency profile, limiter enable, limiter ceiling.
- `0x12010..0x12014`: Stereo encoder.
- `0x12020..0x12023`: indexed Multi encoder source fields.
- `0x12030..0x12045`: Granular encoder.
- `0x12050..0x12057`: scene rotation.
- `0x12060`: headphone EQ selection.
- `0x12100..0x12103`: resource reset, rotation reset, transient Freeze, and runtime reset commands.

Persistent parameters are restored by the App. Freeze is transient and returns Off after driver or audioserver restart.

## Diagnostic Telemetry

The driver publishes processed-frame count, latest/average/maximum callback time, deadline misses, input/output queue faults, grain-pool exhaustion, graph generation, host/internal sample rates, measured latency, bypass reason, enabled/prepared status, active grain count, encoder mode, ambisonics order, fault code, preparation result, and limiter gain reduction.

For release acceptance, verify the App Output tab while audio is active and confirm counters do not continually increase under default settings. Low/Balanced/Stable latency ceilings are 50/64/96 ms after adding the fixed Halo Downmix delay; the Ord3 Granular + KU100 + EQ + limiter p99 callback target remains below 50 percent of the callback period.

## Halo 7.1 Encoder and Render Modes

Halo is a project-local clean-room reconstruction based on the published HaloMixRE reverse-engineering notes. It is not an IEM Plug-in Suite component and does not ship or link the original Halo binary, VST3, or installer payload.

- Encoder mode: `3 = Halo`, mutually exclusive with Stereo, Multi, and Granular.
- Render mode: `0 = Off`, `1 = Halo Downmix`, `2 = KU100`; default is KU100.
- Render mode parameter: `0x12008`.
- Halo controls: `0x12070..0x12081`.
- LFE controls: Enable `0x1207E`, Frequency `0x1207F`, Split `0x12080`, Gain `0x12081`.
- Halo Downmix controls: `0x12090..0x120AC`.
- Model path: `IEMDSP/resources/source/halo/dialog.net`.
- Model SHA-256: `652cbd597b9afbd82eb9b39fe80e3e825a381e448c3c2a269c07842f88eb5b72`.

Halo emits `L, R, C, Ls, Rs, Lsr, Rsr` as directional planes and synthesizes a separate mono LFE plane from post-diffusion `L/R/C`. The LFE stage uses the documented Q=1 RBJ low-pass, `10 * 20^x` cutoff mapping, `55x - 45 dB` gain mapping, and no automatic cinema +10 dB boost.

LFE is never encoded into ACN/SN3D and is never rotated. Off mixes it equally after the direct Halo fold. Halo Downmix accepts it as an eighth logical plane and applies the same 3,072-sample base latency as the directional bed, so it is mixed exactly once. KU100 delays it by the KU100 decoder latency, mixes it equally into the decoded ears, then applies headphone EQ to the combined signal. Off and Halo Downmix skip KU100 and headphone-EQ convolution. The App keeps the IEM card hidden on AIDL in this phase.

## Halo Downmix Decoder

Render mode `1` is a fixed-layout clean-room reconstruction of the relevant Halo Downmix 1.5.x core. Rotated ACN/SN3D is reconstructed to `L, R, C, Ls, Rs, Lsr, Rsr`, joined with optional LFE, then processed in this order:

1. Common 3,072-sample delay and optional side/rear relative advance.
2. Side and rear RBJ high shelves.
3. Pair balance and center divergence.
4. Optional LFE Q=1 low-pass.
5. Normalized M/S trims for front, side, and rear pairs plus center/LFE trims.
6. Sparse unity-fallback stereo routing with optional shared-input scaling.
7. Optional Q=1 output high-pass and left/right output trims.

The FFT Monofilter, Monitor In path, arbitrary layout table, solo/mute bank, phase inversion, metering, and plugin preset compatibility are not part of the project renderer.

## Session 0 Driver Ownership

Legacy global mode uses an AudioPolicy-pinned `v4a_standard_re` music effect. The manager's session 0 `AudioEffect` is a configuration handle rather than the sole lifetime owner.

- Context-local control parameter: `0x120F0` (`0 = bypass`, `1 = active`).
- Activation order: bypass, dispatch complete state/resources, then activate.
- Dynamic mode explicitly bypasses the pinned global path before opening nonzero sessions.
- The App releasing or losing its Binder handle does not deactivate the policy-owned context.
- Validated ViPER/IEM snapshots and immutable committed DDC/convolver resources are cached only for session 0.
- The cache is memory-only and is lost when `audioserver` restarts.
- Boot/sticky App replay remains responsible for recovery after reboot or `audioserver` restart.
- AIDL mode keeps the pinned legacy path bypassed and retains its existing App-owned lifetime behavior.
