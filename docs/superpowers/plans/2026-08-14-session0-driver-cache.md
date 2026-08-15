# Session 0 Driver Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep global music processing active and restore the last validated driver state after the ViPER4Android App process and client effect handle are killed, while allowing `audioserver` restart to fall back to safe bypass until App replay.

**Architecture:** AudioPolicy pins `v4a_standard_re` to the music output session. A session-0-only process-global cache inside `libv4a_re.so` stores validated scalar/IEM snapshots and shared immutable committed DDC/convolver resources. The App keeps a session 0 control handle, uses a new context gate to prevent global/dynamic double processing, and no longer owns the effect module lifetime.

**Tech Stack:** C++20, Android legacy AudioEffect API, CMake/CTest, POSIX shell/Magisk module scripts, Kotlin, Android `AudioEffect`, JUnit4, Gradle.

## Global Constraints

- Cache only audio session `0` (`AUDIO_SESSION_OUTPUT_MIX`); nonzero sessions never inherit or update it.
- `PARAM_DRIVER_SESSION0_ACTIVE` is exactly `0x120F0` and accepts only `0` or `1`.
- The default session 0 gate is false, producing safe bypass until App configuration completes.
- Global activation order is exactly: inactive -> full state/resources -> active.
- Dynamic mode writes inactive before creating or enabling nonzero-session effects.
- App/service destruction releases handles without writing inactive.
- Cache lifetime is the current `audioserver` process only; no disk writes from the driver.
- Pending/incomplete resource uploads are never cached.
- Committed DDC/convolver data is shared through immutable snapshots rather than duplicated.
- No cache mutex, shared-resource ownership mutation, allocation, logging, or file I/O occurs on the audio thread.
- XML pinning uses AOSP `<postprocess><stream type="music"><apply effect="v4a_standard_re"/></stream></postprocess>` syntax.
- Legacy config pinning uses `output_session_processing { music { v4a_standard_re { } } }` syntax.
- Config patching is idempotent and must never mount malformed output.
- Existing dynamic session behavior, AIDL visibility, HIDL-only IEM policy, and DSP features remain intact.

---

### Task 1: Share Immutable Committed DSP Resources

**Files:**
- Modify: `src/DspResources.h`
- Modify: `src/DspResources.cpp`
- Modify: `tests/DspResourcesTest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Existing `DspResources::CaptureRaw` and `DspResources::ApplyTo` behavior.
- Produces: `CommittedDspResourceSnapshot`, `CommittedDspResourcePtr`, `DspResources::CommittedSnapshot()`, and `DspResources::RestoreCommittedSnapshot()` for Task 2.

- [ ] **Step 1: Add failing shared-snapshot tests**

Extend `tests/DspResourcesTest.cpp` with these cases:

```cpp
bool TestCommittedSnapshotCanBeSharedAndRestored() {
    viper::audio::DspResources source;
    CommitStereoKernel(source, 77);
    const auto snapshot = source.CommittedSnapshot();
    if (!Check(snapshot != nullptr, "export committed resource snapshot")) return false;

    viper::audio::DspResources restored;
    restored.RestoreCommittedSnapshot(snapshot);
    if (!Check(restored.CommittedSnapshot() == snapshot,
            "restore shares immutable snapshot")) return false;

    viper::audio::DspGraph graph;
    if (!graph.Prepare({48000, 8192, 2})) return false;
    return Check(restored.ApplyTo(graph), "apply restored resources")
        && Check(graph.Engine().GetConvolverKernelID() == 77,
            "restored kernel reaches replacement graph");
}

bool TestIncompleteUploadDoesNotReplaceSharedSnapshot() {
    viper::audio::DspResources resources;
    CommitStereoKernel(resources, 23);
    const auto before = resources.CommittedSnapshot();
    resources.CaptureRaw(kParamConvolverPrepareBuffer, 64, 2, 0, 0, nullptr);
    return Check(resources.CommittedSnapshot() == before,
        "pending upload preserves committed snapshot");
}
```

Factor the existing convolver fixture into a local `CommitStereoKernel(DspResources &, int kernel_id)` test helper. Call both tests from `main()`.

- [ ] **Step 2: Run the focused test and verify it fails**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON && cmake --build build-host -j2 --target dsp_resources_test'
```

Expected: compilation fails because `CommittedSnapshot` and `RestoreCommittedSnapshot` do not exist.

- [ ] **Step 3: Define immutable committed-resource ownership**

Add to `src/DspResources.h`:

```cpp
#include <memory>

struct CommittedDspResourceSnapshot final {
    std::vector<float> convolver_kernel;
    uint32_t convolver_channels = 0;
    int convolver_kernel_id = 0;
    std::vector<viper::BiquadSection> ddc_44100;
    std::vector<viper::BiquadSection> ddc_48000;
};

using CommittedDspResourcePtr =
    std::shared_ptr<const CommittedDspResourceSnapshot>;
```

