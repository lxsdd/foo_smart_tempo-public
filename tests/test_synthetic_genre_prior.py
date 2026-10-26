"""Fail-closed, oracle-free genre-prior read-only safety probes."""
from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from audit_synthetic_genre_prior import (  # noqa: E402
    audio_only, conservative_genre, compare_audio_and_genre,
)


def board(*entries):
    return [dict(bpm=bpm, local_exact=score, rank=i + 1)
            for i, (bpm, score) in enumerate(entries)]


class SyntheticGenrePriorTests(unittest.TestCase):
    def test_audio_evidence_is_mandatory_and_genre_cannot_invent_candidate(self):
        self.assertIsNone(conservative_genre([], [(120, 130)]))
        self.assertIsNone(conservative_genre(board((80, .50)), [(120, 130)]))
        candidates = board((87, .99), (125, .70))
        self.assertEqual(conservative_genre(candidates, [(120, 130)]), 87)
        self.assertEqual(conservative_genre(candidates, [(40, 60)]), 87)
        self.assertEqual(conservative_genre(candidates, []), 87)

    def test_competing_audio_candidate_may_only_cause_hold(self):
        candidates = board((120, .982), (140, .970))
        self.assertEqual(audio_only(candidates), 120)
        self.assertIsNone(conservative_genre(candidates, [(135, 145)]))
        self.assertEqual(conservative_genre(candidates, [(110, 130)]), 120)
        self.assertEqual(conservative_genre(candidates, [(140, 150), (100, 125)]), 120)
        self.assertEqual(conservative_genre(board((120, .99), (140, .955)),
                                                [(135, 145)]), 120)

    def test_half_double_ambiguity_never_resolved_by_genre(self):
        candidates = board((120, .99), (60, .95))
        self.assertIsNone(audio_only(candidates))
        self.assertIsNone(conservative_genre(candidates, [(115, 125)]))
        self.assertIsNone(conservative_genre(candidates, [(55, 65)]))

    def test_real_legacy_host_required_for_B_and_scenarios_stay_independent(self):
        candidates = board((120, .982), (140, .970))
        scenarios = {'correct': [(115, 125)], 'wrong': [(135, 145)],
                     'contradictory': [(115, 125), (135, 145)],
                     'atypical': [(175, 195)]}
        out = compare_audio_and_genre(candidates, scenarios)
        self.assertTrue(all(not v['legacy_measured'] and v['legacy_host'] is None
                            for v in out.values()))
        self.assertEqual([out[k]['conservative_shadow'] for k in scenarios],
                         [120, None, 120, 120])
        host = {key: 120. for key in scenarios}
        measured = compare_audio_and_genre(candidates, scenarios, host)
        self.assertTrue(all(v['legacy_measured'] for v in measured.values()))
        self.assertEqual(measured['wrong']['legacy_host'], 120)
        with self.assertRaises(ValueError):
            compare_audio_and_genre(candidates, scenarios, {'correct': 120})

    def test_invalid_candidates_ranges_and_scenarios_fail_closed(self):
        for candidate in (board((120, float('nan'))), board((0, .98)),
                          board((120, 1.01)), board((120, .99)) * 33):
            with self.assertRaises(ValueError):
                conservative_genre(candidate, [(110, 130)])
        for ranges in ([(-1, 130)], [(140, 140)], [(120, float('inf'))],
                       [(1, 2)] * 5):
            with self.assertRaises(ValueError):
                conservative_genre(board((120, .99)), ranges)
        with self.assertRaises(ValueError):
            compare_audio_and_genre(board((120, .99)), {'bad name': []})
        with self.assertRaises(ValueError):
            compare_audio_and_genre(board((120, .99)), {'ok': []}, {'ok': float('nan')})


if __name__ == '__main__':
    unittest.main()
