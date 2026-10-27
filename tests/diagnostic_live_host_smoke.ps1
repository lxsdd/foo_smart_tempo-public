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
$script:pilotEvidence=[Collections.Generic.List[object]]::new()
$script:pilotTraceKeys=@('NATIVE_SOURCE_INVENTORY_VERIFIED','RAW','COLLAPSED','WRITE_GUARD_BLOCKED',
  'PHASE_B_CAUSAL_COMPLETENESS','DECODED_WINDOWS','DECLARED_WINDOWS','WINDOWS_COMPLETE',
  'FINAL_ACCEPT','PROCESS_CPU_SECONDS','PROCESS_PEAK_RSS_BYTES','PRIMARY_VETO_REASON_UNKNOWN',
  'WRITER_RESOLUTION_NATIVE','WRITER_SOURCE_INDEXED','WRITER_SOURCE_UNINDEXED',
  'WRITER_POLICY_APPLIED','WRITER_REASON',
  'INDEXED_NATIVE_FINAL_WITNESS','UNINDEXED_SUPPLEMENTAL_FINAL_WITNESS',
  'SUPPLEMENTAL_SELECTED_SOURCES','POLICY_SUPPLEMENTAL_POINTER_IDENTITY_COMPLETE',
  'NEGATIVE_NO_CANDIDATE_TRACE','SELECTOR_LOW_BESTBY_EPISODES',
  'SELECTOR_COMPARE_LOW_WINS','SELECTOR_COMPARE_LOW_LOSSES')
$script:pilotShadowKeys=@('CANDIDATES','REFERENCE_PRESENT','NATIVE_PRIMARY_CORRECT',
  'NATIVE_FINAL_CORRECT','NATIVE_PRIMARY_SOURCE_INDEXED','REFERENCE_LOST_AT_PRIMARY',
  'MODEL_SCORE_SUM_CORRECT','MODEL_SUPPORT_CORRECT','MODEL_COMBINED_CORRECT',
  'MODEL_SCORE_TIMES_SUPPORT_CORRECT',
  'MODEL_SCORE_SUM_MATCHES_PRIMARY','MODEL_SUPPORT_MATCHES_PRIMARY',
  'MODEL_COMBINED_MATCHES_PRIMARY','MODEL_SCORE_TIMES_SUPPORT_MATCHES_PRIMARY',
  'MODEL_SCORE_SUM_REFERENCE_RANK','MODEL_SUPPORT_REFERENCE_RANK',
  'MODEL_COMBINED_REFERENCE_RANK','MODEL_SCORE_TIMES_SUPPORT_REFERENCE_RANK',
  'MODEL_SCORE_SUM_TOP3_REFERENCE','MODEL_SUPPORT_TOP3_REFERENCE',
  'MODEL_COMBINED_TOP3_REFERENCE','MODEL_SCORE_TIMES_SUPPORT_TOP3_REFERENCE')
$script:pilotScoreKeys=@('EVALUATED','REFERENCE_RHYTHMIC','ACCEPTED_RHYTHMIC',
  'HELD_RHYTHMIC','ACCEPTED_WITHIN_0_05_BPM','ACCEPTED_WITHIN_0_20_BPM',
  'ACCEPTED_OUTSIDE_0_20_BPM','NEGATIVE_TOTAL','NEGATIVE_FALSEACCEPTS',
  'HALF_DOUBLE_ALIAS','CORRECT_ABSTENTION')
function Run-Case([string]$installer,[string]$root,[string]$inputFile,[string[]]$files,[int]$minimum,[string]$label,[switch]$RequireLowPulseCoverage) {
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
    $env:SMART_TEMPO_CI_FIXTURE=$inputFile
    try { $hostProcess=Launch $exe @() }
    finally { Remove-Item Env:SMART_TEMPO_CI_FIXTURE -ErrorAction SilentlyContinue }
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
    # Generated ONLY by the original in-host CI-only direct selector probe,
    # which proves the executed five-field winner and the opposite-center
    # loser on the same fabricated candidate board (no decoded audio).
    $nativeProof=$inputFile+'.native-continuous'
    if (-not (Test-Path -LiteralPath $nativeProof -PathType Leaf) -or
        (Get-Content -LiteralPath $nativeProof -Raw) -cne
        "SMART_TEMPO_CI_NATIVE_CONTINUOUS=PASS`n") {
      throw 'Original host did not prove native five-field continuous comparator'
    }
    Write-Host ("SMART_TEMPO_NATIVE_CONTINUOUS_{0}_{1}=PASS" -f $Architecture,$label)
    # Same genuine CI-host C++ Policy selector and writer-invariant path,
    # exercised on a fabricated equal-BPM two-object TopK board. The
    # independent negative controls run before this marker is created.
    # This is NOT a decoded-original-audio supplemental selection.
    $supplementalProof=$inputFile+'.native-supplemental'
    if (-not (Test-Path -LiteralPath $supplementalProof -PathType Leaf) -or
        (Get-Content -LiteralPath $supplementalProof -Raw) -cne
        "SMART_TEMPO_CI_NATIVE_SUPPLEMENTAL=PASS`n") {
      throw 'Original host did not prove native supplemental object and writer guard'
    }
    Write-Host ("SMART_TEMPO_NATIVE_SUPPLEMENTAL_{0}_{1}=PASS" -f $Architecture,$label)
    # Independently qualified raw alias + raw support fallback decisions.
    # Three separate *native* selector calls use CI-only fabricated objects.
    $rawProof=$inputFile+'.native-raw-fallback'
    if (-not (Test-Path -LiteralPath $rawProof -PathType Leaf) -or
        (Get-Content -LiteralPath $rawProof -Raw) -cne
        "SMART_TEMPO_CI_NATIVE_RAW_FALLBACK=PASS`n") {
      throw 'Original host did not prove native raw alias/support fallback sequence'
    }
    Write-Host ("SMART_TEMPO_NATIVE_RAW_FALLBACK_{0}_{1}=PASS" -f $Architecture,$label)
    # Four original C++ General-144 native selector branches: promote, retain,
    # no eligible challenger and non-144 bypass. Fabricated board only.
    $generalAltProof=$inputFile+'.native-general-alt'
    if (-not (Test-Path -LiteralPath $generalAltProof -PathType Leaf) -or
        (Get-Content -LiteralPath $generalAltProof -Raw) -cne
        "SMART_TEMPO_CI_NATIVE_GENERAL_ALT=PASS`n") {
      throw 'Original host did not prove General-144 source-indexed promotion'
    }
    Write-Host ("SMART_TEMPO_NATIVE_GENERAL_ALT_{0}_{1}=PASS" -f $Architecture,$label)
    # Source-ID exact original selector General/Harmonic rail + fallback.
    $railProof=$inputFile+'.native-cluster-rail'
    if (-not (Test-Path -LiteralPath $railProof -PathType Leaf) -or
        (Get-Content -LiteralPath $railProof -Raw) -cne
        "SMART_TEMPO_CI_NATIVE_RAIL=PASS`n") {
      throw 'Original host did not prove native General/Harmonic rail disposition'
    }
    Write-Host ("SMART_TEMPO_NATIVE_RAIL_{0}_{1}=PASS" -f $Architecture,$label)
    # Native branch-local upper-pool veto, including the no-upper fallback.
    $upperProof=$inputFile+'.native-low-upper'
    if (-not (Test-Path -LiteralPath $upperProof -PathType Leaf) -or
        (Get-Content -LiteralPath $upperProof -Raw) -cne
        "SMART_TEMPO_CI_NATIVE_LOW_UPPER=PASS`n") {
      throw 'Original host did not prove low upper branch veto and continuation'
    }
    Write-Host ("SMART_TEMPO_NATIVE_LOW_UPPER_{0}_{1}=PASS" -f $Architecture,$label)
    # Additional positive/negative in-process low bestBy sequence, 60->80->71.
    # This is a separate synthetic board, NOT decoded-audio BPM quality.
    $nestedProof=$inputFile+'.native-low-nested'
    if (-not (Test-Path -LiteralPath $nestedProof -PathType Leaf) -or
        (Get-Content -LiteralPath $nestedProof -Raw) -cne
        "SMART_TEMPO_CI_NATIVE_LOW_NESTED=PASS`n") {
      throw 'Original foobar host did not prove native nested low rescue'
    }
    Write-Host ("SMART_TEMPO_NATIVE_LOW_NESTED_{0}_{1}=PASS" -f $Architecture,$label)
    $rows=[STUI]::Rows($dialog)
    if ($rows -lt $minimum) { throw "Only $rows result rows for $label; expected $minimum" }
    if (-not [STUI]::ButtonDisabled($dialog)) { throw 'Diagnostic Update control enabled' }
    foreach ($file in $files) { if ((Stamp $file) -cne $baseline[$file]) { throw 'File changed during analysis' } }
    [STUI]::ProbeBlockedUpdate($dialog)
    Start-Sleep -Seconds 2
    if (-not [STUI]::IsWindow($dialog)) { throw 'Secondary no-write guard failed' }
    foreach ($file in $files) { if ((Stamp $file) -cne $baseline[$file]) { throw 'File changed after blocked Update handler' } }
    if ($label.StartsWith('SYNTHETIC_')) {
      # Compiled only in CI probe binaries; the engine values are captured
      # after worker join, not scraped through unreliable headless UIA.
      $observed=$inputFile+'.results.csv'
      $reference=Join-Path (Split-Path -Parent $inputFile) 'reference.json'
      if (-not (Test-Path -LiteralPath $observed -PathType Leaf)) {
        throw 'Real foobar numeric BPM oracle observer is not available'
      }
      $scoreOutput=@(python ./tools/score_synthetic_bpm_reference.py --manifest $reference --observed $observed 2>&1)
      if ($LASTEXITCODE -ne 0) { throw ('Real foobar synthetic BPM evaluation failed: '+($scoreOutput -join '; ')) }
      $scoreOutput | ForEach-Object { Write-Host $_ }
      $scoreEvidence=@{}
      foreach ($line in $scoreOutput) {
        if ([string]$line -cmatch '^SYNTHETIC_BPM_([A-Z0-9_]+)=([0-9]+)$' -and
            $Matches[1] -cin $script:pilotScoreKeys) {
          $key=$Matches[1]
          if ($scoreEvidence.ContainsKey($key)) { throw 'Duplicate scorer key' }
          $scoreEvidence[$key]=[int]$Matches[2]
        }
      }
      if ($scoreEvidence.Count -ne $script:pilotScoreKeys.Count) {
        throw 'Incomplete native pilot scorer evidence'
      }
      $trace=$inputFile+'.pipeline.jsonl'
      if ($label -eq 'SYNTHETIC_silence' -and
          -not ($scoreOutput -contains 'SYNTHETIC_BPM_NEGATIVE_FALSEACCEPTS=0')) {
        throw 'Safety negative control auto-accepted an unmeasured BPM'
      }
      if (-not (Test-Path -LiteralPath $trace -PathType Leaf)) {
        throw "Missing native B-pilot lineage for $label"
      }
      $traceOutput=@(python ./tools/audit_smart_tempo_ci_pipeline_trace.py --trace $trace --require-windows 2>&1)
      if ($LASTEXITCODE -ne 0) { throw ('Real foobar native BPM lineage contract failed: '+($traceOutput -join '; ')) }
      if (-not ($traceOutput -contains 'SMART_TEMPO_TRACE_WRITER_RESOLUTION_NATIVE=1')) {
        throw 'Original foobar native applied-writer decision is not evidenced'
      }
      # CI-only capture is deliberately forced for the fixed synthetic
      # fixtures. A technically green trace with zero rhythmic manual rows
      # would NOT prove the production merge/source-ID path.
      if (-not ($traceOutput -contains 'SMART_TEMPO_TRACE_MANUAL_NATIVE_STATE=REQUESTED_VERIFIED')) {
        throw 'Original host did not execute requested manual-export capture'
      }
      if ($label -cne 'SYNTHETIC_silence') {
        foreach ($key in @('MANUAL_SOURCE_COUNT','MANUAL_EXPORT_COUNT')) {
          $positive=@($traceOutput | Where-Object {
            $_ -cmatch ('^SMART_TEMPO_TRACE_'+$key+'=([1-9][0-9]*)
      # Mutation battery runs on the *actual* native-host trace, not only
      # on a fabricated unit-test event list. It must reject corrupted
      # source IDs, decoder windows, policy/witness and write-guard evidence.
      $mutationOutput=@(python ./tools/audit_smart_tempo_native_mutations.py --trace $trace --require-windows 2>&1)
      $mutationMarker=@($mutationOutput | Where-Object {
        $_ -ceq 'SMART_TEMPO_NATIVE_MUTATION_GATE=PASS'
      })
      $mutationCounts=@($mutationOutput | Where-Object {
        $_ -cmatch '^SMART_TEMPO_NATIVE_MUTATIONS_REJECTED=([0-9]+)$'
      })
      if ($LASTEXITCODE -ne 0 -or $mutationMarker.Count -ne 1 -or
          $mutationCounts.Count -ne 1 -or
          [int]($mutationCounts[0] -replace '^.*=', '') -lt 19) {
        throw ('Original-host adversarial lineage gate failed: '+($mutationOutput -join '; '))
      }
      $mutationOutput | ForEach-Object { Write-Host $_ }
      # Independent post-host recall audit: the predeclared reference BPM is
      # read only by this Python scorer AFTER the native worker has exited.
      # Crucially, neither MIR, Policy nor the host component sees the oracle.
      if (-not $RequireLowPulseCoverage) {
        # The recall scorer is deliberately frozen to exactly the original
        # four cases; the additional low-genre CUE probes branch coverage.
        $recallOutput=@(python ./tools/audit_smart_tempo_b_candidate_recall.py --manifest $reference --observed $observed --trace $trace --case $label.Substring(10) 2>&1)
        if ($LASTEXITCODE -ne 0 -or
            @($recallOutput | Where-Object { $_ -ceq 'SMART_TEMPO_B_RECALL_VERIFIED=1' }).Count -ne 1) {
          throw ('Native source/ground-truth recall audit failed: '+($recallOutput -join '; '))
        }
        # Independent native-ID comparator attribution must complete before
        # any reference-conditioned summary is treated as usable evidence.
        if (@($recallOutput | Where-Object {
            $_ -ceq 'SMART_TEMPO_B_RECALL_PRIMARY_COMPARATOR_PROVENANCE=PASS'
          }).Count -ne 1 -or
          @($recallOutput | Where-Object {
            $_ -cmatch '^SMART_TEMPO_B_RECALL_PRIMARY_COMPARATOR_DIAGNOSIS=(reference_primary_selected|reference_missing_cluster|reference_not_ranked|reference_ranked_lost|reference_won_nonterminal_family|negative_no_candidates)$'
          }).Count -ne 1) {
          throw 'Native reference-to-executed-comparator attribution incomplete'
        }
        $recallOutput | ForEach-Object { Write-Host $_ }
      }
      $traceEvidence=@{}
      foreach ($line in $traceOutput) {
        if ([string]$line -cmatch '^SMART_TEMPO_TRACE_([A-Z0-9_]+)=([A-Za-z0-9_.]+)$' -and
            $Matches[1] -cin $script:pilotTraceKeys) {
          $key=$Matches[1]; $value=$Matches[2]
          if ($traceEvidence.ContainsKey($key)) { throw 'Duplicate trace key' }
          if ($key -in @('PHASE_B_CAUSAL_COMPLETENESS','WRITER_REASON')) {
            $traceEvidence[$key]=$value
          } elseif ($key -in @('PROCESS_CPU_SECONDS','PROCESS_PEAK_RSS_BYTES')) {
            $traceEvidence[$key]=[double]::Parse($value,[Globalization.CultureInfo]::InvariantCulture)
          } elseif ($value -cmatch '^[0-9]+$') {
            $traceEvidence[$key]=[int]$value
          } else { throw 'Malformed trace counter' }
        }
      }
      foreach ($required in @('NATIVE_SOURCE_INVENTORY_VERIFIED','RAW','COLLAPSED',
          'WRITE_GUARD_BLOCKED','PHASE_B_CAUSAL_COMPLETENESS','DECODED_WINDOWS',
          'DECLARED_WINDOWS','WINDOWS_COMPLETE','FINAL_ACCEPT',
          'PROCESS_CPU_SECONDS','PROCESS_PEAK_RSS_BYTES',
          'WRITER_RESOLUTION_NATIVE','WRITER_SOURCE_INDEXED',
          'WRITER_SOURCE_UNINDEXED','WRITER_POLICY_APPLIED','WRITER_REASON')) {
        if (-not $traceEvidence.ContainsKey($required)) {
          throw "Incomplete native pilot trace evidence: $required"
        }
      }
      if ($traceEvidence['WRITER_RESOLUTION_NATIVE'] -ne 1 -or
          ($traceEvidence['WRITER_SOURCE_INDEXED'] +
           $traceEvidence['WRITER_SOURCE_UNINDEXED']) -ne
          $traceEvidence['FINAL_ACCEPT']) {
        throw 'Applied writer identity conservation failed'
      }
      Write-Host ("SMART_TEMPO_NATIVE_WRITER_RESOLUTION_{0}_{1}=PASS" -f $Architecture,$label)
      # Independently replay source-ID comparator fates without oracle labels.
      # The per-source partition must reconcile with the native inventory.
      # This does NOT claim first-decisive-veto completeness.
      $fateOutput=@(python ./tools/audit_smart_tempo_b_candidate_fates.py --trace $trace --require-windows 2>&1)
      if ($LASTEXITCODE -ne 0 -or
          @($fateOutput | Where-Object { $_ -ceq 'SMART_TEMPO_B_FATES_LEDGER=PASS' }).Count -ne 1) {
        throw ('Original-host source-ID candidate-fate ledger failed: '+($fateOutput -join '; '))
      }
      $fateOutput | ForEach-Object { Write-Host $_ }
      $fateCounts=@{}
      foreach ($line in $fateOutput) {
        if ([string]$line -cmatch '^SMART_TEMPO_B_FATES_([A-Z_]+)=([0-9]+)$') {
          if ($fateCounts.ContainsKey($Matches[1])) { throw 'Duplicate candidate-fate counter' }
          $fateCounts[$Matches[1]]=[int]$Matches[2]
        }
      }
      foreach ($required in @('TOTAL','SELECTED','UNSELECTED_POOL_WINNER',
          'DISPLACED_WINNER','COMPARATOR_LOSER','GATE_ONLY','UNVISITED',
          'FIRST_DECISIVE_VETO_UNPROVEN')) {
        if (-not $fateCounts.ContainsKey($required)) {
          throw "Missing source-ID fate counter: $required"
        }
      }
      if ($fateCounts['TOTAL'] -ne $traceEvidence['COLLAPSED'] -or
          $fateCounts['FIRST_DECISIVE_VETO_UNPROVEN'] -ne $traceEvidence['COLLAPSED'] -or
          ($fateCounts['SELECTED']+$fateCounts['UNSELECTED_POOL_WINNER']+
           $fateCounts['DISPLACED_WINNER']+$fateCounts['COMPARATOR_LOSER']+
           $fateCounts['GATE_ONLY']+$fateCounts['UNVISITED']) -ne $fateCounts['TOTAL']) {
        throw 'Source-ID fate conservation or unresolved-veto status contradicted'
      }
      $shadowEvidence=@{}
      if (-not $RequireLowPulseCoverage) {
        # Shadow models run ONLY after actual worker completion, using the
        # frozen case oracle solely to SCORE predictions after selection.
        $shadowOutput=@(python ./tools/audit_smart_tempo_b_shadow_rank.py --manifest $reference --observed $observed --trace $trace --case $label.Substring(10) 2>&1)
        if ($LASTEXITCODE -ne 0 -or
            @($shadowOutput | Where-Object { $_ -ceq 'SMART_TEMPO_B_SHADOW_VERIFIED=1' }).Count -ne 1) {
          throw ('Synthetic source-only shadow audit failed: '+($shadowOutput -join '; '))
        }
        $shadowOutput | ForEach-Object { Write-Host $_ }
        foreach ($line in $shadowOutput) {
          if ([string]$line -cmatch '^SMART_TEMPO_B_SHADOW_([A-Z0-9_]+)=([0-9]+)$' -and
              $Matches[1] -cin $script:pilotShadowKeys) {
            if ($shadowEvidence.ContainsKey($Matches[1])) {
              throw 'Duplicate shadow model evidence'
            }
            $shadowEvidence[$Matches[1]]=[int]$Matches[2]
          }
        }
        if ($shadowEvidence.Count -ne $script:pilotShadowKeys.Count) {
          throw 'Missing independent shadow model counter'
        }
        if ($shadowEvidence['CANDIDATES'] -ne $traceEvidence['COLLAPSED'] -or
            $shadowEvidence['NATIVE_FINAL_CORRECT'] -ne $scoreEvidence['ACCEPTED_WITHIN_0_20_BPM']) {
          throw 'Shadow source cardinality/final BPM diverges from original host'
        }
        Write-Host ("SMART_TEMPO_B_SHADOW_{0}_{1}=PASS" -f $Architecture,$label)
      }
      if ($RequireLowPulseCoverage) {
        # The real CUE -> title-format -> default genre mapper must choose
        # the native low-pulse selector, never a synthetic C++ route override.
        foreach ($key in @('SELECTOR_LOW_BESTBY_EPISODES',
                           'SELECTOR_COMPARE_LOW_WINS',
                           'SELECTOR_COMPARE_LOW_LOSSES')) {
          if (-not $traceEvidence.ContainsKey($key)) {
            throw "Missing native low-pulse evidence: $key"
          }
        }
        $episodes=$traceEvidence['SELECTOR_LOW_BESTBY_EPISODES']
        $wins=$traceEvidence['SELECTOR_COMPARE_LOW_WINS']
        $losses=$traceEvidence['SELECTOR_COMPARE_LOW_LOSSES']
        if ($episodes -lt 1 -or $wins -lt 1 -or ($wins+$losses) -lt 1) {
          throw "Real CUE genre route did not exercise low selector: episodes=$episodes wins=$wins losses=$losses"
        }
        Write-Host ("SMART_TEMPO_NATIVE_LOW_PULSE_{0}_EPISODES={1}" -f $Architecture,$episodes)
        Write-Host ("SMART_TEMPO_NATIVE_LOW_PULSE_{0}_COMPARISONS={1}" -f $Architecture,($wins+$losses))
        Write-Host ("SMART_TEMPO_NATIVE_LOW_PULSE_{0}=PASS" -f $Architecture)
      } else {
        # Historical accuracy cohort remains fixed at exactly four WAVs.
        [void]$script:pilotEvidence.Add([pscustomobject]@{
          case=$label.Substring(10);score=$scoreEvidence;trace=$traceEvidence;
          shadow=$shadowEvidence;media_no_write=$true
        })
        Write-Host ("SMART_TEMPO_NATIVE_B_PILOT_TRACE_{0}_{1}=PASS" -f $Architecture,$label)
      }
      Write-Host 'SMART_TEMPO_PRIMARY_VETO_INDIVIDUAL_REASONS=UNMEASURED'
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
  # Fixed, small Phase-B causal-coverage pilot, NOT the frozen Phase-C
  # campaign. One fresh original foobar process per synthetic WAV/architecture.
  $pilotCases=@('pulse_60','pulse_128','pulse_120_sparse','silence')
  foreach ($caseId in $pilotCases) {
    $syntheticRoot=Join-Path (Join-Path $root ('pilot-'+$caseId)) 'synthetic-oracle'
    python ./tools/generate_synthetic_bpm_reference.py --output $syntheticRoot --case $caseId
    if ($LASTEXITCODE -ne 0) { throw "Synthetic B-pilot WAV creation failed: $caseId" }
    $reference=Join-Path $syntheticRoot 'reference.json'
    $cases=@((Get-Content -LiteralPath $reference -Raw | ConvertFrom-Json).cases)
    if ($cases.Count -ne 1 -or $cases[0].id -cne $caseId) {
      throw "Unexpected B-pilot oracle identity: $caseId"
    }
    $wavFile=Join-Path $syntheticRoot ($caseId+'.wav')
    Run-Case $installer $root $wavFile @($wavFile,$reference) 1 ('SYNTHETIC_'+$caseId)
    if ($caseId -ceq 'pulse_60') {
      # Standard external CUE metadata, using the unchanged default mapper.
      $lowCue=Join-Path $syntheticRoot 'pulse_60_chill.cue'
      if (Test-Path -LiteralPath $lowCue) { throw 'Stale synthetic route CUE' }
      [IO.File]::WriteAllLines($lowCue,@(
        'REM GENRE "Chill"',
        'FILE "pulse_60.wav" WAVE',
        '  TRACK 01 AUDIO',
        '    INDEX 01 00:00:00'
      ),[Text.Encoding]::ASCII)
      Run-Case $installer $root $lowCue @($wavFile,$lowCue,$reference) 1 `
        'SYNTHETIC_pulse_60_chill' -RequireLowPulseCoverage
    }
  }
  # CI-only bounded numeric report; consumed again by cross-architecture gate.
  $pilotReport=Join-Path $env:RUNNER_TEMP ("smart-tempo-b-pilot-"+$Architecture+".json")
  if (Test-Path -LiteralPath $pilotReport) { throw 'Stale pilot report' }
  $sourceSha=((& git rev-parse HEAD) | Out-String).Trim().ToLowerInvariant()
  $payload=[ordered]@{
    schema='smart_tempo_b_pilot_cohort_v1'
    architecture=$Architecture
    source_sha=$sourceSha
    cases=@($script:pilotEvidence.ToArray())
  }
  [IO.File]::WriteAllText($pilotReport,($payload | ConvertTo-Json -Depth 8 -Compress),
                          [Text.UTF8Encoding]::new($false))
  python ./tools/audit_smart_tempo_b_pilot_cohort.py --expected-source-sha $sourceSha --report $pilotReport --single-architecture
  if ($LASTEXITCODE -ne 0) { throw 'Original-host B-pilot cohort integrity failed' }
  Write-Host ("SMART_TEMPO_DIAGNOSTIC_RUNTIME_{0}=PASS" -f $Architecture)
} finally { if (Test-Path $root) { Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue } }
)
          })
          if ($positive.Count -ne 1) {
            throw ('Native rhythmic manual-export path not exercised: '+$key)
          }
        }
      }
      $traceOutput | ForEach-Object { Write-Host $_ }
      # Mutation battery runs on the *actual* native-host trace, not only
      # on a fabricated unit-test event list. It must reject corrupted
      # source IDs, decoder windows, policy/witness and write-guard evidence.
      $mutationOutput=@(python ./tools/audit_smart_tempo_native_mutations.py --trace $trace --require-windows 2>&1)
      $mutationMarker=@($mutationOutput | Where-Object {
        $_ -ceq 'SMART_TEMPO_NATIVE_MUTATION_GATE=PASS'
      })
      $mutationCounts=@($mutationOutput | Where-Object {
        $_ -cmatch '^SMART_TEMPO_NATIVE_MUTATIONS_REJECTED=([0-9]+)$'
      })
      if ($LASTEXITCODE -ne 0 -or $mutationMarker.Count -ne 1 -or
          $mutationCounts.Count -ne 1 -or
          [int]($mutationCounts[0] -replace '^.*=', '') -lt 19) {
        throw ('Original-host adversarial lineage gate failed: '+($mutationOutput -join '; '))
      }
      $mutationOutput | ForEach-Object { Write-Host $_ }
      # Independent post-host recall audit: the predeclared reference BPM is
      # read only by this Python scorer AFTER the native worker has exited.
      # Crucially, neither MIR, Policy nor the host component sees the oracle.
      if (-not $RequireLowPulseCoverage) {
        # The recall scorer is deliberately frozen to exactly the original
        # four cases; the additional low-genre CUE probes branch coverage.
        $recallOutput=@(python ./tools/audit_smart_tempo_b_candidate_recall.py --manifest $reference --observed $observed --trace $trace --case $label.Substring(10) 2>&1)
        if ($LASTEXITCODE -ne 0 -or
            @($recallOutput | Where-Object { $_ -ceq 'SMART_TEMPO_B_RECALL_VERIFIED=1' }).Count -ne 1) {
          throw ('Native source/ground-truth recall audit failed: '+($recallOutput -join '; '))
        }
        # Independent native-ID comparator attribution must complete before
        # any reference-conditioned summary is treated as usable evidence.
        if (@($recallOutput | Where-Object {
            $_ -ceq 'SMART_TEMPO_B_RECALL_PRIMARY_COMPARATOR_PROVENANCE=PASS'
          }).Count -ne 1 -or
          @($recallOutput | Where-Object {
            $_ -cmatch '^SMART_TEMPO_B_RECALL_PRIMARY_COMPARATOR_DIAGNOSIS=(reference_primary_selected|reference_missing_cluster|reference_not_ranked|reference_ranked_lost|reference_won_nonterminal_family|negative_no_candidates)$'
          }).Count -ne 1) {
          throw 'Native reference-to-executed-comparator attribution incomplete'
        }
        $recallOutput | ForEach-Object { Write-Host $_ }
      }
      $traceEvidence=@{}
      foreach ($line in $traceOutput) {
        if ([string]$line -cmatch '^SMART_TEMPO_TRACE_([A-Z0-9_]+)=([A-Za-z0-9_.]+)$' -and
            $Matches[1] -cin $script:pilotTraceKeys) {
          $key=$Matches[1]; $value=$Matches[2]
          if ($traceEvidence.ContainsKey($key)) { throw 'Duplicate trace key' }
          if ($key -in @('PHASE_B_CAUSAL_COMPLETENESS','WRITER_REASON')) {
            $traceEvidence[$key]=$value
          } elseif ($key -in @('PROCESS_CPU_SECONDS','PROCESS_PEAK_RSS_BYTES')) {
            $traceEvidence[$key]=[double]::Parse($value,[Globalization.CultureInfo]::InvariantCulture)
          } elseif ($value -cmatch '^[0-9]+$') {
            $traceEvidence[$key]=[int]$value
          } else { throw 'Malformed trace counter' }
        }
      }
      foreach ($required in @('NATIVE_SOURCE_INVENTORY_VERIFIED','RAW','COLLAPSED',
          'WRITE_GUARD_BLOCKED','PHASE_B_CAUSAL_COMPLETENESS','DECODED_WINDOWS',
          'DECLARED_WINDOWS','WINDOWS_COMPLETE','FINAL_ACCEPT',
          'PROCESS_CPU_SECONDS','PROCESS_PEAK_RSS_BYTES',
          'WRITER_RESOLUTION_NATIVE','WRITER_SOURCE_INDEXED',
          'WRITER_SOURCE_UNINDEXED','WRITER_POLICY_APPLIED','WRITER_REASON')) {
        if (-not $traceEvidence.ContainsKey($required)) {
          throw "Incomplete native pilot trace evidence: $required"
        }
      }
      if ($traceEvidence['WRITER_RESOLUTION_NATIVE'] -ne 1 -or
          ($traceEvidence['WRITER_SOURCE_INDEXED'] +
           $traceEvidence['WRITER_SOURCE_UNINDEXED']) -ne
          $traceEvidence['FINAL_ACCEPT']) {
        throw 'Applied writer identity conservation failed'
      }
      Write-Host ("SMART_TEMPO_NATIVE_WRITER_RESOLUTION_{0}_{1}=PASS" -f $Architecture,$label)
      # Independently replay source-ID comparator fates without oracle labels.
      # The per-source partition must reconcile with the native inventory.
      # This does NOT claim first-decisive-veto completeness.
      $fateOutput=@(python ./tools/audit_smart_tempo_b_candidate_fates.py --trace $trace --require-windows 2>&1)
      if ($LASTEXITCODE -ne 0 -or
          @($fateOutput | Where-Object { $_ -ceq 'SMART_TEMPO_B_FATES_LEDGER=PASS' }).Count -ne 1) {
        throw ('Original-host source-ID candidate-fate ledger failed: '+($fateOutput -join '; '))
      }
      $fateOutput | ForEach-Object { Write-Host $_ }
      $fateCounts=@{}
      foreach ($line in $fateOutput) {
        if ([string]$line -cmatch '^SMART_TEMPO_B_FATES_([A-Z_]+)=([0-9]+)$') {
          if ($fateCounts.ContainsKey($Matches[1])) { throw 'Duplicate candidate-fate counter' }
          $fateCounts[$Matches[1]]=[int]$Matches[2]
        }
      }
      foreach ($required in @('TOTAL','SELECTED','UNSELECTED_POOL_WINNER',
          'DISPLACED_WINNER','COMPARATOR_LOSER','GATE_ONLY','UNVISITED',
          'FIRST_DECISIVE_VETO_UNPROVEN')) {
        if (-not $fateCounts.ContainsKey($required)) {
          throw "Missing source-ID fate counter: $required"
        }
      }
      if ($fateCounts['TOTAL'] -ne $traceEvidence['COLLAPSED'] -or
          $fateCounts['FIRST_DECISIVE_VETO_UNPROVEN'] -ne $traceEvidence['COLLAPSED'] -or
          ($fateCounts['SELECTED']+$fateCounts['UNSELECTED_POOL_WINNER']+
           $fateCounts['DISPLACED_WINNER']+$fateCounts['COMPARATOR_LOSER']+
           $fateCounts['GATE_ONLY']+$fateCounts['UNVISITED']) -ne $fateCounts['TOTAL']) {
        throw 'Source-ID fate conservation or unresolved-veto status contradicted'
      }
      $shadowEvidence=@{}
      if (-not $RequireLowPulseCoverage) {
        # Shadow models run ONLY after actual worker completion, using the
        # frozen case oracle solely to SCORE predictions after selection.
        $shadowOutput=@(python ./tools/audit_smart_tempo_b_shadow_rank.py --manifest $reference --observed $observed --trace $trace --case $label.Substring(10) 2>&1)
        if ($LASTEXITCODE -ne 0 -or
            @($shadowOutput | Where-Object { $_ -ceq 'SMART_TEMPO_B_SHADOW_VERIFIED=1' }).Count -ne 1) {
          throw ('Synthetic source-only shadow audit failed: '+($shadowOutput -join '; '))
        }
        $shadowOutput | ForEach-Object { Write-Host $_ }
        foreach ($line in $shadowOutput) {
          if ([string]$line -cmatch '^SMART_TEMPO_B_SHADOW_([A-Z0-9_]+)=([0-9]+)$' -and
              $Matches[1] -cin $script:pilotShadowKeys) {
            if ($shadowEvidence.ContainsKey($Matches[1])) {
              throw 'Duplicate shadow model evidence'
            }
            $shadowEvidence[$Matches[1]]=[int]$Matches[2]
          }
        }
        if ($shadowEvidence.Count -ne $script:pilotShadowKeys.Count) {
          throw 'Missing independent shadow model counter'
        }
        if ($shadowEvidence['CANDIDATES'] -ne $traceEvidence['COLLAPSED'] -or
            $shadowEvidence['NATIVE_FINAL_CORRECT'] -ne $scoreEvidence['ACCEPTED_WITHIN_0_20_BPM']) {
          throw 'Shadow source cardinality/final BPM diverges from original host'
        }
        Write-Host ("SMART_TEMPO_B_SHADOW_{0}_{1}=PASS" -f $Architecture,$label)
      }
      if ($RequireLowPulseCoverage) {
        # The real CUE -> title-format -> default genre mapper must choose
        # the native low-pulse selector, never a synthetic C++ route override.
        foreach ($key in @('SELECTOR_LOW_BESTBY_EPISODES',
                           'SELECTOR_COMPARE_LOW_WINS',
                           'SELECTOR_COMPARE_LOW_LOSSES')) {
          if (-not $traceEvidence.ContainsKey($key)) {
            throw "Missing native low-pulse evidence: $key"
          }
        }
        $episodes=$traceEvidence['SELECTOR_LOW_BESTBY_EPISODES']
        $wins=$traceEvidence['SELECTOR_COMPARE_LOW_WINS']
        $losses=$traceEvidence['SELECTOR_COMPARE_LOW_LOSSES']
        if ($episodes -lt 1 -or $wins -lt 1 -or ($wins+$losses) -lt 1) {
          throw "Real CUE genre route did not exercise low selector: episodes=$episodes wins=$wins losses=$losses"
        }
        Write-Host ("SMART_TEMPO_NATIVE_LOW_PULSE_{0}_EPISODES={1}" -f $Architecture,$episodes)
        Write-Host ("SMART_TEMPO_NATIVE_LOW_PULSE_{0}_COMPARISONS={1}" -f $Architecture,($wins+$losses))
        Write-Host ("SMART_TEMPO_NATIVE_LOW_PULSE_{0}=PASS" -f $Architecture)
      } else {
        # Historical accuracy cohort remains fixed at exactly four WAVs.
        [void]$script:pilotEvidence.Add([pscustomobject]@{
          case=$label.Substring(10);score=$scoreEvidence;trace=$traceEvidence;
          shadow=$shadowEvidence;media_no_write=$true
        })
        Write-Host ("SMART_TEMPO_NATIVE_B_PILOT_TRACE_{0}_{1}=PASS" -f $Architecture,$label)
      }
      Write-Host 'SMART_TEMPO_PRIMARY_VETO_INDIVIDUAL_REASONS=UNMEASURED'
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
  # Fixed, small Phase-B causal-coverage pilot, NOT the frozen Phase-C
  # campaign. One fresh original foobar process per synthetic WAV/architecture.
  $pilotCases=@('pulse_60','pulse_128','pulse_120_sparse','silence')
  foreach ($caseId in $pilotCases) {
    $syntheticRoot=Join-Path (Join-Path $root ('pilot-'+$caseId)) 'synthetic-oracle'
    python ./tools/generate_synthetic_bpm_reference.py --output $syntheticRoot --case $caseId
    if ($LASTEXITCODE -ne 0) { throw "Synthetic B-pilot WAV creation failed: $caseId" }
    $reference=Join-Path $syntheticRoot 'reference.json'
    $cases=@((Get-Content -LiteralPath $reference -Raw | ConvertFrom-Json).cases)
    if ($cases.Count -ne 1 -or $cases[0].id -cne $caseId) {
      throw "Unexpected B-pilot oracle identity: $caseId"
    }
    $wavFile=Join-Path $syntheticRoot ($caseId+'.wav')
    Run-Case $installer $root $wavFile @($wavFile,$reference) 1 ('SYNTHETIC_'+$caseId)
    if ($caseId -ceq 'pulse_60') {
      # Standard external CUE metadata, using the unchanged default mapper.
      $lowCue=Join-Path $syntheticRoot 'pulse_60_chill.cue'
      if (Test-Path -LiteralPath $lowCue) { throw 'Stale synthetic route CUE' }
      [IO.File]::WriteAllLines($lowCue,@(
        'REM GENRE "Chill"',
        'FILE "pulse_60.wav" WAVE',
        '  TRACK 01 AUDIO',
        '    INDEX 01 00:00:00'
      ),[Text.Encoding]::ASCII)
      Run-Case $installer $root $lowCue @($wavFile,$lowCue,$reference) 1 `
        'SYNTHETIC_pulse_60_chill' -RequireLowPulseCoverage
    }
  }
  # CI-only bounded numeric report; consumed again by cross-architecture gate.
  $pilotReport=Join-Path $env:RUNNER_TEMP ("smart-tempo-b-pilot-"+$Architecture+".json")
  if (Test-Path -LiteralPath $pilotReport) { throw 'Stale pilot report' }
  $sourceSha=((& git rev-parse HEAD) | Out-String).Trim().ToLowerInvariant()
  $payload=[ordered]@{
    schema='smart_tempo_b_pilot_cohort_v1'
    architecture=$Architecture
    source_sha=$sourceSha
    cases=@($script:pilotEvidence.ToArray())
  }
  [IO.File]::WriteAllText($pilotReport,($payload | ConvertTo-Json -Depth 8 -Compress),
                          [Text.UTF8Encoding]::new($false))
  python ./tools/audit_smart_tempo_b_pilot_cohort.py --expected-source-sha $sourceSha --report $pilotReport --single-architecture
  if ($LASTEXITCODE -ne 0) { throw 'Original-host B-pilot cohort integrity failed' }
  Write-Host ("SMART_TEMPO_DIAGNOSTIC_RUNTIME_{0}=PASS" -f $Architecture)
} finally { if (Test-Path $root) { Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue } }
