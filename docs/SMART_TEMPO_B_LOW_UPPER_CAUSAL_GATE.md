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
