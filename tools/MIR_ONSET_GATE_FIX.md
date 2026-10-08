# MIR onset single-event gate (synthetic regression)

## Verified logic error in previous frontend

The previous `is_single_event` counted peaks whose magnitudes were **strictly
greater than** the arithmetic mean of all peak magnitudes, and returned
`single_event` when at most one exceeded it.

For a legitimate regularly pulsed synthetic sequence `[1,1,1,1]`, exactly
zero peaks exceed the mean. For `[1,1,1,1.001]`, only one does. Both sequences
were therefore rejected before quantization, possible bar divisions, and
candidate generation. This is a deterministic, data-independent frontend
false-rejection pattern; it is **not** proof that this is D12's real cause.

## Bounded fix

The source now uses an effective count of **positive** onset magnitudes:
`(sum(w))^2 / sum(w*w)`. This equals the number of equal-magnitude positive
onsets, and approaches one when a single transient dominates. Fewer than 1.5
effective onsets are still rejected. Empty/inconsistent arrays, nonfinite
magnitudes, and zero positive support fail closed.

This is the only change to the production MIR path: the result still passes
through existing tatum quantization, complete/partial-bar candidates,
scoring, and **unchanged** terminal policy/writer gates.

The public CTest target covers equal/near-equal peaks, a single transient,
strongly dominant transients with background noise, nonfinite values,
zero/negative support, and malformed counts using synthetic arrays.

## Qualification boundary

- Synthetically proven: previously rejected valid-looking regular onset
  sequences now pass this **one** early gate; a genuinely isolated dominant
  transient still fails the gate.
- Not proven: which real D12 samples hit the gate, whether an accepted sample
  creates a good measured BPM, whether aggregate hold rates improve, or
  whether held-out accuracy increases.
- Before distributing a new behavior as a validated user release, retain
  baseline/variant real-audio D12 evidence privately, compare candidate-stage
  transitions and newly available `HodgkinsonPrimarySegment` reasons, and
  check both accuracy and regressions. No private log, musical audio, track
  names, or reference tables may go into public GitHub.
