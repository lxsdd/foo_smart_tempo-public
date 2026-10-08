#include "stdafx.h"

#include "foobar2000/SDK/foobar2000.h"

DECLARE_COMPONENT_VERSION(
    "Smart Tempo", "2.2.0",
    "Smart Tempo v2.2.0\r\n"
    "BPM analysis and review component for foobar2000.\r\n\r\n"
    "Analysis:\r\n"
    "- Hodgkinson/Audacity-MIR analysis with KissFFT-backed onset functions\r\n"
    "- Measured-candidate board and pulse-family consistency matrix\r\n"
    "- Soft genre centers that can only re-rank measured BPM candidates\r\n"
    "- Measured-writer guard and Review Hold for unresolved pulse ambiguity\r\n"
    "- Confidence, uncertainty, Standard, and Verbose diagnostics\r\n"
    "- Parallel library analysis with content-difference tag writing\r\n\r\n"
    "Review tools:\r\n"
    "- Measured BPM candidate inspection without generated tempo values\r\n"
    "- Direct candidate inspection for one selected playlist track\r\n"
    "- Robust manual tap calculation with timing-stability feedback\r\n"
    "- Shared-mode metronome for measured pulse comparison\r\n"
    "- Optional playlists for unmatched metadata and Review Hold results\r\n\r\n"
    "Some sparse, variable-tempo, or metrically ambiguous recordings may require\r\n"
    "manual review. No BPM is written when the available evidence is insufficient.\r\n\r\n"
    "Created by: alexinc (2026)\r\n"
    "Uses: Hodgkinson/Audacity MIR methods, KissFFT, WTL, and foobar2000 SDK.");

VALIDATE_COMPONENT_FILENAME("foo_smart_tempo.dll");

