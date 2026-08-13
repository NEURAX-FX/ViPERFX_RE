# Matrix Convolver Replacement Design

## Goal

Replace the current two-channel `PConvSingle` wrapper with a native stereo 2x2 matrix partitioned-convolution engine based on the behavior of `/root/vipefx-re/Neurax08_V4A_Embedded_Convolver.eel`.

The replacement must:

- accept mono, stereo, and canonical four-channel IR data;
- reproduce the EEL direct/cross routing and normalization;
- preserve the existing real-time graph/resource publication model;
- add Wet, Output Gain, Routing, and precise Cross Delay parameters without removing existing controls;
- remain valid from 8 kHz through 384 kHz;
- perform no allocation, locking, file I/O, or destruction on the audio thread.

## Alternatives Considered

### Patch the existing wrapper

Add two more `PConvSingle` instances and mix four outputs. This is the smallest source change, but duplicates input FFT work, preserves the current coarse 4096-segment behavior, and does not constitute a real engine replacement.

### Native matrix partitioned FFT engine

Maintain one frequency-domain history per input channel and multiply it by four independently partitioned kernels. This directly models a stereo 2x2 transfer matrix, shares FFT work, supports the EEL routing semantics, and gives deterministic behavior for mono, stereo, and four-channel IRs.

This is the selected approach.

### Embed or interpret the EEL implementation

Ship the JSFX/EEL source or an interpreter in the driver. This would introduce an unsuitable runtime and allocation model into the Android audio effect and would make ABI, performance, and 384 kHz behavior harder to control.

## Canonical Matrix

The engine computes:

```text
outL = H_LL * inL + H_LR * inR
outR = H_RL * inL + H_RR * inR
```

Canonical four-channel storage order is:

```text
[H_LL, H_RL, H_LR, H_RR]
```

This is the order produced by the revised DAW workflow: split the two stereo pairs into four mono tracks, exchange tracks 2 and 3, then export. Cross delay is controlled by the driver instead of being baked into new IR files.

Existing four-channel IRs that already contain delayed cross tracks remain usable by setting Cross Delay to `0 ms`.

## Mono And Stereo Expansion

Mono and stereo IRs are expanded into the same matrix internally.

For mono `M`:

```text
H_LL = M
H_RR = M
H_LR = M
H_RL = M
```

For stereo `[L, R]`:

```text
H_LL = L
H_RR = R
H_LR = L
H_RL = R
```

Cross delay is applied to the two cross branch outputs at runtime:

```text
delaySamples = round(crossDelayMs * sampleRate / 1000)
```

The parameter range is `0..10 ms`, uses `0.0001 ms` transport/UI precision, and defaults to the EEL value `0.3125 ms`.

## Runtime Routing And Mixing

Parameters:

- `Cross`: 0 to 100 percent, converted to `c = value / 100`.
- `Wet`: 0 to 100 percent, converted to equal linear dry/wet interpolation as in the EEL.
- `Output Gain`: -24.0 to +24.0 dB, transported as integer centibels.
- `Routing`: Direct+Cross, Direct Only, or Cross Only.
- `Cross Delay`: 0 to 10 ms, applied only to the cross branches.

Branch behavior matches the EEL:

```text
branchNorm = 1 / (directEnabled + crossEnabled * c)
wetL = (directEnabled * directL + crossEnabled * c * crossL) * branchNorm
wetR = (directEnabled * directR + crossEnabled * c * crossR) * branchNorm
out = (dry * (1 - wet) + wetSignal * wet) * dbToLinear(outputGain)
```

If the normalization denominator is zero, `branchNorm` is 1 and the selected branch produces silence. This preserves Cross Only at zero cross gain without division by zero.

The old post-convolution L/R linear cross-mix is removed. Its existing parameter ID becomes the EEL cross-branch gain.

## Engine Structure

Introduce a project-local matrix convolver under `ViPERDSP/viper/utils` and keep the public `Convolver` effect class as the graph-facing wrapper.

The matrix engine owns:

- one overlap/input block per input channel;
- one ring of frequency-domain input partitions per input channel;
- four immutable partitioned kernel banks;
- preallocated FFT scratch and output overlap buffers;
- preallocated dry-delay state if block scheduling requires dry/wet latency alignment.
- preallocated variable-tap cross-delay rings sized for 10 ms at the active sample rate.

Kernel construction happens only while building a new `DspResources`/`DspGraph` off the audio thread. The audio thread only advances preallocated ring indices, performs FFT multiply-accumulate work, and applies scalar parameters.

Partition size is 1024 samples to match the reference EEL engine. If the host callback is not exactly 1024 frames, the engine uses preallocated input/output FIFOs and emits a continuous stream. Dry samples are delayed by the same FIFO latency before Wet interpolation so partial Wet settings do not create an unintended comb filter.

## Resource And Graph Publication

The existing immutable resource snapshot and two-slot graph publication model remains in charge of lifetime.

- Four-channel IR buffers are accepted by `DspResources` and `Convolver` validation.
- IR channel count, frame count, and sample rate are fixed in the resource snapshot.
- A graph is published only after all four kernel banks and scratch buffers are valid.
- Invalid uploads leave the currently published graph active.
- Retired kernels are destroyed only after the audio thread has acknowledged the graph switch.

## Parameter ABI And App UI

Keep existing convolver IDs unchanged and add four IDs after the current convolver range:

- Wet percent.
- Output gain in centibels.
- Routing enum.
- Cross delay in 100 ns units.

Defaults preserve current audible behavior where possible:

- Wet: 100 percent.
- Output Gain: 0 dB.
- Routing: Direct+Cross.
- Cross Delay: 0.3125 ms.
- Cross: existing persisted value; new installs use the current project default.

The Android app adds MiuiX-style controls to the existing Convolver effect card using project-local wrappers:

- Wet slider.
- Output Gain slider.
- Routing dropdown.
- Cross Delay slider with an adjacent precise numeric-entry popup.

Existing kernel selection and explicit resource management remain unchanged. Unknown/older drivers are handled through the project's existing driver-version compatibility gate rather than hidden fallback behavior in the DSP.

## Errors And Limits

- Reject channel counts other than 1, 2, or 4.
- Reject zero frames, non-finite samples, malformed upload sizes, and allocation/FFT setup failure.
- Preserve the currently active kernel when a replacement is rejected.
- Apply the existing maximum kernel-frame/byte limits before matrix expansion.
- Clear all convolution and dry-alignment history on reset, sample-rate change, kernel replacement, and disable/enable discontinuity.
- Clamp all scalar parameters at the control-thread boundary and again when applying a snapshot.

## Tests

Native tests will cover:

- exact impulse routing for mono, stereo, and canonical four-channel IRs;
- 0..10 ms user-controlled cross-delay scaling at 44.1, 48, 96, 192, and 384 kHz;
- Direct+Cross, Direct Only, and Cross Only routing;
- EEL branch normalization, Wet interpolation, and dB gain;
- arbitrary callback sizes around the 1024 partition boundary;
- reset and kernel replacement clearing stale history;
- rejection of malformed channel counts/uploads without replacing the active graph;
- no allocations on the processing path after setup;
- parity against the Python compatibility engine for deterministic fixtures.

Integration verification will include the existing native suite, ASan/UBSan host coverage where supported, arm64 release build, Android app build, installation through the module mount namespace, and live AudioFlinger playback checks.