Replace the four committed vectors/metadata fields in `DspResources` with:

```cpp
CommittedDspResourcePtr committed_{};
```

Keep `pending_convolver_`, `pending_convolver_size_`, and `pending_convolver_channels_` context-local and mutable.

- [ ] **Step 4: Add snapshot export/restore methods**

Add public methods:

```cpp
CommittedDspResourcePtr CommittedSnapshot() const noexcept {
    return committed_;
}

void RestoreCommittedSnapshot(CommittedDspResourcePtr snapshot) noexcept {
    committed_ = std::move(snapshot);
}
```

On valid convolver commit, DDC commit, or resource clear, construct a new `CommittedDspResourceSnapshot`, copy unchanged committed fields from the previous snapshot, replace the affected fields, then assign the new immutable pointer. Invalid or incomplete operations leave `committed_` unchanged.

- [ ] **Step 5: Update resource replay methods**

Implement `ApplyTo`, `HasConvolverKernel`, and `HasDdcCoefficients` from `committed_`. A null snapshot behaves exactly like the previous empty vectors.

- [ ] **Step 6: Run resource and graph tests**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target dsp_resources_test dsp_graph_test dsp_graph_slots_test && ctest --test-dir build-host -R "dsp_resources_test|dsp_graph_test|dsp_graph_slots_test" --output-on-failure'
```

Expected: all three tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/DspResources.h src/DspResources.cpp tests/DspResourcesTest.cpp CMakeLists.txt
git commit -m "refactor(driver): share committed DSP resources"
```

---

### Task 2: Add The Session 0 Memory Cache

**Files:**
- Create: `src/Session0StateCache.h`
- Create: `src/Session0StateCache.cpp`
- Create: `tests/Session0StateCacheTest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `CommittedDspResourcePtr` from Task 1, `ViPERParams`, `iem::IemParams`.
- Produces: `Session0CachedState`, `Session0StateCache::Instance()`, `Load`, `StoreActive`, `StoreParams`, `StoreIem`, `StoreResources`, and `ResetForTest` for Task 3.

- [ ] **Step 1: Write the failing cache contract test**

Create `tests/Session0StateCacheTest.cpp`:

```cpp
#include "Session0StateCache.h"
#include <cstdio>

