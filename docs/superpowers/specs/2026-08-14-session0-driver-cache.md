# Session 0 Driver-Owned State Design

## Status

Approved direction for keeping global output processing active when the ViPER4Android manager process, foreground service, and client `AudioEffect` handle are killed. The guarantee stops at `audioserver` restart; reboot and `audioserver` recovery continue to rely on the App boot/sticky service replaying state.

## Problem

The current legacy effect stores all live state inside one `ViperContext`:

- `ViPERParams parameter_snapshot_`
- `iem::IemParams iem_parameter_snapshot_`
- committed DDC/convolver resources
- `IemResources`
- the prepared DSP graph

The App owns the session 0 `AudioEffect` handle through `ViperService.globalEffect`. `START_STICKY` and foreground-service priority make ordinary process eviction less likely, but do not make the driver independent from the App. When the App process dies, Binder removes its effect handle. Without another system-owned handle, AudioFlinger may disable or destroy the session 0 effect module, so the in-context snapshots alone cannot guarantee uninterrupted processing.

## Goals

- Make global music processing system-owned rather than App-owned.
- Keep the last validated session 0 scalar, IEM, DDC, and convolver state in `audioserver` memory.
- Restore cached state if the pinned session 0 context is recreated while `audioserver` remains alive.
- Keep existing dynamic per-player sessions working without inheriting the global cache.
- Prevent global and dynamic modes from processing the same audio twice.
- Keep audio processing lock-free and allocation-free.
- Preserve the current App state/preferences as the durable source of truth after reboot or `audioserver` restart.

## Non-Goals

- No disk writes from `audioserver`.
- No recovery across `audioserver` restart or device reboot without App replay.
- No persistent ownership of dynamic nonzero audio sessions after the App dies.
- No private native AudioFlinger client daemon.
- No App `android:persistent` privilege, OOM hacks, or reliance on `START_STICKY` as the primary guarantee.
- No changes to AIDL visibility or the current HIDL-only IEM policy.

## Chosen Architecture

### 1. System-Pinned Music Effect

The Magisk module patches the active audio effects configuration so AudioPolicy automatically applies `v4a_standard_re` to the music stream.

XML configuration uses the standard AOSP postprocess form:

```xml
<postprocess>
    <stream type="music">
        <apply effect="v4a_standard_re"/>
    </stream>
</postprocess>
```

Legacy `.conf` configuration uses:

```text
output_session_processing {
    music {
        v4a_standard_re {
        }
    }
}
```

The patcher must support these cases:

- no postprocess/output-session section exists;
- a music section exists and needs one additional apply/effect entry;
- a postprocess section exists without music;
- the ViPER entry already exists;
- multiple vendor/product/odm configuration files are present.

Injection is idempotent. A generated config is mounted only after structural validation; a failed patch leaves the original config active. Existing library/effect registration remains unchanged.

The policy-owned handle keeps the session 0 effect module alive and enabled. The App's session 0 `AudioEffect` becomes a control handle attached to the same effect implementation/session rather than the sole owner.

### 2. Session 0 Context Gate

Add a context-local control parameter:

```text
PARAM_DRIVER_SESSION0_ACTIVE = 0x120F0
```

Rules:

- session `0` processes only when the effect module is enabled and `session0_active == true`;
- nonzero sessions ignore this parameter and retain existing behavior;
- the default is `false`, so the auto-created policy effect is a safe bypass before the manager sends state;
- the value is part of the session 0 memory cache;
- App process death does not change the cached value;
- switching to dynamic mode explicitly writes `false` before dynamic sessions are enabled;
- switching to global mode writes the full state while inactive, then writes `true` last.

This gate prevents the pinned global effect from double-processing audio while the App is using per-player dynamic sessions.

### 3. Process-Global Session 0 Cache

`ViperLibraryCreate` passes `session_id` and `io_id` into `ViperContext`. A new process-global `Session0StateCache` exists inside `libv4a_re.so` and is used only for session `0`.

The cache stores:

```cpp
struct Session0CachedState {
    bool initialized = false;
    bool active = false;
    uint64_t generation = 0;
    ViPERParams params{};
    iem::IemParams iem_params{};
    std::shared_ptr<const CommittedDspResourceSnapshot> dsp_resources{};
    uint64_t iem_resource_generation = 0;
};
```

Cache behavior:

1. A session 0 context copies the cache during construction, before graph preparation.
2. A valid scalar or IEM parameter update changes the context-local snapshot first, then publishes the validated snapshot to the cache.
3. A committed or cleared DDC/convolver transfer publishes a new immutable committed-resource snapshot.
4. Invalid, incomplete, or abandoned resource transfers never update the cache.
5. Runtime telemetry, temporary capture buffers, limiter history, delay lines, granular grains, and other process history are not cached.
6. Nonzero session contexts always start from normal defaults and App dispatch; they never read or write this cache.

`std::mutex` is allowed inside the cache because create/set-parameter/resource-commit calls are control-plane operations. `Process`, `DspGraph::Process`, and every audio-thread path use only context-local snapshots and never touch the mutex.

### 4. Immutable Committed Resources

Large convolver kernels must not be duplicated merely to keep a cache copy. Refactor committed resource ownership into an immutable shared snapshot:

```cpp
struct CommittedDspResourceSnapshot {
    DdcResource ddc;
    ConvolverResource convolver;
};
```

`DspResources` keeps pending upload state locally, but committed resources are held through `std::shared_ptr<const CommittedDspResourceSnapshot>`. The active context and session 0 cache share the same immutable object. A successful commit creates a new snapshot and atomically replaces the control-plane pointer; graph preparation consumes the context-local pointer.

This preserves resource state across context recreation without maintaining a second 16+ MB kernel copy.

### 5. App Ownership And Mode Switching

`ViperService` retains a session 0 control handle whenever the service is alive, regardless of global/dynamic mode. It is no longer the lifetime owner of the effect.

Global mode sequence:

1. create/acquire the session 0 control handle;
2. write `PARAM_DRIVER_SESSION0_ACTIVE = 0`;
3. dispatch the complete current effect state and resources;
4. write `PARAM_DRIVER_SESSION0_ACTIVE = 1`;
5. create no dynamic session effects.

Dynamic mode sequence:

1. acquire the session 0 control handle;
2. write `PARAM_DRIVER_SESSION0_ACTIVE = 0`;
3. create and configure dynamic nonzero-session effects as today.

Master-off behavior keeps the mode gate unchanged and dispatches the existing disabled effect state. App/service destruction only releases the App's handles; it must not write `session0_active = false` as part of cleanup.

On App process death in global mode, AudioPolicy's handle and the cached `active=true` state remain. Current playback continues, and a later music stream recreates/restores the context from the in-process cache if necessary.

### 6. Single-Module Assumption And Validation

Android normally coalesces handles for the same implementation UUID and audio session onto one AudioFlinger effect module. The design relies on the App control handle addressing the policy-pinned session 0 module.

Before release, device validation must confirm:

- only one `ViperContext` is created for the pinned music/session 0 effect plus App control handle;
- App parameter writes affect the policy-pinned processing path;
- releasing/killing the App handle does not release the policy-owned context.

If the target OEM creates separate session 0 modules, the implementation must add a control-plane subscriber hub that republishes session 0 snapshots to every live session 0 context. Do not ship with two unsynchronized session 0 contexts.

## Lifecycle

### Boot / audioserver start

- AudioPolicy creates the pinned music effect.
- Driver cache is empty; session 0 gate defaults to false; audio passes through.
- Boot/sticky App service starts, sends full state, and activates global mode when configured.

### App process killed in global mode

- Binder removes only the App control handle.
- Policy handle keeps the effect module enabled.
- `ViperContext`, graph, and resources continue processing.
- If the policy context is later recreated without restarting `audioserver`, it restores from `Session0StateCache`.

### App process killed in dynamic mode

- session 0 gate remains false.
- App-owned dynamic session handles disappear, matching the selected scope; dynamic-session persistence is not promised.

### audioserver restart

- context and process-global cache are lost.
- policy effect returns in safe bypass.
- App boot/sticky service replays saved state and reactivates global mode.

## Error Handling

- A malformed audio-effects config is never mounted.
- Failure to add policy processing leaves the existing App-owned behavior available; it must not prevent the driver from loading.
- Cache restore failure falls back to defaults with `session0_active=false`.
- Resource snapshot allocation failure leaves the previous committed cache entry active.
- Mode-switch activation occurs only after full-state dispatch succeeds.

## Diagnostics

IEM telemetry version 4 exposes the current audio session, session 0 active gate, cache generation, context instance ID, and live session 0 context count. These fields are control-plane diagnostics only; they are not read from the audio callback. A healthy pinned global path reports audio session `0`, active `true`, and exactly one live session 0 context.
- Unknown/nonboolean values for `0x120F0` are rejected.

## Verification

### Native host tests

- session 0 scalar and IEM state survive context destruction/recreation;
- session 0 committed DDC and convolver resources survive recreation;
- incomplete resource uploads are not cached;
- nonzero sessions neither inherit nor modify the global cache;
- active=false is bit-transparent while the policy effect is enabled;
- active=true resumes processing without graph rebuild on the audio thread;
- process-time allocation and lock audits remain clean.

### Module patch tests

- XML and legacy config fixtures cover missing/existing postprocess blocks;
- repeated patching produces one ViPER entry;
- unrelated OEM effects and stream entries remain byte-for-byte equivalent where untouched;
- malformed output is rejected and not mounted.

### App tests

- global mode dispatches inactive -> full state/resources -> active;
- dynamic mode writes inactive before creating session effects;
- service cleanup releases handles without writing inactive;
- existing global/dynamic preference behavior is preserved.

### Device acceptance

1. Enable global mode and a clearly measurable EQ/filter.
2. Start music and verify processing.
3. Kill or force-stop the manager process.
4. Verify current playback remains processed without interruption.
5. Stop playback, start a new music stream while the App remains stopped, and verify cached processing returns.
6. Restart the App and verify it reconnects without resetting sound.
7. Switch to dynamic mode and verify the pinned global path is bypassed with no double processing.
8. Restart `audioserver` and verify safe bypass until the App replays state.

## Source Basis

- AOSP XML postprocessing uses `<postprocess><stream type="music"><apply effect="..."/></stream></postprocess>`.
- Legacy AOSP configuration uses `output_session_processing { music { effect_name { } } }`.
- Audio session `0` is `AUDIO_SESSION_OUTPUT_MIX` for effects applied to the output mix.
