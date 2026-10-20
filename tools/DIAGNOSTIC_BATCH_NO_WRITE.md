# Smart Tempo diagnostic batch: no tag writes

The dedicated foobar2000 context action **Smart Tempo > Analyze selected
tracks (no tag writes)** is separate from ordinary Smart Tempo Analysis.

## Safety boundary

- Analyze every selected track, including tracks already carrying a BPM tag.
  Do not show the normal rescan/overwrite prompt or silently skip tracks.
- Ignore the persistent **Write tags automatically** setting for this run.
  The result dialog's auto-write dispatcher is explicitly bypassed.
- Never create/update the unmatched or BPM review playlists in this mode.
- Present result rows for comparison. The result window is visibly labeled
  `DIAGNOSTIC / NO WRITE`. The tag-writing button is disabled and the
  underlying handler refuses any write even when invoked programmatically.
- Manual tap, halve/double scaling, and measured-candidate result edits are
  unavailable in this mode. All corresponding handlers also have guards.
- The existing normal context menu and its write behavior remain unchanged.

**Scope:** These guards prevent the Smart Tempo tagging paths from being
called by this new action. This is NOT a claim that all third-party foobar
components or arbitrary host-side file operations are safe.

## Qualification

The public CI includes a source-level hardening contract and Win32/x64 builds.
The shared foobar2000 2.26.1 runner loads the DLL in a real process in both
architectures, with synthetic WAV/CUE no-write *CLI* checks.

Host module loading is not the same as invoking the diagnostic context
action and observing its results. Automated product-specific GUI execution,
zero-writes during a full analysis, reference BPM correctness, unknown-length
audio and D12 holdout cases remain separate qualifications. Run the genuine
test on disposable audio copies until those gates pass.

## Private D12 comparison

Enable verbose logging before each run and use the same local paths,
requested window length/passes, host architecture, audio copies, and list
of tracks for both the pre-fix and new versions. Old and new binaries cannot
run simultaneously inside one foobar process. Save separate complete console
logs, and process them offline with `tools/compare-d12-builds.py`.

No personal audio or detailed logs may be put in the public repository.
The analyzer only provides candidate/hold availability comparisons; BPM
accuracy and tag safety require independent evidence.
