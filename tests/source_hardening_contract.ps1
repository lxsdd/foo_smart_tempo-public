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
$safeDispatch = $writer.IndexOf('update_info_async(', $guard)
$asyncStart = $writer.IndexOf("  try {", $guard)
if ($guard -lt 0 -or $safeDispatch -lt 0 -or $asyncStart -lt 0 -or $guard -ge $safeDispatch -or $guard -ge $asyncStart) {
    throw 'Embedded-Cue write guard must execute before the async dispatch path.'
}
$guardBlock = $writer.Substring($guard, $asyncStart - $guard)
if (-not $guardBlock.Contains('return false;')) {
    throw 'Embedded-Cue safety guard must reject before dispatch.'
}
if ($guardBlock.Contains('completionCallback(') -or $guardBlock.Contains('update_info_async(')) {
    throw 'Pre-dispatch safety rejection must not enter the async lifecycle.'
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

Require-Text '.github/workflows/private-personal-component.yml' @(
    'workflow_dispatch:',
    'contents: write',
    "github.actor == github.repository_owner && github.ref == 'refs/heads/main'",
    'Require absence of personal research',
    'verify-private-data-package-boundary.ps1',
    'verify-fb2k-component.ps1',
    'gh release create',
    '--draft',
    'if (-not $release.draft)',
    'git/ref/tags/$tag',
    'gh release delete $tag',
    'DRAFT_RELEASE_ONLY'
) @(
    'uses: actions/upload-artifact@',
    '--prerelease',
    '--latest'
)

$privateWorkflow = Get-Content -LiteralPath '.github/workflows/private-personal-component.yml' -Raw
if ($privateWorkflow -match '(?m)^\s*push\s*:') {
    throw 'Private personal component workflow must never run on push.'
}
if ($privateWorkflow -match '(?m)^\s*pull_request\s*:') {
    throw 'Private personal component workflow must never run on pull_request.'
}

Write-Host 'SOURCE_HARDENING_CONTRACT=PASS'
