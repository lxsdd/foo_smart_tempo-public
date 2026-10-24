# Private D12 old-versus-new engine comparison

This is a **local-only** aggregate comparison. No local music, logs, paths,
track labels, per-track BPM values or research references are sent to GitHub.

## Why a different tool?

The older private component `b193b73c2409` predates per-track
`MirSamplingPlan` logging. The newer private component
`1c3a22bdbd57` has it and also contains the synthetically qualified MIR
single-onset gate correction. The ordinary MW1 comparator deliberately
rejects different compilation build IDs and incomplete sampling evidence.

`tools/compare-d12-builds.py` instead permits *different build IDs* while
requiring **identical stable track identities, subsongs, policy schemas,
requested window length/pass count, and 20–80% offset range**. For legacy
logs, it requires exactly one unambiguous global
`foo_smart_tempo prefs (MIR Engine)` settings line. The output explicitly
marks the legacy per-track effective geometry **unverified**.

## Safe collection in foobar2000 (current state: 20 October 2026)

The latest owner-only private component `foo_smart_tempo-private-c0f4c7a8ce0d.fb2k-component`
(sha256 `8AF63FC0F31A09336D353D27FF32A3340A3E2F0B4B9CEAEE1F5607BD587F8E93`,
workflow run `39481279638`) now contains the independent context command
**Smart Tempo > Analyze selected tracks (no tag writes)**.
It analyzes selected tracks even if they already have a BPM tag; its
auto-write, result Update, candidate changes and playlist mutation paths are
disabled/guarded. Ordinary **Smart Tempo Analysis** is NOT this mode.

Synthetic Win32/x64 runtime qualification (PR #21, run `39444331276`)
exercised the real menu GUID using disposable WAV and external CUE:
1 WAV row / 2 CUE rows, disabled Update and a blocked simulated Update,
with unchanged SHA256, length and LastWriteTime. This does **not** establish
D12 reference BPM accuracy, real-audio tag safety or owner acceptance.

### x64-first fixture integrity guard (optional, recommended)

For the first owner acceptance pass, the x64 test instance is sufficient.
Public GitHub CI continues to build and test both Win32 and x64. Keep the
32-bit local instance available for a short final architecture-specific check.

Use *copied* media and CUE/sidecar files in one **isolated fixture directory**,
for example \`C:\private-d12\copies\`. Keep the hash state JSON *outside*
that directory. The helper verifies every file in the tree, not that a foobar
playlist contains precisely 12 subsongs; choose the twelve intended items
in foobar yourself. **Do not run the snapshot against original music.**

Run from PowerShell 5.1+ with a local checkout of these public tools:

\`\`\`powershell
$fixtures = 'C:\private-d12\copies'
$state = 'C:\private-d12\x64-before.json'
.\tools\verify-private-d12-fixtures.ps1 -Phase Before -Root $fixtures -StateFile $state
# In the x64 foobar test profile: select the copied D12 tracks;
# Smart Tempo > Analyze selected tracks (no tag writes); close without writing.
.\tools\verify-private-d12-fixtures.ps1 -Phase After -Root $fixtures -StateFile $state
\`\`\`

Accept \`D12_FIXTURE_INTEGRITY=PASS\` only after the final After step. The
guard checks file identities, size, full SHA256 and LastWriteTimeUtc for
all files (including external .cue files). New/deleted sidecars, symlinks,
overwritten fixture state, any changed bytes or modified timestamps cause
a nonzero failure. Do not repeat Before using the same state file; create
a new state file for each new test. The output contains only status and
aggregate file counts. The locally saved state uses salted hashes of
relative names instead of raw filenames, but it and all console logs
**remain private** and must not be uploaded to GitHub.

The helper does not start foobar, inspect its UI, prove reference BPM
accuracy, or inspect the foobar profile outside this directory. It cannot
rule out changes made to other directories: use an isolated portable test
profile. A passing result means only that the tested copied fixture tree
remained identical across that analysis.

### Local-only baseline and variant evidence

1. Download the two **unpublished owner-only** components while logged
   in to GitHub with repository write access. Baseline:
   `foo_smart_tempo-private-b193b73c2409.fb2k-component` (draft
   `407141776`). New/current variant:
   `foo_smart_tempo-private-c0f4c7a8ce0d.fb2k-component` (draft
   `418771710`). Verify each component against its own draft
   `private-component-manifest.json` SHA256 before installing.
2. Use **disposable private copies** of all 12 tracks and keep their full
   paths identical for both runs. Make separate private backup copies of
   the fixtures before either analysis. Never place original media, BPM
   references or full logs in a public GitHub repository/Actions run.
3. Use the same architecture and analysis settings for both runs:
   window length, pass count and 20–80% sampling offset geometry.
   Enable verbose MIR logging; keep the full console logs privately.
   Do not load both component builds into a single foobar process.
4. For the **older baseline**, the new multi-track no-write command does
   not exist. For a read-only inspection use **Measured BPM candidates**
   on exactly one selected file at a time and do not select its write
   button. If a full baseline batch instead uses ordinary analysis,
   run it only on disposable copies and be prepared to restore those
   copies, since pre-result auto-write is possible.
5. For the **current variant**, select the same twelve *copies* and
   invoke **Analyze selected tracks (no tag writes)**. Do not substitute
   the ordinary analysis action. Keep the console log as a private file.
6. Preserve identical track identity, subsong, policy schema and
   requested sampling geometry; do not silently label a partial or
   mismatched comparison PASS. Restoring altered baseline fixtures must
   preserve the same full paths for the current run.

Local no-write behavior beyond the tested synthetic fixtures remains a
separate owner acceptance gate. The actual held-out reference BPM values
are required to evaluate accuracy; `compare-d12-builds.py` outputs only
reference-independent aggregate transitions.

## Run the comparator offline

Save the complete baseline and current-version verbose consoles as files
on your computer, then run:

```powershell
python .\tools\compare-d12-builds.py `
  --baseline "C:\private-d12\before.log" `
  --variant  "C:\private-d12\after.log" `
  --output   "C:\private-d12\aggregate.json"
```

Run on the original local files; the tool does **not** access music files
or modify tags. It rejects partial runs, missing legacy preferences,
track-identity mismatches, mixed builds, policy changes, differing requested
window/pass settings, duplicate telemetry or malformed events.

The output has only run-level summaries: counts of HOLD/OUTPUT transitions,
candidate-stage transitions, candidate-board changes, MIR segment-exit
reasons, timing averages and the explicit evidence limitation.
The report contains **no individual track BPM, file path, label, raw track
key or source mapping**. Still treat it as private research data.

`HOLD_TO_OUTPUT` or improved candidate availability is *not* an accuracy
proof. Actual BPM accuracy versus references, regressions, and any physical
tag-write qualification must be evaluated separately using private evidence.

## Missing coverage

The legacy binary cannot retroactively produce per-track effective sampling
plans. The resulting `LEGACY_GLOBAL_UNVERIFIED` classification is an
intentional conservative restriction, not a parser failure. No individual
D12 result should be accepted based on these aggregate statistics alone.
