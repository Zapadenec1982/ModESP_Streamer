<#
.SYNOPSIS
    Cleanup of the D:\P4_DSP working root. Dry run by default; deletes only with -Force.

.DESCRIPTION
    The root of D:\P4_DSP accumulated
    one-off build logs, soak-run markers and copies of files that are tracked in the
    repository. This script classifies every candidate and prints the verdict. Nothing is
    removed unless -Force is given, and even then only the rows marked DELETE.

    Verdicts
      DELETE    build_*.log, build_done.txt, soak_done.txt, soak_stop.txt, soak_build.log:
                superseded one-off logs/markers of builds that no longer exist.
                testfiles\*.wav: only when MD5-identical to the tracked testfiles\ copy.
                Docs\*: only when byte-identical to a tracked docs\ counterpart.
      REVIEW    Docs\* that differ from, or have no, tracked counterpart: a person decides.
      CUSTOMER  testfiles\bt-test-96kHz-24bit-stereo.wav and any other WAV without a tracked
                counterpart: the customer's decision. Never deleted by this script.

.PARAMETER Root
    The working root to clean. Default D:\P4_DSP.

.PARAMETER Repo
    The player repository that holds the tracked counterparts. Default: the checkout this
    script lives in (tools\..).

.PARAMETER Force
    Delete the DELETE rows. Without it the script only prints and exits 0.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\cleanup_root.ps1           # dry run
    powershell -ExecutionPolicy Bypass -File tools\cleanup_root.ps1 -Force    # delete
