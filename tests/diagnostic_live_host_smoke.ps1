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
function Run-Case([string]$installer,[string]$root,[string]$inputFile,[string[]]$files,[int]$minimum,[string]$label,[int]$windowSeconds=0,[int]$windowPasses=0,[int]$offsetPct=0) {
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
    # Only the CI-probe DLL knows this disposable media path. It invokes
    # menu_helpers::run_command_context() by GUID on the host's main thread.
    if (($windowSeconds -eq 0) -ne ($windowPasses -eq 0)) {
      throw 'Window and pass overrides must be supplied together'
    }
    if ($offsetPct -ne 0 -and
        ($windowSeconds -ne 20 -or $windowPasses -ne 1 -or
         $offsetPct -notin @(20,80))) {
      throw 'Only public 20s/1-pass early/late phase probes are supported'
    }
    $env:SMART_TEMPO_CI_FIXTURE=$inputFile
    if ($windowSeconds -gt 0) {
      $env:SMART_TEMPO_CI_WINDOW_SECONDS=[string]$windowSeconds
      $env:SMART_TEMPO_CI_SAMPLE_PASSES=[string]$windowPasses
    }
    if ($offsetPct -gt 0) {
      $env:SMART_TEMPO_CI_OFFSET_PCT=[string]$offsetPct
    }
    try { $hostProcess=Launch $exe @() }
    finally {
      Remove-Item Env:SMART_TEMPO_CI_FIXTURE -ErrorAction SilentlyContinue
      Remove-Item Env:SMART_TEMPO_CI_WINDOW_SECONDS -ErrorAction SilentlyContinue
      Remove-Item Env:SMART_TEMPO_CI_SAMPLE_PASSES -ErrorAction SilentlyContinue
      Remove-Item Env:SMART_TEMPO_CI_OFFSET_PCT -ErrorAction SilentlyContinue
    }
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
    # A new portable foobar profile starts behind the modal appearance
    # setup. Acknowledge only this identified first-run UI, no write dialog.
    $quick=[IntPtr]::Zero
    $quickDeadline=[DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $quickDeadline) {
      $quick=[STUI]::Find($hostProcess.Id,'Quick Appearance Setup')
      if ($quick -ne [IntPtr]::Zero) { break }
      Start-Sleep -Milliseconds 300
    }
    if ($quick -ne [IntPtr]::Zero) {
      Write-Host ("FOOBAR_FIRST_RUN_SETUP_{0}=DETECTED" -f $label)
      if ([STUI]::GetDlgItem($quick,1) -eq [IntPtr]::Zero) {
        throw 'Quick Appearance Setup does not have a standard OK button'
      }
      [void][STUI]::SendMessage($quick,0x111,[IntPtr]::new(1),[IntPtr]::Zero)
      Start-Sleep -Seconds 2
      if ([STUI]::IsWindow($quick)) { throw 'Quick Appearance Setup did not close' }
      Write-Host ("FOOBAR_FIRST_RUN_SETUP_{0}=CLOSED" -f $label)
    }
    Start-Sleep -Seconds 2
    # Authorize dispatch only after the host has completed first-run setup.
    # This readiness marker is not a media file and is created in runner temp.
    $ready=$inputFile+'.ready'
    if (Test-Path -LiteralPath $ready) { throw 'Unexpected stale probe marker' }
    New-Item -ItemType File -Path $ready -ErrorAction Stop | Out-Null
    Write-Host ("HOST_SDK_PROBE_ARMED_{0}=TRUE" -f $label)
    $analysisStopwatch=[Diagnostics.Stopwatch]::StartNew()
    $dialog=[IntPtr]::Zero
    $until=[DateTime]::UtcNow.AddSeconds(150)
    while ([DateTime]::UtcNow -lt $until) {
      $hostProcess.Refresh()
      if ($hostProcess.HasExited) { throw 'foobar crashed during analysis' }
      $dialog=[STUI]::Find($hostProcess.Id,'DIAGNOSTIC / NO WRITE')
      if ($dialog -ne [IntPtr]::Zero) { break }
      Start-Sleep -Milliseconds 700
    }
    if ($dialog -eq [IntPtr]::Zero) {
      Write-Host ("HOST_WINDOWS_AT_TIMEOUT_{0}: {1}" -f $label,[STUI]::Titles($hostProcess.Id))
      throw "CI SDK probe did not reach diagnostic results for $label"
    }
    $analysisStopwatch.Stop()
    $script:LastSyntheticAnalysisMs=$analysisStopwatch.ElapsedMilliseconds
    $rows=[STUI]::Rows($dialog)
    if ($rows -lt $minimum) { throw "Only $rows result rows for $label; expected $minimum" }
    if (-not [STUI]::ButtonDisabled($dialog)) { throw 'Diagnostic Update control enabled' }
    foreach ($file in $files) { if ((Stamp $file) -cne $baseline[$file]) { throw 'File changed during analysis' } }
    [STUI]::ProbeBlockedUpdate($dialog)
    Start-Sleep -Seconds 2
    if (-not [STUI]::IsWindow($dialog)) { throw 'Secondary no-write guard failed' }
    foreach ($file in $files) { if ((Stamp $file) -cne $baseline[$file]) { throw 'File changed after blocked Update handler' } }
    if ($label.StartsWith('SYNTHETIC_', [StringComparison]::Ordinal)) {
      # The oracle output is aggregate-scored after every public test case.
      # No fixture-specific reference is used by the analyzer or selector.
      $observed=$inputFile+'.results.csv'
      if (-not (Test-Path -LiteralPath $observed -PathType Leaf)) {
        throw 'Real foobar numeric BPM oracle observer is not available'
      }
      $evidenceFile=$inputFile+'.candidates.csv'
      if (-not (Test-Path -LiteralPath $evidenceFile -PathType Leaf)) {
        throw 'Synthetic-only measured candidate evidence is missing'
      }
      $evidenceRows=@(Import-Csv -LiteralPath $evidenceFile)
      if ($evidenceRows.Count -gt 32) { throw 'Synthetic candidate evidence cap exceeded' }
      if ($windowSeconds -eq 0) {
        foreach ($candidate in $evidenceRows) {
          Write-Host ("SYNTHETIC_BPM_CANDIDATE id={0} rank={1} bpm={2} evidence={3} support={4} winner={5} local_exact={6}" -f
            $label.Substring(10),$candidate.rank,$candidate.bpm,$candidate.evidence,
            $candidate.support,$candidate.winner,$candidate.local_exact)
        }
      } elseif ($offsetPct -gt 0) {
        foreach ($candidate in $evidenceRows) {
          Write-Host ("TEMPORAL_CANDIDATE id={0} offset={1} rank={2} bpm={3} local_exact={4} support={5}" -f
            $label.Substring(19),$offsetPct,$candidate.rank,$candidate.bpm,
            $candidate.local_exact,$candidate.support)
        }
      }
      Write-Host ("SYNTHETIC_BPM_CANDIDATE_COUNT id={0} n={1}" -f
        $label.Substring(10),$evidenceRows.Count)
    }
    if ($windowSeconds -gt 0) {
      $planFile=$inputFile+'.plan.csv'
      if (-not (Test-Path -LiteralPath $planFile -PathType Leaf)) {
        throw "Missing actual host window geometry for $label"
      }
      $plan=@(Import-Csv -LiteralPath $planFile)
      if ($plan.Count -ne 1 -or
          [int]$plan[0].requested_seconds -ne $windowSeconds -or
          [int]$plan[0].requested_passes -ne $windowPasses -or
          [double]$plan[0].effective_seconds -le 0 -or
          [double]$plan[0].effective_seconds -gt $windowSeconds -or
          [int]$plan[0].effective_passes -le 0 -or
          [int]$plan[0].effective_passes -gt $windowPasses -or
          (($offsetPct -ne 0) -and
           ([int]$plan[0].offset_min_pct -ne $offsetPct -or
            [int]$plan[0].offset_max_pct -ne $offsetPct)) -or
          (($offsetPct -eq 0) -and
           ([int]$plan[0].offset_min_pct -ne 20 -or
            [int]$plan[0].offset_max_pct -ne 80))) {
        throw "Requested/effective window mismatch for $label"
      }
      Write-Host ("WINDOW_HOST_GEOMETRY id={0} requested={1}s/{2} actual={3}s/{4} analysis_ms={5}" -f
        $label,$windowSeconds,$windowPasses,
        $plan[0].effective_seconds,$plan[0].effective_passes,
        $script:LastSyntheticAnalysisMs)
    }
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
  if ($Architecture -eq 'x64') {
    # One public matrix in one CI run. Each individual WAV gets a fresh
    # portable foobar instance because multi-track MIR exceeds the UI timeout.
    # Only after ALL cases finish do we score the full fixed synthetic corpus.
    $oracleCases=@(python ./tools/generate_synthetic_bpm_reference.py --list-cases)
    if ($LASTEXITCODE -ne 0 -or $oracleCases.Count -ne 80 -or
        @($oracleCases | Sort-Object -Unique).Count -ne $oracleCases.Count) {
      throw 'Public synthetic matrix inventory mismatch'
    }
    $referenceCases=[System.Collections.Generic.List[object]]::new()
    $observedRows=[System.Collections.Generic.List[object]]::new()
    $candidateRows=[System.Collections.Generic.List[object]]::new()
    foreach ($caseId in $oracleCases) {
      if ($caseId -cnotmatch '^[a-z0-9_]+$') {
        throw 'Unexpected non-public synthetic case identifier'
      }
      $syntheticRoot=Join-Path $root ('synthetic-oracle-'+$caseId)
      python ./tools/generate_synthetic_bpm_reference.py --output $syntheticRoot --case $caseId
      if ($LASTEXITCODE -ne 0) { throw "Synthetic oracle WAV creation failed for $caseId" }
      $reference=Join-Path $syntheticRoot 'reference.json'
      $cases=@((Get-Content -LiteralPath $reference -Raw | ConvertFrom-Json).cases)
      if ($cases.Count -ne 1 -or $cases[0].id -cne $caseId) {
        throw "Unexpected single-track oracle identity for $caseId"
      }
      $wavFile=Join-Path $syntheticRoot ($caseId+'.wav')
      Run-Case $installer $root $wavFile @($wavFile,$reference) 1 ('SYNTHETIC_'+$caseId)
      $numbers=@(Import-Csv -LiteralPath ($wavFile+'.results.csv'))
      if ($numbers.Count -ne 1 -or $numbers[0].index -cne '0') {
        throw 'Missing or misaligned synthetic oracle observation'
      }
      $referenceCases.Add($cases[0])
      $observedRows.Add([pscustomobject][ordered]@{
        index=($observedRows.Count)
        bpm=$numbers[0].bpm
        held=$numbers[0].held
      })
      # Strictly numeric measured candidates from this *public synthetic*
      # fixture only. The same bounded observer already prints these rows.
      foreach ($candidate in @(Import-Csv -LiteralPath ($wavFile+'.candidates.csv'))) {
        $candidateRows.Add([pscustomobject][ordered]@{
          id=$caseId
          rank=$candidate.rank
          bpm=$candidate.bpm
          evidence=$candidate.evidence
          support=$candidate.support
          winner=$candidate.winner
          local_exact=$candidate.local_exact
        })
      }
    }
    $aggregateReference=Join-Path $root 'synthetic-reference-matrix.json'
    $aggregateObserved=Join-Path $root 'synthetic-observed-matrix.csv'
    [ordered]@{
      schema='synthetic_bpm_reference_v1'
      cases=@($referenceCases.ToArray())
    } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $aggregateReference -Encoding utf8
    $observedRows | Export-Csv -LiteralPath $aggregateObserved -NoTypeInformation -Encoding utf8
    $aggregateCandidates=Join-Path $root 'synthetic-candidates-matrix.csv'
    $candidateRows | Export-Csv -LiteralPath $aggregateCandidates -NoTypeInformation -Encoding utf8
    python ./tools/score_synthetic_bpm_reference.py --manifest $aggregateReference --observed $aggregateObserved --candidates $aggregateCandidates --precision-details
    if ($LASTEXITCODE -ne 0) { throw 'Complete synthetic BPM matrix scoring failed' }
    Write-Host ("SYNTHETIC_REAL_CASE_COUNT_x64={0}" -f $oracleCases.Count)
    Write-Host 'SYNTHETIC_REAL_MATRIX_COMPLETED_x64=PASS'

    # Independent, actual-foobar sweep (not a simulated sampling scheduler):
    # 8 window lengths at 5 passes, 6 pass counts at 20s, excluding the
    # duplicate 20s/5pass cell. Every case uses 120s public generated audio.
    $windowCases=@(python ./tools/generate_synthetic_bpm_reference.py --list-window-cases)
    if ($LASTEXITCODE -ne 0 -or $windowCases.Count -ne 6 -or
        @($windowCases | Sort-Object -Unique).Count -ne 6) {
      throw 'Public long-window corpus inventory mismatch'
    }
    $windowAxes=[System.Collections.Generic.List[object]]::new()
    foreach ($seconds in @(5,10,15,20,30,45,60,90)) {
      $windowAxes.Add([pscustomobject]@{seconds=$seconds;passes=5})
    }
    foreach ($passes in @(1,3,10,20,50)) {
      $windowAxes.Add([pscustomobject]@{seconds=20;passes=$passes})
    }
    if ($windowAxes.Count -ne 13) { throw 'Window/pass experiment is not paired' }
    $windowObserved=[System.Collections.Generic.List[object]]::new()
    foreach ($caseId in $windowCases) {
      if ($caseId -cnotmatch '^window_[a-z0-9_]+$') {
        throw 'Unexpected window-sweep fixture identity'
      }
      $seedRoot=Join-Path $root ('sweep-seed-'+$caseId)
      python ./tools/generate_synthetic_bpm_reference.py --output $seedRoot --window-case $caseId
      if ($LASTEXITCODE -ne 0) { throw "Long synthetic generation failed for $caseId" }
      $seedManifest=Join-Path $seedRoot 'reference.json'
      $seedWave=Join-Path $seedRoot ($caseId+'.wav')
      $seedCases=@((Get-Content -LiteralPath $seedManifest -Raw | ConvertFrom-Json).cases)
      if ($seedCases.Count -ne 1 -or $seedCases[0].id -cne $caseId -or
          [int]$seedCases[0].duration_sec -ne 120 -or
          $seedCases[0].wav_sha256 -cne (Get-FileHash $seedWave -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw "Long synthetic reference invalid for $caseId"
      }
      $truth=$seedCases[0]
      $refBpm=if ($null -eq $truth.reference_bpm) { '' } else { [string]$truth.reference_bpm }
      foreach ($axis in $windowAxes) {
        $seconds=[int]$axis.seconds
        $passes=[int]$axis.passes
        # Unique parent directories guarantee fresh observer sidecars and
        # isolated host profiles; fixture suffix remains strictly allowlisted.
        $variantRoot=Join-Path $root ("sweep-$caseId-w$seconds-p$passes")
        $fixtureRoot=Join-Path $variantRoot ('synthetic-oracle-'+$caseId)
        New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
        $wavFile=Join-Path $fixtureRoot ($caseId+'.wav')
        $manifest=Join-Path $fixtureRoot 'reference.json'
        Copy-Item -LiteralPath $seedWave -Destination $wavFile
        Copy-Item -LiteralPath $seedManifest -Destination $manifest
        $label="SYNTHETIC_WINDOW_${caseId}_w${seconds}_p${passes}"
        Run-Case $installer $root $wavFile @($wavFile,$manifest) 1 $label $seconds $passes
        $observed=@(Import-Csv -LiteralPath ($wavFile+'.results.csv'))
        $plan=@(Import-Csv -LiteralPath ($wavFile+'.plan.csv'))
        if ($observed.Count -ne 1 -or $observed[0].index -cne '0' -or $plan.Count -ne 1) {
          throw "Missing numeric window-sweep observation: $label"
        }
        $windowObserved.Add([pscustomobject][ordered]@{
          id=$caseId
          style=$truth.style
          reference_bpm=$refBpm
          ambiguity=$truth.ambiguity
          seconds=$seconds
          passes=$passes
          effective_seconds=$plan[0].effective_seconds
          effective_passes=$plan[0].effective_passes
          bpm=$observed[0].bpm
          held=$observed[0].held
          analysis_ms=$script:LastSyntheticAnalysisMs
        })
      }
    }
    if ($windowObserved.Count -ne 78) { throw 'Incomplete 6-by-13 host sweep' }
    $windowCsv=Join-Path $root 'window-sweep-observed.csv'
    $windowObserved | Export-Csv -LiteralPath $windowCsv -NoTypeInformation -Encoding utf8
    python ./tools/score_synthetic_window_sweep.py --observed $windowCsv
    if ($LASTEXITCODE -ne 0) { throw 'Measured host window-sweep scoring failed' }
    Write-Host ("SYNTHETIC_REAL_WINDOW_SWEEP_CASE_COUNT_x64={0}" -f $windowObserved.Count)
    Write-Host 'SYNTHETIC_REAL_WINDOW_SWEEP_COMPLETED_x64=PASS'

    # Independent temporal stationarity probe: same 120s public WAV, one
    # 20-second read at 20% (early) vs 80% (late). This is diagnostic
    # evidence for tempo changes; it cannot affect normal MIR selection.
    $temporalRows=[System.Collections.Generic.List[object]]::new()
    $temporalCandidates=[System.Collections.Generic.List[object]]::new()
    foreach ($caseId in $windowCases) {
      $seedRoot=Join-Path $root ('sweep-seed-'+$caseId)
      $seedWave=Join-Path $seedRoot ($caseId+'.wav')
      $seedManifest=Join-Path $seedRoot 'reference.json'
      $truth=@((Get-Content -LiteralPath $seedManifest -Raw | ConvertFrom-Json).cases)[0]
      $referenceBpm=if ($null -eq $truth.reference_bpm) { '' } else { [string]$truth.reference_bpm }
      foreach ($offsetPct in @(20,80)) {
        $phase=if ($offsetPct -eq 20) { 'early' } else { 'late' }
        $variantRoot=Join-Path $root ("phase-$caseId-$phase")
        $fixtureRoot=Join-Path $variantRoot ('synthetic-oracle-'+$caseId)
        New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
        $wavFile=Join-Path $fixtureRoot ($caseId+'.wav')
        $manifest=Join-Path $fixtureRoot 'reference.json'
        Copy-Item -LiteralPath $seedWave -Destination $wavFile
        Copy-Item -LiteralPath $seedManifest -Destination $manifest
        $label="SYNTHETIC_TEMPORAL_${caseId}_${phase}"
        Run-Case $installer $root $wavFile @($wavFile,$manifest) 1 $label 20 1 $offsetPct
        $observed=@(Import-Csv -LiteralPath ($wavFile+'.results.csv'))
        $plan=@(Import-Csv -LiteralPath ($wavFile+'.plan.csv'))
        if ($observed.Count -ne 1 -or $plan.Count -ne 1) {
          throw "Missing independent real-host temporal observation: $label"
        }
        # Preserve the already-emitted, bounded numeric candidate inventory
        # for each independent host phase. No verbose foobar logging, personal
        # media metadata, or reference-guided decision path is involved.
        $phaseEvidence=@(Import-Csv -LiteralPath ($wavFile+'.candidates.csv'))
        if ($phaseEvidence.Count -gt 32) {
          throw "Unbounded phase candidate inventory: $label"
        }
        foreach ($candidate in $phaseEvidence) {
          $temporalCandidates.Add([pscustomobject][ordered]@{
            id=$caseId
            phase=$phase
            rank=$candidate.rank
            bpm=$candidate.bpm
            evidence=$candidate.evidence
            support=$candidate.support
            winner=$candidate.winner
            local_exact=$candidate.local_exact
          })
        }
        $temporalRows.Add([pscustomobject][ordered]@{
          id=$caseId
          style=$truth.style
          reference_bpm=$referenceBpm
          ambiguity=$truth.ambiguity
          phase=$phase
          offset_pct=$offsetPct
          seconds=20
          passes=1
          effective_seconds=$plan[0].effective_seconds
          effective_passes=$plan[0].effective_passes
          bpm=$observed[0].bpm
          held=$observed[0].held
          analysis_ms=$script:LastSyntheticAnalysisMs
        })
      }
    }
    if ($temporalRows.Count -ne 12) { throw 'Incomplete temporal phase-pair grid' }
    $temporalCsv=Join-Path $root 'temporal-sweep-observed.csv'
    $temporalRows | Export-Csv -LiteralPath $temporalCsv -NoTypeInformation -Encoding utf8
    if ($temporalCandidates.Count -eq 0) { throw 'Phase candidate inventory is empty' }
    $temporalCandidateCsv=Join-Path $root 'temporal-sweep-candidates.csv'
    $temporalCandidates | Export-Csv -LiteralPath $temporalCandidateCsv -NoTypeInformation -Encoding utf8
    python ./tools/score_synthetic_temporal_sweep.py --observed $temporalCsv --candidates $temporalCandidateCsv
    if ($LASTEXITCODE -ne 0) { throw 'Temporal phase comparison scoring failed' }
    Write-Host ("SYNTHETIC_REAL_TEMPORAL_CASE_COUNT_x64={0}" -f $temporalRows.Count)
    Write-Host 'SYNTHETIC_REAL_TEMPORAL_COMPLETED_x64=PASS'
    # Prospective synthetic parameter holdout, declared before this run.
    # It is not owner audio or an independent recording-domain validation.
    # The fixed shadow reads only measured candidates after the real host
    # completes; no hypothetical choice reaches production or tag writes.
    $prospectiveIds=@(python ./tools/generate_synthetic_bpm_reference.py --list-prospective-cases)
    if ($LASTEXITCODE -ne 0 -or $prospectiveIds.Count -ne 20 -or
        @($prospectiveIds | Sort-Object -Unique).Count -ne 20) {
      throw 'Prospective synthetic fixture inventory mismatch'
    }
    $prospectiveReference=[System.Collections.Generic.List[object]]::new()
    $prospectiveObserved=[System.Collections.Generic.List[object]]::new()
    $prospectiveCandidates=[System.Collections.Generic.List[object]]::new()
    foreach ($caseId in $prospectiveIds) {
      if ($caseId -cnotmatch '^prospective_[a-z0-9_]+$') {
        throw 'Unexpected prospective synthetic fixture identity'
      }
      $fixtureRoot=Join-Path $root ('synthetic-oracle-'+$caseId)
      python ./tools/generate_synthetic_bpm_reference.py --output $fixtureRoot --prospective-case $caseId
      if ($LASTEXITCODE -ne 0) { throw "Prospective WAV generation failed for $caseId" }
      $wavFile=Join-Path $fixtureRoot ($caseId+'.wav')
      $manifest=Join-Path $fixtureRoot 'reference.json'
      $cases=@((Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json).cases)
      if ($cases.Count -ne 1 -or $cases[0].id -cne $caseId -or
          $cases[0].wav_sha256 -cne (Get-FileHash $wavFile -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw "Prospective fixture reference invalid for $caseId"
      }
      Run-Case $installer $root $wavFile @($wavFile,$manifest) 1 ('SYNTHETIC_'+$caseId)
      $result=@(Import-Csv -LiteralPath ($wavFile+'.results.csv'))
      if ($result.Count -ne 1 -or $result[0].index -cne '0') {
        throw "Missing prospective real-host BPM result for $caseId"
      }
      $prospectiveReference.Add($cases[0])
      $prospectiveObserved.Add([pscustomobject][ordered]@{
        index=$prospectiveObserved.Count
        bpm=$result[0].bpm
        held=$result[0].held
      })
      $evidence=@(Import-Csv -LiteralPath ($wavFile+'.candidates.csv'))
      if ($evidence.Count -gt 32) { throw 'Prospective candidate cap exceeded' }
      foreach ($candidate in $evidence) {
        $prospectiveCandidates.Add([pscustomobject][ordered]@{
          id=$caseId
          rank=$candidate.rank
          bpm=$candidate.bpm
          evidence=$candidate.evidence
          support=$candidate.support
          winner=$candidate.winner
          local_exact=$candidate.local_exact
        })
      }
    }
    if ($prospectiveObserved.Count -ne 20 -or $prospectiveCandidates.Count -eq 0) {
      throw 'Incomplete prospective real-host matrix'
    }
    $prospectiveManifest=Join-Path $root 'prospective-reference-matrix.json'
    [ordered]@{
      schema='synthetic_bpm_reference_v1'
      cases=@($prospectiveReference.ToArray())
    } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $prospectiveManifest -Encoding utf8
    $prospectiveCsv=Join-Path $root 'prospective-observed-matrix.csv'
    $prospectiveObserved | Export-Csv -LiteralPath $prospectiveCsv -NoTypeInformation -Encoding utf8
    $prospectiveCandidateCsv=Join-Path $root 'prospective-candidates-matrix.csv'
    $prospectiveCandidates | Export-Csv -LiteralPath $prospectiveCandidateCsv -NoTypeInformation -Encoding utf8
    python ./tools/score_prospective_bpm_reference.py --manifest $prospectiveManifest --observed $prospectiveCsv --candidates $prospectiveCandidateCsv
    if ($LASTEXITCODE -ne 0) { throw 'Prospective real-host BPM scoring failed' }
    Write-Host 'SYNTHETIC_REAL_PROSPECTIVE_CASE_COUNT_x64=20'
    Write-Host 'SYNTHETIC_REAL_PROSPECTIVE_COMPLETED_x64=PASS'
  }
  Write-Host ("SMART_TEMPO_DIAGNOSTIC_RUNTIME_{0}=PASS" -f $Architecture)
} finally { if (Test-Path $root) { Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue } }
