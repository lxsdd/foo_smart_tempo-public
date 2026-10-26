#!/usr/bin/env python3
"""Fail-closed source-only replay of preexisting public BPM research tests.

Four exact legacy checks require the original #37/#38 CI-only host-sidecar
contract, which must be integrated separately. They are counted as BLOCKED,
never silently skipped or marked passing. All other test methods execute.
"""
from __future__ import annotations

import unittest
from pathlib import Path

SUITES = (
    "test_synthetic_frozen_oracle_anchors.py",
    "test_synthetic_meter_challenge.py",
    "test_synthetic_window_sweep.py",
    "test_synthetic_temporal_sweep.py",
    "test_prospective_bpm_scorer.py",
    "test_prospective_pass_sweep.py",
    "test_synthetic_complex_challenge.py",
    "test_synthetic_export_rank.py",
    "test_synthetic_genre_prior.py",
    "test_synthetic_policy_stage.py",
    "test_synthetic_source_bits.py",
    "test_synthetic_stage_shadow.py",
)
BLOCKED_HOST_CONTRACTS = frozenset((
    "test_synthetic_meter_challenge.MeterChallengeTests.test_ci_only_meter_allowlist_and_no_write_host_contract",
    "test_synthetic_export_rank.ExportRankTests.test_cpp_source_exports_rank_not_primary_origin",
    "test_synthetic_source_bits.ProvenanceTests.test_ci_powershell_source_sidecar_contract",
    "test_synthetic_source_bits.ProvenanceTests.test_compiled_ci_source_mask_contract",
))


def flatten(suite: unittest.TestSuite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from flatten(item)
        else:
            yield item


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    loader = unittest.TestLoader()
    cases = []
    found = set()
    for pattern in SUITES:
        loaded = list(flatten(loader.discover(
            str(root / "tests"), pattern=pattern, top_level_dir=str(root / "tests"))))
        if not loaded:
            raise SystemExit("MISSING_FROZEN_TEST_SUITE=" + pattern)
        for case in loaded:
            if not isinstance(case, unittest.TestCase):
                raise SystemExit("INVALID_FROZEN_TEST=" + repr(case))
            name = case.id()
            if name in found:
                raise SystemExit("DUPLICATE_FROZEN_TEST=" + name)
            found.add(name)
            if name not in BLOCKED_HOST_CONTRACTS:
                cases.append(case)
    if not BLOCKED_HOST_CONTRACTS.issubset(found):
        raise SystemExit("MISSING_BLOCKED_HOST_CONTRACT_EXPECTATION=" +
                         repr(sorted(BLOCKED_HOST_CONTRACTS - found)))
    report = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(cases))
    if not report.wasSuccessful():
        raise SystemExit("FROZEN_PORTFOLIO_PORTABLE_REGRESSION=FAIL")
    print(f"SMART_TEMPO_REUSED_PORTFOLIO_PORTABLE_CASES={report.testsRun}")
    print(f"SMART_TEMPO_REUSED_PORTFOLIO_HOST_CONTRACT_BLOCKED={len(BLOCKED_HOST_CONTRACTS)}")
    print("SMART_TEMPO_REUSED_PORTFOLIO_HOST_CONTRACT_STATUS=NOT_INTEGRATED")
    print("SMART_TEMPO_REUSED_PORTFOLIO_PORTABLE_REGRESSION=PASS")


if __name__ == "__main__":
    main()