#>
[CmdletBinding()]
param(
    [string]$Root = 'D:\P4_DSP',
    [string]$Repo = '',
    [switch]$Force
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

# $PSScriptRoot is still empty while the param() defaults are evaluated, so resolve it here.
if ([string]::IsNullOrEmpty($Repo)) {
    $scriptDir = $PSScriptRoot
    if ([string]::IsNullOrEmpty($scriptDir)) { $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
    $Repo = Split-Path -Parent $scriptDir
}

function Get-Md5([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm MD5).Hash }
function Get-Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }

function Test-SameText([string]$A, [string]$B) {
    # Same bytes after CRLF -> LF. Tells "a copy with other line endings" from "a different
    # document"; the caller decides what that is worth.
    $ta = [System.IO.File]::ReadAllText($A) -replace "`r`n", "`n"
    $tb = [System.IO.File]::ReadAllText($B) -replace "`r`n", "`n"
    return $ta -eq $tb
}

# Everything git tracks in the repository, forward slashes, one call.
$tracked = @(& git -C $Repo ls-files)
if ($tracked.Count -eq 0) { throw "git ls-files returned nothing for $Repo - is it a checkout?" }
function Get-RepoPath([string]$Rel) { return (Join-Path $Repo ($Rel -replace '/', '\')) }

$rows = New-Object System.Collections.Generic.List[object]
function Add-Row([string]$Verdict, [string]$Path, [string]$Reason) {
    $size = 0
    if (Test-Path -LiteralPath $Path) { $size = (Get-Item -LiteralPath $Path).Length }
    $rows.Add([pscustomobject]@{ Verdict = $Verdict; Size = $size; Path = $Path; Reason = $Reason })
}

# 1. Superseded one-off build/soak logs and run markers.
foreach ($pattern in 'build_*.log', 'build_done.txt', 'soak_done.txt', 'soak_stop.txt', 'soak_build.log') {
    foreach ($f in @(Get-ChildItem -LiteralPath $Root -File -Filter $pattern -ErrorAction SilentlyContinue)) {
        Add-Row 'DELETE' $f.FullName 'superseded one-off build/soak log or marker; the build tree it records is gone'
    }
}

# 2. testfiles\*.wav against the tracked testfiles\ of the repository.
$trackedWav = @{}
foreach ($t in @($tracked | Where-Object { $_ -like 'testfiles/*.wav' })) {
    $trackedWav[(Split-Path -Leaf $t)] = Get-RepoPath $t
}
$wavDir = Join-Path $Root 'testfiles'
if (Test-Path -LiteralPath $wavDir) {
    foreach ($f in @(Get-ChildItem -LiteralPath $wavDir -File -Filter '*.wav')) {
        if (-not $trackedWav.ContainsKey($f.Name)) {
            Add-Row 'CUSTOMER' $f.FullName 'no tracked counterpart (unique bench WAV) - customer decision, never deleted here'
        } elseif ((Get-Md5 $f.FullName) -eq (Get-Md5 $trackedWav[$f.Name])) {
            Add-Row 'DELETE' $f.FullName "MD5-identical to tracked testfiles/$($f.Name)"
        } else {
            Add-Row 'CUSTOMER' $f.FullName "same name as tracked testfiles/$($f.Name) but different content - customer decision"
        }
    }
}

# 3. Docs\* against tracked docs\ (same file name, plus known renames).
$aliases = @{ 'P4_PLAYER_CONCEPT.md' = 'docs/CONCEPT.md' }
$docsDir = Join-Path $Root 'Docs'
if (Test-Path -LiteralPath $docsDir) {
    foreach ($f in @(Get-ChildItem -LiteralPath $docsDir -File -Recurse)) {
        $candidates = @($tracked | Where-Object { ($_ -like 'docs/*') -and ((Split-Path -Leaf $_) -eq $f.Name) })
        if ($aliases.ContainsKey($f.Name)) { $candidates += $aliases[$f.Name] }
        $verdict = 'REVIEW'
        $reason = 'no tracked docs/ counterpart with this name - review manually'
        foreach ($rel in $candidates) {
            $c = Get-RepoPath $rel
            if (-not (Test-Path -LiteralPath $c)) { continue }
            if ((Get-Sha $f.FullName) -eq (Get-Sha $c)) {
                $verdict = 'DELETE'; $reason = "byte-identical to tracked $rel"; break
            } elseif (Test-SameText $f.FullName $c) {
                $verdict = 'REVIEW'; $reason = "same text as tracked $rel, only line endings differ - review manually"
            } else {
                $verdict = 'REVIEW'; $reason = "differs from tracked $rel - review manually"
            }
        }
        Add-Row $verdict $f.FullName $reason
    }
}

# Report.
$mode = 'DRY RUN - nothing is deleted; pass -Force to delete the DELETE rows'
if ($Force) { $mode = 'FORCE - deleting the DELETE rows' }
Write-Output "cleanup_root: root=$Root"
Write-Output "cleanup_root: repo=$Repo ($($tracked.Count) tracked files)"
Write-Output "cleanup_root: $mode"
$rows | Sort-Object Verdict, Path |
    Format-Table -AutoSize -Wrap Verdict, @{ n = 'Bytes'; e = { $_.Size }; align = 'right' }, Path, Reason |
    Out-String -Width 220 | Write-Output

function Count-Verdict([string]$V) { return @($rows | Where-Object { $_.Verdict -eq $V }).Count }
$del = @($rows | Where-Object { $_.Verdict -eq 'DELETE' })
$bytes = 0
foreach ($r in $del) { $bytes += $r.Size }
$inv = [System.Globalization.CultureInfo]::InvariantCulture   # "15,781,032", not a locale NBSP
Write-Output ("cleanup_root: {0} DELETE ({1} bytes), {2} REVIEW, {3} CUSTOMER" -f `
    $del.Count, $bytes.ToString('N0', $inv), (Count-Verdict 'REVIEW'), (Count-Verdict 'CUSTOMER'))

if ($Force) {
    foreach ($r in $del) {
        Remove-Item -LiteralPath $r.Path -Force -Confirm:$false
        Write-Output "deleted: $($r.Path)"
    }
    Write-Output ("cleanup_root: deleted {0} file(s), {1} bytes" -f $del.Count, $bytes.ToString('N0', $inv))
}
