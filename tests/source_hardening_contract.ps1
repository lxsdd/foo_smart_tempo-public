$ErrorActionPreference = 'Stop'

function Require-Text {
    param(
        [string]$Path,
        [string[]]$Required,
        [string[]]$Forbidden = @()
    )
    $text = Get-Content -LiteralPath $Path -Raw
    foreach ($needle in $Required) {
        if (-not $text.Contains($needle)) {
            throw "Required source contract missing in $Path : $needle"
        }
    }
    foreach ($needle in $Forbidden) {
        if ($text.Contains($needle)) {
            throw "Forbidden source contract present in $Path : $needle"
        }
    }
}

Require-Text 'src/foo_smart_tempo/file_info_filter_scale_bpm.cpp' @(
    'try_parse_positive_bpm_strict',
    'errno == ERANGE',
    '!std::isfinite(parsed)',
    '*parseEnd != ''\0''',
    '!std::isfinite(m_scale)',
    '!std::isfinite(scaledBpm)'
)

Require-Text 'src/foo_smart_tempo/tag_write_dispatch.cpp' @(
    'location.get_subsong() != 0',
    'pfc::stricmp_ascii(extension, "mp3") == 0',
    'pfc::stricmp_ascii(extension, "flac") == 0',
    'pfc::stricmp_ascii(extension, "wv") == 0',
    'unsafe-embedded-cue-virtual-subsong',
    'if (!selection_is_safe(items'
)

$writer = Get-Content -LiteralPath 'src/foo_smart_tempo/tag_write_dispatch.cpp' -Raw
$guard = $writer.IndexOf('if (!selection_is_safe(items')
$dispatch = $writer.IndexOf('update_info_async(', $guard)
if ($guard -lt 0 -or $dispatch -lt 0 -or $guard -ge $dispatch) {
    throw 'Embedded-Cue write guard must execute before update_info_async.'
}
$preDispatch = $writer.Substring($guard, $dispatch - $guard)
if ($preDispatch.Contains('completionCallback(')) {
    throw 'Pre-dispatch safety rejection must not enter the async completion lifecycle.'
}

Require-Text 'src/foo_smart_tempo/hodgkinson_full_mir.cpp' @(
    'struct OdfSmoothingWorkspace',
    'thread_local OdfSmoothingWorkspace workspace',
    'subtract_moving_average(odf, odfFrameRate)'
) @(
    'const std::vector<float> avg = moving_average'
)

Require-Text 'src/foo_smart_tempo/mir_candidate_pipeline.cpp' @(
    'm_scoreByBpmBits.reserve(16384);'
) @(
    'm_scoreByBpmBits.reserve(8192);'
)

Require-Text 'src/foo_smart_tempo/bpm_contextmenu_item.cpp' @(
    '"scale-bpm-context-menu", NULL, true'
)

Write-Host 'SOURCE_HARDENING_CONTRACT=PASS'
