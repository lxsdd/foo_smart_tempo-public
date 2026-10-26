"""Public source-only conservation gate for already-frozen Smart Tempo tests.

Do not synthesize a new matrix or treat repeated windows as independent audio.
All source imports are bounded and must remain decoupled from owner music.
"""
from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"tools"))
from generate_synthetic_bpm_reference import (
    CASES as BASE, PROSPECTIVE_CASES, WINDOW_CASES)
from generate_synthetic_complex_challenge import CASES as COMPLEX
from generate_synthetic_meter_challenge import CASES as METER
from score_synthetic_window_sweep import AXES, REFERENCE
from score_synthetic_temporal_sweep import PHASE_REFERENCES
from score_prospective_pass_sweep import keys as prospective_pass_keys


class IntegratedResearchPortfolio(unittest.TestCase):
    def test_original_public_frozen_partition_counts_and_no_collisions(self):
        self.assertEqual((len(BASE),len(PROSPECTIVE_CASES),len(COMPLEX),
                          len(METER),len(WINDOW_CASES)),(80,20,16,5,6))
        ids=[[row[0] for row in partition] for partition in (
            BASE,PROSPECTIVE_CASES,COMPLEX,METER,WINDOW_CASES)]
        self.assertEqual(len(set(sum(ids,[]))),sum(map(len,ids)))
        self.assertTrue(all(name.startswith("challenge_") for name in ids[2]))

    def test_original_repeat_measurement_counts_are_not_wav_counts(self):
        self.assertEqual(len(REFERENCE)*len(AXES),78)
        self.assertEqual(set(REFERENCE),set(row[0] for row in WINDOW_CASES))
        self.assertEqual(len(PHASE_REFERENCES)*2,12)
        self.assertEqual(len(prospective_pass_keys()),33)
        self.assertEqual(len(BASE)+78+12+len(PROSPECTIVE_CASES)+33+len(COMPLEX),
                         239)

    def test_real_host_and_source_reuse_are_wired_and_fail_closed(self):
        ps=(ROOT/"tests/diagnostic_live_host_smoke.ps1").read_text(encoding="utf-8")
        sink=(ROOT/"tests/ci_bpm_oracle_sink.h").read_text(encoding="utf-8")
        wf=(ROOT/".github/workflows/source.yml").read_text(encoding="utf-8")
        for value in (
            "SYNTHETIC_REAL_COMPLEX_COMPLETED_x64=PASS",
            "SYNTHETIC_REAL_PROSPECTIVE_COMPLETED_x64=PASS",
            "SYNTHETIC_REAL_TEMPORAL_COMPLETED_x64=PASS",
            "SYNTHETIC_REAL_PROSPECTIVE_PASS_SWEEP_COUNT_x64",
            "score_synthetic_window_sweep.py",
            "score_synthetic_complex_challenge.py",
            "Run-Case $installer $root $wav"):
            self.assertIn(value,ps)
        for name,_,_ in COMPLEX:
            self.assertIn('L"'+name+'"',sink)
        self.assertEqual(ps.count("SMART_TEMPO_DIAGNOSTIC_RUNTIME_"),1)
        self.assertEqual(ps.count("if ($challengeObserved.Count -ne 16"),1)
        self.assertIn("if ($caseId -cnotmatch '^challenge_[a-z0-9_]+$') {",ps)
        self.assertEqual(ps.count("SYNTHETIC_REAL_COMPLEX_COMPLETED_x64=PASS"),1)
        self.assertIn("bounded_source_bits",sink)
        self.assertIn("CREATE_NEW",sink)
        self.assertIn('read_env(L"GITHUB_ACTIONS")',sink)
        for test in ("test_synthetic_complex_challenge.py",
                     "test_synthetic_source_bits.py",
                     "test_synthetic_meter_challenge.py",
                     "test_synthetic_frozen_oracle_anchors.py"):
            self.assertIn(test,wf)

    def test_shadow_does_not_replace_production_or_become_public_media(self):
        sink=(ROOT/"tests/ci_bpm_oracle_sink.h").read_text(encoding="utf-8")
        self.assertIn("#ifndef SMART_TEMPO_CI_ORACLE_PROBE",sink)
        self.assertIn("candidate.sources.get_ptr()",sink)
        self.assertNotIn("reference_bpm",sink.split("inline int bounded_source_bits")[1])


if __name__=="__main__":
    unittest.main()
