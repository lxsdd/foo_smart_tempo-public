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

**CI-only first-exclusion witness (Phase B, partial):** the original production selector's run-local source-indexed events now produce exactly one `primary_first_exclusion` record per collapsed candidate. Its reason, native score and one-based position in the *entire executed selector-event stream* are independently replayed against source events; a zero position explicitly means **no observed exclusion**, not eligibility. A provisional exclusion followed by fallback/ranking/selection remains provisional; this witness is **not** a first-*decisive*-veto proof, and raw-only lanes may leave clusters unvisited. The original foobar Win32/x64 host gate, Python oracle and native-trace adversarial mutations must all pass at the **same new commit SHA** before this increment is qualified. Until per-branch final eligibility and all unindexed Policy branches are independently proven, Phase B is incomplete and Phase C remains closed.

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

### Package B – exact policy BPM source-comparison audit

The previous native sidecar joined Policy output to **all** FullBoard BPM rows
within ±0.20 BPM. This can conflate nearby, independently measured candidates
and equal-BPM aliases; it is *not* proof of the actual chosen candidate pointer.

The new bounded `policy_resolution` event reports zero/one/multiple **exact
same-BPM (≤0.001)** FullBoard matches or no output. A parent candidate ID is
emitted only for the unique same-BPM **comparison**, and the `policy_change`
stage leaves its parent list empty to avoid false chosen-pointer provenance.
The record exposes independent numeric boolean flags for native
`writer_override_candidate` and `review_hold_promotion_candidate`.
The strict validator re-counts actual FullBoard matches, catches ambiguous
duplicates or falsely asserted uniqueness, ensures output consistency,
confirms independent flags and rejects invented policy-selected identities.
The JSON schema is kept aligned with every native emitted stage, including
`primary_terminal` and `policy_resolution`.

This remains CI-only and source/synthetic-only. No production metadata,
selection, DSP, thresholds, writer semantics, media or private data changes.
Policy's *actual chosen object identity*, supplemental TopK/partial-bar
candidate lineage, nested per-branch veto causes, CPU/RSS performance and
an independent pilot audit remain Phase-B gates. No Phase-C study until
causal completeness is genuinely achieved.

### Package B – capture actual Policy-selected object, not BPM proximity

A new **CI-only** observer records the *actual candidate reference* handed
to `select_mir_policy_candidate`. Its thread-local FullBoard span is set at
entry to `compute_mir_policy_decision`, and the observer matches pointer
identity by address equality to exactly one FullBoard array element. Thus
multiple candidates with identical BPM remain distinct. Direct material-risk,
partial-bar aggregate and TopK exact releases which bypass that selector
explicitly invalidate the FullBoard attribution and are reported as
supplemental/unindexed. If the final policy output BPM differs from the last
native-selected object, pointer evidence is also withheld.

The dedicated `policy_pointer` event distinguishes native same-output
FullBoard pointer, supplemental/changed pointer, unavailable observation and
zero policy output. When exact pointer identity is available, only that
candidate ID is attached to `policy_change`; otherwise there is **no
invented parent**. The original `policy_resolution` same-BPM-multiplicity
comparison remains separate from actual object identity. The validator
cross-checks pointer ID, exact BPM, 0/1 status and decision links; adversarial
tests reject forged, orphan, contradictory and stale IDs.

This measures the chosen **policy helper** object, not proof of the *entire*
writer pipeline: later residual micro-upgrades and supplementary rescue
stages may still alter the final BPM. Only an actual same-run witness can
authorize a numeric final output. The existing native production measured
witness guard and no-write checks are unchanged. Audio windows, CPU/RSS,
first-veto exhaustiveness and independent blind review remain separate gates.

### Package B – exact source-identity conservation across raw, cluster and FullBoard

Independent source review found a causal identity hazard in the CI observer:
the original raw→cluster reporter assigned **every** raw row within 0.001 BPM
to **every** nearby cluster. Actual `collapse_mir_candidates` instead uses
**ordered first-match assignment**. For three nearby raw BPMs, one raw row can
be within tolerance of two distinct cluster centers, making a naive proximity
join duplicate its ancestry and falsely imply two independently supported
sources. The observer now replays the native first-match partition, rejects
orphan/empty/reordered clusters, and the independent Python auditor verifies
every raw ID belongs to **exactly one** ordered cluster with the original
representative BPM. Positive overlapping-neighbor and adversarial missing,
reordered, duplicated and misattributed-source tests protect this invariant.

The policy FullBoard is a filtered, re-ordered transform of the same collapsed
objects. It may omit out-of-rail clusters, but **each surviving row must have
exactly one unique indexed cluster parent**. Both native capture and independent
validator reject unknown, repeated, orphan or missing FullBoard source IDs;
the former `full_board_origin_unknown` success path is no longer accepted.
A synthetic out-of-rail raw alias remains valid when it is *not* laundered
into an unrelated FullBoard row. Reports expose exact partition coverage,
overlap disambiguations, unique policy origins and filtered-cluster counts,
not personal audio information.

This is a **single end-to-end source-conservation gate**, not a claim of
complete branch-specific selector eligibility or music accuracy. Full native
Win32/x64 qualification and independent source audit remain required;
`pool_only_gate_unresolved` remains explicitly **INCOMPLETE** and package C
is still blocked. Productive MIR, policy, writer and user media are untouched.

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

### Phase B: native source-to-JSONL count conservation (26 October 2026)

One terminal CI-only `source_inventory` record binds the actual native
raw-board, collapsed Primary pool, Policy FullBoard, executed selector-event,
decoded-window and selected supplemental-object counts to the serialized
per-candidate trace. The independent parser re-counts all six event families,
requires exact nonnegative integers and one final inventory for every real
Win32/x64 host trace, and rejects missing, duplicated, reordered, fractional,
privacy-polluted or contradictory evidence. This catches a syntactically valid
but truncated source/selector capture that would otherwise look self-consistent.
A genuinely empty native raw board can now be distinguished from missing
capture without inventing a BPM or candidate identity. Positive and negative
source-only regressions cover both nonempty and empty native boundaries.

**Evidence boundary:** this proves source cardinality conservation at the
observed native board/selector boundary, not why MIR generated no raw
candidates, nor a terminal first-veto for every nonwinner. Exact-SHA real
Win32/x64 host CI and independent source review are still required. **Phase B
remains INCOMPLETE; Phase C CLOSED; musical NO_GO** until first-veto
exhaustiveness, positive native supplemental Policy/Writer identity and
independent audit are qualified.

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


### 25 Oct 2026: actual executed low/rescue trace extension (CI-only; Phase B still open)

- Low-pulse bestBy emits source-indexed score comparisons across ordinary and nested rescue selections; this is executed ranking, not a definitive first-veto verdict.
- Threshold and general-rail fallbacks emit separate source-indexed readmission events only when those paths execute. The strict validator requires a preceding actual provisional exclusion of the same candidate, rejecting fabricated readmissions and duplicate-BPM identity substitutions.
- High-pulse and harmonic residual rescue predicates, and the harmonic direct-protection gate, now emit their executed candidate checks. Gate admission may still lose in the original ranking/tie logic; rejection is marked provisional rather than a final exclusion.
- All native additions are under SMART_TEMPO_CI_ORACLE_PROBE. None changes production selector/DSP/Policy/Writer behavior. Full nested first-veto semantics, supplemental source IDs, CPU/RSS and independent Phase-B approval remain absent. Phase C and musical GO stay closed.

### 25 Oct 2026: fail-closed native trace capture saturation (prepared CI-only package)

The ephemeral selector recorder formerly discarded events silently at 4096
observations or on allocation failure, while an incomplete JSONL could still
look well formed. A failed snapshot of the executed primary input pool could
also lose provenance. The CI-only per-analysis capture-complete flag now
tracks overflow, event-allocation and pool-copy failures; the host trace gate
rejects incomplete captures before writing the disposable synthetic JSONL.
This protects diagnostic integrity only. It does not establish every first
veto or supplemental source ID, does not change production BPM/metadata and
does not authorize package C.


### 25 Oct 2026: CI-only process resource sampling (prepared, not a speedup claim)

Single-analysis entry/exit snapshots record elapsed wall time, process CPU-time delta, process-lifetime peak working-set before/after and end working-set. The native probe uses Windows process counters under the disposable synthetic fixture gate. The full-process CPU delta may include background host threads and the process-lifetime peak cannot be attributed entirely to the analysis; both limitations are explicit. Neither is a per-pass FFT profile or music-quality evidence. Python requires one bounded resource event on the strict real-host path, rejects nonfinite/non-monotonic/missing/duplicated values and foreign fields, and reports numeric aggregates. Production builds do not compile or emit this observer. No performance policy, candidate, writer or threshold changes; Phase B remains open pending independent host + code qualification.


### 25 Oct 2026: exact native supplemental Policy pointer provenance (CI-only)

Three typed supplemental sources—material-risk local-exact, partial-bar
aggregate, and partial-bar TopK—carry the actual selected object pointer.
The synthetic host recorder checks that pointer against the same-analysis
input objects (not an equal BPM), assigns at most one source-indexed ID, and
emits numeric-only trace. The Python validator rejects missing, duplicate,
foreign or mismatched pointer/candidate identity and preserves the distinction
between a selected Policy pointer and the final writer's measured witness.
This is not full first-veto or final supplemental witness completeness; no
production selector, writer or audio DSP changed. Phase C remains closed.

### Phase-B native-host trace contract hardening (25 October 2026)

The strict host parser now requires the actual ordered lane/residual/final
selector-terminal witness whenever a native host primary BPM was selected;
minimal source-only unit fixtures may omit this witness but **cannot** claim
native-host qualification. Primary outcome/reason pairs, orthogonal
writer-override/review-hold-release flag precedence and the zero-valued
CI no-write guard are checked bidirectionally. Adversarial regressions
explicitly attempt forged selected/held reasons, false writer-override and
hold-release attribution, missing terminal evidence and injected write-guard
numeric data. These are **integrity gates only**; they do not establish
first-veto completeness or positive real-host supplemental selection, and
therefore do not unlock package C or musical release.


### 25 October 2026: exact native supplemental-to-final-witness source ID (CI-only)

When the Policy's actually selected supplemental native object remains the
accepted numeric final BPM, the synthetic host recorder now includes its
already-indexed source ID as an additional same-run final measured witness.
This requires exact live pointer identity, not just a same-BPM FullBoard row.
The strict validator binds the supplemental final-witness ID bidirectionally
to the independently witnessed Policy selected pointer and rejects fabricated,
omitted or substituted supplemental IDs. Source-only positive/adversarial
regressions cover the two directions. This does not assert the musical BPM is
correct, that all alternate measured source origins have been enumerated,
or that every primary lane first-veto is accounted for. It is CI-only and
changes no productive BPM decisions, no metadata writes and no private data.


### Phase B: executed-path disposition for every Primary source object

A bounded CI-only native summary now assigns every actual collapsed Primary
input object one falsifiable, source-ID-preserving observation: final terminal
object, scored in an executed ranking but not ultimately final, observed in a gate or intermediate terminal without ranking, or no
recorded cluster-level observation.
Python strictly recomputes each disposition from the *earlier actual native*
selector event sequence, rejects missing/duplicated source rows for Windows
host qualification and catches retrospective re-labeling. Raw-only selector
lanes can leave nonwinning clusters with no cluster-level observation;
this is deliberately **not** interpreted as a final veto. This closes a
specific completeness question about observed vs unvisited candidate paths,
not the still-open comprehensive first-veto stage reasons. No production
selection, BPM policy, writer, or private data is changed.


### Phase B: comparator-parity and per-family evidence gate (CI-only)

The four observed comparator branches now have an independently checked
source contract: the CI-only winning predicate must match the original
production `if` predicate byte-for-byte after whitespace normalization,
while all observer variables remain behind the compile guard. Positive and
adversarial regressions mutate either side, remove a guard, or corrupt the
logged outcome. The read-only trace scorer reports explicit won/lost counts
for each of raw, harmonic, general-baseline and general-alternative families.
These are **executed comparisons**, not proof that a different lane or a
supplementary Policy source was selectable. A tie is still not independently
reconstructed from the full ordered tuple, and package B remains incomplete.


### Phase B: exact executed tie comparison and observed first exclusion (CI-only)

