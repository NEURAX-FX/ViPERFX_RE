# IEM Spatial Audio Attribution

The optional IEM spatial audio engine is derived from the IEM Plug-in Suite:

- Project: https://plugins.iem.at
- Repository: https://git.iem.at/audioplugins/IEMPluginSuite.git
- Pinned commit: `39de1dd5883f1bd8d65fe1662487f2470a1d7b55`
- License: GNU GPL version 3 or later

The integration retains selected upstream algorithm and resource material, adapts JUCE-dependent DSP for the Android real-time driver, and does not include JUCE processors, editors, OSC/MIDI support, desktop state, file pickers, or runtime resource loading. See `docs/iem-upstream-attribution.md` in the source repository for the complete unchanged/adapted file manifest and implementation notes.

## Resource Provenance

The Neumann KU100 far-field HRIR/HRTF data and all 23 headphone equalization impulse responses are attributed to Benjamin Bernschuetz. The KU100 binaural rendering follows the magnitude-least-squares work of Christian Schoerkhuber, Markus Zaunschirm, and Robert Hoeldrich. Source assets are deterministically resampled and embedded at build time; no runtime asset conversion occurs.

## Citations

1. Benjamin Bernschuetz, "A Spherical Far Field HRIR/HRTF Compilation of the Neumann KU 100," AIA-DAGA, 2013. http://audiogroup.web.th-koeln.de/ku100hrir.html
2. Christian Schoerkhuber, Markus Zaunschirm, and Robert Hoeldrich, "Binaural Rendering of Ambisonic Signals via Magnitude Least Squares," Fortschritte der Akustik, DAGA, 2018.

Phase 1 is supported only by the legacy non-AIDL (HIDL-era) driver path.
