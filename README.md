# foo_smart_tempo — public engine source candidate

foobar2000 Win32/x64 tempo/BPM analyzer, with manual tempo controls, configurable algorithms and normal foobar tagging support.

**No music library or BPM research corpus is part of this public repository.** All artist/title lists, manual BPM reference values, holdouts, audio, user paths and reports are retained in the owner's private archive and are never passed into this repository or public CI.

## Public Windows build

CI obtains pinned, publicly redistributable source dependencies separately from `reupen/foobar2000-sdk-unmodified` (official foobar2000 SDK 2026-10-01 snapshot) and `Win32-WTL/WTL`; it does **not** clone any private source repository. KissFFT's minimal sources and license are included under `src/kiss_fft` and `licenses`. `build.bat` builds Win32 and x64 using Visual Studio; `pack_component.bat` produces a standard foobar2000 `.fb2k-component` archive containing only executable binaries and its required KissFFT notice.

This fresh public lineage is not descended from any historical privately qualified Smart Tempo branch/tag. **A successful source build is not a BPM accuracy or foobar runtime qualification.** Real-data and hardware test results must be evaluated offline and privately; only appropriately redacted aggregate outcomes may be published.

Development and CI should use this repository after the initial public candidate is qualified. Previous private release tag `v2.2.0` remains in the separate original archive and is not recreated without verification.

## Configurable MIR sampling (source development candidate)

In foobar2000 Preferences > Tools > Smart Tempo > BPM Analysis Engine, select
**Seconds/window** (5, 10, 15, 20, 30, 45, 60 or 90 seconds) and
**Windows/track** (1, 3, 5, 10, 20 or 50). The historical defaults remain
20 seconds and 50 windows, preserving the prior analysis policy until a user
chooses otherwise. Both options persist per foobar profile; lower settings
usually reduce decode time at a potential accuracy cost. Unknown-length tracks
remain single-pass. These settings control sampling, **not** any tag writes.
No owner audio or BPM ground truth is used for public testing; changes to
accuracy must later be evaluated on private holdouts before a release.

## Copyright

Third-party licenses: foobar2000 SDK notice is in the downloaded SDK at `sdk/sdk-license.txt` (and its pfc/libPPUI subdirectories); WTL MS-PL under `licenses/wtl-MS-PL.txt`; KissFFT under `licenses/kissfft_license.txt`. See upstream projects for their respective copyright details. No third-party benchmark corpus is vendored.