The CI-only selector observer now captures all four ordered production tie
components (support, winner support, cumulative score, negative route-center
distance) on the **raw** and **general-baseline** executed comparisons. The
strict Python auditor replays the original epsilon-sensitive score predicate
and lexicographic tuple, including equal-score wins/losses; harmonic and
general-alt comparisons retain their original score-only ordering. Extra,
missing, nonfinite, reordered or contradictory tuple values fail closed.
Source parity guards continue to require the original production predicates.

Separately, the auditor computes **the first actually observed exclusion**
for each source-ID-linked collapsed candidate from ordered native filter events,
with denominators for checked/unvisited candidates, later ranking and fallback
readmission. Raw-only exclusions remain distinct from collapsed candidates.
An observed exclusion is **not** proof of final ineligibility: fallbacks can
readmit, and uninstrumented lane decisions cannot be inferred. The native
pilot's existing `pool_only_gate_unresolved` remains explicitly unresolved;
this package does not declare Phase B complete, unlock C, modify production
DSP/Selector/Policy/Writer, or qualify musical BPM accuracy.


### 26 October 2026: four-case dual-architecture Phase-B host pilot

One shared, compiled-only allowlist now permits exactly four predeclared
public synthetic cases: pulse_60, pulse_128, pulse_120_sparse and silence.
Each has a one-case immutable generated reference manifest and its own
isolated original foobar Win32 or x64 host process. The same four cases
run in BOTH architectures, with media and manifest SHA-256/size/mtime
no-write checks, a disabled Update control and strict native lineage
validation where a candidate trace exists. No media, traces or binaries
are uploaded.

The scorer now reports explicit rhythmic reference/accepted/held
denominators, accepted absolute errors <=0.05 and <=0.20 BPM, accepted
outside 0.20 BPM, and negative falseaccepts. Any positive accepted BPM
on silence is a technical safety failure. The sparse case is metrically
underdetermined and cannot establish a musical correctness PASS.

This is a *Phase-B branch/negative-path pilot*, not the frozen large
Phase-C campaign. The no-candidate silence path may have no native
candidate trace and is reported INCOMPLETE_NO_CANDIDATES, never causal
completeness. All first-veto and supplemental-witness gates remain
unresolved; musical GO, genre-B, unseen music and production writes
remain blocked pending independent proof.


### 26 October 2026: independently verified zero-candidate negative lineage

The new Win32 pilot exposed an actual no-tempo native trace with **zero raw
and zero collapsed MIR objects**. The old validator rejected every such
trace as "missing raw/collapsed MIR", although the native reporter correctly
wrote the disposable trace and the independent BPM observer reported
silence abstention. The auditor now accepts this *only* when it proves the
complete zero-candidate terminal chain: no fabricated selector, FullBoard,
supplemental or parent identities; zero BPM at Primary, Policy, final and
witness; actual complete decoded windows and process resource observation;
no Policy write; rejected measured witness and blocked CI write guard.
Positive and adversarial source tests include forged acceptance, fake
Policy pointers, nonzero values and missing windows/resources/guard.
Both original hosts now REQUIRE the negative trace; no silent omission.

The accepted trace is explicitly
`INCOMPLETE_EMPTY_BOARD_CAUSAL`—a verified negative *no-candidate* path,
not a proof of all positive selector vetoes, a genre-B result or a musical
GO. Production MIR, policy, DSP and writer behavior remain untouched.

### 26 October 2026: joined original-host Phase-B pilot cohort gate

The eight fixed, predeclared synthetic pilot observations are now
joined across original foobar Win32 and x64 by one independent, bounded
CI-only source→host→scorer→trace→privacy/no-write aggregate.
It requires exact source SHA parity, case order, class parity,
correct/wrong/held and negative denominators, source-inventory
conservation, actual decode windows, same-run indexed witness and
process CPU/peak RSS. The positive and adversarial tests run before
SDK downloads. Only numeric allowlisted reports are held in
RUNNER_TEMP; nothing is uploaded.

**Evidence boundary:** `PHASE_B_CAUSAL_COMPLETENESS=INCOMPLETE`,
`PHASE_C_GATE=CLOSED`, `MUSICAL_RELEASE=NO_GO`.
A green cohort-integrity gate is NOT first-veto exhaustiveness,
positive supplemental Policy→Writer identity, no-raw causal origin,
real-music holdout, or measured speedup. No production code changed.

### 26 October 2026: original-host cohort harness integrity repair

Exact-head Windows run 40257481442 built both production and CI-probe Win32/x64
binaries but failed before the first original-host pilot: the PowerShell
harness was accidentally truncated at two regex conditions and contained
several duplicated trailing copies of its entrypoint. Python-only source
checks did not parse the PowerShell file and therefore missed this error.
The repair restores **one** complete original-host harness, reconstructs
allowlisted numeric scorer/trace evidence with duplicate/missing-key rejection,
and adds an actual PowerShell AST parse plus source structural regression to
the **pre-SDK** fast gate. The new cohort contract remains fail-closed; a
syntax PASS is not native-host qualification or musical evidence. Phase B
INCOMPLETE, C CLOSED, musical NO_GO. No production DSP, selector, policy,
preferences, writer, user media or binary publication changes.

### 26 October 2026: exact-SHA original-host cohort serialization repair

The exact `2bfbdadd...` Windows source job `121376689566` compiled and loaded
both original architectures, then completed all four real Win32 synthetic
observations before failing at the **joined-cohort JSON input boundary**:
PowerShell's `[Text.Encoding]::UTF8` wrote a BOM and Python's strict `utf-8`
JSON reader rejected it. The host now explicitly writes **UTF-8 without BOM**;
the independent CLI keeps rejecting BOMs and now also rejects duplicate JSON
keys at any depth. Both per-host and joined-host validations bind their
reported source SHA to the actual checked-out git HEAD. The pre-SDK gate runs
positive two-host and adversarial BOM/duplicate-key/stale-SHA CLI tests.
These changes repair evidence transport only, not MIR, candidate ranking,
Policy, Writer or any musical decision. Real Win32/x64 joined-cohort PASS must
be established by a new exact-SHA CI run; Phase B causal completeness and
musical quality remain **INCOMPLETE / NO_GO**, Phase C **CLOSED**.


### Package B: actual native MIR supply and per-object origin conservation

