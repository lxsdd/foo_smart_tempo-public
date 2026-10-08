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

## Safe collection in foobar2000

**Important: the ordinary Smart Tempo Analysis action may perform automatic
tag writes if Auto-write is enabled.** Do not assume clicking Cancel in its
results dialog reverses an automatic write. A future explicit multi-track
no-write action is still a separate UI change; it is **not in the current
plugin**.

For an immediately available read-only inspection, use
`Smart Tempo > Measured BPM candidates` on **exactly one selected track**.
The analysis phase opens a candidate-inspection window without submitting
an automatic BPM-tag write. Do not click `Write selected BPM` in the
standalone candidate dialog. Enable verbose logging in the plugin settings
before testing. Close the candidate inspection window without writing.

An apples-to-apples *full D12 batch* comparison needs complete logs for
the same twelve source tracks and the same sampling settings on both
component versions. Until a verified batch no-write mode is available,
do not put the original data at risk through automatic writing.
A complete reference-independent local evaluation can be performed against
disposable, privately kept **copies** of the audio, not the originals.
Do not upload the copies or the console logs.

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
