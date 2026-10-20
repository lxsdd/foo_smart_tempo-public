#requires -Version 7.2
[CmdletBinding()]
param(
  [Parameter(Mandatory)][ValidateSet('Win32','x64')][string]$Architecture,
  [Parameter(Mandatory)][string]$ComponentDll
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or -not $env:RUNNER_TEMP) { throw 'Disposable CI only' }
if (Get-Process foobar2000 -ErrorAction SilentlyContinue) { throw 'Non-isolated foobar instance' }
$component=(Resolve-Path -LiteralPath $ComponentDll).Path
$spec=(@{
  Win32=@('https://www.foobar2000.org/files/foobar2000_v2.26.1.exe','849BC4424326DA81049746E9A2255A3A707E56B76CC69ED11714ED23C69ECE52',332)
  x64=@('https://www.foobar2000.org/files/foobar2000-x64_v2.26.1.exe','72FE15A88B085EE4E49D68114E2BB9655000C2E0FDF40DCC128BBEF8BCAF3600',34404)
})[$Architecture]
function Assert-PE([string]$p) {
  $b=[IO.File]::ReadAllBytes($p)
  if ($b.Length -lt 512 -or $b[0] -ne 77 -or $b[1] -ne 90) { throw "Not PE: $p" }
  $n=[BitConverter]::ToInt32($b,60)
  if ($n -lt 64 -or $n+6 -gt $b.Length -or
      [Text.Encoding]::ASCII.GetString($b,$n,4) -ne ("PE"+[char]0+[char]0) -or
      [BitConverter]::ToUInt16($b,$n+4) -ne [UInt16]$spec[2]) { throw "Wrong PE architecture: $p" }
}
Assert-PE $component
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class STUI {
  public delegate bool Callback(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(Callback cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  public static IntPtr Find(int expected,string text) {
    IntPtr result=IntPtr.Zero;
    EnumWindows((h,p)=>{
      uint pid;
      GetWindowThreadProcessId(h,out pid);
      if(pid != (uint)expected) return true;
      var s=new StringBuilder(512);
      GetWindowText(h,s,s.Capacity);
      if(s.ToString().Contains(text)) { result=h; return false; }
      return true;
    },IntPtr.Zero);
    return result;
  }
  public static string Titles(int expected) {
    var names=new List<string>();
    EnumWindows((h,p)=>{
      uint pid;
      GetWindowThreadProcessId(h,out pid);
      if(pid==(uint)expected) {
        var s=new StringBuilder(512);
        GetWindowText(h,s,s.Capacity);
        if(s.Length>0) names.Add(s.ToString());
      }
      return true;
    },IntPtr.Zero);
    return string.Join(" || ",names);
  }
  public static int Rows(IntPtr h) {
    var list=GetDlgItem(h,1006);
    return list==IntPtr.Zero ? -1 : (int)SendMessage(list,0x1004,IntPtr.Zero,IntPtr.Zero);
  }
  public static bool ButtonDisabled(IntPtr h) {
    var b=GetDlgItem(h,1);
    return b!=IntPtr.Zero && !IsWindowEnabled(b);
  }
  public static void ProbeBlockedUpdate(IntPtr h) {
    SendMessage(h,0x111,new IntPtr(1),IntPtr.Zero);
  }
}
'@
function Stamp([string]$p) {
  $f=Get-Item -LiteralPath $p
  return ((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash+':'+$f.Length+':'+$f.LastWriteTimeUtc.Ticks)
}
function Launch([string]$exe,[string[]]$args) {
  $i=[Diagnostics.ProcessStartInfo]::new($exe)
  $i.WorkingDirectory=[IO.Path]::GetDirectoryName($exe)
  $i.UseShellExecute=$false
  foreach ($a in $args) { [void]$i.ArgumentList.Add($a) }
  return [Diagnostics.Process]::Start($i)
}
function Run-Case([string]$installer,[string]$root,[string]$inputFile,[string[]]$files,[int]$minimum,[string]$label) {
  $hostDir=Join-Path $root ('host-'+$label)
  $install=Start-Process $installer -ArgumentList @('/S',"/D=$hostDir") -Wait -PassThru
  if ($install.ExitCode -ne 0) { throw 'Official host installer failed' }
  $exe=Join-Path $hostDir 'foobar2000.exe'
  Assert-PE $exe
  $folder=Join-Path $hostDir 'components'
  New-Item -ItemType Directory $folder -Force | Out-Null
  Copy-Item -LiteralPath $component -Destination (Join-Path $folder 'foo_smart_tempo.dll')
  New-Item -ItemType File (Join-Path $hostDir 'portable_mode_enabled') | Out-Null
  $baseline=@{}
  foreach ($file in $files) { $baseline[$file]=Stamp $file }
  $hostProcess=$null
  try {
    $hostProcess=Launch $exe @()
    $until=[DateTime]::UtcNow.AddSeconds(40)
    $loaded=$false
    while ([DateTime]::UtcNow -lt $until) {
      Start-Sleep -Milliseconds 750
      $hostProcess.Refresh()
      if ($hostProcess.HasExited) { throw 'foobar crashed during load' }
      try {
        $loaded=@((Get-Process -Id $hostProcess.Id).Modules |
                  Where-Object ModuleName -EQ 'foo_smart_tempo.dll').Count -gt 0
      } catch [ComponentModel.Win32Exception] {}
      if ($loaded) { break }
    }
    if (-not $loaded) { throw 'Component did not load in product test' }
    Start-Sleep -Seconds 2
    # Built-in context_command naming differs across foobar versions.
    # Probe leaf and grouped command, never the potentially writing command.
    $dialog=[IntPtr]::Zero
    $seenProgress=$false
    Write-Host ("HOST_WINDOWS_BEFORE_{0}: {1}" -f $label,[STUI]::Titles($hostProcess.Id))
    foreach ($commandName in @(
      'Analyze selected tracks (no tag writes)',
      'Smart Tempo/Analyze selected tracks (no tag writes)'
    )) {
      Write-Host ("DIAGNOSTIC_CLI_TRY_{0}: {1}" -f $label,$commandName)
      $dispatch=Launch $exe @(('/context_command:'+$commandName),$inputFile)
      if (-not $dispatch.WaitForExit(15000) -or $dispatch.ExitCode -ne 0) { throw 'Diagnostic launcher failed' }
      $probeUntil=[DateTime]::UtcNow.AddSeconds(12)
      while ([DateTime]::UtcNow -lt $probeUntil) {
        $dialog=[STUI]::Find($hostProcess.Id,'DIAGNOSTIC / NO WRITE')
        $progress=[STUI]::Find($hostProcess.Id,'Analyzing...')
        if ($dialog -ne [IntPtr]::Zero -or $progress -ne [IntPtr]::Zero) {
          $seenProgress=$true
          break
        }
        Start-Sleep -Milliseconds 600
      }
      if ($seenProgress) { break }
      Write-Host ("HOST_WINDOWS_AFTER_ATTEMPT_{0}: {1}" -f $label,[STUI]::Titles($hostProcess.Id))
    }
    if (-not $seenProgress) {
      throw "No evidence of context-command execution for $label; see host window titles"
    }
    $until=[DateTime]::UtcNow.AddSeconds(150)
    while ($dialog -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $until) {
      $hostProcess.Refresh()
      if ($hostProcess.HasExited) { throw 'foobar crashed during analysis' }
      $dialog=[STUI]::Find($hostProcess.Id,'DIAGNOSTIC / NO WRITE')
      if ($dialog -ne [IntPtr]::Zero) { break }
      Start-Sleep -Milliseconds 700
    }
    if ($dialog -eq [IntPtr]::Zero) {
      Write-Host ("HOST_WINDOWS_AT_TIMEOUT_{0}: {1}" -f $label,[STUI]::Titles($hostProcess.Id))
      throw "Diagnostic result did not open for $label"
    }
    $rows=[STUI]::Rows($dialog)
    if ($rows -lt $minimum) { throw "Only $rows result rows for $label; expected $minimum" }
    if (-not [STUI]::ButtonDisabled($dialog)) { throw 'Diagnostic Update control enabled' }
    foreach ($file in $files) { if ((Stamp $file) -cne $baseline[$file]) { throw 'File changed during analysis' } }
    [STUI]::ProbeBlockedUpdate($dialog)
    Start-Sleep -Seconds 2
    if (-not [STUI]::IsWindow($dialog)) { throw 'Secondary no-write guard failed' }
    foreach ($file in $files) { if ((Stamp $file) -cne $baseline[$file]) { throw 'File changed after blocked Update handler' } }
    Write-Host ("DIAGNOSTIC_MENU_{0}_{1}=PASS" -f $Architecture,$label)
    Write-Host ("DIAGNOSTIC_ROWS_{0}_{1}={2}" -f $Architecture,$label,$rows)
    Write-Host ("MEDIA_WRITE_SAFETY_{0}_{1}=PASS" -f $Architecture,$label)
  } finally {
    if ($null -ne $hostProcess -and -not $hostProcess.HasExited) {
      Stop-Process -Id $hostProcess.Id -Force -ErrorAction SilentlyContinue
      [void]$hostProcess.WaitForExit(5000)
    }
    foreach ($file in $files) { if ((Stamp $file) -cne $baseline[$file]) { throw 'Fixture changed after exit' } }
  }
}
$root=Join-Path $env:RUNNER_TEMP ('smart-tempo-'+$Architecture+'-'+[guid]::NewGuid().ToString('N'))
try {
  New-Item -ItemType Directory $root -Force | Out-Null
  $installer=Join-Path $root 'official.exe'
  Invoke-WebRequest -Uri $spec[0] -OutFile $installer
  if ((Get-FileHash $installer -Algorithm SHA256).Hash -ine $spec[1]) { throw 'Official host hash mismatch' }
  $wav=Join-Path $root 'rhythm.wav'
  $cue=Join-Path $root 'rhythm.cue'
  [byte[]]$audio=[byte[]]::new(16000*40*2)
  for ($beat=0; $beat -lt 80; $beat++) {
    for ($j=0; $j -lt 160; $j++) {
      $p=2*($beat*8000+$j)
      $audio[$p]=[byte]0
      $audio[$p+1]=[byte](($j%2)*90)
    }
  }
  $file=[IO.File]::Open($wav,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
  $writer=[IO.BinaryWriter]::new($file)
  try {
    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
    $writer.Write([int](36+$audio.Length))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
    $writer.Write([int]16)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]1)
    $writer.Write([int]16000)
    $writer.Write([int]32000)
    $writer.Write([UInt16]2)
    $writer.Write([UInt16]16)
    $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
    $writer.Write([int]$audio.Length)
    $writer.Write($audio)
  } finally { $writer.Dispose();$file.Dispose() }
  [IO.File]::WriteAllLines($cue,@('FILE "rhythm.wav" WAVE','  TRACK 01 AUDIO','    INDEX 01 00:00:00','  TRACK 02 AUDIO','    INDEX 01 00:20:00'),[Text.Encoding]::ASCII)
  Run-Case $installer $root $wav @($wav) 1 'WAV'
  Run-Case $installer $root $cue @($wav,$cue) 2 'CUE'
  Write-Host ("SMART_TEMPO_DIAGNOSTIC_RUNTIME_{0}=PASS" -f $Architecture)
} finally { if (Test-Path $root) { Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue } }
