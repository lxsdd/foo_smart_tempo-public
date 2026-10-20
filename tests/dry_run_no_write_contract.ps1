$ErrorActionPreference = 'Stop'

# Fail closed: the diagnostic batch menu must not reach a writer, even when
# the ordinary Auto-write preference is enabled. No personal audio used here.
$menu = Get-Content 'src/foo_smart_tempo/bpm_contextmenu_item.cpp' -Raw
$worker = Get-Content 'src/foo_smart_tempo/bpm_auto_analysis_thread.cpp' -Raw
$results = Get-Content 'src/foo_smart_tempo/bpm_result_dialog.cpp' -Raw
$header = Get-Content 'src/foo_smart_tempo/bpm_result_dialog.h' -Raw

function Require-Literal([string]$source, [string]$pattern, [string]$message) {
    if (-not $source.Contains($pattern)) { throw $message }
}

Require-Literal $menu 'run_batch_dry_run(p_data);' 'Missing diagnostic context dispatch'
Require-Literal $menu 'bpm_auto_analysis_thread>(p_data, false, true)' 'Diagnostic analysis not explicitly non-writing'
Require-Literal $worker 'm_batchDryRunMode(p_batchDryRunMode)' 'Worker did not retain diagnostic flag'
Require-Literal $worker 'm_batchDryRunMode);' 'Worker did not transfer diagnostic flag to result dialog'
Require-Literal $worker '!m_batchDryRunMode' 'Worker did not separate normal analysis path'
Require-Literal $header 'bool m_batchDryRunMode = false;' 'Result dialog diagnostic mode missing'
Require-Literal $results 'if (bpm_config_auto_write_tag && !m_batchDryRunMode)' 'Auto-write not bypassed'
Require-Literal $results 'if (m_batchDryRunMode) return 0; // Secondary hard guard: no tag dispatch.' 'Manual writer not protected'
Require-Literal $results 'if (updateButton != nullptr) ::EnableWindow(updateButton, FALSE);' 'Update action still enabled'
Require-Literal $results 'DIAGNOSTIC / NO WRITE' 'Results do not identify write-safety mode'
Require-Literal $results 'DIAGNOSTIC MODE: tag writes and playlist edits disabled.' 'Diagnostic safety label missing'
Require-Literal $results 'void bpm_result_dialog::ApplyMeasuredCandidateSelection(' 'Selection callback absent'
Require-Literal $results 'void bpm_result_dialog::ScaleSelectionBPM(' 'Scale callback absent'

# Three independently controlled paths: unmatched creation, review collection,
# review playlist reconciliation. None may activate for diagnostic batches.
$playlistGuard = '!m_measuredCandidateInspectionMode && !m_batchDryRunMode &&'
if (($worker.Split(@($playlistGuard), [StringSplitOptions]::None).Count - 1) -ne 3) {
    throw 'Expected exactly three protected playlist paths'
}

# Writer hard guard must occur BEFORE the write dispatcher, not after it.
$onOk = $results.IndexOf('LRESULT bpm_result_dialog::OnOK(')
$guard = $results.IndexOf('if (m_batchDryRunMode) return 0; // Secondary hard guard', $onOk)
$update = $results.IndexOf('smart_tempo::tag_write::safe_update_info_async(', $onOk)
$nextHandler = $results.IndexOf('LRESULT bpm_result_dialog::OnCancel(', $onOk)
if ($onOk -lt 0 -or $guard -lt 0 -or $update -lt 0 -or
    $nextHandler -lt 0 -or $guard -ge $update -or $update -ge $nextHandler) {
    throw 'Diagnostic guard not before results writer dispatch'
}


# Verify guards at the beginning of all result mutation entrypoints.
function Require-EarlyDiagnosticGuard([string]$source, [string]$method, [string]$guard) {
    $signature = [regex]::Escape('bpm_result_dialog::' + $method)
    $pattern = '(?s)\b' + $signature + '\s*\([^)]*\)\s*\{\s*' + [regex]::Escape($guard)
    if (-not [regex]::IsMatch($source, $pattern)) {
        throw "Diagnostic guard missing or late in $method"
    }
}
foreach ($handler in @('OnOK', 'OnManualBpmClicked', 'OnMeasuredCandidatesClicked')) {
    Require-EarlyDiagnosticGuard $results $handler 'if (m_batchDryRunMode) return 0;'
}
foreach ($handler in @('ApplyMeasuredCandidateSelection', 'ScaleSelectionBPM')) {
    Require-EarlyDiagnosticGuard $results $handler 'if (m_batchDryRunMode) return;'
}
foreach ($method in @('EnableScaleBPMButtons', 'EnableMeasuredCandidatesButton')) {
    $signature = [regex]::Escape('bpm_result_dialog::' + $method)
    $pattern = '(?s)\b' + $signature + '\s*\([^)]*\)\s*\{\s*if \(m_batchDryRunMode\)\s*\{'
    if (-not [regex]::IsMatch($results, $pattern)) {
        throw "Diagnostic control re-enable guard missing in $method"
    }
}
Write-Host 'DRY_RUN_NO_WRITE_ENTRYPOINT_CONTRACT=PASS'

Write-Host 'DRY_RUN_NO_WRITE_SOURCE_CONTRACT=PASS'
