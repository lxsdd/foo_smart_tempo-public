#!/usr/bin/env python3
"""Read actual CI-probe BPM outputs. No fabricated accuracy PASS when all review."""
import argparse
import json
import math
from pathlib import Path


def evaluate(manifest_path: Path, report_path: Path) -> dict:
    corpus = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert corpus.get("schema") == "smart_tempo_synthetic_corpus_v1"
    refs = corpus["cases"]
    lines = report_path.read_text(encoding="utf-8").splitlines()
    if len(lines) != len(refs) + 3 or lines[:2] != [
        "smart_tempo_ci_result_v1", "index\tbpm\tblocked\tconfidence"
    ]:
        raise ValueError("Invalid or incomplete CI numeric report")
    if not lines[-1].startswith("elapsed_seconds\t"):
        raise ValueError("Missing elapsed runtime evidence")
    elapsed = float(lines[-1].split("\t")[1])
    if not math.isfinite(elapsed) or elapsed <= 0:
        raise ValueError("Non-positive elapsed time")
    observed = []
    for i, (ref, line) in enumerate(zip(refs, lines[2:-1])):
        fields = line.split("\t")
        if len(fields) != 4 or int(fields[0]) != i:
            raise ValueError("Unstable or incomplete fixture row identity")
        bpm, blocked, confidence = float(fields[1]), int(fields[2]), float(fields[3])
        if (not math.isfinite(bpm) or not math.isfinite(confidence) or
            bpm < 0 or bpm > 500 or confidence < 0 or confidence > 100 or
            blocked not in (0, 1)):
            raise ValueError("Invalid result")
        if (blocked and bpm > 0) or (not blocked and bpm == 0):
            # A blocked measurement must not be presented as a writable BPM.
            raise ValueError("Result violates write-safety state")
        reference = float(ref["reference_bpm"])
        direct_error = abs(bpm - reference) if bpm else None
        observed.append({
            "id": ref["id"], "category": ref["category"],
            "reference_bpm": reference,
            "observed_bpm": round(bpm, 4) if bpm else None,
            "blocked": bool(blocked),
            "error_bpm": round(direct_error, 4) if direct_error is not None else None,
            "within_1_bpm": direct_error <= 1.0 if direct_error is not None else False,
        })
    evaluated = [r for r in observed if r["category"] == "unambiguous"]
    measurable = [r for r in evaluated if r["observed_bpm"] is not None]
    return {
        "schema": "smart_tempo_ci_bpm_measurement_v1",
        "samples": len(observed),
        "unambiguous_tracks": len(evaluated),
        "unambiguous_measured": len(measurable),
        "unambiguous_within_1_bpm": sum(r["within_1_bpm"] for r in evaluated),
        "unambiguous_mae_bpm": (
            round(sum(r["error_bpm"] for r in measurable) / len(measurable), 4)
            if measurable else None
        ),
        "elapsed_seconds": round(elapsed, 3),
        "rows": observed,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--minimum-within-1", type=int, default=0)
    args = parser.parse_args()
    metrics = evaluate(args.manifest, args.report)
    print(f"CI_SYNTHETIC_TEMPO_ROWS={metrics['samples']}")
    print(f"CI_SYNTHETIC_UNAMBIGUOUS_MEASURED={metrics['unambiguous_measured']}/{metrics['unambiguous_tracks']}")
    print(f"CI_SYNTHETIC_WITHIN_1_BPM={metrics['unambiguous_within_1_bpm']}/{metrics['unambiguous_tracks']}")
    print(f"CI_SYNTHETIC_MAE_BPM={metrics['unambiguous_mae_bpm']}")
    print(f"CI_SYNTHETIC_ELAPSED_SECONDS={metrics['elapsed_seconds']}")
    print("CI_SYNTHETIC_NUMERIC_CAPTURE=PASS")
    print("CI_SYNTHETIC_BPM_ACCURACY=MEASURED_NOT_YET_QUALIFIED")
    if metrics["unambiguous_within_1_bpm"] < args.minimum_within_1:
        raise SystemExit("Numeric accuracy below explicitly configured gate")


if __name__ == "__main__":
    main()
