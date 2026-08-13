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

For release acceptance, verify the App Output tab while audio is active and confirm counters do not continually increase under default settings. Low/Balanced/Stable measured latency targets are at most 10/20/40 ms; the Ord3 Granular + KU100 + EQ + limiter p99 callback target is below 50 percent of the callback period.

## Halo Encoder and Render Modes

Halo is a project-local clean-room reconstruction based on the published HaloMixRE reverse-engineering notes. It is not an IEM Plug-in Suite component and does not ship or link the original Halo binary, VST3, or installer payload.

- Encoder mode: `3 = Halo`, mutually exclusive with Stereo, Multi, and Granular.
- Render mode: `0 = Off`, `1 = Simple`, `2 = KU100`; default is KU100.
- Render mode parameter: `0x12008`.
- Halo controls: `0x12070..0x1207D`.
- Model path: `IEMDSP/resources/source/halo/dialog.net`.
- Model SHA-256: `652cbd597b9afbd82eb9b39fe80e3e825a381e448c3c2a269c07842f88eb5b72`.

Off and Simple skip KU100 and headphone-EQ convolution. The App keeps the IEM card hidden on AIDL in this phase.
