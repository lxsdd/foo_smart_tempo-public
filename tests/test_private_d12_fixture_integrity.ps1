$ErrorActionPreference = 'Stop'
# Synthetic-only safety regression; no owner data or private paths.
$tool = Join-Path $PSScriptRoot '../tools/verify-private-d12-fixtures.ps1'
$working = Join-Path ([System.IO.Path]::GetTempPath()) ('d12-integrity-synthetic-' + [guid]::NewGuid().ToString('N'))
$root = Join-Path $working 'copied-test-media'
New-Item -ItemType Directory -Force -Path $root | Out-Null

function Check([bool]$Condition, [string]$Reason) {
    if (-not $Condition) { throw $Reason }
}
function RunGuard([string]$Phase, [string]$StatePath) {
    $output = @(& pwsh -NoLogo -NoProfile -File $tool -Phase $Phase -Root $root -StateFile $StatePath 2>&1)
    return [pscustomobject]@{
        code = $LASTEXITCODE
        message = ($output -join [Environment]::NewLine)
    }
}
function AssertPrivate([string]$Text) {
    Check (-not $Text.Contains('UnsharedSyntheticArtist')) 'Fixture identifier leaked'
    Check (-not $Text.Contains($root)) 'Fixture directory leaked'
    Check (-not $Text.Contains('copied-test-media')) 'Fixture directory name leaked'
}

try {
    $wav = Join-Path $root 'UnsharedSyntheticArtist - Fake 01.wav'
    $cue = Join-Path $root 'UnsharedSyntheticArtist - Fake 01.cue'
    [System.IO.File]::WriteAllBytes($wav, [byte[]](1, 2, 3, 4, 5))
    Set-Content -LiteralPath $cue -Value 'FILE "Fake 01.wav" WAVE' -Encoding UTF8
    $stateA = Join-Path $working 'snapshot-a.json'

    $before = RunGuard 'Before' $stateA
    Check ($before.code -eq 0 -and $before.message.Contains('D12_FIXTURE_SNAPSHOT=PASS')) 'Snapshot should pass'
    AssertPrivate $before.message
    $raw = Get-Content -LiteralPath $stateA -Raw
    AssertPrivate $raw
    Check (-not $raw.Contains('Fake 01')) 'State must not expose private filenames'

    $after = RunGuard 'After' $stateA
    Check ($after.code -eq 0 -and $after.message.Contains('D12_FIXTURE_INTEGRITY=PASS')) 'Unchanged files should pass'
    AssertPrivate $after.message
    $secondBefore = RunGuard 'Before' $stateA
    Check ($secondBefore.code -ne 0 -and $secondBefore.message.Contains('STATE_ALREADY_EXISTS')) 'Snapshot overwrite must fail'
    AssertPrivate $secondBefore.message

    [System.IO.File]::WriteAllBytes($wav, [byte[]](1, 2, 3, 4, 9))
    $mutated = RunGuard 'After' $stateA
    Check ($mutated.code -ne 0 -and $mutated.message.Contains('FILE_INTEGRITY_CHANGED')) 'Content mutation must be caught'
    AssertPrivate $mutated.message

    $stateB = Join-Path $working 'snapshot-b.json'
    Check ((RunGuard 'Before' $stateB).code -eq 0) 'Second snapshot should pass'
    $item = Get-Item -LiteralPath $wav
    $item.LastWriteTimeUtc = $item.LastWriteTimeUtc.AddMinutes(5)
    $timeChanged = RunGuard 'After' $stateB
    Check ($timeChanged.code -ne 0 -and $timeChanged.message.Contains('FILE_INTEGRITY_CHANGED')) 'Timestamp-only mutation must be caught'
    AssertPrivate $timeChanged.message

    $stateC = Join-Path $working 'snapshot-c.json'
    Check ((RunGuard 'Before' $stateC).code -eq 0) 'Third snapshot should pass'
    Set-Content -LiteralPath (Join-Path $root 'extra.txt') -Value 'sidecar'
    $extra = RunGuard 'After' $stateC
    Check ($extra.code -ne 0 -and $extra.message.Contains('FILE_COUNT_CHANGED')) 'New sidecar file must be caught'
    AssertPrivate $extra.message

    $inside = RunGuard 'Before' (Join-Path $root 'forbidden-state.json')
    Check ($inside.code -ne 0 -and $inside.message.Contains('STATE_MUST_BE_OUTSIDE_FIXTURES')) 'State must remain outside fixture tree'
    AssertPrivate $inside.message

    $missing = RunGuard 'After' (Join-Path $working 'missing.json')
    Check ($missing.code -ne 0 -and $missing.message.Contains('STATE_MISSING')) 'Missing snapshot must fail closed'
    AssertPrivate $missing.message

    Write-Output 'D12_FIXTURE_INTEGRITY_SYNTHETIC_TESTS=PASS'
} finally {
    Remove-Item -LiteralPath $working -Recurse -Force -ErrorAction SilentlyContinue
}
