#!/usr/bin/env python3
"""Compare measured BPM with declared, synthetic-only pulse references.

This scorer never claims that a synthetic beat is equivalent to human tempo
perception. Silence, deliberate halftime patterns, and swing are reported
separately. No user audio, reference recordings or personal paths.
"""
import argparse
import csv
import json
import math
from pathlib import Path


def evaluate(reference, observed):
    rows = reference["cases"]
    if len(rows) != len(observed):
        raise ValueError("observation count mismatched")
    if len({c["id"] for c in rows}) != len(rows):
        raise ValueError("duplicate synthetic case identity")
    scored = []
    for expected, got in zip(rows, observed):
        value = float(got["bpm"])
        held = str(got["held"]) == "1"
        if not math.isfinite(value) or value < 0:
            raise ValueError("non-finite/negative BPM")
        bpm = expected["reference_bpm"]
        ambiguous = expected.get("ambiguity") == "meter_underdetermined"
        absolute_error_bpm = None
        if held and value > 0:
            # A held candidate is not an accepted BPM even if it is measured.
            accepted = False
        else:
            accepted = value > 0 and not held
        if bpm is None:
            if not accepted:
                category = "correct_abstention"
            elif expected["style"] == "silence":
                category = "false_positive_silence"
            else:
                category = "false_positive_nonrhythmic"
            relative_error = None
        elif ambiguous:
            # The same PCM pulse train admits multiple metrical BPM labels.
            # A numerically lucky match is not proof of identifiability.
            category = "ambiguous_hold" if not accepted else "ambiguous_accept"
            absolute_error_bpm = abs(value - bpm) if accepted else None
            relative_error = (absolute_error_bpm / bpm) if accepted else None
        elif not accepted:
            category = "abstained"
            relative_error = None
        else:
            absolute_error_bpm = abs(value - bpm)
            relative_error = absolute_error_bpm / bpm
            if relative_error <= 0.02:
                category = "within_two_percent"
            elif min(abs(value / bpm - 2), abs(value / bpm - .5)) <= 0.04:
                category = "half_double_alias"
            else:
                category = "other_error"
        scored.append({"id": expected["id"], "style": expected["style"],
                       "reference_bpm": bpm, "reported_bpm": value,
                       "held": held, "category": category,
                       "relative_error": relative_error,
                       "ambiguous": ambiguous,
                       "absolute_error_bpm": absolute_error_bpm})
    counts = {kind: sum(row["category"] == kind for row in scored)
              for kind in ("within_two_percent", "half_double_alias",
                           "other_error", "abstained", "ambiguous_accept",
                           "ambiguous_hold", "correct_abstention",
                           "false_positive_silence",
                           "false_positive_nonrhythmic")}
    clear = [row for row in scored
             if row["reference_bpm"] is not None and not row["ambiguous"]]
    accepted_errors = [row["absolute_error_bpm"] for row in clear
                       if row["absolute_error_bpm"] is not None]
    precision = {
        "clear_reference_n": len(clear),
        "accepted_reference_n": len(accepted_errors),
        "within_0_05_bpm": sum(error <= 0.05 + 1e-9
                               for error in accepted_errors),
        "within_0_20_bpm": sum(error <= 0.20 + 1e-9
                               for error in accepted_errors),
        "max_accepted_error_bpm": max(accepted_errors, default=0.0),
    }
    unsafe = sum(
        row["category"] in ("half_double_alias", "other_error",
                            "ambiguous_accept", "false_positive_silence",
                            "false_positive_nonrhythmic") or
        (not row["ambiguous"] and row["absolute_error_bpm"] is not None and
         row["absolute_error_bpm"] > 0.20 + 1e-9)
        for row in scored
    )
    verdict = ("NO_GO" if unsafe else
               "REVIEW_HOLDS" if counts["abstained"] else
               "SYNTHETIC_ONLY_PASS" if precision["within_0_05_bpm"] ==
               len(clear) else "REVIEW_PRECISION")
    return {"schema": "synthetic_bpm_score_v1", "n": len(rows),
            "counts": counts, "precision": precision,
            "quality_verdict": verdict, "unsafe_count": unsafe,
            "results": scored}


def load_observed(path):
    with path.open(newline="", encoding="utf-8") as handle:
        entries = list(csv.DictReader(handle))
    if any(set(entry) != {"index", "bpm", "held"} for entry in entries):
        raise ValueError("observation header mismatch")
    if [int(entry["index"]) for entry in entries] != list(range(len(entries))):
        raise ValueError("observation ordinal mismatch")
    if any(entry["held"] not in ("0", "1") for entry in entries):
        raise ValueError("bad held flag")
    return entries


# Only public generator case identifiers may be printed by the optional
# CI precision observer. This is a synthetic-only diagnostic, not a path or
# metadata logger. Normal offline scoring does not print individual rows.
PUBLIC_CASE_IDS = frozenset({
    "pulse_60", "pulse_90", "pulse_120", "pulse_128",
    "pulse_140", "pulse_175", "pulse_128_syncopated", "pulse_120_swing",
    "pulse_128_half_pulse", "pulse_120_sparse", "silence", "pulse_40",
    "pulse_45", "pulse_50", "pulse_70", "pulse_80",
    "pulse_100", "pulse_110", "pulse_130", "pulse_150",
    "pulse_160", "pulse_180", "pulse_200", "pulse_220",
    "pulse_59_73", "pulse_93_37", "pulse_123_45", "pulse_127_31",
    "pulse_174_62", "pulse_199_25", "pulse_90_syncopated", "pulse_175_syncopated",
    "pulse_90_swing", "pulse_175_swing", "pulse_90_half_pulse", "pulse_175_half_pulse",
    "pulse_128_sparse", "pulse_175_sparse", "pulse_128_8s", "pulse_128_12s",
    "pulse_128_40s", "pulse_128_44100_mono", "pulse_128_48000_stereo", "pulse_123_45_44100_stereo",
    "single_click", "steady_tone", "noise",
    "drum_60_four_floor", "drum_90_four_floor", "drum_110_four_floor", "drum_120_four_floor", "drum_128_four_floor", "drum_140_four_floor", "drum_175_four_floor", "drum_123_45_four_floor", "drum_90_breakbeat", "drum_128_breakbeat", "drum_175_breakbeat", "drum_128_8s", "drum_128_44100_stereo",
})



def load_candidate_inventory(path):
    """Read CI-only numeric evidence for fixed, public synthetic cases."""
    fields = {"id", "rank", "bpm", "evidence", "support", "winner", "local_exact"}
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if set(reader.fieldnames or ()) != fields:
            raise ValueError("candidate inventory header mismatch")
        entries = list(reader)
    counts = {}
    for entry in entries:
        if set(entry) != fields or entry["id"] not in PUBLIC_CASE_IDS:
            raise ValueError("unexpected candidate field or non-public ID")
        rank = int(entry["rank"])
        if not 1 <= rank <= 32:
            raise ValueError("candidate rank outside bounded CI probe")
        values = [float(entry[field]) for field in
                  ("bpm", "evidence", "support", "winner", "local_exact")]
        if not all(map(math.isfinite, values)) or values[0] <= 0:
            raise ValueError("invalid numeric candidate")
        counts[entry["id"]] = counts.get(entry["id"], 0) + 1
        if counts[entry["id"]] > 32:
            raise ValueError("candidate evidence cap exceeded")
    return entries


