# Smart Tempo: end-to-end architecture audit and qualification programme

**Status:** approved research architecture, phase A (static source audit) under implementation; **NOT** a claim of improved BPM accuracy or permission for production writes.  
**Canonical source:** `lxsdd/foo_smart_tempo-public` at exact commits specified by each CI run.  
**Privacy:** public source/synthetic fixtures only. Never commit original music, identities, private holdouts or telemetry.

## Executive decision

Do **not** begin by replacing Hodgkinson/MIR or tweaking one threshold. The observed production issues involve **candidate availability, primary selection, lane routing, policy and writer override**. A candidate in the merged manual-review export is *not equivalent* to an eligible primary-selector candidate. Separate measurement and selection, freeze independent evaluation cohorts, benchmark all alternatives over the **same decoded inputs**, and choose a replacement only when its advantage is proven on unseen material.

The first complete engineering unit is a reproducible, **28-transition source audit** plus an explicit future read-only trace schema, safety-negative tests and CI gate. The subsequent units implement runtime traces, one batched measurement campaign, offline replays, and a final blind evaluation. These are gated packages, not an endless sequence of case-by-case threshold edits.

## Verified current path and source ownership

| Boundary | Current observed implementation | Audit obligation |
|---|---|---|
| Worker and metadata snapshot | `bpm_auto_analysis_thread.cpp`: genre-route snapshot, worker pool, `analyzer->analyze`, suppression result | Preserve original tag order; run audio-only and wrong-tag controls; quantify worker/IO contention |
| Decode, windows and frontend | `modern_bpm_analyzer.cpp` and `analysis_pass_schedule.h`: input helper, multiple seeks, optional resampler, ODF and peaks | Record **actual** pass offsets, frames requested/decoded, PCM/ODF features and decoder errors |
| MIR and meter | `hodgkinson_full_mir.cpp`: STFT with KissFFT, ODF, circular autocorrelation, tatum quantization, bar-division, top-K candidates | Independently verify candidate completeness, pulse/meter/beat-unit identity and high/low range coverage |
| Candidate lineage | `mir_candidate_pipeline.cpp`: raw alias families, clustered primary pool and separate measured full-board; supplementary partial-bar probes | Stable IDs and lineage, exclusions, eligibility, per-stage rank, source origins and loss of exact candidate |
| Primary selector | `compute_mir_primary_selection`, lane classification and route-dependent select variants | Freeze input candidates; record each candidate considered, reasons excluded, selected and margin, hold reasons |
| Policy / final resolution | `compute_mir_policy_decision`, partial-bar releases, writer override, `enforce_mir_measured_candidate_invariant` | Counterfactual retain-primary/disable-override/retain-hold tests on SAME candidates; measured witness proof |
| Manual export | `prepare_mir_manual_review_candidates` merges full-board, primary and supplementary sources | Export composition must not be mistaken for primary eligibility; merged bits may overlap |
| Write boundary | `tag_write_dispatch.cpp`, `selection_is_safe` before `update_info_async` | Nonzero MP3/FLAC/WavPack embedded-Cue subsongs fail closed, mixed selections atomic, no-write CI |

**Reality check:** technical Windows CI PASS is not musical PASS. Previous real x64 public 16-case complex cohort reported **3/11 clear correct, 8/11 wrong, 0 clear held, 4/5 risk negatives autoaccepted**. Of 8 wrong outputs, five followed the observed primary final path and three writer override. Seven wrong tracks had a precise *manual merged export* alternative, but only four of those alternatives bore the full-board origin and three were supplementary-only; none carried the primary-selection origin marker. This does not prove which alternatives were independently eligible. This is the major unresolved causal boundary.

### Architectural mismatches confirmed statically

- Default geometry: **20 seconds × 50 sample passes**, offsets spanning 20–80% of known duration. For a **180s** file the sampled window requests sum to **1,000s**, while their union is approximately **128s** (estimated repeat factor ~**7.81×**). **This is theoretical geometry, not measured decode time or CPU waste.** The pass planner may clamp/overlap; CI must instrument actual reads and use independent short/long tests.
- Core Hodgkinson-MIR domain is **50–200 BPM**; primary/production rails are **40–220 BPM**. Any proof of edges 40–50 and 200–220 must identify actual source/alias pathways.
- Metadata matching currently selects the **first route**; `classify_mir_lane` can choose low-pulse context based on low route bounds or text before examining the measured high-pulse branch. A/B tests must explicitly introduce mismatched, absent, contradictory and generic tags, while preserving audio evidence.
- Review-hold may be released after primary selection; after-policy decisions are not identical to original primary outcome. **Final action/hold/override must be logged separately.**
- Numeric confidence must not be treated as a calibrated probability without a separate calibration evaluation.