The isolated CI probe captures full-MIR segment evaluations, retained segment
results, TopK rows/submissions, family ranks, pulse-alias and continuous
submissions, and newly created raw objects per native producer. Each raw object
has a compile-guarded TopK/pulse/continuous origin mask (including upserts).
The independent Python auditor checks count conservation and origin membership,
including adversarial negative fixtures, before the Win32/x64 host gate.
Only bounded numeric counts and fixed enums are emitted. Production DSP,
selector, preferences and writer are unchanged. This proves the candidate
supply boundary, NOT upstream why no TopK/family rows, all first-veto causes,
or a positive supplemental Policy→Writer witness. B INCOMPLETE; C BLOCKED;
musical NO_GO until independent complete evidence.


### Phase B – bounded executed selector branch coverage (26 October 2026)

The CI-only native observer now records the actual executed membership checks
of all eight nested low-pulse rescue pools (58–61 mid/lower-mid, 56–58 mid-low,
64–67 deep-low, 79–81 lower-mid, 86–90 upper-direct, 50–53 very-low and
upper recovery), the general-144 alternate window, and both conservative
raw alias/support fallback readmissions. Every event is source-indexed and
keeps provisional exclusions distinct from terminal eligibility; a later
readmission is independently required to have a preceding actual exclusion.
The strict JSONL parser rejects fabricated gate outcomes, wrong source IDs,
missing/reordered fallback predecessors and duplicate keys even in nested
metrics. Positive and adversarial synthetic fixtures run in the early gate.

**Scope limit:** the selector's nested bestBy score-only comparisons, full
first-decisive candidate veto semantics across all lane branches, positive
real-host supplemental Policy-to-Writer source identity and upstream zero-MIR
causation still require a single independent closure audit. A provisional
negative gate is not an irreversible veto. This source/trace coverage does not
change production DSP, policy, writer, preferences or user files and does
**not** authorize Phase C or musical release. The source gate and exact-head
original Win32/x64 host must pass before these observations count as measured.

### Phase B: native MIR evaluator first-return causal ledger (26 October 2026)

The CI-only source observer now captures one bounded, source-reason-whitelisted
`mir_segment` row per **actually executed** Hodgkinson MIR evaluation,
including pass identity, onset-peak count, native first-return reason,
actual TopK cardinality, candidate flag and whether the segment entered the
track-level MIR result accumulator. It uses the evaluator's existing reason,
not an inferred cause from a later empty board. The independent parser
reconciles segment indices to decoded windows, per-segment TopK sums to the
native source inventory, and accumulator counts to actual producer counts.
A separate strict host flag enforces the full ledger while legacy source-only
minimal trace fixtures remain testable. Positive and adversarial negatives
cover forged reasons, peak thresholds, missing rows, TopK count drift,
nonintegral indices and private fields. The **first MIR early-return gate**
can now be identified on zero-board audio when actually observed; later
TopK/alias rail filtering still requires distinct evidence. No source-oracle
BPM, real-music data, DSP/selector/policy/writer modification or user-file
write is introduced. Package B remains incomplete until branch-specific
first-veto and positive actual supplemental Policy-to-writer provenance are
independently proven on native Win32/x64; Phase C CLOSED, musical NO_GO.


### 26 October 2026: executed nested low-pulse bestBy comparator episodes

The previously untraced low-pulse bestBy branch now records exact C++
incumbent comparisons in the disposable CI-only synthetic host, including
all nested rescue calls. Each nonempty bestBy invocation receives an ordered,
numeric, per-analysis pool epoch; *rank and immediately adjacent comparison*
share the exact epoch and actual cluster object ID. The independent Python
auditor resets its score incumbent only on a strictly increasing adjacent
pool epoch and replays all score-only > comparisons. Forged omitted/mismatched
rank, renamed candidate, false winner, missing/duplicate/revisited/skipped
pool or nonintegral epoch is rejected. Source-parity tests prove the CI
comparison predicate matches the productive selection predicate, without
changing productive comparison itself. This closes a meaningful previously
untraced low-lane nested ranking chain, but not branch-specific final
first-veto, positive real-host supplemental Policy->Writer identity, or
musical precision. Those still block Phase C and release. No user data,
metadata writer, production DSP, policy, preferences or thresholds changed.



### Phase B: executed comparator winner to actual lane terminal (CI-only)

The independent trace auditor now carries each *executed* raw, harmonic,
general-baseline, general-alt and low-bestBy comparator's final **source
candidate ID**, not merely its winning score. It checks the actual first
`terminal_lane_choice` against a winning collapsed source, the native raw
winner's first-match cluster parent, or the separately observed low continuous
rescue eligibility. Every low-bestBy invocation has its own source-indexed
pool epoch. Positive and adversarial tests cover all five comparator families
and a later strictly better winner with a deliberately stale terminal ID.
A valid BPM or existing cluster ID alone cannot launder a lost candidate into
a selected lane object.

A low continuous-center rescue has only an executed *eligibility* observation,
not its full native five-field comparator tuple. The audit therefore reports
`selector_lane_continuous_rescue_unranked=1` separately and **never** claims
that this branch has a proven winner. Other later residual/direct rescue
decisions are also not certified by this first-lane check. This source-only
gate must still pass the exact-HEAD original Win32/x64 host and negative
regressions; it does not resolve the 51 first-decisive Primary veto reasons
or provide a positive supplemental Policy-to-writer host witness. Package B
remains **INCOMPLETE**, C **BLOCKED**, musical release **NO_GO**.

### 26 October 2026: pre-SDK low-episode fixture + report-key repair

Initial exact-head native low-bestBy instrumentation CI 40293133050 failed
at the Python pre-SDK gate, with two concrete **test/evidence contract**
errors: one old fallback regression supplied a `low_rank_scored` row
without the now-required native pool episode and comparator, and the new
positive multi-family regression expected `selector_compare_low_wins` but
its summary key was not yet emitted. The following test/parser-only repair
makes the historical fallback positive fixture reflect the executed rank+
comparison and publishes the independently recomputed low won/lost counts.
No host, SDK or musical pass is inherited from the failed parent; the new
exact SHA requires full source and original Win32/x64 host qualification.


### 26 October 2026: actual residual/direct terminal winner replay (CI-only, awaiting exact host)

