[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ComponentPath,
    [string]$DllName
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-PeMachine {
    param([Parameter(Mandatory=$true)][string]$Path)
    $bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path))
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "$Path is not a valid PE file (missing MZ header)."
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or ($peOffset + 6) -gt $bytes.Length) {
        throw "$Path has an invalid PE header offset."
    }
    if ($bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45 -or $bytes[$peOffset + 2] -ne 0 -or $bytes[$peOffset + 3] -ne 0) {
        throw "$Path has no PE signature."
    }
    [BitConverter]::ToUInt16($bytes, $peOffset + 4)
}

$component = (Resolve-Path -LiteralPath $ComponentPath).Path
$temp = Join-Path ([IO.Path]::GetTempPath()) ('fb2k-verify-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    [IO.Compression.ZipFile]::ExtractToDirectory($component, $temp)

    if (Test-Path -LiteralPath (Join-Path $temp 'x86')) {
        throw 'Invalid combined component layout: x86/ directory exists. Win32 DLL must be in the archive root.'
    }

    if ([string]::IsNullOrWhiteSpace($DllName)) {
        $rootDlls = @(Get-ChildItem -LiteralPath $temp -File -Filter '*.dll')
        if ($rootDlls.Count -ne 1) { throw "Expected exactly one root DLL, found $($rootDlls.Count)." }
        $DllName = $rootDlls[0].Name
    }

    $rootDll = Join-Path $temp $DllName
    $x64Dll = Join-Path (Join-Path $temp 'x64') $DllName
    if (-not (Test-Path -LiteralPath $rootDll -PathType Leaf)) { throw "Missing root Win32 DLL: $DllName" }
    if (-not (Test-Path -LiteralPath $x64Dll -PathType Leaf)) { throw "Missing x64 DLL: x64/$DllName" }

    $rootMachine = Get-PeMachine -Path $rootDll
    $x64Machine = Get-PeMachine -Path $x64Dll
    if ($rootMachine -ne 0x014c) { throw ('Root DLL machine 0x{0:X4}; expected x86/I386 0x014C.' -f $rootMachine) }
    if ($x64Machine -ne 0x8664) { throw ('x64 DLL machine 0x{0:X4}; expected AMD64 0x8664.' -f $x64Machine) }

    $componentHash = (Get-FileHash -LiteralPath $component -Algorithm SHA256).Hash.ToLowerInvariant()
    [pscustomobject]@{
        Component = $component
        DllName = $DllName
        RootMachine = ('0x{0:X4}' -f $rootMachine)
        X64Machine = ('0x{0:X4}' -f $x64Machine)
        Sha256 = $componentHash
        Status = 'PASS'
    }
} finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
}
