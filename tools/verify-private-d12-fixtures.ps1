#requires -Version 5.1
<#
Local-only integrity guard for disposable Smart Tempo D12 fixtures.
No audio/tag/playlist writes. Only the caller-specified JSON state file is written
in Before mode, outside the fixture tree. Neither state nor personal logs belong
in GitHub, Actions, or a bug report.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('Before', 'After')]
    [string]$Phase,
    [Parameter(Mandatory=$true)]
    [string]$Root,
    [Parameter(Mandatory=$true)]
    [string]$StateFile
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Reason) {
    # Do not accidentally print private paths, filenames, or per-file hashes.
    Write-Output 'D12_FIXTURE_INTEGRITY=FAIL'
    Write-Output "D12_FIXTURE_REASON=$Reason"
    exit 1
}

function Identifier([string]$Text, [string]$Salt) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Salt + [char]0 + $Text)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Snapshot([string]$Directory, [string]$Salt) {
    $entries = @(Get-ChildItem -LiteralPath $Directory -Force -Recurse -ErrorAction Stop)
    foreach ($entry in $entries) {
        if (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail 'REPARSE_POINT'
        }
    }

    $files = @($entries | Where-Object { -not $_.PSIsContainer })
    if ($files.Count -eq 0) { Fail 'EMPTY_FIXTURE' }

    $records = New-Object 'System.Collections.Generic.List[object]'
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($Directory.Length).TrimStart([char]'\', [char]'/')
        $digest = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
        $records.Add([pscustomobject][ordered]@{
            id = Identifier $relative $Salt
            bytes = [long]$file.Length
            utc_ticks = [long]$file.LastWriteTimeUtc.Ticks
            sha256 = $digest
        })
    }
    return @($records | Sort-Object -Property id)
}

try {
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { Fail 'INVALID_ROOT' }
    $rootItem = Get-Item -LiteralPath $Root -Force
    if (($rootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        Fail 'REPARSE_POINT'
    }
    $directory = [System.IO.Path]::GetFullPath($rootItem.FullName).TrimEnd([char]'\', [char]'/')
    $volumeRoot = [System.IO.Path]::GetPathRoot($directory)
    if ($directory -eq $volumeRoot.TrimEnd([char]'\', [char]'/')) {
        Fail 'FILESYSTEM_ROOT_REJECTED'
    }
    $statePath = [System.IO.Path]::GetFullPath($StateFile)
    $prefix = $directory + [System.IO.Path]::DirectorySeparatorChar
    if ($statePath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) -or
        $statePath.Equals($directory, [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail 'STATE_MUST_BE_OUTSIDE_FIXTURES'
    }
    if ($Phase -eq 'Before') {
        if (Test-Path -LiteralPath $statePath) { Fail 'STATE_ALREADY_EXISTS' }
        if (-not (Test-Path -LiteralPath (Split-Path -Parent $statePath) -PathType Container)) {
            Fail 'STATE_PARENT_MISSING'
        }
        $salt = [guid]::NewGuid().ToString('N')
        $records = @(Snapshot $directory $salt)
        $state = [ordered]@{
            schema = 'smart_tempo_d12_integrity_v1'
            salt = $salt
            root_id = Identifier $directory $salt
            file_count = [int]$records.Count
            files = @($records)
        }
        $json = $state | ConvertTo-Json -Depth 6
        Set-Content -LiteralPath $statePath -Value $json -Encoding UTF8 -NoNewline -ErrorAction Stop
        Write-Output 'D12_FIXTURE_SNAPSHOT=PASS'
        Write-Output "D12_FIXTURE_FILE_COUNT=$($records.Count)"
        exit 0
    }

    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) { Fail 'STATE_MISSING' }
    $state = Get-Content -LiteralPath $statePath -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
    if ($state.schema -cne 'smart_tempo_d12_integrity_v1' -or
        $state.salt -notmatch '^[a-fA-F0-9]{32}$' -or
        $state.root_id -cne (Identifier $directory ([string]$state.salt)) -or
        [int]$state.file_count -le 0) {
        Fail 'STATE_INVALID'
    }
    $actual = @(Snapshot $directory ([string]$state.salt))
    $expected = @($state.files | Sort-Object -Property id)
    if ($actual.Count -ne [int]$state.file_count -or $actual.Count -ne $expected.Count) {
        Fail 'FILE_COUNT_CHANGED'
    }
    for ($i = 0; $i -lt $actual.Count; ++$i) {
        $a = $actual[$i]
        $b = $expected[$i]
        if ($a.id -cne $b.id -or
            $a.sha256 -cne $b.sha256 -or
            $a.bytes -ne [long]$b.bytes -or
            $a.utc_ticks -ne [long]$b.utc_ticks) {
            Fail 'FILE_INTEGRITY_CHANGED'
        }
    }
    Write-Output 'D12_FIXTURE_INTEGRITY=PASS'
    Write-Output "D12_FIXTURE_FILE_COUNT=$($actual.Count)"
    exit 0
} catch {
    # Deliberately withhold exception text: filesystem and PowerShell exceptions
    # can include private filenames or absolute paths.
    Fail 'LOCAL_ERROR'
}