The previous first-lane comparator proof could not distinguish an actual
residual rescue or later harmonic-direct protection from a forged terminal
pointer. This package records the *executed* eligible source, rank and
immediately adjacent winner/loser comparison for the high-pulse residual,
harmonic residual (ordered evidence/support/winner-support tie) and harmonic
direct-protection (ordered support/combined/winner-support tie) branches.
The independent parser replays the exact comparator order, requires each
rank to follow its actual eligible gate, checks the correct phase between
the three native terminal snapshots, and requires the residual and final
terminal object IDs to equal their respective executed winners (or to
retain the preceding object if no rescue won). Positive and adversarial
source-only fixtures exercise all three families, stale terminal pointers,
missing gates/comparisons, wrong winners, misplaced phases and tie tampering.
This is a causal *source-object identity* gate, not a BPM-quality improvement.
The CI-only recorder is compiled exclusively with
SMART_TEMPO_CI_ORACLE_PROBE; no productive DSP, selector predicate, policy,
writer, preferences, metadata or audio was changed. A new exact-HEAD
original Win32/x64 host run is required to qualify this change. First-veto
completeness, unexercised nested low rescue and positive native supplemental
Policy→Writer selection still block B and C; musical release remains NO_GO.


### 26 October 2026: independently localized synthetic accuracy errors (Phase B)

Without touching the source engine, an isolated post-host Python-only scorer
now joins each of the four immutable public synthetic B-pilot references to its
strictly validated *actual* source-to-writer trace. For each rhythmic case it
computes how many raw MIR objects, clustered objects and FullBoard candidate
objects are within ±0.20 BPM of the reference, and separately whether the
Primary/Policy/final accepted BPM is within that window. It classifies the
**earliest observable stage** where the expected BPM disappeared: no raw
source, lost clustering, absent Policy board, other Primary winner, other
Policy winner, different final writer witness, held result or correct accepted
result. The silence negative must have no fabricated source objects and must
abstain. The synthetic-only reference is read AFTER foobar worker completion,
never passed to the component/DSP/selector/Policy or productive writer.

The executable native-host PowerShell gate requires the independent post-host
scorer's success marker and prints bounded predeclared numeric stage counters
and fixed reason enums. Python positive and adversarial tests run before SDK
checkout. This differentiates candidate-supply problems from ranking/policy
problems without changing any production audio logic or making an external BPM
oracle into an engine input. The four cases are still a SMALL synthetic pilot;
no Phase C, measured improvement, real-music generalization or musical GO.

### 26 October 2026: original-host native low-pulse branch from synthetic external CUE

The four untagged BPM-pilot WAVs cannot establish matched low-genre
routing, and the host trace correctly reports zero low bestBy episodes.
A **separate** synthetic external-CUE control declares standard
`REM GENRE "Chill"`, references the already-generated pulse_60.wav,
and runs the ordinary foobar file/metadata/genre mapper and original
MIR selector on both Win32 and x64. No routing context, BPM reference,
genre field or threshold is injected into the productive code.

The bounded original-host source-indexed observer MUST show a
low-pulse bestBy epoch and a winning low-lane comparison; otherwise
the host gate fails closed. The generated WAV, external CUE and one-case
reference manifest are verified by SHA-256+size+mtime across host setup,
diagnostics, disabled Update and teardown, on each platform. The
compiled temporary file allowlist admits exactly the new CUE suffix
under RUNNER_TEMP. The synthetic reference score is checked, but the
new CUE remains OUTSIDE the frozen 4-case accuracy/cohort and post-host
candidate-recall localization denominators. The strict four-case
joined audit remains unchanged; it may carry allowlisted numeric
low-comparator count fields without accepting new/private sources.
This branch fixture must pass exact original-host Win32+x64 qualification
before being called an observed positive selector transition.

This is Phase-B source/host branch-coverage only, NOT causal first-veto
completeness, a positive supplemental Policy source, performance
improvement, real music qualification or musical GO. Production DSP,
genre rules, preferences, selector, Policy, writer and owner media
remain unchanged.

### 26 October 2026 — frozen post-host Phase-B shadow-ranking comparison

The real, independent post-host recall oracle already proves that the
correct pulse_60 and pulse_120_sparse BPMs survive MIR raw, collapse and
Policy FullBoard, and that *Primary* selects another BPM. To isolate the
contribution of candidate ranking **without touching production behavior**,
a CI-only post-host independent source-ID scorer now compares four fixed
unconstrained candidate-ranking controls on the *same native collapsed
Primary pool*: (1) maximum raw score sum; (2) maximum segment support;
(3) maximum native best-combined score recorded by Primary eligibility;
(4) maximum score sum × support. Ties keep the original source order;
none of the rules uses the reference BPM, filename, genre, aliases, or a
per-case learned parameter during selection. They are diagnostic baselines,
not candidate production policies or suggested fixes.

Only after foobar has completed, Python validates the original exact
same-analysis candidate IDs and strict native trace, independently checks
the frozen synthetic reference and actual Primary→Policy→Writer output,
scores each hypothetical winner within the predeclared ±0.20 BPM window,
and prints predefined bounded numeric counters. The original Win32/x64
host hard gate checks completeness, identity, source/score conservation and
separation from the standard four-case accuracy cohort. The joined
dual-architecture cohort additionally checks the source-indexed shadow
results for cross-host decision parity and strict allowed fields. Silence
remains a negative with *zero* invented candidates or selected winners.
Positive/adversarial tests corrupt candidate IDs, score/eligibility joins,
host observation, reference manifest, candidate presence and cross-host
shadow parity; each must fail closed.

A shadow policy appearing stronger on **three** public rhythms would be
an in-sample observation with a high overfit risk. No formula, coefficient
or candidate preference can be installed from this pilot; independent
Phase C alternative-architecture registration, numerous complex rhythms,
half/double and 3:2 controls, genre classes, multiple window sizes and
private blinded holdout are still required. Phase B first-decisive
per-object veto and selected supplemental Policy→Writer native positive
remain incomplete. Product algorithm, selector, writer, preferences,
user tags/cuesheets and published artifacts stay unchanged. No musical GO.


### 26 Oct 2026 — independent post-host Primary comparator attribution (B subgate)

