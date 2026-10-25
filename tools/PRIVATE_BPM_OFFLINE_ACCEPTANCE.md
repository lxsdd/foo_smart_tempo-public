# Private offline BPM accuracy audit — no tag writes

This is the **last-mile private reference check** for the public 60-case
synthetic research in Draft PR #37. It does **not** make private music,
reference BPM values, file paths, logs, or per-track findings available
to public GitHub Actions, issues, pull requests, or this repository.

## Scope and boundaries

- Use the existing **Smart Tempo > Analyze selected tracks (no tag writes)**
  context action in an isolated, portable foobar2000 x64 test profile.
  The ordinary Smart Tempo Analysis action is **not** equivalent.
- Analyze **copied media** rather than originals. Include external CUE files
  and relevant sidecars in one dedicated fixture tree.
- The existing `tools/verify-private-d12-fixtures.ps1` performs the Before /
  After SHA256, length and timestamp integrity comparison, with the state
  file **outside** the fixture tree. Follow
  [D12_CROSSBUILD_LOCAL_RUNBOOK.md](D12_CROSSBUILD_LOCAL_RUNBOOK.md) for
  this no-write guard and the known 12-track D12 comparison.
- Console logs and generated reference templates are **private** even if
  track labels are later removed. Do not post or upload them.
- This scorer uses `compare-mir-window-runs.py`'s pre-existing strict
  per-track provenance, sampling-plan, result and completeness parser.
  It accepts no incomplete or duplicate track set.
- Human or independently verified reference BPM values are required.
  Existing BPM tags or another analyzer's estimate are **not** automatically
  authoritative. Inference always runs **before** reference scoring; no
  metadata, label, filename or reference influences production selection.

## One batch, two offline commands

On the Windows test PC, after running the verified private diagnostic
component and saving the complete verbose foobar console as
`C:\\private-d12\\analysis.log`:

```powershell
python .\tools\score-private-bpm-holdout.py --log "C:\private-d12\analysis.log" --prepare "C:\private-d12\references.json"
```

This creates **one private local JSON template** with all analyzed track
labels and stable track keys. It is deliberately not created within a Git
checkout and never overwrites an existing file. The terminal prints only
success and the case count.

Fill `reference_class` and `reference_bpm` for each row:

- `"clear"` — a uniquely justified tempo; fill a finite numeric BPM,
  e.g. `"reference_bpm": 127.95`.
- `"ambiguous"` — more than one defensible half/double metrical level;
  keep `"reference_bpm": null`.
- `"no_tempo"` — non-rhythmic/no reliable pulse; keep
  `"reference_bpm": null`.

Then run:

```powershell
python .\tools\score-private-bpm-holdout.py --log "C:\private-d12\analysis.log" --references "C:\private-d12\references.json"
```

No files are written by the **scoring** command. The output has **only**
aggregate counts and `NO_GO` / `REVIEW_HOLDS` /
`REVIEW_PRECISION` / `REVIEW_NO_CLEAR_REFERENCES` / `PRIVATE_SAMPLE_MEETS_PRECISION`. It gives absolute
within-0.05 and within-0.20 BPM precision, unsafe clear-reference accepts,
ambiguous accepts, non-rhythmic false accepts, holds, and an **offline
shadow-only** `local_exact` alternative count. It never outputs labels,
identifiers, paths or per-track BPM values and it cannot change a BPM
decision or write tags. Even `PRIVATE_SAMPLE_MEETS_PRECISION` describes only
the supplied sample, never release qualification.

The underlying verbose log must have a complete `MirSamplingPlan`,
`MirPolicyDecision`, `Decision:`, `Final Output:` and `Track Timing:`
for each track. Legacy logs without these fields are **not** silently scored
as if complete; use the earlier D12 cross-build comparator separately.

## Acceptance interpretation

A high shadow-ranking score is **not** evidence that the hypothetical
ranking would correctly Review Hold ambiguous signals. In particular,
`shadow_ambiguous_proposals_n` and
`shadow_no_tempo_proposals_n` must be considered with the accuracy rate.
Changing any production selector, DSP, tie-breaker or writer still needs
independent review of the locally retained results and the separate
license/release gate.

The public CI exercises this scorer **only with constructed synthetic
telemetry**. No owner audio or ground-truth file is used or uploaded.
