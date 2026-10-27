#!/usr/bin/env python3
"""Fail-closed independent aggregate of CI-only Phase-B real-foobar probes."""
import argparse
import json
import re
from pathlib import Path

CASES = ("pulse_90", "pulse_120", "pulse_140", "pulse_175",
         "pulse_128_syncopated", "pulse_120_swing", "pulse_128_half_pulse")
SCORE = ("EVALUATED", "REFERENCE_RHYTHMIC", "ACCEPTED_RHYTHMIC", "HELD_RHYTHMIC",
         "ACCEPTED_WITHIN_0_05_BPM", "ACCEPTED_WITHIN_0_20_BPM",
         "ACCEPTED_OUTSIDE_0_20_BPM", "NEGATIVE_TOTAL", "NEGATIVE_FALSEACCEPTS",
         "HALF_DOUBLE_ALIAS", "CORRECT_ABSTENTION")
TRACE = ("NATIVE_SOURCE_INVENTORY_VERIFIED", "RAW", "COLLAPSED",
         "WRITE_GUARD_BLOCKED", "PHASE_B_CAUSAL_COMPLETENESS",
         "DECODED_WINDOWS", "DECLARED_WINDOWS", "WINDOWS_COMPLETE",
         "FINAL_ACCEPT", "PROCESS_CPU_SECONDS", "PROCESS_PEAK_RSS_BYTES")

class EvidenceError(ValueError):
    pass

def verify(data, sha):
    if type(data) is not dict or set(data) != {"schema","architecture","source_sha","cases"}:
        raise EvidenceError("unexpected fields")
    if (data["schema"] != "smart_tempo_b_extended_pilot_v1" or
        data["architecture"] not in ("Win32","x64") or data["source_sha"] != sha):
        raise EvidenceError("source or schema mismatch")
    rows = data["cases"]
    if type(rows) is not list or len(rows) != len(CASES) or [
        x.get("case") if type(x) is dict else None for x in rows] != list(CASES):
        raise EvidenceError("missing, duplicate or reordered case")
    total = dict(correct_005=0,correct_020=0,wrong=0,held=0,auto=0)
    for row in rows:
        if set(row) != {"case","score","trace","media_no_write"} or row["media_no_write"] is not True:
            raise EvidenceError("privacy or no-write evidence invalid")
        s,t = row["score"],row["trace"]
        if type(s) is not dict or set(s) != set(SCORE) or any(
            type(x) is not int or x < 0 for x in s.values()):
            raise EvidenceError("invalid score fields")
        if (s["EVALUATED"] != 1 or s["REFERENCE_RHYTHMIC"] != 1 or
            s["NEGATIVE_TOTAL"] or s["NEGATIVE_FALSEACCEPTS"] or s["CORRECT_ABSTENTION"] or
            s["ACCEPTED_RHYTHMIC"] + s["HELD_RHYTHMIC"] != 1 or
            s["ACCEPTED_WITHIN_0_05_BPM"] > s["ACCEPTED_WITHIN_0_20_BPM"] or
            s["ACCEPTED_WITHIN_0_20_BPM"] + s["ACCEPTED_OUTSIDE_0_20_BPM"] != s["ACCEPTED_RHYTHMIC"] or
            s["HALF_DOUBLE_ALIAS"] > s["ACCEPTED_OUTSIDE_0_20_BPM"]):
            raise EvidenceError("precision denominator conservation failed")
        if type(t) is not dict or not set(TRACE).issubset(t) or any(
            type(t[k]) is not int or t[k] < 0 for k in
            ("RAW","COLLAPSED","WRITE_GUARD_BLOCKED","DECODED_WINDOWS",
             "DECLARED_WINDOWS","WINDOWS_COMPLETE","FINAL_ACCEPT")):
            raise EvidenceError("incomplete native source trace")
        if (t["NATIVE_SOURCE_INVENTORY_VERIFIED"] != 1 or
            t["WRITE_GUARD_BLOCKED"] != 1 or t["WINDOWS_COMPLETE"] != 1 or
            t["DECODED_WINDOWS"] < 1 or t["DECODED_WINDOWS"] != t["DECLARED_WINDOWS"] or
            t["FINAL_ACCEPT"] != s["ACCEPTED_RHYTHMIC"] or
            t["PHASE_B_CAUSAL_COMPLETENESS"] not in
            ("INCOMPLETE_PRIMARY_VETO","INCOMPLETE_EMPTY_BOARD_CAUSAL") or
            not isinstance(t["PROCESS_CPU_SECONDS"],(int,float)) or
            t["PROCESS_CPU_SECONDS"] < 0 or
            not isinstance(t["PROCESS_PEAK_RSS_BYTES"],(int,float)) or
            t["PROCESS_PEAK_RSS_BYTES"] < 1):
            raise EvidenceError("unproven host/source/write guard")
        for dest, key in (("correct_005","ACCEPTED_WITHIN_0_05_BPM"),
                          ("correct_020","ACCEPTED_WITHIN_0_20_BPM"),
                          ("wrong","ACCEPTED_OUTSIDE_0_20_BPM"),
                          ("held","HELD_RHYTHMIC"),("auto","ACCEPTED_RHYTHMIC")):
            total[dest] += s[key]
    return data["architecture"], total

def read(path):
    if not path.is_file() or not 0 < path.stat().st_size < 150000:
        raise EvidenceError("missing or oversized report")
    def unique(pairs):
        d={}
        for k,v in pairs:
            if k in d: raise EvidenceError("duplicate JSON key")
            d[k]=v
        return d
    return json.loads(path.read_text(encoding="utf-8"),object_pairs_hook=unique,
        parse_constant=lambda x: (_ for _ in ()).throw(EvidenceError("nonfinite")))

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--expected-source-sha",required=True)
    p.add_argument("--report",type=Path,action="append",required=True)
    p.add_argument("--single-architecture",action="store_true")
    a=p.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}",a.expected_source_sha) or len(a.report) != (
            1 if a.single_architecture else 2):
        p.error("invalid SHA or report count")
    try:
        results=dict(verify(read(path),a.expected_source_sha) for path in a.report)
        if len(results) != len(a.report) or (
            not a.single_architecture and set(results) != {"Win32","x64"}):
            raise EvidenceError("missing or duplicate architecture")
    except (ValueError,OSError,TypeError,UnicodeError) as exc:
        p.error(str(exc))
    for arch in sorted(results):
        print(f"SMART_TEMPO_B_EXTENDED_{arch}_CLEAR={len(CASES)}")
        for key,value in results[arch].items():
            print(f"SMART_TEMPO_B_EXTENDED_{arch}_{key.upper()}={value}")
    print("SMART_TEMPO_B_EXTENDED_COHORT=PASS")
    print("SMART_TEMPO_PHASE_B_CAUSAL_COMPLETENESS=INCOMPLETE")
    print("SMART_TEMPO_MUSICAL_RELEASE=NO_GO")

if __name__=="__main__": main()
