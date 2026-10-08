#pragma once

#include "experimental_feature_overrides.generated.h"

// Only MIR/Hodgkinson research surfaces remain. Retired Aubio, PhaseRefinement,
// same-pulse, resolver-promotion, and runtime-alias switches are intentionally
// absent so obsolete experiments cannot silently re-enter a build.
#ifndef SMART_TEMPO_ENABLE_HODGKINSON_ALIAS_PULSE_EVIDENCE
#define SMART_TEMPO_ENABLE_HODGKINSON_ALIAS_PULSE_EVIDENCE 0
#endif

#ifndef SMART_TEMPO_ENABLE_HODGKINSON_CONTINUOUS_REFINEMENT_EVIDENCE
#define SMART_TEMPO_ENABLE_HODGKINSON_CONTINUOUS_REFINEMENT_EVIDENCE 0
#endif

#ifndef SMART_TEMPO_ENABLE_HODGKINSON_SEGMENT_TOPK_TELEMETRY
#define SMART_TEMPO_ENABLE_HODGKINSON_SEGMENT_TOPK_TELEMETRY 0
#endif

#ifndef SMART_TEMPO_ENABLE_MIR_CANDIDATE_BOARD_TELEMETRY
#define SMART_TEMPO_ENABLE_MIR_CANDIDATE_BOARD_TELEMETRY 0
#endif

#ifndef SMART_TEMPO_ENABLE_MIR_SOFT_CENTER_TELEMETRY
#define SMART_TEMPO_ENABLE_MIR_SOFT_CENTER_TELEMETRY 0
#endif

#ifndef SMART_TEMPO_ENABLE_HODGKINSON_PARTIAL_BAR_PROBE
#define SMART_TEMPO_ENABLE_HODGKINSON_PARTIAL_BAR_PROBE 0
#endif

#ifndef SMART_TEMPO_ENABLE_HODGKINSON_PARTIAL_BAR_TOPK_EXACT_TELEMETRY
#define SMART_TEMPO_ENABLE_HODGKINSON_PARTIAL_BAR_TOPK_EXACT_TELEMETRY 0
#endif

#ifndef SMART_TEMPO_ENABLE_MIR_RESEARCH_ALL
#define SMART_TEMPO_ENABLE_MIR_RESEARCH_ALL 0
#endif

namespace smart_tempo::experimental {

inline constexpr bool kMirProductionEngine = true;
inline constexpr bool kEnableMirResearchAll =
    (SMART_TEMPO_ENABLE_MIR_RESEARCH_ALL != 0);

// These paths are part of the production MIR primary engine. Keeping them as
// explicit invariants avoids no-op "experimental" switches that could never
// disable the active implementation.
inline constexpr bool kEnableHodgkinsonTatumProbe = true;
inline constexpr bool kEnableHodgkinsonAliasPulseEvidence =
    (SMART_TEMPO_ENABLE_HODGKINSON_ALIAS_PULSE_EVIDENCE != 0) ||
    kEnableMirResearchAll;
inline constexpr bool kEnableHodgkinsonContinuousRefinementEvidence =
    (SMART_TEMPO_ENABLE_HODGKINSON_CONTINUOUS_REFINEMENT_EVIDENCE != 0) ||
    kEnableMirResearchAll;
inline constexpr bool kEnableHodgkinsonSegmentTopKTelemetry =
    (SMART_TEMPO_ENABLE_HODGKINSON_SEGMENT_TOPK_TELEMETRY != 0) ||
    kEnableMirResearchAll;
inline constexpr bool kEnableMirCandidateBoardTelemetry =
    (SMART_TEMPO_ENABLE_MIR_CANDIDATE_BOARD_TELEMETRY != 0) ||
    kEnableMirResearchAll;
inline constexpr bool kEnableMirSoftCenterTelemetry =
    (SMART_TEMPO_ENABLE_MIR_SOFT_CENTER_TELEMETRY != 0) ||
    kEnableMirResearchAll;
inline constexpr bool kEnableMirPolicyRuntime = true;
inline constexpr bool kEnableMirPolicyPipeline =
    kEnableMirPolicyRuntime || kEnableMirSoftCenterTelemetry;
// Validated on the 7,398-track Top-K stream: one intended Midway-like hold,
// zero known over-holds. This is a production write-safety invariant, not an
// experimental BPM mutation.
inline constexpr bool kEnableHodgkinsonMaterialRiskEvidence = true;
inline constexpr bool kEnableHodgkinsonPartialBarProbe =
    (SMART_TEMPO_ENABLE_HODGKINSON_PARTIAL_BAR_PROBE != 0) ||
    kEnableMirResearchAll;
inline constexpr bool kEnableHodgkinsonPartialBarTopKExactTelemetry =
    (SMART_TEMPO_ENABLE_HODGKINSON_PARTIAL_BAR_TOPK_EXACT_TELEMETRY != 0) ||
    kEnableMirResearchAll;
inline constexpr bool kEnableMirPrimaryTelemetry = true;
inline constexpr bool kEnableMirPrimaryRuntime = true;

}  // namespace smart_tempo::experimental
