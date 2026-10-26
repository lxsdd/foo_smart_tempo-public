#!/usr/bin/env python3
"""CI-only dual-architecture synthetic pilot proof; NEVER musical or Phase-C GO."""
import argparse
import json
import math
from pathlib import Path
import re

CASES=("pulse_60","pulse_128","pulse_120_sparse","silence")
SCORE_KEYS=set("""EVALUATED REFERENCE_RHYTHMIC ACCEPTED_RHYTHMIC HELD_RHYTHMIC ACCEPTED_WITHIN_0_05_BPM ACCEPTED_WITHIN_0_20_BPM ACCEPTED_OUTSIDE_0_20_BPM NEGATIVE_TOTAL NEGATIVE_FALSEACCEPTS HALF_DOUBLE_ALIAS CORRECT_ABSTENTION""".split())
TRACE_KEYS=set("""NATIVE_SOURCE_INVENTORY_VERIFIED RAW COLLAPSED WRITE_GUARD_BLOCKED PHASE_B_CAUSAL_COMPLETENESS DECODED_WINDOWS DECLARED_WINDOWS WINDOWS_COMPLETE FINAL_ACCEPT PROCESS_CPU_SECONDS PROCESS_PEAK_RSS_BYTES""".split())
OPTIONAL=set("""PRIMARY_VETO_REASON_UNKNOWN INDEXED_NATIVE_FINAL_WITNESS UNINDEXED_SUPPLEMENTAL_FINAL_WITNESS SUPPLEMENTAL_SELECTED_SOURCES POLICY_SUPPLEMENTAL_POINTER_IDENTITY_COMPLETE NEGATIVE_NO_CANDIDATE_TRACE SELECTOR_LOW_BESTBY_EPISODES SELECTOR_COMPARE_LOW_WINS SELECTOR_COMPARE_LOW_LOSSES""".split())
SHADOW_MODELS=("SCORE_SUM","SUPPORT","COMBINED","SCORE_TIMES_SUPPORT")
SHADOW_KEYS={"CANDIDATES","REFERENCE_PRESENT","NATIVE_PRIMARY_CORRECT",
             "NATIVE_FINAL_CORRECT","NATIVE_PRIMARY_SOURCE_INDEXED",
             "REFERENCE_LOST_AT_PRIMARY"} | {
    f"MODEL_{model}_{suffix}" for model in SHADOW_MODELS
    for suffix in ("CORRECT","MATCHES_PRIMARY")
}

class PilotError(ValueError): pass

def unique_json_object(pairs):
    """Reject duplicated keys at EVERY depth, not just the report root."""
    result={}
    for key,value in pairs:
        if key in result: raise PilotError("duplicate JSON key: "+key)
        result[key]=value
    return result

def count(x, key):
    if type(x) is not int or not 0<=x<=100000: raise PilotError("bad integer "+key)
    return x

def resource(x,key):
    if type(x) not in (float,int) or not math.isfinite(x) or not 0<=x<=1e12:
        raise PilotError("bad resource "+key)
    return x

def validate(reports,require_both=True):
    if len(reports)!=(2 if require_both else 1): raise PilotError("missing host architecture")
    out={}; shas=set()
    for report in reports:
        if type(report) is not dict or set(report)!={"schema","architecture","source_sha","cases"}:
            raise PilotError("unexpected report fields")
        if report["schema"]!="smart_tempo_b_pilot_cohort_v1": raise PilotError("wrong schema")
        arch=report["architecture"]
        if arch not in ("Win32","x64") or arch in out: raise PilotError("duplicate/unknown architecture")
        sha=report["source_sha"]
        if type(sha) is not str or not re.fullmatch("[a-f0-9]{40}",sha):
            raise PilotError("invalid source SHA")
        shas.add(sha)
        rows=report["cases"]
        if type(rows) is not list or len(rows)!=4: raise PilotError("missing pilot cases")
        m=dict(clear=0,within_005=0,within_020=0,wrong=0,held=0,
               aliases=0,negative=0,falseaccept=0,unknown_veto=0,
               cpu_seconds=0.0,peak_rss_bytes=0,classes=[],
               shadow_primary_correct=0,shadow_reference_present=0,
               shadow_lost_primary=0,
               shadow_models={model:0 for model in SHADOW_MODELS})
        for i,row in enumerate(rows):
            if type(row) is not dict or set(row)!={"case","score","trace","shadow","media_no_write"}:
                raise PilotError("unknown case fields")
            if row["case"]!=CASES[i] or row["media_no_write"] is not True:
                raise PilotError("case identity/order or no-write failed")
            s,t,h=row["score"],row["trace"],row["shadow"]
            if type(h) is not dict or set(h)!=SHADOW_KEYS:
                raise PilotError("native shadow score incomplete/unknown fields")
            for key,val in h.items():
                count(val,key)
                if key!="CANDIDATES" and val not in (0,1):
                    raise PilotError("nonbinary shadow model evidence")
            if (h["CANDIDATES"]!=t["COLLAPSED"] or
                h["NATIVE_FINAL_CORRECT"]!=s["ACCEPTED_WITHIN_0_20_BPM"] or
                h["NATIVE_PRIMARY_CORRECT"]>h["REFERENCE_PRESENT"] or
                h["REFERENCE_LOST_AT_PRIMARY"] and (
                    not h["REFERENCE_PRESENT"] or h["NATIVE_PRIMARY_CORRECT"])):
                raise PilotError("shadow model source/decision conservation")
            for name in SHADOW_MODELS:
                c=h[f"MODEL_{name}_CORRECT"]
                agree=h[f"MODEL_{name}_MATCHES_PRIMARY"]
                if c>h["REFERENCE_PRESENT"] or (
                    agree and (c!=h["NATIVE_PRIMARY_CORRECT"] or
                               not h["NATIVE_PRIMARY_SOURCE_INDEXED"])):
                    raise PilotError("shadow winner/source identity contradiction")
            if type(s) is not dict or set(s)!=SCORE_KEYS: raise PilotError("scorer incomplete")
            if type(t) is not dict or not TRACE_KEYS<=set(t) or set(t)-TRACE_KEYS-OPTIONAL:
                raise PilotError("native trace incomplete/privacy field")
            for k,v in s.items(): count(v,k)
            for k,v in t.items():
                if k=="PHASE_B_CAUSAL_COMPLETENESS": continue
                if k in ("PROCESS_CPU_SECONDS","PROCESS_PEAK_RSS_BYTES"): resource(v,k)
                else: count(v,k)
            if (s["EVALUATED"]!=1 or t["NATIVE_SOURCE_INVENTORY_VERIFIED"]!=1 or
                t["WRITE_GUARD_BLOCKED"]!=1 or t["WINDOWS_COMPLETE"]!=1 or
                not 1<=t["DECODED_WINDOWS"]==t["DECLARED_WINDOWS"]<=128 or
                t["FINAL_ACCEPT"]!=s["ACCEPTED_RHYTHMIC"]+s["NEGATIVE_FALSEACCEPTS"]):
                raise PilotError("native/scorer conservation or write-guard failed")
            if i<3:
                if (s["REFERENCE_RHYTHMIC"]!=1 or s["NEGATIVE_TOTAL"]!=0 or
                    s["ACCEPTED_RHYTHMIC"]+s["HELD_RHYTHMIC"]!=1 or
                    s["ACCEPTED_WITHIN_0_05_BPM"]>s["ACCEPTED_WITHIN_0_20_BPM"] or
                    s["ACCEPTED_WITHIN_0_20_BPM"]+s["ACCEPTED_OUTSIDE_0_20_BPM"]!=s["ACCEPTED_RHYTHMIC"] or
                    s["HALF_DOUBLE_ALIAS"]>s["ACCEPTED_OUTSIDE_0_20_BPM"] or
                    t["PHASE_B_CAUSAL_COMPLETENESS"]!="INCOMPLETE_PRIMARY_VETO" or
                    t.get("PRIMARY_VETO_REASON_UNKNOWN",0)<=0 or
                    t.get("INDEXED_NATIVE_FINAL_WITNESS",0)!=s["ACCEPTED_RHYTHMIC"] or
                    t["RAW"]<=0 or t["COLLAPSED"]<=0):
                    raise PilotError("rhythmic provenance/denominator mismatch")
                if h["NATIVE_PRIMARY_SOURCE_INDEXED"]!=1:
                    raise PilotError("rhythmic Primary source not witnessed")
                m["shadow_primary_correct"]+=h["NATIVE_PRIMARY_CORRECT"]
                m["shadow_reference_present"]+=h["REFERENCE_PRESENT"]
                m["shadow_lost_primary"]+=h["REFERENCE_LOST_AT_PRIMARY"]
                for model in SHADOW_MODELS:
                    m["shadow_models"][model]+=h[f"MODEL_{model}_CORRECT"]
                m["clear"]+=1
                m["within_005"]+=s["ACCEPTED_WITHIN_0_05_BPM"]
                m["within_020"]+=s["ACCEPTED_WITHIN_0_20_BPM"]
                m["wrong"]+=s["ACCEPTED_OUTSIDE_0_20_BPM"]
                m["held"]+=s["HELD_RHYTHMIC"]
                m["aliases"]+=s["HALF_DOUBLE_ALIAS"]
                m["unknown_veto"]+=t["PRIMARY_VETO_REASON_UNKNOWN"]
                m["classes"].append("correct" if s["ACCEPTED_WITHIN_0_20_BPM"] else
                                    "held" if s["HELD_RHYTHMIC"] else "wrong")
            else:
                if (s["REFERENCE_RHYTHMIC"]!=0 or s["NEGATIVE_TOTAL"]!=1 or
                    s["ACCEPTED_RHYTHMIC"]!=0 or s["HELD_RHYTHMIC"]!=0 or
                    s["NEGATIVE_FALSEACCEPTS"]!=0 or s["CORRECT_ABSTENTION"]!=1 or
                    t["FINAL_ACCEPT"]!=0 or t["RAW"]!=0 or t["COLLAPSED"]!=0 or
                    t.get("NEGATIVE_NO_CANDIDATE_TRACE",0)!=1 or
                    t["PHASE_B_CAUSAL_COMPLETENESS"]!="INCOMPLETE_EMPTY_BOARD_CAUSAL"):
                    raise PilotError("negative falseaccept or invented source")
                if (h["CANDIDATES"] or h["REFERENCE_PRESENT"] or
                    h["NATIVE_PRIMARY_SOURCE_INDEXED"] or
                    h["REFERENCE_LOST_AT_PRIMARY"] or
                    any(h[k] for k in h if k!="CANDIDATES")):
                    raise PilotError("negative shadow invented candidates or selection")
                m["negative"]+=1
                m["classes"].append("negative_held")
            m["cpu_seconds"]+=t["PROCESS_CPU_SECONDS"]
            m["peak_rss_bytes"]=max(m["peak_rss_bytes"],int(t["PROCESS_PEAK_RSS_BYTES"]))
        m["cpu_seconds"]=round(m["cpu_seconds"],6)
        out[arch]=m
    if len(shas)!=1 or (require_both and set(out)!={"Win32","x64"}):
        raise PilotError("source SHA or host coverage mismatch")
    if require_both and out["Win32"]["classes"]!=out["x64"]["classes"]:
        raise PilotError("cross-architecture decision class drift")
    if require_both and any(
            left["shadow"] != right["shadow"]
            for left,right in zip(reports[0]["cases"],reports[1]["cases"])):
        raise PilotError("cross-architecture shadow ranking drift")
    return {"source_sha":shas.pop(),"architectures":out,"phase_b":"INCOMPLETE",
            "phase_c":"CLOSED","musical_release":"NO_GO"}

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--report",type=Path,action="append",required=True)
    p.add_argument("--single-architecture",action="store_true")
    p.add_argument("--expected-source-sha",required=True,
                   help="Exact git checkout SHA of the running source")
    args=p.parse_args()
    reports=[]
    for f in args.report:
        if not f.is_file() or not 0<f.stat().st_size<=16384: p.error("missing/oversized report")
        try:
            reports.append(json.loads(f.read_text(encoding="utf-8"),
                                      object_pairs_hook=unique_json_object,
                                      parse_constant=lambda v: (_ for _ in ()).throw(PilotError("nonfinite"))))
        except (ValueError,UnicodeError) as exc: p.error(str(exc))
    try: result=validate(reports,require_both=not args.single_architecture)
    except PilotError as exc: p.error(str(exc))
    if args.expected_source_sha != result["source_sha"]:
        p.error("report source SHA does not match checked-out source")
    print("SMART_TEMPO_B_PILOT_AGGREGATE=PASS")
    print("SMART_TEMPO_B_PILOT_SOURCE_SHA="+result["source_sha"])
    for arch,m in result["architectures"].items():
        for key in ("clear","within_005","within_020","wrong","held","aliases","negative",
                    "falseaccept","unknown_veto","cpu_seconds","peak_rss_bytes"):
            print(f"SMART_TEMPO_B_PILOT_{arch}_{key.upper()}={m[key]}")
        for key in ("shadow_primary_correct","shadow_reference_present",
                    "shadow_lost_primary"):
            print(f"SMART_TEMPO_B_PILOT_{arch}_{key.upper()}={m[key]}")
        for name in SHADOW_MODELS:
            print(f"SMART_TEMPO_B_PILOT_{arch}_SHADOW_{name}_CORRECT={m['shadow_models'][name]}")
    print("SMART_TEMPO_PHASE_B_CAUSAL_COMPLETENESS=INCOMPLETE")
    print("SMART_TEMPO_PHASE_C_GATE=CLOSED")
    print("SMART_TEMPO_MUSICAL_RELEASE=NO_GO")

if __name__=="__main__": main()