The original four-case Win32/x64 native pilot now joins its **already strictly validated** raw-to-cluster source IDs and actual executed Primary rank/compare events with the frozen public synthetic reference **only after foobar exits**. The separate scorer reports whether a reference-near cluster was absent, never ranked, ranked but lost, or won an executed comparator family without becoming the final Primary lane object; it also counts ranked near-source IDs, observed wins and last family winners, and labels the first observed **provisional** exclusion (not a definitive veto). Both architectures must produce a bounded comparator-provenance PASS or the host gate fails. The original reference/held/negative denominators remain unchanged. This is not a causal proof of which branch-specific veto is decisive; branch choice, low nested rescues and supplemental final writer object remain Phase-B blockers. No production DSP, policy, selector or writer changes.

### 26 October 2026 — genuine host Review-Hold source correction

The first Win32 run of the shadow package (40313818151) passed source,
adversarial, build, original foobar load and dispatcher gates, then failed
during the very first rhythmic synthetic host case: the diagnostic scorer
incorrectly required `primary_select.outcome == selected` whenever the
Primary had a valid same-run selected-object ID. The live source evidence
proved a **real selected source with Primary Review Hold** and a later
Policy writer override. Review Hold is a legitimate disposition of a
measured Primary object, not loss of its identity or an automatic rejection
of the later measured Policy source. The scorer now accepts source-indexed
`selected` or `review_hold` and requires exact candidate-ID/BPM
consistency in both; genuine `no_candidate` can have no source pointer.
Both valid cases and a forged source ID receive explicit adversarial tests.
No productive Primary, Policy, CUE, selector, writer or review decision was
changed. A fresh exact-head host CI is mandatory to qualify the correction.


### 26 Oct 2026: where are reference-near candidates inside frozen shadow orders?

The first exact-head shadow Win32/x64 run 40315094059 succeeded all 24
native/architecture and no-write gates. Untuned SOURCE-only rank winners
were respectively 1/3 (score sum), 0/3 (support), 2/3 (best combined),
1/3 (score×support); native original 1/3, with full agreement across both
architectures and zero public artifacts. The top combined model fixed the
60 BPM case but still missed 120 BPM sparse. These are three synthetic
rhythms, **not** model selection, comparative superiority or musical GO.

A strictly post-host secondary diagnostic now freezes the ENTIRE candidate
order for each of the *same four unchanged source-only metrics*, preserving
native cluster ordering on score ties, then reports the one-based position
of the nearest reference-band candidate and whether it appears in top 3.
Reference BPM is joined **after** the source-only order exists, as before;
no oracle-dependent algorithm, adapted threshold, model learning or
production change is introduced. Missing references and genuine negative
silence are encoded as rank 0, not silently invented. Unit adversaries
exercise ties, four-way near candidate loss, wrong rank / top-3 falsification,
out-of-bounds ranks, host source-count conservation and Win32/x64 parity.
Full native exact-head Win32/x64 host qualification remains mandatory.
Only independent synthetic post-host read-only numeric diagnostics; no
writer, DSP, Selector, Policy, settings, private music, release or merge.


### 26 October 2026: actual low continuous-center five-field winner replay

The **production** low-pulse continuous-center rescue previously produced only
allowed/excluded source observations and an explicitly UNPROVEN lane-terminal
transition. The CI-only C++ observer now captures actual rank/comparison
pairs for **each executed eligible continuous-center candidate**, including
the full five-field tuple (continuous-score ratio, section support, section
stability, absolute continuous score, negative route-center distance) and the
exact incumbent winner. The independent Python auditor replays the strict
lexicographic comparison, enforces immediate allowed gate → rank → comparison,
source-ID/BPM conservation, native terminal source equality with the last
actual winner, five-field numeric-only framing and all ordered outcomes.
Forged verdicts, swapped source objects, missing tuple fields and a promoted
ranked loser must fail before native acceptance. A legacy filter-only unit
trace may be reported as incomplete but may **never** receive native-host
qualification. The productive five-field C++ comparison, candidate set,
thresholds, route, review hold, Policy and writer remain unchanged.

This is **not** proof that any fixed synthetic WAV actually triggered the
continuous-center rescue, nor completion of all first-decisive candidate
vetoes or Policy supplemental provenance. Win32/x64 original-host source
qualification is mandatory. Phase B still INCOMPLETE, C CLOSED, musical NO_GO.


### 26 October 2026: original host positive and negative actual C++ continuous-center comparator proof

The independent synthetic post-host parser now has full five-field
lexicographic provenance, but the four frozen decoded-WAV pilot cases did
not trigger this branch (Win32/x64 actual 0 comparator events). To avoid
falsely counting a positive branch not exercised by those WAVs, an
**additional, clearly separated CI-only in-host native selector control**
now constructs a public three-candidate `MirCandidateBoard` (60/96/99
BPM) in the foobar process, using only invented support and continuous
feature values. It calls the **actual productive**
`compute_mir_primary_selection` function (not a shadow ranker),
once with route-center 99 and once with center 96. Both use the same
three input candidates, same matched public `Chill` route, same feature
evidence and normal production selector. The full recorded native
five-field comparisons must prove the winner changes from 99 to 96,
with 2 wins/0 losses and 1 win/1 loss respectively, and that the lane
terminal exactly references the chosen cluster. CI-only code creates
a unique PASS marker strictly adjacent to the synthetic input under
RUNNER_TEMP, which the real Win32 and x64 host harness must verify
**before** it credits the original diagnostic result. It may never be
counted as a decoded-audio BPM-accuracy case or substituted for the
original four-case WAV/CUE decoder/writer-safety and independent
host-trace audit. A fake marker without native in-process evidence,
a reordered or stale winner, a missing fifth field or an incorrect
candidate source must fail closed. No production DSP/selector/writer,
user files or publication gate is changed.

## Phase-B native supplemental object and writer-invariant control (26 October 2026)

A CI-probe-only C++ control now calls the **actual existing** partial-bar
TopK Policy release selector on two distinct, equal-BPM, public fabricated
source objects. It verifies that the stronger second object is the actual
selected pointer (not an equal-BPM lookup), then executes the production
measured-candidate writer invariant with only those TopK sources present.
Negative controls reject an unwitnessed 145 BPM output, a >0.001 BPM
substitution, disabled/incorrect route and generic-route conditions, an
unarmed family conflict, and insufficient local-exact evidence. The original
Win32 and x64 foobar diagnostic harness requires a separate, exact-content
CI marker from this control on each host case; source-contract tests and
PowerShell AST parsing run before SDK compilation.

**Evidence boundary:** This is genuine native C++ execution inside the
original foobar host with a *fabricated in-process* candidate board. It is
NOT evidence that decoded WAV audio naturally selected a supplemental TopK
source, nor does it prove final Policy→writer provenance for that decoded
audio. The four-case original WAV pilot retains 3 rhythmic/1 silence
denominators per architecture; Phase B remains **INCOMPLETE** and Phase C
**CLOSED**. No productive DSP/selector/Policy/writer/threshold changes.
The exact-commit CI result must be read before claiming this new control
passed; no user audio, tags, CUE, databases, artifacts or binaries are written.


### 26 October 2026: actual native three-stage low-pulse nested rescue with negative alias control

A separate CI-only original-host C++ control now runs the productive
`compute_mir_primary_selection` twice on small, publicly fabricated
same-evidence candidate boards containing 60/80/71 BPM. With an 80-BPM
**two-thirds alias** bit, the executed low-pulse ranking chain must consist
of three independent bestBy episodes (3, 2 and 1 source comparisons),
selecting winners 60 → 80 → 71. Without that one alias bit, the same
candidate values must take **only two** episodes and retain 80 BPM. Exact
source-index, per-episode score/comparator adjacency, 1-based contiguous
epoch resets, 3/2/1 ranking counts, actual terminal pointer and winning BPM
are asserted from native CI-only observer events. Failure emits NO PASS marker.
The real original foobar Win32 and x64 host runs require a unique
runner-temp marker generated only after both productive selector calls
pass; a fabricated marker cannot be accepted as decoded-audio evidence.

This positive/negative *in-process selector branch* qualification is not an
additional decoded-WAV BPM reference, not actual MIR front-end natural
selection of this branch, and does not alter four-case accuracy denominators.
It cannot establish global per-candidate first-veto completeness or natural
selected supplemental Policy→Writer witness; those remain Phase-B blockers.
No production DSP, comparator, threshold, Policy, CUE, writer, preference,
user audio or private data is modified. Fail-closed release NO_GO retained.


### Phase B: independent executed-comparator candidate-fate ledger (26 Oct 2026)

The new source-only Python auditor `tools/audit_smart_tempo_b_candidate_fates.py`
replays the **same native source IDs**, executed rank/compare pairs, actual
score/tie incumbents and per-source dispositions **after** the strict native
trace validator. For each collapsed source it assigns exactly one observable
fate: final selected, winner of an unselected comparator pool, earlier winner
later displaced, comparator loser, gate-only observed, or never visited. The
six disjoint counts must equal the native collapsed-board count. Provisional
exclusions and exact-source later readmissions are counted separately; no
BPM-nearest joins or ground-truth labels are allowed. Positive/adversarial
regressions cover source substitution, fake comparator win, incumbent score
drift, invented readmission, false disposition, missing rank, duplicate
terminal, and empty-board abstention. The original Win32 and x64 foobar host
script now demands this independent partition for every bounded synthetic
WAV/CUE case and checks it against the actual native source inventory.

**Important limitation:** an executed comparator loss or provisional gate is
not automatically a *first decisive exclusion*. Candidates can win another
pool, be readmitted, or be bypassed by a branch that never executed. The
ledger intentionally leaves the first-decisive-veto denominator unresolved
for **every** collapsed source; it is a chronology/conservation subgate, not
permission to start Phase C. No productive DSP/selector/policy/writer change,
private audio, public binaries or user writes. This change requires an
exact-head original-host Windows CI PASS before native qualification.


### 26 October 2026: source-indexed native raw alias/support fallback on actual foobar hosts

The CI-only original-host driver now calls the unchanged real C++
`compute_mir_primary_selection` on three variants of a fixed two-source
(103/129 BPM) board under the actual `sparse_or_acapella` lane. The
independent exact native-event order is checked for three paths: (A)
both raw aliases provisionally excluded and **both readmitted**, then
support filtered and **both readmitted**; (B) only the 129 BPM direct
alias admitted, with no fallback; (C) both aliases readmitted but only
the 129 BPM source surviving the support floor, thus **no support
fallback**. Every path checks native score/rank/compare adjacency,
source-index identity, event reason/outcome, and the same 129 BPM
terminal clustered source. The variants exercise the original selector,
not synthetic Python or a new production threshold.

Win32 and x64 original foobar driver must emit their unique runner-temp
`.native-raw-fallback` marker only after **all three** paths pass;
PowerShell validates the exact marker in each original-host media test.
The frozen four-case decoded-audio accuracy denominator stays unchanged.
A provisional `raw_alias_provisional_exclusion` must never be claimed
as a decisive veto when a later fallback readmits the same source.
This proves these *in-process* C++ branches, not that ordinary decoded
WAV/CUE naturally took those particular paths. Remaining Phase B
first-decisive universal veto and selected supplemental decoded-source
Policy/writer lineage gates are still INCOMPLETE; Phase C CLOSED and
musical NO_GO. No private media, productive DSP/threshold/writer,
user metadata/DB writes, merge, tag, release or public binary.

## Native-host adversarial provenance rejection (CI-only)

The source-indexed pipeline audit and candidate-fate ledger are now followed
by `tools/audit_smart_tempo_native_mutations.py` on **each actual Win32/x64
original-host pilot JSONL trace**, including the empty/silence negative.
The independent tool first validates the unmodified host trace, then corrupts
separate in-memory copies (sequence, analysis identity, privacy field, decoded
extent, MIR numeric validity, process resources, raw/window source inventory,
primary candidate IDs, comparator verdict, Policy flags, final witness and
writer guard). It must observe a parser/fate-replay rejection for **every**
applicable corruption. The tool neither reads reference BPM nor writes a trace,
media, tag, CUE, database, or artifact. The FAST source gate also exercises
positive and negative synthetic fixtures and a deliberately permissive auditor
to prove that an escaped corruption fails the gate.

**Interpretation:** This checks the integrity and fail-closed rejection of
the *observed* production-path telemetry. It is not a first-decisive-veto
proof for unvisited candidates, an eligibility proof for merged manual-review
rows, a musical hit-rate improvement, or permission for Phase C/production
selector or writer changes. The workflow now checks out the **exact PR head SHA** rather than GitHub's
synthetic pull-request merge commit and fails before compilation if its Git
HEAD differs. Source SHA, original host CI job and negative
mutation count must be reported together; only the actual completed Windows
run can establish runtime qualification.


