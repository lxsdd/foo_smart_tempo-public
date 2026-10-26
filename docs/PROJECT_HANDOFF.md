# Smart Tempo – PROJECT HANDOFF (2026-10-26)

## Source of truth and safe ownership

- **Repository:** `lxsdd/foo_smart_tempo-public`, public source only. **Scope:** Smart Tempo; no Music Metadata Studio, Discogger, MyTagSync or other project work in this branch.
- **Open DRAFT PR:** [#39](https://github.com/lxsdd/foo_smart_tempo-public/pull/39), branch `research/full-pipeline-architecture-audit-20261025`.
- **Latest independently qualified functional checkpoint:** `3ae50d19e9970a4611308459f3a7f47bf79ff449`; exact original Windows [run 40325835901](https://github.com/lxsdd/foo_smart_tempo-public/actions/runs/40325835901), job `121582157932`, **COMPLETED SUCCESS: 24/24** workflow steps, 0 Actions artifacts. This Markdown handoff itself is a subsequent documentation-only change; it does NOT inherit a green conclusion for its own commit until its own exact-head CI completes. Check the PR's current HEAD and latest CI on every return.
- **Protected main:** `ac4945f7816d05f5a5a3cef5f4147eb86d8080d7` (verified on 2026-10-26), unchanged during this block.
- Other open PRs #25, #29, #36, #37, #38 remained untouched. Do not merge, retarget, tag, release, close or publish unapproved modified foobar2000 binaries.
- Read `AGENTS.md`, this handoff and `docs/SMART_TEMPO_ARCHITECTURE_RESEARCH_PLAN.md`. The central `lxsdd/dev-infrastructure/STANDARDS/CHAT-CONTINUITY.md` could not be retrieved from main in this chat (404); do not claim its live contents have been reviewed.

## Large validated development blocks

**A. Actual five-field low continuous-center winner/loser provenance**, code commit `0bb50d1fc45b322eb3d9793a5ba9f2891520b275` and [run 40323804751](https://github.com/lxsdd/foo_smart_tempo-public/actions/runs/40323804751), **SUCCESS 24/24**, 0 artifacts. Seven dependent source/trace/parser/test/schema/plan files integrated:

1. The CI-only C++ observation records real executed candidate rank, comparator results and the full **five native lexicographic tuple fields**: continuous score ratio, continuous section support, section stability, continuous score and negative distance from route center; exact source-object index preserved. Production comparison predicate, selector, DSP, Policy, Review Hold and writer untouched.
2. `tests/ci_pipeline_trace.h` serializes bounded numeric-only source-indexed events. `tools/audit_smart_tempo_ci_pipeline_trace.py` independently replays the actual five-field score/tie incumbent and rejects source-ID/orphan/false-victory/gate-only terminal contamination. `tests/test_smart_tempo_ci_pipeline_trace.py` includes positive/adversarial native provenance regressions and CI-source-to-production tuple parity.
3. The frozen original decoded-WAV and CUE native Win32/x64 tests remained green. They **did not** actually trigger this specific continuous-center comparator (wins=0, losses=0 per architecture). A green interpreter test alone is therefore **NOT** positive decoded-audio coverage.

**B. Direct actual productive C++ Primary selector execution under original foobar host**, commit `3ae50d19e9970a4611308459f3a7f47bf79ff449`, exact [run 40325835901](https://github.com/lxsdd/foo_smart_tempo-public/actions/runs/40325835901) **SUCCESS 24/24**, 0 artifacts. Four dependent in-host test-driver, host harness, independent source contract and research-plan files:

1. Additional **CI-only** `tests/smart_tempo_ci_probe.cpp` safely creates the exact same small publicly fabricated MIR candidate board (60/96/99 BPM) twice. The actual `compute_mir_primary_selection` runs in the original Windows foobar2000 process, with a matched synthetic `Chill` route. When center=99, actual five-field comparator must select the 99 BPM source with 2 wins/0 losses. When center=96, it must select 96 BPM with 1 win/1 loss. The terminal event and exact tuple/source-ID fields are checked; fake successes fail closed.
2. Only successful *actual native in-host execution* can write a unique marker beside the generated runner-temp fixture. The original `tests/diagnostic_live_host_smoke.ps1` checks it for **every Win32 and x64 host case** before crediting a diagnostic result. Exact logs show `SMART_TEMPO_NATIVE_CONTINUOUS_Win32_*=PASS` and `SMART_TEMPO_NATIVE_CONTINUOUS_x64_*=PASS`. Original no-write media/WAV/CUE, first-run GUI and blocked Update, source/summary and redaction gates all still passed.
3. **Important limitation:** fabricated in-host MirCandidateBoard is **not** decoded audio, not a new rhythm corpus, not a positive example of the actual MIR first-stage parser choosing this path. It qualifies the productive C++ selector decision semantics under both original foobar host architectures. Do not add it to the audio accuracy denominator or claim general reliability.

The first code stage encountered three pre-SDK adversarial test fixture errors and one formatting-parity regex error. They were corrected at subsequent exact SHAs; [40323804751](https://github.com/lxsdd/foo_smart_tempo-public/actions/runs/40323804751) is the final green code checkpoint, not its red predecessors. No product algorithm was tuned to make a test pass.

## Music-quality truth (unchanged)

Exact original-host **four-case** synthetic cohort, independently joined across Win32/x64, contains three clear rhythmic references and one negative silence fixture:

| Frozen reference class | Win32 | x64 |
|---|---:|---:|
| Rhythm within ±0.20 BPM | 1/3 | 1/3 |
| Wrong BPM automatically accepted | 2/3 | 2/3 |
| Rhythm held for review | 0/3 | 0/3 |
| Silence falsely accepted | 0/1 | 0/1 |

Independent post-host shadow rule `COMBINED` (source-only, not deployed) selects correct BPM **2/3**; reference-near candidate is ranked **#1, #1, #2** for 60/128/sparse-120 BPM, respectively, on both architectures. Other three fixed ranking baselines do not improve this decisive sparse case. These are **in-sample three-rhythm observations**, not established superior product performance. Correct 60/sparse120 MIR candidates actually exist through raw, cluster and Policy FullBoard but are displaced at Primary; in both failing cases the correct candidate won an intermediate executed comparison before the final family winner displaced it. Do not retroactively choose a release policy from the labels.

**Research gates:** Phase A source map complete. Phase B **INCOMPLETE**: terminal/first-decisive conditional veto explanation for every candidate, positive actual selected supplemental Policy→final writer identity, all conditional nested rescue paths, independent complete native provenance review remain. Phase C comprehensive predeclared multi-window/rhythm/genre matrices **CLOSED** until independent full Phase B evidence. Phase D competing replay architectures and Phase E private blinded real-music acceptance **NOT QUALIFIED**. **MUSICAL RELEASE NO_GO**, regardless of 24/24 CI. Process CPU/RSS if shown are process-scoped, not proven per-algorithm speedup.

## Safety, user action and next coherent block

No actual user's tracks, reference labels, paths, titles, artists, private telemetry, source libraries or secrets entered this public branch or CI; no user WAV/MP3/FLAC/tag/CUE/rekordbox DB changes, application/device installations, paid private runner minutes or signed/publicly uploaded packages. Only temporary generated synthetic fixtures in ephemeral GitHub Actions. The productive BPM candidate must remain an actual measured same-analysis source; Review Hold must remain safe/fail-closed.

**Next largest coherent block:** finish true positive original-host native source-ID witness for selected supplemental Policy→final writer (do not create it by merely matching BPM), complete **ordered branch-specific first-decisive veto vs provisional exclusion/fallback and never-visited semantics**, and implement independent positive/negative source/harness qualification as one block. Protect baseline regression and no-write gates. Once an independent *different review* accepts Phase B, freeze broad synthetic rhythm/alias/sparse/3:2/half/double/genre/window matrix before any new results and compete source-only selector/hold hypotheses on genuine independent cases. Later perform private owner-blind real-music acceptance locally; only request a user installation/test action after a source- and CI-frozen candidate qualifies. Currently **no user action required**.

**Authoritative later checkpoint:** this document's own exact commit SHA and its CI results must be taken from live GitHub PR #39; don't inherit `3ae50d19e9970a4611308459f3a7f47bf79ff449`'s PASS. If GitHub inaccessible, treat a copied Markdown handoff as UNVERIFIED and never claim a repository write.


### 26 Oct 2026 next exact-HEAD native low nested control

CI-only in-process positive/negative selector control added in the draft
branch: three distinct native low-pulse `bestBy` episodes, expected 60→80→71
vs a one-alias-change control that must stop at 80 after two episodes.
Windows Win32/x64 exact SHA CI **must pass** before qualifying this milestone;
actual decoded-audio natural nested rescue remains unmeasured. Production
BPM and privacy gates unchanged; Phase B INCOMPLETE, release NO_GO.
