# IEM Plug-in Suite Provenance

- Repository: `https://git.iem.at/audioplugins/IEMPluginSuite.git`
- Commit: `39de1dd5883f1bd8d65fe1662487f2470a1d7b55`
- License: GPL-3.0-or-later

| Upstream path | Local path | Status | Compiled |
|---|---|---|---|
| `resources/efficientSHvanilla.cpp` | `efficientSHvanilla.cpp` | unchanged | yes |
| `resources/efficientSHvanilla.h` | `efficientSHvanilla.h` | unchanged reference | no; includes JUCE helpers |
| `resources/Quaternion.h` | `Quaternion.h` | unchanged reference | no; includes JUCE vector types |

`iem/SphericalHarmonics.h` wraps the unchanged generated spherical-harmonic
functions with the fixed ACN/SN3D convention. `iem/Quaternion.h` is a
JUCE-free scalar adaptation of the pinned quaternion multiplication and vector
rotation formulas.

JUCE, PluginProcessor classes, editors, OSC/MIDI support, desktop state, file
pickers, and runtime resource loading are intentionally excluded from the
Android audio driver.
