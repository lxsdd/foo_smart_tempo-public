"""Independent source/runtime proof of original nested Policy first-return sites.

This is CI-only, label-blind, and does not infer global candidate eligibility.
The production algorithm and Policy reason/action remain unchanged.
"""
from __future__ import annotations

import re

class PolicyBranchError(ValueError):
    pass

BRANCHES = {
    "qualification_terminal": frozenset(("qualification_disabled", "qualification_empty")),
    "current_release_terminal": frozenset(f"current_release_{i:02d}" for i in range(1, 9)),
    "hold_resolution_terminal": frozenset(f"hold_resolution_{i:02d}" for i in range(1, 18)),
    "default_resolution_terminal": frozenset(("default_resolution",)),
}
SOURCE_RANGES = (
    ("current_release", "[[nodiscard]] bool try_apply_mir_policy_current_release_stage(",
     "[[nodiscard]] bool try_apply_mir_policy_hold_resolution_stage(", 8),
    ("hold_resolution", "[[nodiscard]] bool try_apply_mir_policy_hold_resolution_stage(",
     "void apply_mir_policy_default_resolution_stage(", 17),
)
ZERO = {"bpm": 0.0, "score": 0.0, "support": 0.0}


def verify_source_contract(source: str) -> dict:
    """Every original early true-return must carry an adjacent CI-only branch ID."""
    counts = {}
    for name, start, end, expected in SOURCE_RANGES:
        first = source.find(start)
        last = source.find(end, first + len(start))
        if first < 0 or last < 0:
            raise PolicyBranchError("Policy source function boundary missing")
        body = source[first:last]
        returns = list(re.finditer(r"\breturn\s+true\s*;", body))
        calls = re.findall(r'ci_policy_branch\("([^"]+)"\)', body)
        if len(returns) != expected or len(calls) != expected:
            raise PolicyBranchError("Policy source branch cardinality drift: " + name)
        for index, match in enumerate(returns, 1):
            before = body[max(0, match.start() - 250):match.start()]
            witness = re.search(
                r'ci_policy_branch\("([^"]+)"\);\s*#endif\s*$', before)
            token = f"{name}_{index:02d}"
            if not witness or witness.group(1) != token or calls[index - 1] != token:
                raise PolicyBranchError("unwitnessed or reordered Policy return: " + token)
        counts[name] = len(returns)
    if ('ci_policy_branch(!enabled ? "qualification_disabled" : "qualification_empty")'
            not in source or 'ci_policy_branch("default_resolution")' not in source or
            source.count("g_ci_policy_branch_token = nullptr;") != 2):
        raise PolicyBranchError("Policy qualification/default/reset witness missing")
    return counts


def reconcile(rows: list[dict], *, required: bool = False) -> dict:
    """Validate one native branch token against the actual first-return stage."""
    branches = [(i, row) for i, row in enumerate(rows)
                if row.get("stage") == "policy_branch"]
    if not branches:
        if required:
            raise PolicyBranchError("native nested Policy branch missing")
        return {"native_policy_branch_complete": 0}
    if len(branches) != 1:
        raise PolicyBranchError("duplicate native Policy branch")
    index, row = branches[0]
    stages = [(i, item.get("reason_code")) for i, item in enumerate(rows)
              if item.get("stage") == "policy_stage_path"]
    if not stages or index != stages[-1][0] + 1:
        raise PolicyBranchError("Policy branch must follow the complete stage path")
    reason = row.get("reason_code")
    if reason not in BRANCHES.get(stages[-1][1], frozenset()):
        raise PolicyBranchError("Policy branch contradicts executed terminal stage")
    if (row.get("outcome") != "observed" or row.get("candidate_id") is not None or
            row.get("parent_candidate_ids") != [] or row.get("metrics") != ZERO):
        raise PolicyBranchError("Policy branch payload not source-blind")
    resolution = [i for i, item in enumerate(rows)
                  if item.get("stage") == "policy_resolution"]
    if resolution and index >= resolution[0]:
        raise PolicyBranchError("Policy branch emitted after resolution")
    return {"native_policy_branch_complete": 1, "policy_branch_token": reason}
