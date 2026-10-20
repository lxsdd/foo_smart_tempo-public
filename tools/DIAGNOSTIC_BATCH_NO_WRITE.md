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

## Qualification (20 October 2026)

The public CI includes source-level no-write entrypoint contracts and
Win32/x64 builds. Shared foobar2000 2.26.1 host smoke confirms component
loading. **In addition**, product-specific runtime CI in PR #21
(run [39444331276](https://github.com/lxsdd/foo_smart_tempo-public/actions/runs/39444331276))
invoked the actual diagnostic context command GUID on isolated synthetic
rhythmic WAV and two-track external CUE fixtures for **both** architectures.

The runtime test required an identifiable `DIAGNOSTIC / NO WRITE` result
window, at least one WAV/two CUE result rows, disabled Update control, a
blocked simulated Update invocation, and unchanged SHA256 / file size /
LastWriteTimeUtc before and after. CI-only dispatcher code is excluded from
the personal production component.

Scope remains intentionally limited: no live-user audio was analyzed.
Real-media metadata-writer safety, full keyboard/UI accessibility,
unknown-length media, true reference BPM accuracy, private D12 holdout
cases and owner acceptance are **NOT TESTED** by these synthetic checks.

## Private D12 comparison

Enable verbose logging before each run and use the same local paths,
requested window length/passes, host architecture, audio copies, and list
of tracks for both the pre-fix and new versions. Old and new binaries cannot
run simultaneously inside one foobar process. Save separate complete console
logs, and process them offline with `tools/compare-d12-builds.py`.

No personal audio or detailed logs may be put in the public repository.
The analyzer only provides candidate/hold availability comparisons; BPM
accuracy and tag safety require independent evidence.