## Diagnosis contract v1: capture once, compare many

Read-only event schema: `tools/smart_tempo_decision_trace_v1.schema.json`.

Each event carries a run-local `analysis_id`, monotonic `sequence`, `stage`, typed `outcome`, bounded `reason_code`, run-local stable `candidate_id`, `parent_candidate_ids` and bounded numeric `metrics`. The internal implementation must additionally associate stage events with actual analysis windows, unwrapped candidate families and eligibility flags. No source labels, track paths, titles, artists, raw audio or oracle BPMs are admitted in **public CI traces**. Owner material and any deep feature-cache remain entirely **private/offline**, with only approved anonymous aggregates allowed in public.

**Mandatory per-candidate chain:** raw measurement → source/lane/alias transform → primary eligibility and *veto reason* → primary score/rank → selected or rejected → downstream policy eligibility/rank/reason → final override/release/hold → same-analysis measured-witness proof → no-write decision.

**Sufficient trace completeness gate** before expensive host matrix:
1. Every final accepted BPM has a measured same-run witness ID; no orphan candidates.
2. Every candidate entering Primary, FullBoard or supplemental pool has a stable origin/parent; count and rank denominators reconcile.
3. Each rejected candidate records the actual **first** excluding gate; nested policy decisions preserve all later reasons.
4. Final action always resolves to exactly one of accepted primary, accepted override, explicit review-hold release, held or no candidate, with orthogonal attributes recorded separately.
5. BPM aliases are grouped by pulse family; no half/double/3:2 inference is hidden in genre text.
6. Each audio window includes planned position, actual decoded extent and sample count; feature reuse and CPU/RAM are instrumented without per-frame debug overhead.
7. Negative controls prove privacy, sidecar allowlist, no tag/media writes, parse integrity and fail-closed behavior.
8. Two independently reviewed audit contexts qualify a trace schema before it is treated as causal evidence.

**Anti-circularity:** oracle/ground truth cannot participate in frontend, candidate generation, primary, policy or writer. Join only in the offline scorer, after complete frozen audio decisions.

## Cohorts and full qualification matrix (declare before results)

| Partition | Purpose | Restrictions |
|---|---|---|
| Existing regression | Protect 80 public synthetic cases, windows 5/10/15/20/30/45/60/90, passes 1/3/5/10/20/50; existing 16-case complex cohort and previously seen meter/other probes | **Seen**; never tune and then cite as unseen validation |
| New training/research | Diverse generated tempo, polyrhythm, phase, meter, syncopation, 40–220 BPM, dynamics, silence and nonstationary segments | Fixed generator seed, source SHA, SHA-256 WAV, labels quarantined to scorer |
| Independent synthetic holdout | Unseen instrument/arrangement families and meter/beat units, mismatched genre, different sample-rates/channels, long/short | Freeze *before* selector choice; no case-specific exceptions |
| Private real music holdout | Representative acoustics/genres/real beatgrid and difficult DJ transitions | Runs **locally**, no public track metadata/audio/scorecards; report aggregates only |
| Fail-closed negatives | Silence, steady tone, noise, nonstationary, unrelated tempo hints, half/double/3:2 ambiguity, incorrect tags | No unsafe automatic acceptance or write |
| Performance | Real-time and batch cases at Windows Win32/x64, 1/3/5/10/20/50 passes | CPU time, wall-clock P50/P95/P99, peak RSS, decode/FFT cost and true coverage; same inputs |

Per-class mandatory denominators: N clear / accepted within **±0.05 BPM** / accepted within **±0.20 BPM** / wrong auto / held / oracle absence / missed candidate / 2×/½×/3:2 alias / negative false accepts. Include false-hold rates and calibration, not just mean error across accepted predictions. Separate per-time/worker/memory and tag safety. A **zero-wrong** shadow may still have unacceptable coverage.

## Single coordinated expensive host campaign

1. **Preflight cheap local/Python/PowerShell source tests** for trace/fixture/schema integrity and limits; do not burn a Windows host run on parse errors.
2. **Pilot only for trace completeness** on a strict public fixed positive and negative fixture set; errors at any boundary halt the campaign.
3. Run **one batched, SHA-bound Win32/x64 test plan** on a frozen corpus, collecting all candidate and decision stages, planned/actual window geometry and performance. Use disposable isolated runners and no uploads. Store only permitted bounded synthetic traces during CI; aggregate in-job before deletion.
4. Replay alternatives **offline against identical captured evidence** without rerunning the expensive decoder for each candidate Selector/Policy variant. Reject any replay that requires audio features not captured; arrange a *bounded* additional capture cohort instead of ad-hoc patchwork.
5. One independent validation pass, then private real-audio holdout gate.

## Package B: real native CI-only candidate lineage pilot

The first native observer (`tests/ci_pipeline_trace.h`) is compiled **only**
with `SMART_TEMPO_CI_ORACLE_PROBE`, restricted to the single public
`synthetic-oracle/pulse_128.wav` under `RUNNER_TEMP` and
`GITHUB_ACTIONS=true`. It writes `pulse_128.wav.pipeline.jsonl` beside the
disposable synthetic fixture via `CREATE_NEW`. The production DLL never
includes or emits it. The existing real x64 foobar host smoke must verify
the bounded JSONL with `tools/audit_smart_tempo_ci_pipeline_trace.py`;
nine independent positive/adversarial Python tests run before the SDK
download. It never changes a BPM proposal or the file/write outcome.

The initial live-boundary trace includes all raw alias candidates, collapsed
cluster candidates, the **observed primary input pool**, separately
transformed exact-BPM policy FullBoard, the primary choice, policy proposal,
final accepted-or-held BPM and the no-write CI guard. It uses stable
per-analysis candidate IDs with verified source and cluster parentage.
Only numeric measurements and fixed reason enums appear in the public trace.

**Crucial remaining proof distinction:** a member of the collapsed primary
*input pool* is not necessarily eligible under that lane's branch-specific
selector and veto conditions. The next CI-only selector instrumentation reads
**actual executed** raw-alias and minimum-support checks in the conservative
raw selector; 40–220 BPM rail and rank checks in harmonic/standard selectors;
and the upper/threshold gates in the low-pulse branch. It records included and
provisionally excluded candidates (because a fall-back can re-admit them)
using stable matching raw/cluster candidate IDs; all numeric score values
are read-only. The native auditor validates reason enums, source membership,
stage order, raw→cluster BPM parentage and exact primary selected BPM.
This is strictly non-interfering in production (compile guard excludes
all event state and logging). Rescue stages, tie-break ranking in other
branches and auxiliary partial-bar witness paths are still incomplete.
Therefore the real observer
**explicitly emits** `pool_only_gate_unresolved`, and the strict scorer
reports `PHASE_B_CAUSAL_COMPLETENESS=INCOMPLETE_PRIMARY_VETO`.
This is meaningful empirical pilot evidence, NOT evidence that package B is
finished. Before package B completes, native instrumentation must record
every actual lane filter/veto/rank decision, all auxiliary measured candidates
and exact final-witness identity. The same frozen synthetic positive and
negative cases must then reconcile all counts without inferred join logic.
Do not launch the large batched phase C musical experiment until that proof
and an independent audit pass.

### Package B – native origin/witness integrity hardening

The CI-only selector now snapshots the **exact collapsed vector used for the
primary decision**, rather than recomputing it when emitting the trace.
It independently records the exact raw/cluster source index of each gate/rank
observation. Native diagnostics fail closed when a numeric score/support is
non-finite; they must not silently serialize NaN/Inf as `0.0`.

The per-run analysis ID is derived from the public Actions run identifier
plus disposable test process/thread identity; the JSONL validator rejects
malformed or mixed-session IDs. A dedicated `measured_witness` transition
uses the **existing production same-analysis MIR candidate-witness function**
without changing its behavior. Exact FullBoard/primary witness matches are
linked to candidate IDs. Supplemental TopK/partial-bar witness matches that
are not represented in the FullBoard are explicitly reported as
`supplemental_witness_unindexed` with **no invented source ID**. Such traces
may be internally safe but DO NOT yet prove complete causal provenance.

All these changes are within `SMART_TEMPO_CI_ORACLE_PROBE`; no production
selection, confidence threshold, write policy or user files are changed.
Adversarial regressions cover orphan/mismatched final witness, mixed-session
IDs, unindexed supplement flags, branch lineage and loss of the no-write gate.

**Additional queued native trace stage:** instrument the executed general
baseline and alternate-144 ranking functions and the actual continuous-rescue
qualification check. Publish three indexed terminal transitions *before
residual rescue*, *after residual rescue* and *after harmonic direct protection*.
These permit exact comparisons of how the primary choice changes through
rescue stages without rerunning audio. Their presence does **not** prove
complete per-candidate veto causality, nor does a provisional excluded state
mean final ineligibility when fallback re-admits a candidate.

**Gate remains incomplete:** low-pulse nested rescues, remaining general-144 scoring
and fallback, residual rescue candidate vetoes, partial-bar/TopK origin identity, real audio
window extents and independently measured CPU/RSS require full runtime capture
and a distinct audit before the large C campaign.

## Candidate architecture candidates: all compared under identical evidence

- **A:** unchanged current MIR + primary + policy baseline.
- **B:** original MIR candidates with a unified audio-first alias-competition selector; strict ambiguous hold.
- **C:** same MIR and primary, but policy ablation (remove each writer override, review-hold release or center prior independently).
- **D:** original MIR plus independent onset/tatum/downbeat accent evidence for pulse/beat unit, not only a harder confidence threshold.
- **E:** alternative frontend/core only **if** correct BPM repeatedly absent from raw candidate search after range and window controls.
- **F:** independent window plan, hop/FFT caching and true decode/worker scheduling improvements after musical parity is established.

No candidate may be synthesized from genre or oracle; no tuned track-specific rules.

## Decision gates (not a promise that targets already hold)

- If a precise candidate is **never generated**, repair or replace the frontend.
- If generated but **filtered before primary**, repair eligibility and range/alias semantics.
- If considered but **ranked below an incorrect winner**, replace scorer/tie-breaking or improve disambiguating evidence.
- If primary is correct but **policy changes it incorrectly**, simplify or remove that particular policy action.
- If reference is inherently underdetermined (insufficient acoustic metrical evidence), prefer Review-Hold.
- If source/model is musically equivalent, optimize performance **only** with decision and witness parity.
- If alternative engine beats the current algorithm on frozen unseen material including negative controls and resource limits, independently qualify it for implementation; do not swap kernels based on retrospective seen examples.

**Research targets, not current performance claims:** ≥99% of accepted unambiguously labeled real-music examples within ±0.20 BPM; ≥90% auto coverage subject to precision; no observed auto acceptance of fixed unresolvable negative controls; measured performance improvement without a quality/safety regression. Freeze confidence intervals, sample sizes, and an independent review procedure before interpreting PASS. True final acceptance includes no-write and actual Win32/x64 foobar host tests.

## Work packaging and stop rules

**Package A (this PR):** authoritative 28-stage static source inventory; architecture/documented risks; typed future trace schema; positive and adversarial negative source tests; fast Windows pre-build gate. This closes the *static source map*, NOT runtime proof.

**Package B:** C++ CI-only full event trace capture (separate from any private feature cache), stable candidate IDs and transition/eligibility audit; explicit independent evidence review. Stop if trace fails completeness.

**Package C:** frozen matrix, all grouped no-write host runs, single canonical scorer and reproducible aggregate tables. Do not run fragmented check-of-the-hour tests.

**Package D:** research alternatives A–F, deterministic offline replay, separate unrelated holdout and performance-parity analyses. No production write.

**Package E:** pick best qualified algorithm; isolate production DSP/Selector/Policy changes in a separate reviewable diff; run Win32/x64 runtime and private real-music endgate. Musical release **NO_GO** until independently proven.

No further minute-by-minute approvals requested for safe research. GitHub Actions must remain zero-cost under existing free runners. Draft PR only, no main merge, no release, no uploaded binaries.

### Phase B decoder-window capture (25 Oct 2026, proposed CI-only block)

The disposable `pulse_128.wav` host observer now records **actual** per-pass
source frames consumed and analysis frames emitted (after resampler flush),
planned pass index/offset, requested frames and both sample rates. The strict
read-only parser computes decoded/requested seconds, union of actually decoded
intervals, overlap-derived repeat factor and complete-vs-partial pass counts.
The Windows host gate requires a complete declared pass sequence. These are
observed decode extents, **not** a measured CPU speedup, per-frame ODF reuse,
peak RSS or proof of independent musical evidence. This work neither changes
production window planning nor authorizes Phase C: first-veto selector and
supplemental policy/witness identities are still incomplete.
