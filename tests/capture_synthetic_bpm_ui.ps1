#requires -Version 7.2
# CI-only read-only extraction of synthetic analysis ListView cells via UI Automation.
# No cross-process writes/injection and no access to private music.
[CmdletBinding()]
param(
  [Parameter(Mandatory)][long]$DialogHandle,
  [Parameter(Mandatory)][string]$Manifest,
  [Parameter(Mandatory)][string]$OutputCsv
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$manifest = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
if ($manifest.schema -ne 'synthetic_bpm_reference_v1') {
  throw 'Unknown synthetic corpus'
}
$cases = @($manifest.cases)
if ($cases.Count -ne 11) { throw 'Unexpected synthetic case count' }
if (Test-Path -LiteralPath $OutputCsv) { throw 'Oracle CSV already exists' }
$dialog = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]::new($DialogHandle))
if ($null -eq $dialog) { throw 'Diagnostic dialog UI Automation handle missing' }
$listCondition = [System.Windows.Automation.PropertyCondition]::new(
  [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
  [System.Windows.Automation.ControlType]::DataGrid)
$control = $dialog.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $listCondition)
if ($null -eq $control) {
  $listCondition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::List)
  $control = $dialog.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $listCondition)
}
if ($null -eq $control) { throw 'Synthetic result list UI Automation provider missing' }
$grid = $control.GetCurrentPattern([System.Windows.Automation.GridPattern]::Pattern)
if ($null -eq $grid -or $grid.Current.RowCount -ne $cases.Count -or
    $grid.Current.ColumnCount -lt 4) {
  throw 'Synthetic result list grid dimensions unavailable'
}
$observed = @('index,bpm,held')
for ($i = 0; $i -lt $cases.Count; ++$i) {
  # Normal default result layout: Artist, Title, Genre, BPM, Method, Error, Confidence.
  $title = $grid.GetItem($i,1).Current.Name.Trim()
  if ($title -ne $cases[$i].id) {
    throw 'Synthetic result row identity/order mismatch'
  }
  $text = $grid.GetItem($i,3).Current.Name.Trim()
  if ($text -eq 'Review') {
    $observed += "$i,0,1"
  } elseif ($text -match '^([0-9]+(?:[.,][0-9]+)?)$') {
    $bpm = $Matches[1].Replace(',','.')
    $observed += "$i,$bpm,0"
  } else {
    throw 'Unparseable synthetic BPM cell'
  }
}
# Save only synthetic ordinals + numeric results; no audio paths.
[IO.File]::WriteAllLines($OutputCsv, $observed, [Text.UTF8Encoding]::new($false))
Write-Output 'SYNTHETIC_BPM_UIA_EXTRACTION=PASS'
