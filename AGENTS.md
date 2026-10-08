# AGENTS.md

## Repository role

This public repository is the canonical source-development repository for Smart Tempo. Keep all owner music libraries, audio, track/title/artist lists, BPM ground truth, holdouts, user filesystem paths, private research reports and raw private telemetry outside this repository and outside public CI.

Do not copy history, reports, fixtures or artifacts from the historical private repository merely for convenience. Port only source/tooling changes that are independently justified and contain no private data.

## GitHub-first development

- Use branches and pull requests against protected `main`.
- Public GitHub Actions are the primary source-build and synthetic-test path.
- A green source build is not a BPM-accuracy, real-audio or foobar runtime qualification.
- Do not upload or publish compiled Smart Tempo binaries while the redistribution gate in issue #2 remains unresolved. Ephemeral CI compilation/package validation is allowed.
- Bind build evidence to the exact source commit being tested.
- Keep source changes small enough that a failed gate can be attributed to a bounded diff.

## Engine invariants

- The final BPM must remain a genuinely measured audio candidate. Do not synthesize a BPM from metadata or references.
- Titles, artists, filenames, ground truth and external BPM references must never select, generate or tune a production BPM.
- Genre/metadata routing may provide only the already-defined soft prior/routing behavior; audio evidence remains authoritative.
- Review Hold is a valid safety outcome when evidence is insufficient.
- Do not add track-specific exceptions.
- Accuracy changes require private/offline evaluation before release admission; only appropriately redacted aggregate results may return to this public repository.
- Performance work must not change candidate identity, branch ordering, thresholds, tie-breakers, writer semantics or hold behavior unless a separately reviewed accuracy change explicitly intends to do so.

## Tag-write safety

- Generic writes to known unsafe non-zero MP3/FLAC/WavPack embedded-Cue subsongs fail closed before host async dispatch.
- Mixed selections containing a blocked target fail atomically.
- Do not infer that external CUE or other virtual carriers share the same semantics without qualification.
- Do not introduce a generic embedded-Cue BPM serializer without an explicit format/storage contract and preservation tests.

## Local foobar2000 runtime contract

Real host testing is separate from source builds. When a runtime gate is explicitly required, use:

- Win32: `C:\Projects\foobar2000\32bit`
- x64: `C:\Projects\foobar2000\64bit`

Never use runtime directories as source checkouts or build worktrees. Do not run host/runtime tests during source-only work merely because these installations exist.

## Agentic execution and model economy

- Read this file before changing source, CI, packaging or research tooling.
- Work agentically and keep the coordinating context small.
- Use the smallest capable model/reasoning level for mechanical inventories, hashes, log extraction and bounded edits.
- Use medium reasoning for implementation after the contract is frozen.
- Reserve high reasoning for architecture, safety/security boundaries, parity decisions and independent result/source audits.
- For research gates that can unlock real-data or runtime progression, implementation and independent audit must not be the same agent/context.
- Subtasks may report evidence; they do not self-authorize progression to a later research/runtime phase.
- Never weaken privacy, licensing, measured-candidate or fail-closed writer boundaries to make CI pass.
