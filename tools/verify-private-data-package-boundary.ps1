[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$ComponentPath)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$expected = @('foo_smart_tempo.dll', 'x64/foo_smart_tempo.dll', 'kissfft_license.txt')
$seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ComponentPath).Path)
try {
    foreach ($entry in $archive.Entries) {
        $name = $entry.FullName.Replace('\', '/')
        if ($name.EndsWith('/')) {
            if ($name -ne 'x64/') { throw "Unexpected directory in component: $name" }
            continue
        }
        if ($name -notin $expected) { throw "Non-product/private data cannot be shipped in component: $name" }
        if (-not $seen.Add($name)) { throw "Duplicate package entry: $name" }
        if ($entry.Length -eq 0) { throw "Empty package entry: $name" }
    }
    foreach ($name in $expected) {
        if (-not $seen.Contains($name)) { throw "Missing required package entry: $name" }
    }
    Write-Host 'PASS: foo_smart_tempo binary-only payload: Win32 DLL, x64 DLL and KissFFT notice; no research/test/audio data.'
}
finally { $archive.Dispose() }
