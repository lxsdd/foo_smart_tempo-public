# Phase B: native low-pulse upper-branch exclusion and continuation

This CI-only contract is attached to the **actual** low-pulse primary selector in
`mir_candidate_pipeline.cpp`, not to a replay of manual-review candidates.
When the 95–125 BPM upper pool is nonempty, every excluded source receives
`low_upper_branch_veto` after the full native eligibility sweep and before
the winning pool's rank/comparator events. When the upper pool is empty, the
same original selector must instead visit the 45–90 BPM threshold sweep.
Both cases are exercised in the original Win32/x64 foobar host using
fabricated, disposable numeric boards and an independent Python automaton.

The event is deliberately **branch-local**, not a final first-veto:
`select_mir_low_pulse_continuous_center_rescue` runs afterward and can
select a source excluded from the upper pool. The independent parser checks
the exact source-ID sequence, polarity, BPM, missing/duplicate closures,
rank/comparator pairing and conditional lane-terminal identity. No oracle
label, private music, tag, file path or production write enters this trace.

This closes one previously unobserved upper-pool branch without asserting
global selector eligibility completeness. The final primary first-veto
remains unproven in other rescue/policy paths; Phase C and owner musical
acceptance are not unlocked by this contract. Do not change production
DSP, candidate scoring, Policy, writer or Review Hold on this evidence.

## Executed continuous-center rescue: complete branch-local closure

The original low-pulse selector may replace an already-selected 45–90 BPM
candidate with a 95–102 BPM continuous-center candidate. The CI-only
observer now emits a **full source-indexed closure** after the executed
five-field lexicographic comparator: each collapsed source is either
`low_continuous_branch_veto`, `low_continuous_branch_ranked_out` or
`low_continuous_branch_winner`. No closure is fabricated when the rescue
function was not invoked.

An independent oracle-blind parser replays gate, rank, comparison, source
identity and terminal ordering; positive and adversarial regressions run
before SDK acquisition. The original Win32/x64 foobar host probe checks
both opposing route-center winners on the same fabricated numeric board.
The native marker now requires the three exhaustive branch outcomes.
This is **branch-local** only: the prior low lane may still win a rejected
source. No global first-veto, musical quality, Phase C readiness, productive
selector change, ReviewHold release or writer permission is inferred.