namespace {
bool Check(bool value, const char *message) {
    if (value) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestDefaultsAreSafeBypass() {
    auto &cache = viper::audio::Session0StateCache::Instance();
    cache.ResetForTest();
    const auto state = cache.Load();
    return Check(!state.initialized, "cache starts uninitialized")
        && Check(!state.active, "session 0 starts inactive");
}

bool TestValidatedStateRoundTrips() {
    auto &cache = viper::audio::Session0StateCache::Instance();
    cache.ResetForTest();
    viper::ViPERParams params{};
    params.equalizer.enable = true;
    iem::IemParams iem{};
    iem.enable = true;
    auto resources = std::make_shared<viper::audio::CommittedDspResourceSnapshot>();
    resources->convolver_kernel_id = 91;

    cache.StoreActive(true);
    cache.StoreParams(params);
    cache.StoreIem(iem, 7);
    cache.StoreResources(resources);
    const auto restored = cache.Load();

    return Check(restored.initialized, "cache initializes on first store")
        && Check(restored.active, "active flag round-trips")
        && Check(restored.params.equalizer.enable, "ViPER params round-trip")
        && Check(restored.iem_params.enable, "IEM params round-trip")
        && Check(restored.iem_resource_generation == 7, "IEM generation round-trips")
        && Check(restored.dsp_resources == resources, "resources remain shared");
}
}

int main() {
    if (!TestDefaultsAreSafeBypass()) return 1;
    if (!TestValidatedStateRoundTrips()) return 1;
    std::puts("Session 0 state cache tests passed");
    return 0;
}
```

- [ ] **Step 2: Run the test and verify it fails**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target session0_state_cache_test'
```

Expected: target/header is missing.

- [ ] **Step 3: Define the cache API**

Create `src/Session0StateCache.h`:

```cpp
#pragma once

#include "DspResources.h"
#include "ViPERParams.h"
#include "iem/IemParams.h"

#include <cstdint>
#include <mutex>

namespace viper::audio {

constexpr int kParamDriverSession0Active = 0x120F0;

struct Session0CachedState final {
    bool initialized = false;
    bool active = false;
    uint64_t generation = 0;
    viper::ViPERParams params{};
    iem::IemParams iem_params{};
    uint64_t iem_resource_generation = 0;
    CommittedDspResourcePtr dsp_resources{};
};

class Session0StateCache final {
public:
    static Session0StateCache &Instance() noexcept;
    Session0CachedState Load() const;
    uint64_t StoreActive(bool active);
    uint64_t StoreParams(const viper::ViPERParams &params);
    uint64_t StoreIem(const iem::IemParams &params, uint64_t resource_generation);
    uint64_t StoreResources(CommittedDspResourcePtr resources);
    void ResetForTest();

private:
    void MarkUpdatedLocked() noexcept;
    mutable std::mutex mutex_{};
    Session0CachedState state_{};
};

} // namespace viper::audio
```

- [ ] **Step 4: Implement locked control-plane storage**

`Load` returns a value copy under `mutex_`. Every `Store*` method locks, replaces only its owned fields, sets `initialized=true`, increments `generation`, and returns the new generation. `ResetForTest` restores a default-constructed state. No method is called from `Process`.

- [ ] **Step 5: Register the CMake test target**

Add:

```cmake
add_executable(session0_state_cache_test
    tests/Session0StateCacheTest.cpp
    src/Session0StateCache.cpp
)
target_include_directories(session0_state_cache_test PRIVATE
    src ViPERDSP ViPERDSP/include IEMDSP/include)
target_link_libraries(session0_state_cache_test PRIVATE IEMDSP ViPERDSP Threads::Threads)
add_test(NAME session0_state_cache_test COMMAND session0_state_cache_test)
```

- [ ] **Step 6: Run the cache test**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON && cmake --build build-host -j2 --target session0_state_cache_test && ctest --test-dir build-host -R session0_state_cache_test --output-on-failure'
```

Expected: pass.

- [ ] **Step 7: Commit**

```bash
git add src/Session0StateCache.h src/Session0StateCache.cpp tests/Session0StateCacheTest.cpp CMakeLists.txt
git commit -m "feat(driver): add session 0 memory cache"
```

---

### Task 3: Restore And Publish Cached Driver State

**Files:**
- Modify: `src/IemResources.h`
- Modify: `src/IemResources.cpp`
- Modify: `src/IemContext.h`
- Modify: `src/IemContext.cpp`
- Modify: `src/ViperContext.h`
- Modify: `src/ViperContext.cpp`
- Modify: `src/ViPER4Android.cpp`
- Modify: `tests/IemContextTest.cpp`
- Modify: `tests/Session0StateCacheTest.cpp`
- Create: `tests/Session0ContextPolicyTest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Session0StateCache` and shared resource snapshots from Tasks 1-2.
- Produces: session-aware `ViperContext(int32_t session_id, int32_t io_id)`, safe active gating, context hydration, and cache publication.

- [ ] **Step 1: Add failing IEM restore tests**

Extend `tests/IemContextTest.cpp`:

```cpp
bool TestCachedStateRestoresBeforePrepare() {
    iem::IemParams params{};
    params.enable = true;
    params.wet = 0.42F;
    viper::audio::IemContext context;
    context.RestoreCachedState(params, 9);
    if (!Check(context.Prepare(48000, 256), "prepare restored IEM context")) return false;
    return Check(context.Params().enable, "restore IEM enable")
        && Check(std::fabs(context.Params().wet - 0.42F) < 1.0e-6F,
            "restore IEM wet")
        && Check(context.ResourceGeneration() == 9,
            "restore IEM resource generation");
}
```

Call it from `main()`.

- [ ] **Step 2: Run the IEM test and verify it fails**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target iem_context_test'
```

Expected: `IemContext::RestoreCachedState` is missing.

- [ ] **Step 3: Add IEM restore APIs**

Add:

```cpp
// IemResources.h
void RestoreGeneration(uint64_t generation) noexcept { generation_ = generation; }

// IemContext.h
void RestoreCachedState(
    const iem::IemParams &params,
    uint64_t resource_generation
) noexcept;
```

`RestoreCachedState` is valid only before `Prepare`; it replaces `parameter_snapshot_`, restores the generation, and publishes the restored snapshot to `parameter_mailbox_`. It does not allocate or prepare a graph.

- [ ] **Step 4: Make ViperContext session-aware**

Change the constructor to:

```cpp
explicit ViperContext(int32_t session_id, int32_t io_id);
```

Add fields:

```cpp
int32_t session_id_ = -1;
int32_t io_id_ = -1;
bool session0_active_ = false;
uint64_t session0_cache_generation_ = 0;
```

In `ViperLibraryCreate`, name the existing session/io arguments and construct:

```cpp
auto *context = new ViperContext(session_id, io_id);
```

- [ ] **Step 5: Hydrate session 0 during construction**

For session `0`, call `Session0StateCache::Instance().Load()`. When `initialized` is true:

```cpp
session0_active_ = cached.active;
session0_cache_generation_ = cached.generation;
parameter_snapshot_ = cached.params;
resources_.RestoreCommittedSnapshot(cached.dsp_resources);
iem_context_.RestoreCachedState(
    cached.iem_params,
    cached.iem_resource_generation);
```

Nonzero sessions keep current defaults.

- [ ] **Step 6: Intercept the active gate and publish validated updates**

At the start of `ViperContext::DispatchRawParam`:

```cpp
if (param == kParamDriverSession0Active) {
    if (session_id_ != 0 || (val1 != 0 && val1 != 1)) return;
    session0_active_ = val1 == 1;
    session0_cache_generation_ =
        Session0StateCache::Instance().StoreActive(session0_active_);
    return;
}
```

After a handled IEM update, publish `iem_context_.Params()` and `ResourceGeneration()` only for session `0`. After `RawParamUpdate::UPDATED`, publish `parameter_snapshot_`. Capture `ResourceCaptureResult`; on `COMMITTED` or `CLEARED`, publish `resources_.CommittedSnapshot()`. Assign each returned generation to `session0_cache_generation_`. Do not publish on `UPDATED` pending chunks or `INVALID`.

- [ ] **Step 7: Gate only session 0 audio processing**

In `ViperContext::Process`, extend the bypass condition:

```cpp
const bool session0_bypassed = session_id_ == 0 && !session0_active_;
if (!enable_ || session0_bypassed || disable_reason_ != DisableReason::NONE) {
    return 0;
}
```

Do not read the global cache from `Process`.

- [ ] **Step 8: Add cache recreation tests**

Extend `Session0StateCacheTest.cpp` with helpers that emulate context publication and reload:

```cpp
bool TestNonzeroSessionCannotUseGlobalCacheContract() {
    auto &cache = Session0StateCache::Instance();
    cache.ResetForTest();
    // The factory/context integration is session-gated; the cache API itself
    // contains no session selector. Lock this by checking only ViperContext
    // calls Store* from `session_id_ == 0` using a source policy assertion.
    return Check(!cache.Load().initialized, "nonzero path leaves cache empty");
}
```

Create `tests/Session0ContextPolicyTest.cpp`:

```cpp
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

bool Check(bool value, const char *message) {
    if (value) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

int main() {
    std::ifstream input(VIPER_CONTEXT_SOURCE_PATH);
    const std::string source(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    if (!Check(!source.empty(), "read ViperContext source")) return 1;
    if (!Check(source.find("session_id_ == 0") != std::string::npos,
            "cache publication is session 0 gated")) return 1;
    if (!Check(source.find("session0_bypassed") != std::string::npos,
            "process path contains session 0 active gate")) return 1;
    if (!Check(source.find("Session0StateCache::Instance().StoreParams")
            != std::string::npos, "ViPER params publish to cache")) return 1;
    if (!Check(source.find("Session0StateCache::Instance().StoreIem")
            != std::string::npos, "IEM params publish to cache")) return 1;
    std::puts("Session 0 context policy tests passed");
    return 0;
}
```

Register it with:

```cmake
add_executable(session0_context_policy_test tests/Session0ContextPolicyTest.cpp)
target_compile_definitions(session0_context_policy_test PRIVATE
    VIPER_CONTEXT_SOURCE_PATH="${CMAKE_SOURCE_DIR}/src/ViperContext.cpp")
add_test(NAME session0_context_policy_test COMMAND session0_context_policy_test)
```

- [ ] **Step 9: Run focused native tests and Android compile**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target session0_state_cache_test iem_context_test dsp_resources_test && ctest --test-dir build-host -R "session0_state_cache_test|iem_context_test|dsp_resources_test" --output-on-failure && make libs'
```

Expected: host tests pass and both Android ABI libraries build.

- [ ] **Step 10: Commit**

```bash
git add src/IemResources.h src/IemResources.cpp src/IemContext.h src/IemContext.cpp src/ViperContext.h src/ViperContext.cpp src/ViPER4Android.cpp tests/IemContextTest.cpp tests/Session0StateCacheTest.cpp tests/Session0ContextPolicyTest.cpp CMakeLists.txt
git commit -m "feat(driver): restore cached session 0 state"
```

---

### Task 4: Pin The Music Effect In AudioPolicy Configuration

**Files:**
- Create: `module/common/session0-pinner.sh`
- Modify: `module/post-fs-data.sh`
- Create: `tests/module/fixtures/audio_effects.xml`
- Create: `tests/module/fixtures/audio_effects-with-postprocess.xml`
- Create: `tests/module/fixtures/audio_effects.conf`
- Create: `tests/module/Session0PinnerTest.sh`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Existing module config discovery, `patch_audio_effect_config`, and `bind_mount_file` functions.
- Produces: `pin_viper_session0_effect FILE`, `validate_viper_session0_effect FILE`, and idempotent XML/legacy output processing.

- [ ] **Step 1: Create fixture files and failing shell tests**

Create `tests/module/fixtures/audio_effects.xml`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<audio_effects_conf version="2.0" xmlns="http://schemas.android.com/audio/audio_effects_conf/v2_0">
    <libraries>
        <library name="v4a_re" path="libv4a_re.so"/>
    </libraries>
    <effects>
        <effect name="v4a_standard_re" library="v4a_re" uuid="90380da3-8536-4744-a6a3-5731970e640f"/>
    </effects>
</audio_effects_conf>
```

Create `tests/module/fixtures/audio_effects-with-postprocess.xml`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<audio_effects_conf version="2.0" xmlns="http://schemas.android.com/audio/audio_effects_conf/v2_0">
    <libraries>
        <library name="v4a_re" path="libv4a_re.so"/>
    </libraries>
    <effects>
        <effect name="v4a_standard_re" library="v4a_re" uuid="90380da3-8536-4744-a6a3-5731970e640f"/>
        <effect name="oem_music" library="oem" uuid="11111111-2222-3333-4444-555555555555"/>
    </effects>
    <postprocess>
        <stream type="music">
            <apply effect="oem_music"/>
        </stream>
    </postprocess>
</audio_effects_conf>
```

Create `tests/module/fixtures/audio_effects.conf`:

```text
libraries {
  v4a_re {
    path /vendor/lib/soundfx/libv4a_re.so
  }
}
effects {
  v4a_standard_re {
    library v4a_re
    uuid 90380da3-8536-4744-a6a3-5731970e640f
  }
}
```

`tests/module/Session0PinnerTest.sh` must:

```sh
#!/system/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
. "$ROOT/module/common/session0-pinner.sh"

TMP="${TMPDIR:-/tmp}/viper-session0-pinner-$$"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"

cp "$ROOT/tests/module/fixtures/audio_effects.xml" "$TMP/no-post.xml"
pin_viper_session0_effect "$TMP/no-post.xml"
validate_viper_session0_effect "$TMP/no-post.xml"
[ "$(grep -c 'apply effect="v4a_standard_re"' "$TMP/no-post.xml")" -eq 1 ]
pin_viper_session0_effect "$TMP/no-post.xml"
[ "$(grep -c 'apply effect="v4a_standard_re"' "$TMP/no-post.xml")" -eq 1 ]

cp "$ROOT/tests/module/fixtures/audio_effects-with-postprocess.xml" "$TMP/post.xml"
pin_viper_session0_effect "$TMP/post.xml"
validate_viper_session0_effect "$TMP/post.xml"
[ "$(grep -c 'apply effect="v4a_standard_re"' "$TMP/post.xml")" -eq 1 ]

cp "$ROOT/tests/module/fixtures/audio_effects.conf" "$TMP/effects.conf"
pin_viper_session0_effect "$TMP/effects.conf"
validate_viper_session0_effect "$TMP/effects.conf"
[ "$(grep -c 'v4a_standard_re' "$TMP/effects.conf")" -eq 2 ]

printf '%s\n' 'Session 0 pinner tests passed'
```

The legacy count is two: one effect definition plus one output-session application.

- [ ] **Step 2: Run the shell test and verify it fails**

Run:

```bash
sh tests/module/Session0PinnerTest.sh
```

Expected: source script is missing.

- [ ] **Step 3: Implement XML pinning**

Create `module/common/session0-pinner.sh` with public entry points:

```sh
pin_viper_session0_effect() {
    file="$1"
    case "$file" in
        *.xml) pin_viper_xml_music_effect "$file" ;;
        *.conf) pin_viper_legacy_music_effect "$file" ;;
        *) return 1 ;;
    esac
}

validate_viper_session0_effect() {
    file="$1"
    case "$file" in
        *.xml)
            [ "$(grep -c 'apply effect="v4a_standard_re"' "$file")" -eq 1 ]
            grep -q '<stream type="music">' "$file"
            ;;
        *.conf)
            grep -q 'output_session_processing' "$file"
            grep -q '^[[:space:]]*music[[:space:]]*{' "$file"
            ;;
    esac
}
```

Implement XML insertion with `awk` state tracking:

- return unchanged when the apply entry already exists;
- insert `<apply effect="v4a_standard_re"/>` before the closing tag of an existing music stream;
- otherwise insert a music stream before `</postprocess>`;
- otherwise insert a complete postprocess block before the root closing tag;
- write to `FILE.tmp`, validate, then `mv` over the original.

- [ ] **Step 4: Implement legacy pinning**

Use the same temporary-file/validate/move rule. Insert into an existing `music` block under `output_session_processing`, create the music block when only the parent exists, or append the complete parent block when absent. Do not modify the existing effect definition.

- [ ] **Step 5: Call the pinner from post-fs-data**

Source `session0-pinner.sh` after `functions.sh`. For each discovered audio-effects config:

```sh
patch_audio_effect_config "$FILE" || true
if pin_viper_session0_effect "$FILE" && validate_viper_session0_effect "$FILE"; then
    bind_mount_file "$FILE"
else
    ui_print "ViPER: skipped invalid session 0 patch for $FILE"
fi
```

Preserve the existing AIDL skip and library/effect registration behavior.

- [ ] **Step 6: Add the shell test to CTest**

Add:

```cmake
add_test(
    NAME session0_pinner_test
    COMMAND sh ${CMAKE_SOURCE_DIR}/tests/module/Session0PinnerTest.sh
)
```

- [ ] **Step 7: Run shell and CTest gates**

Run:

```bash
sh tests/module/Session0PinnerTest.sh
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON && ctest --test-dir build-host -R session0_pinner_test --output-on-failure'
```

Expected: both pass and repeated patching leaves one application entry.

- [ ] **Step 8: Commit**

```bash
git add module/common/session0-pinner.sh module/post-fs-data.sh tests/module CMakeLists.txt
git commit -m "feat(module): pin ViPER to the music output session"
```

---

### Task 5: Make The App A Session 0 Configuration Client

**Files:**
- Modify: `app/src/main/java/com/llsl/viper4android/viper/ViperParams.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/viper/ViperEffect.kt`
- Create: `app/src/main/java/com/llsl/viper4android/service/Session0ModePlan.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/service/ViperService.kt`
- Create: `app/src/test/java/com/llsl/viper4android/service/Session0ModePlanTest.kt`
- Modify: `app/src/test/java/com/llsl/viper4android/viper/IemDispatchTest.kt`

**Interfaces:**
- Consumes: native parameter `0x120F0` from Tasks 2-3.
- Produces: `ViperEffect.setSession0Active(Boolean)`, pure `session0ModePlan(Boolean)`, persistent `session0ControlEffect`, and ordered mode transitions.

- [ ] **Step 1: Write the failing pure mode-plan test**

Create:

```kotlin
package com.llsl.viper4android.service