### 27 October 2026: actual native Policy FullBoard rail provenance (CI-only)

The original `prepare_mir_policy_fullboard_candidates` now emits an indexed,
CI-only decision for **every** collapsed source before its 40–220 BPM
FullBoard filter: source identity, actual local-exact-resolved BPM, inclusion
or decisive rail veto. The independent parser checks original source order,
exactly one verdict per source, numeric gate truth, and one-to-one FullBoard
membership/BPM identity. Original Win32/x64 foobar host and mutation gates
require the proof; missing/forged/reordered/veto-flipped evidence fails closed.
Primary eligibility, merged manual review and downstream Policy selection
remain separate. No production DSP/selector/Policy output/writer behavior
change, no owner media or labels; Phase B INCOMPLETE, Phase C CLOSED,
private musical acceptance NO_GO.

## Phase-B General-144 native promotion provenance (CI-only, 2026-10-27)

This source-bound qualification slice closes one previously unobserved causal
decision **after** the General lane baseline comparator. The original C++
selector can consider a 126–141.5 BPM alternative when its initial winner
is a 143–145 BPM 3:2/4:3 alias and the route center permits it. An alternative
being ranked is not proof that it wins the later evidence comparison.

- CI-only native events now distinguish the actual baseline object,
  no-challenger/non-144/route bypass, exact baseline and challenger measured
  evidence, and the original `challenger >= 1.20 * baseline` promotion verdict.
  The production predicate, candidate ranking, final BPM, ReviewHold and
  writer path are untouched. No reference/oracle BPM enters this decision.
- The independent JSONL auditor enforces source IDs, stage chronology,
  challenger comparator winner, numerical evidence ratio and exact terminal
  lane object. Positive promotion, retained baseline, missing challenger and
  bypass are all tested; corrupt source identity, verdict, ratio, evidence,
  chronology and terminal transitions fail closed.
- The CI-only original foobar Win32/x64 host dispatcher exercises all four
  cases on disposable **fabricated** candidate boards and emits a guarded
  marker only after the real selector passes. The host harness requires the
  marker alongside existing no-write WAV/CUE and pipeline traces.
- This is **not** a real decoded-WAV observation of a General-144 promotion,
  not the complete first-veto proof for all primary branches, and not a
  replacement for independent musical holdouts. Package B remains open;
  Package C and production changes remain gated. The technical CI result
  must be attached to its exact post-change commit before qualification.


## Package B — applied native writer resolution vs Policy proposal (CI-only)

The source-only trace's former `policy_change` describes a **Policy helper
proposal**, not necessarily the final action taken by the original analyzer.
The new `writer_resolution` event is captured **after** the original MIR
Policy runtime branch and same-run measured-witness invariant, from the
actual bounded `m_diag.policy_reason` enum. It never serializes diagnostic
free text, metadata, oracle BPM, media paths or user tracks. It also records
whether Policy runtime was enabled and whether the helper proposed a write.

For a final primary result, the writer parent must be the actual selected
terminal cluster object. For an applied override, Review Hold release or
micro-upgrade, a parent is emitted only when the **native Policy-selected
pointer** matches the final numeric BPM. If the effective output changes or
the Policy pointer was not observed, the event is deliberately unindexed;
a same-BPM search cannot manufacture writer identity. Held/no-candidate/
invariant-veto results carry no accepted source parent.

The independent Python writer oracle checks seven native applied outcomes,
Policy proposal/runtime consistency, final BPM/outcome, source-pointer
identity, and the measured-witness result. The original Win32/x64 foobar
host pipeline requires exactly one applied writer event for each synthetic
case, and its adversarial audit mutates the **actual host trace** to reject
missing, forged, reordered and contradictory writer reasons, flags, BPMs
and source IDs. The host additionally conserves indexed + unindexed
accepted writer results against the actual final-accept counter.

This remains **Phase B incomplete**, not a license to proceed to the frozen
Phase C corpus. Missing first-decisive Primary veto coverage, never-visited
branches, all supplemental/partial-bar source identities, and an independent
review of complete candidate causality are explicitly unresolved. Even
if source/host CI is green, it is not independent real-music BPM QA.
No productive DSP, Selector, Policy, writer, media or preferences change.

## CI-only native manual-export provenance (Phase B, not final eligibility)

The native analyzer's **actual** manual-review vector is passed to the
disposable observer after measured-witness/Writer resolution. The observer
captures requested/not-requested state and source-indexed numeric inputs for
Policy FullBoard, selected Primary, material risk, partial-bar aggregate and
partial-bar TopK. Each native merged output has first-match source IDs, a
bounded five-bit origin mask and the final rank/score/support. No source
labels, titles, genres, filenames, oracle BPM or paths are serialized.

An independent Python replay checks 0.001-BPM first-match grouping, numeric
maxima, minimum positive original rank, stable sort and final unranked rank.
Native host mutations target actual output rows. Not-requested capture is
explicit missing evidence, never complete manual coverage. Supplemental-only
origins are not proof of Primary eligibility. No production DSP, selector,
Policy, writer or user-media behavior changes. This block does not close
first-decisive-veto and unindexed Policy gaps; Phase C remains gated.

The fixed CI-only allowlisted synthetic host invokes the unchanged productive
manual-merge implementation even when ordinary UI capture was not requested.
The Win32/x64 host gate must observe requested capture and at least one
source and one merged output for each rhythmic pilot. Silence may correctly
have zero candidates. This does not enable diagnostic capture in production
builds or on nonallowlisted media.


### 27 October 2026 — native Primary ReviewHold cause provenance (CI-only)

The original Primary selector's final flags and prioritized native reason are
recorded after its actual terminal object and before Policy. The `primary_hold`
row carries the exact selected source parent, all four simultaneous hold
predicates, candidate/auto/route flags and the original reason. An independent,
label-blind Python oracle checks native priority, OR semantics, source identity
and consistency with the applied writer reason. The original Win32/x64 WAV
host gate must pass at the exact new SHA; synthetic fixtures are positive and
negative contract tests, not musical acceptance. ReviewHold and fail-closed
writer semantics remain unchanged. Phase B stays incomplete; Phase C is closed.
