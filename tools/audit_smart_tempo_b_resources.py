#!/usr/bin/env python3
"""Original-host Phase-B per-process resource evidence, synthetic fixtures only.

The process counters are native GetProcessTimes/GetProcessMemoryInfo snapshots.
Peak working set is a process lifetime high-water mark, NOT a per-case delta.
A fresh foobar2000 process per case is mandatory in the calling harness.
"""
import argparse
import copy
import json
import math
import re
from pathlib import Path

CASES = ("pulse_60", "pulse_128", "pulse_120_sparse", "silence",
         "probe_72_regular", "probe_104_offbeat", "probe_132_swing",
         "probe_156_half_pulse", "probe_90_polyrhythm", "probe_150_sparse")
SCHEMA = "smart_tempo_b_original_host_resources_v1"
SHA = re.compile(r"[0-9a-f]{40}\Z")
FIELDS = {"case", "architecture", "source_sha", "analysis_id",
          "wall_seconds", "cpu_seconds", "peak_rss_bytes", "media_no_write"}

class ResourceError(ValueError):
    pass

def unique(pairs):
    result = {}
    for k, v in pairs:
        if k in result:
            raise ResourceError("duplicate JSON field")
        result[k] = v
    return result

def read_json(path, limit):
    if not path.is_file() or not 0 < path.stat().st_size <= limit:
        raise ResourceError("missing or oversized evidence")
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique,
                      parse_constant=lambda x: (_ for _ in ()).throw(ResourceError("nonfinite")))

def number(value, name, positive=False):
    if type(value) not in (float, int) or not math.isfinite(value):
        raise ResourceError("invalid " + name)
    if value < 0 or (positive and value <= 0) or value > 1e12:
        raise ResourceError("out-of-range " + name)
    return value

def validate_row(row, case, architecture, sha):
    if type(row) is not dict or set(row) != FIELDS:
        raise ResourceError("missing or unknown resource fields")
    if row["case"] != case or row["architecture"] != architecture or row["source_sha"] != sha:
        raise ResourceError("foreign case, architecture or source")
    if type(row["analysis_id"]) is not str or not re.fullmatch(r"[A-Za-z0-9_-]{8,128}", row["analysis_id"]):
        raise ResourceError("missing or malformed native analysis ID")
    if row["media_no_write"] is not True:
        raise ResourceError("missing no-write witness")
    number(row["wall_seconds"], "wall seconds", True)
    number(row["cpu_seconds"], "CPU seconds")
    number(row["peak_rss_bytes"], "peak RSS bytes", True)
    if type(row["peak_rss_bytes"]) is not int:
        raise ResourceError("noninteger RSS")
    if row["wall_seconds"] > 7200 or row["cpu_seconds"] > row["wall_seconds"] * 256 + 1:
        raise ResourceError("implausible resource interval")

def extract(trace, case, architecture, sha, expected_fixture_sha256):
    if case not in CASES or architecture not in ("Win32", "x64") or not SHA.fullmatch(sha):
        raise ResourceError("invalid requested identity")
    if type(expected_fixture_sha256) is not str or not re.fullmatch(r"[0-9a-f]{64}", expected_fixture_sha256):
        raise ResourceError("missing expected fixture SHA256")
    if not trace.is_file() or not 0 < trace.stat().st_size <= 2 * 1024 * 1024:
        raise ResourceError("missing native trace")
    events = []
    with trace.open(encoding="utf-8") as f:
        for line in f:
            if len(events) >= 16500:
                raise ResourceError("oversized native event count")
            events.append(json.loads(line, object_pairs_hook=unique))
    if not events or any(type(e) is not dict for e in events):
        raise ResourceError("invalid native event sequence")
    ids = {e.get("analysis_id") for e in events}
    if len(ids) != 1:
        raise ResourceError("mixed native analysis IDs")
    analysis_id = next(iter(ids))
    if type(analysis_id) is not str:
        raise ResourceError("missing native analysis ID")
    if any(e.get("sequence") != i for i, e in enumerate(events)):
        raise ResourceError("mixed or discontinuous event sequence")
    resource = [e for e in events if e.get("stage") == "resource_usage"]
    guards = [e for e in events if e.get("stage") == "write_guard"]
    if len(resource) != 1 or len(guards) != 1 or guards[0].get("outcome") != "write_blocked" or guards[0].get("reason_code") != "ci_dry_run":
        raise ResourceError("missing, duplicate or foreign native resource/no-write event")
    event = resource[0]
    if event.get("outcome") != "observed" or event.get("reason_code") != "process_resource_snapshot":
        raise ResourceError("unverified resource origin")
    m = event.get("metrics")
    if type(m) is not dict or not {"wall_seconds", "process_cpu_seconds",
          "process_peak_rss_before_bytes", "process_peak_rss_after_bytes",
          "process_working_set_after_bytes"} <= set(m):
        raise ResourceError("missing process resource metrics")
    if m.get("fixture_sha256") != expected_fixture_sha256:
        raise ResourceError("native fixture SHA256 does not match expected case input")
    before = m["process_peak_rss_before_bytes"]
    peak = m["process_peak_rss_after_bytes"]
    working = m["process_working_set_after_bytes"]
    for name, value in (("before", before), ("peak", peak), ("working", working)):
        number(value, name, True)
        if type(value) is not int:
            raise ResourceError("noninteger process memory")
    if peak < before or peak < working:
        raise ResourceError("inconsistent process memory lifetime")
    row = dict(case=case, architecture=architecture, source_sha=sha,
               analysis_id=analysis_id, wall_seconds=m["wall_seconds"],
               cpu_seconds=m["process_cpu_seconds"], peak_rss_bytes=peak,
               media_no_write=True)
    validate_row(row, case, architecture, sha)
    return row