def inspect_candidate_inventory(scored, entries):
    """Separate candidate-generation failure from downstream rejection.

    This reports availability only. It never selects or promotes a BPM.
    References must not be passed into any production DSP or selector.
    """
    grouped = {}
    known_ids = {row["id"] for row in scored["results"]}
    for entry in entries:
        if entry["id"] not in known_ids:
            raise ValueError("candidate ID absent from the public manifest")
        grouped.setdefault(entry["id"], []).append(float(entry["bpm"]))
    incorrect = [row for row in scored["results"]
                 if not row["ambiguous"] and row["reference_bpm"] is not None
                 and row["absolute_error_bpm"] is not None
                 and row["absolute_error_bpm"] > 0.20 + 1e-9]
    present_reference = 0
    present_output = 0
    details = []
    for row in incorrect:
        values = grouped.get(row["id"], [])
        reference_found = any(abs(value - row["reference_bpm"]) <= 0.20 + 1e-9
                              for value in values)
        output_found = any(abs(value - row["reported_bpm"]) <= 0.20 + 1e-9
                           for value in values)
        present_reference += reference_found
        present_output += output_found
        details.append((row["id"], int(reference_found), int(output_found)))
    return {
        "incorrect_clear_reference_n": len(incorrect),
        "correct_measured_candidate_present_n": present_reference,
        "chosen_measured_candidate_present_n": present_output,
        "candidate_records_n": len(entries),
        "per_error": details,
    }


def precision_detail_lines(scored):
    for row in scored["results"]:
        if row["id"] not in PUBLIC_CASE_IDS:
            raise ValueError("precision detail requires a known public fixture")
        reference = (f'{row["reference_bpm"]:.6f}'
                     if row["reference_bpm"] is not None else "none")
        error = (f'{row["absolute_error_bpm"]:.6f}'
                 if row["absolute_error_bpm"] is not None else "unavailable")
        yield (f'SYNTHETIC_BPM_PRECISION id={row["id"]} '
               f'reference={reference} observed={row["reported_bpm"]:.6f} '
               f'abs_error_bpm={error} held={int(row["held"])} '
               f'category={row["category"]}')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--observed", required=True, type=Path)
    parser.add_argument("--json-report", type=Path)
    parser.add_argument("--candidates", type=Path,
                        help="bounded CI-only measured candidate inventory CSV")
    parser.add_argument("--precision-details", action="store_true",
                        help="print synthetic-only numeric deviation for known public cases")
    args = parser.parse_args()
    reference = json.loads(args.manifest.read_text(encoding="utf-8"))
    if reference.get("schema") != "synthetic_bpm_reference_v1":
        parser.error("invalid synthetic reference schema")
    scored = evaluate(reference, load_observed(args.observed))
    if args.json_report:
        if args.json_report.exists():
            parser.error("refuse to overwrite existing report")
        args.json_report.write_text(json.dumps(scored, indent=2) + "\n", encoding="utf-8")
    # Stable summaries only; no disk paths or source data.
    print("SYNTHETIC_BPM_OBSERVATION=PASS")
    for category, count in scored["counts"].items():
        print(f"SYNTHETIC_BPM_{category.upper()}={count}")
    for category, count in scored["precision"].items():
        print(f"SYNTHETIC_BPM_{category.upper()}={count}")
    print(f"SYNTHETIC_BPM_UNSAFE_ACCEPT_COUNT={scored['unsafe_count']}")
    print(f"SYNTHETIC_QUALITY_VERDICT={scored['quality_verdict']}")
    if args.precision_details:
        for line in precision_detail_lines(scored):
            print(line)
    if args.candidates:
        diagnostics = inspect_candidate_inventory(
            scored, load_candidate_inventory(args.candidates))
        for key in ("incorrect_clear_reference_n",
                    "correct_measured_candidate_present_n",
                    "chosen_measured_candidate_present_n",
                    "candidate_records_n"):
            print(f"SYNTHETIC_CANDIDATE_{key.upper()}={diagnostics[key]}")
        if args.precision_details:
            for case_id, reference_found, output_found in diagnostics["per_error"]:
                print(f"SYNTHETIC_CANDIDATE_RESCUE id={case_id} "
                      f"reference_present={reference_found} "
                      f"output_present={output_found}")
    print(f"SYNTHETIC_BPM_EVALUATED={scored['n']}")


if __name__ == "__main__":
    main()
