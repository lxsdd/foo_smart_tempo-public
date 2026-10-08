# MW1/D12 offline evidence comparisons

This public tooling is **reference-blind**. Run it **on your own computer** against locally retained foobar2000 verbose logs. Never commit or upload logs, music files, track labels, hashed track keys, BPM reference tables, ground truth, or generated per-track reports to the public repository.

## Procedure

1. Install an exact source-verified Smart Tempo component with `MirSamplingPlan` telemetry (the build produced *after* this change). In foobar2000, enable verbose diagnostic logging. Keep production BPM write/tag actions disabled during research.
2. Select the **same ordered set of tracks** and analyze once using the baseline `20 seconds / 50 windows` (or record whichever settings are the established baseline). Save the **complete** console output to a private local text file.
3. For an MW1-v2 test, change **only** the window seconds (5, 10, 15, 20, 30, 45, 60, 90); leave the window count and all other settings fixed. Separately test window count (1, 3, 5, 10, 20, 50) with seconds held fixed. Save each run to a distinct private file.
4. Execute the comparator with **no network access or credentials required**:

```powershell
python tools\compare-mir-window-runs.py --baseline "C:\private-data\baseline.log" --variant "C:\private-data\trial.log" --output-json "C:\private-data\compare.json" --output-csv "C:\private-data\compare.csv"
```

The files are written **only** where you request them locally. The output uses synthetic `T0001`-style row identifiers, not track names or raw `track_key` values. The numerical BPM and timing evidence is still personal research data; do not upload the output files to public Actions or issues.

## Validation boundaries

- Provenance is joined by stable `track_key` **and** subsong. Per-track sampling uses the **effective** geometry after clamping/truncation (including unknown-length audio).
- Runs must contain exactly the same tracks, build IDs, and policy schemas. Repeated tracks, incomplete logs, incompatible schemas, and ambiguous duplicated labels are rejected instead of silently compared.
- Each run must use a **uniform requested** window length and pass count. Only **one** setting may change between baseline and trial (or neither, for a determinism repeat). Short-track effective-window truncation is still reported separately and is not mistaken for mixed user settings.
- Candidate equality uses the full recorded policy candidate evidence row (excluding row order), **not merely the BPM number**. Candidate count and identical row count are also reported.
- Comparison includes WRITE/HOLD transitions, terminal BPM, policy status when present, decision-class changes, candidate-board changes, and runtime. **WRITE only means the log emitted a positive `Final Output: ... BPM`; it is not proof of a physical tag write or BPM correctness.**
- The tool deliberately does **not** use genre, title, artist, filenames, ground truth, or reference BPM to select a candidate. Any human accuracy evaluation must happen **after** the blind inference and remains private.
- A green public C++ build or synthetic parser test is not a real-audio/holdout/foobar runtime PASS. D12's retained local Repair 4 worktree and its real audio evidence gate remain independent; do not rewrite/clean that worktree to use this tool.

Never enable an auto-voting or averaging policy based on these comparisons. Any change to candidate generation, writer behavior, or selector thresholds requires a separately controlled experiment and real-audio acceptance.

## D12 candidate-stage diagnosis (read-only)

The comparator now separates an **empty policy candidate board with no source candidate**
(`NO_MEASURED_SOURCE`) from a **measured board that nevertheless results in a
hold** (`MEASURED_BUT_HOLD`). It also reports `MEASURED_AND_OUTPUT`,
`SOURCE_WITHOUT_POLICY_BOARD`, and `UNKNOWN_POLICY` explicitly.

These are *observable telemetry states*, not explanations for *why* the
frontend generated no candidate, and not BPM accuracy judgments.
`SOURCE_WITHOUT_POLICY_BOARD` requires follow-up; it must not be silently
counted as a successful candidate-generation recovery.

The aggregate `candidate_stage_changes` and per-track anonymized stage
fields can identify whether a revised window changed the **candidate
generation stage** or only a later hold decision. Real D12 classification,
the retained dirty Repair 4 worktree, and true BPM reference scoring still
require privately retained evidence. No synthetic result licenses a change
to production candidate generation or a physical tag write.