def validate_report(report, architecture, sha):
    if type(report) is not dict or set(report) != {"schema", "architecture", "source_sha", "cases"}:
        raise ResourceError("unknown report fields")
    if report["schema"] != SCHEMA or report["architecture"] != architecture or report["source_sha"] != sha:
        raise ResourceError("report identity mismatch")
    rows = report["cases"]
    if type(rows) is not list or len(rows) != len(CASES):
        raise ResourceError("missing resource cases")
    seen = set()
    for case, row in zip(CASES, rows):
        validate_row(row, case, architecture, sha)
        if row["analysis_id"] in seen:
            raise ResourceError("reused native process analysis")
        seen.add(row["analysis_id"])
    return True

def self_test():
    sha = "a" * 40
    arch = "Win32"
    def event(stage, index, analysis="native-run-1234", **metrics):
        return dict(stage=stage, sequence=index, analysis_id=analysis,
                    outcome="observed" if stage == "resource_usage" else "write_blocked",
                    reason_code="process_resource_snapshot" if stage == "resource_usage" else "ci_dry_run",
                    metrics=metrics)
    fixture_a = "1" * 64
    fixture_b = "2" * 64
    resource = event("resource_usage", 0, fixture_sha256=fixture_a, wall_seconds=1.25,
                     process_cpu_seconds=0.25, process_peak_rss_before_bytes=100,
                     process_peak_rss_after_bytes=200, process_working_set_after_bytes=150)
    guard = event("write_guard", 1)
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "trace.jsonl"
        def write(items):
            path.write_text("".join(json.dumps(x) + "\n" for x in items), encoding="utf-8")
        write([resource, guard])
        row = extract(path, CASES[0], arch, sha, fixture_a)
        assert row["wall_seconds"] == 1.25 and row["peak_rss_bytes"] == 200
        negatives = ([guard], [resource], [resource, resource, guard],
                     [resource, event("write_guard", 1, "foreign-run-1234")],
                     [dict(resource, metrics=dict(resource["metrics"], wall_seconds=0)), guard],
                     [dict(resource, metrics=dict(resource["metrics"], process_peak_rss_after_bytes=50)), guard])
        for sample in negatives:
            write(sample)
            try:
                extract(path, CASES[0], arch, sha, fixture_a)
            except ResourceError:
                pass
            else:
                raise AssertionError("adversarial native resource accepted")
    # Two individually valid complete traces cannot be exchanged.
    with tempfile.TemporaryDirectory() as d:
        first = Path(d) / "first.jsonl"
        second = Path(d) / "second.jsonl"
        first.write_text("".join(json.dumps(x) + "\n" for x in [resource, guard]), encoding="utf-8")
        foreign = copy.deepcopy(resource)
        foreign["analysis_id"] = "native-run-5678"
        foreign["metrics"]["fixture_sha256"] = fixture_b
        second_guard = event("write_guard", 1, "native-run-5678")
        second.write_text("".join(json.dumps(x) + "\n" for x in [foreign, second_guard]), encoding="utf-8")
        assert extract(first, CASES[0], arch, sha, fixture_a)
        assert extract(second, CASES[1], arch, sha, fixture_b)
        for trace, case, expected in ((first, CASES[1], fixture_b), (second, CASES[0], fixture_a)):
            try:
                extract(trace, case, arch, sha, expected)
            except ResourceError:
                pass
            else:
                raise AssertionError("swapped valid native trace accepted")
    rows = [dict(row, case=case, analysis_id="native-run-%04d" % i)
            for i, case in enumerate(CASES)]
    report = dict(schema=SCHEMA, architecture=arch, source_sha=sha, cases=rows)
    validate_report(report, arch, sha)
    for change in (lambda r: r["cases"].pop(),
                   lambda r: r["cases"][1].update(analysis_id=r["cases"][0]["analysis_id"]),
                   lambda r: r["cases"][0].update(source_sha="b"*40),
                   lambda r: r["cases"][0].update(media_no_write=False),
                   lambda r: r["cases"][0].update(architecture="x64")):
        bad = copy.deepcopy(report)
        change(bad)
        try:
            validate_report(bad, arch, sha)
        except ResourceError:
            pass
        else:
            raise AssertionError("adversarial case mapping accepted")
    return True

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--trace", type=Path)
    p.add_argument("--case")
    p.add_argument("--architecture")
    p.add_argument("--source-sha")
    p.add_argument("--expected-fixture-sha256")
    p.add_argument("--report", type=Path)
    p.add_argument("--expected-source-sha")
    p.add_argument("--self-test", action="store_true")
    args = p.parse_args()
    try:
        if args.self_test:
            self_test()
            print("SMART_TEMPO_B_RESOURCES_MUTATIONS=PASS")
        elif args.report:
            report = read_json(args.report, 32768)
            validate_report(report, args.architecture, args.expected_source_sha)
            print("SMART_TEMPO_B_RESOURCES_"+args.architecture+"=PASS")
        elif args.trace:
            print(json.dumps(extract(args.trace, args.case, args.architecture, args.source_sha,
                                     args.expected_fixture_sha256),
                             separators=(",", ":"), allow_nan=False))
        else:
            p.error("expected --trace, --report or --self-test")
    except (ResourceError, OSError, UnicodeError, ValueError, TypeError, KeyError) as exc:
        p.error(str(exc))

if __name__ == "__main__":
    main()