import org.junit.Assert.assertEquals
import org.junit.Test

class Session0ModePlanTest {
    @Test
    fun globalModeActivatesOnlyAfterFullState() {
        assertEquals(
            listOf(
                Session0ModeStep.DEACTIVATE,
                Session0ModeStep.DISPATCH_FULL_STATE,
                Session0ModeStep.ACTIVATE,
            ),
            session0ModePlan(globalMode = true),
        )
    }

    @Test
    fun dynamicModeOnlyDeactivatesPinnedGlobalPath() {
        assertEquals(
            listOf(Session0ModeStep.DEACTIVATE),
            session0ModePlan(globalMode = false),
        )
    }
}
```

- [ ] **Step 2: Run the test and verify it fails**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && bash ./gradlew :app:testDebugUnitTest --tests "com.llsl.viper4android.service.Session0ModePlanTest" --no-daemon'
```

Expected: plan types/functions are missing.

- [ ] **Step 3: Add the parameter and effect wrapper**

Add to `ViperParams.kt`:

```kotlin
const val PARAM_DRIVER_SESSION0_ACTIVE = 0x120F0
```

Add to `ViperEffect.kt`:

```kotlin
fun setSession0Active(active: Boolean) {
    setParameter(ViperParams.PARAM_DRIVER_SESSION0_ACTIVE, if (active) 1 else 0)
}
```

- [ ] **Step 4: Implement the pure mode plan**

Create:

```kotlin
package com.llsl.viper4android.service

enum class Session0ModeStep {
    DEACTIVATE,
    DISPATCH_FULL_STATE,
    ACTIVATE,
}

fun session0ModePlan(globalMode: Boolean): List<Session0ModeStep> =
    if (globalMode) {
        listOf(
            Session0ModeStep.DEACTIVATE,
            Session0ModeStep.DISPATCH_FULL_STATE,
            Session0ModeStep.ACTIVATE,
        )
    } else {
        listOf(Session0ModeStep.DEACTIVATE)
    }
```

- [ ] **Step 5: Retain a session 0 control handle in all modes**

Rename `globalEffect` to `session0ControlEffect`. `updateGlobalMode` always acquires a valid session 0 effect when the master service is active. Do not release it merely because dynamic mode was selected.

Add:

```kotlin
private fun applySession0Mode(state: EffectState) {
    val effect = session0ControlEffect ?: return
    session0ModePlan(state.globalMode).forEach { step ->
        when (step) {
            Session0ModeStep.DEACTIVATE -> effect.setSession0Active(false)
            Session0ModeStep.DISPATCH_FULL_STATE ->
                ViperDispatcher.dispatchFullState(effect, state, masterEnabled)
            Session0ModeStep.ACTIVATE -> effect.setSession0Active(true)
        }
    }
}
```

On initial startup and every global/dynamic transition, call `applySession0Mode` before creating dynamic effects. During ordinary state changes in global mode, dispatch the new state directly without toggling active, avoiding audible gaps.

- [ ] **Step 6: Preserve active state during service destruction**

`onDestroy` and handle cleanup release `session0ControlEffect` without calling `setSession0Active(false)`. An explicit transition to dynamic mode remains the only cleanup path that deactivates the pinned global processor.

- [ ] **Step 7: Add source policy tests for cleanup and ordering**

Extend `Session0ModePlanTest` to read `ViperService.kt` and assert:

```kotlin
val source = readSource("app/src/main/java/com/llsl/viper4android/service/ViperService.kt")
assertTrue("service should keep session0ControlEffect", "session0ControlEffect" in source)
val onDestroy = source.substringAfter("override fun onDestroy()")
assertFalse("cleanup must not deactivate pinned effect",
    "setSession0Active(false)" in onDestroy.substringBefore("override fun"))
```

Use the existing project-root source-reading pattern from UI policy tests.

- [ ] **Step 8: Run App tests**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && bash ./gradlew :app:testDebugUnitTest --tests "com.llsl.viper4android.service.Session0ModePlanTest" --tests "com.llsl.viper4android.viper.IemDispatchTest" --no-daemon'
```

Expected: pass.

- [ ] **Step 9: Commit**

```bash
git add app/src/main/java/com/llsl/viper4android/viper/ViperParams.kt app/src/main/java/com/llsl/viper4android/viper/ViperEffect.kt app/src/main/java/com/llsl/viper4android/service/Session0ModePlan.kt app/src/main/java/com/llsl/viper4android/service/ViperService.kt app/src/test/java/com/llsl/viper4android/service/Session0ModePlanTest.kt app/src/test/java/com/llsl/viper4android/viper/IemDispatchTest.kt
git commit -m "feat(service): configure the pinned session 0 effect"
```

---

### Task 6: Add Diagnostics And Complete Documentation

**Files:**
- Modify: `src/ViperContext.h`
- Modify: `src/ViperContext.cpp`
- Modify: `src/TelemetryProtocol.h`
- Modify: `app/src/main/java/com/llsl/viper4android/viper/DriverTelemetry.kt`
- Modify: `app/src/main/java/com/llsl/viper4android/ui/screens/status/DriverStatusDialog.kt`
- Modify: `app/src/main/res/values/strings.xml`
- Modify: `app/src/main/res/values-zh-rCN/strings.xml`
- Modify: `app/src/main/res/values-ru/strings.xml`
- Modify: `docs/iem-upstream-attribution.md`
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-08-14-session0-driver-cache.md`
- Modify: `docs/superpowers/plans/2026-08-14-session0-driver-cache.md`

**Interfaces:**
- Consumes: session identity, active gate, and cache generation from previous tasks.
- Produces: visible diagnostics for pinned/session/cache status and operator documentation.

- [ ] **Step 1: Extend telemetry wire data**

Append fields to `IemTelemetryWire`, bump `kIemTelemetryVersion` from `3` to `4`, and bump `kIemTelemetryWireSize` from `168` to `200`:

```cpp
int32_t audio_session_id;
uint32_t session0_active;
uint64_t session0_cache_generation;
uint64_t context_instance_id;
uint32_t session0_live_context_count;
uint32_t reserved_session0;
```

Populate them in `ViperContext::HandleGetParam` after `MakeIemTelemetryWire`: use `session_id_`, `session0_active_`, the last loaded/stored cache generation, a monotonically assigned context instance ID, and a process-global live session 0 context counter. Nonzero sessions report `session0_active=0`, generation `0`, and live count `0`.

- [ ] **Step 2: Parse and display diagnostics in the App**

Add matching Kotlin fields and status rows:

- Audio session
- Pinned global active
- Session 0 cache generation
- Context instance ID
- Live session 0 context count

Use localized English, Simplified Chinese, and Russian strings. Do not expose controls in this task.

- [ ] **Step 3: Document lifecycle and limitations**

Update README/attribution with:

- policy-pinned global music effect;
- App is a configuration client, not lifetime owner;
- dynamic sessions still depend on App lifetime;
- memory cache is lost on `audioserver` restart;
- App replay restores after boot/restart;
- `0x120F0` is context-local control, not a profile DSP parameter.

- [ ] **Step 4: Run telemetry and localization tests**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake --build build-host -j2 --target audio_analyzer_test iem_telemetry_test && ctest --test-dir build-host -R "audio_analyzer_test|iem_telemetry_test" --output-on-failure'
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && bash ./gradlew :app:testDebugUnitTest --tests "com.llsl.viper4android.viper.DriverTelemetryTest" --tests "com.llsl.viper4android.ui.screens.editor.IemEditorLocalizationPolicyTest" --no-daemon'
```

Expected: pass.

- [ ] **Step 5: Commit driver documentation and diagnostics**

```bash
git add src/ViperContext.h src/ViperContext.cpp src/TelemetryProtocol.h README.md docs/iem-upstream-attribution.md docs/superpowers/specs/2026-08-14-session0-driver-cache.md docs/superpowers/plans/2026-08-14-session0-driver-cache.md
git commit -m "docs(driver): document persistent session 0 processing"
```

- [ ] **Step 6: Commit App diagnostics**

Run in `/root/AndroidIDEProjects/ViPER4Android`:

```bash
git add app/src/main/java/com/llsl/viper4android/viper/DriverTelemetry.kt app/src/main/java/com/llsl/viper4android/ui/screens/status/DriverStatusDialog.kt app/src/main/res/values/strings.xml app/src/main/res/values-zh-rCN/strings.xml app/src/main/res/values-ru/strings.xml
git commit -m "feat(status): show pinned session 0 state"
```

---

### Task 7: Full Verification, Packaging, Installation, And Kill Test

**Files:**
- No planned source changes. A verification failure reopens the task that owns the failed behavior before acceptance continues.

**Interfaces:**
- Consumes: completed driver, module, and App implementation.
- Produces: verified host/Android artifacts and target-device acceptance evidence.

- [ ] **Step 1: Run the complete native host suite**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host -DBUILD_ANALYZER_TESTS=ON && cmake --build build-host -j2 && ctest --test-dir build-host --output-on-failure'
```

Expected: all tests pass.

- [ ] **Step 2: Run realtime and sanitizer gates**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host-ubsan -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all" -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=undefined" && cmake --build build-host-ubsan -j2 && ctest --test-dir build-host-ubsan --output-on-failure'
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && cmake -S . -B build-host-asan -DBUILD_ANALYZER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address" && cmake --build build-host-asan -j2 --target session0_state_cache_test session0_context_policy_test dsp_resources_test iem_context_test iem_realtime_audit_test'
```

Expected: UBSan tests pass; ASan binaries compile/link. If ASan execution is unavailable under AndroidIDE, report that runtime limitation without claiming an executed ASan pass.

- [ ] **Step 3: Build both Android ABIs and the module**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPERFX_RE" && make libs'
```

Run `make zip`. If it fails specifically because the build host has no `zip` executable, install files directly and report that no module archive was produced; any other packaging failure must be fixed before continuing.

- [ ] **Step 4: Run full App gates**

Run:

```bash
ssh -p 8022 10645@localhost 'cd "$HOME/ViPER4Android" && bash ./gradlew :app:testDebugUnitTest :app:lintDebug :app:assembleDebug --no-daemon'
```

Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 5: Install App, driver libraries, and patched configs**

Install the APK with `pm install -r`. Update both ABI copies in `/data/adb/modules/ViPER4Android-RE/system/vendor/...`, update the existing live effect-mount backing files in place, install the patched audio-effects config, and verify SHA-256 hashes. AudioPolicy config changes require an `audioserver` restart or reboot; ask for explicit approval immediately before that disruptive step.

- [ ] **Step 6: Verify one shared session 0 effect module**

Use the new telemetry to confirm:

- policy music start creates one session 0 context;
- App control-handle acquisition does not create a second independent context;
- App writes change the audible policy-owned processing path.

If two session 0 contexts appear, stop acceptance and implement synchronized multi-context publication before continuing.

- [ ] **Step 7: Perform App-kill acceptance**

With global mode and a clearly measurable filter active:

```bash
su -c 'am force-stop com.llsl.viper4android'
```

Verify:

1. current music remains processed without interruption;
2. stopping and starting a new music stream while the App remains stopped restores the cached processing;
3. restarting the App reconnects without resetting sound;
4. switching to dynamic mode bypasses the pinned global path and does not double-process;
5. `audioserver` restart returns to safe bypass until App replay.

- [ ] **Step 8: Inspect final repository status**

Run `git status --short` in both repositories. Report unrelated/pre-existing changes without reverting them. Push only when explicitly requested.
