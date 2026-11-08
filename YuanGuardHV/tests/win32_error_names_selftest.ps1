$ErrorActionPreference = 'Stop'

# win32_error_names_selftest.ps1 -- 9.583
#
# THE RULE: a checker that cannot fail is worthless. win32_error_names.ps1 exists
# because 9.582 recorded "0x490 ERROR_NOT_READY" and a whole round of investigation
# went down the wrong axis on the strength of that name.
#
# This self-test does not read the checker's code. It plants a mislabelled pair in a
# throwaway copy of docs\ and requires the checker to catch it, then plants a
# CORRECT pair and requires it to stay silent. The second half is not decoration:
# a checker that fails on everything would pass the first half.
#
# The mutations:
#
#   1. wrong name on the number        "0x490 ERROR_NOT_READY" (no [HIST])
#   2. wrong number on the name        "ERROR_NOT_FOUND = 0x15"
#   3. a blind checker                 remove every pair -> must FAIL, not pass

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$realDocs = Join-Path (Split-Path -Parent $here) '..\docs'
$realDocs = (Resolve-Path -LiteralPath $realDocs).Path
$checker = Join-Path $here 'win32_error_names.ps1'

if (-not (Test-Path -LiteralPath $checker)) { throw "selftest: cannot find $checker" }

$work = Join-Path ([IO.Path]::GetTempPath()) ("yghv_errname_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$copy = Join-Path $work 'docs'
New-Item -ItemType Directory -Path $copy -Force | Out-Null
Copy-Item -Path $realDocs -Destination $work -Recurse -Force

function ReadText {
    param([string] $Path)
    return [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
}

function WriteText {
    param([string] $Path, [string] $Text)
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}

function Invoke-Check {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $env:YGHV_DOCS_ROOT = $copy
    $out = ''
    $code = 0
    try {
        $out = (& powershell -NoProfile -ExecutionPolicy Bypass -File $checker 2>&1 | Out-String)
        $code = $LASTEXITCODE
    }
    catch {
        $out = "$_"
        $code = 1
    }
    finally {
        $ErrorActionPreference = $saved
        $env:YGHV_DOCS_ROOT = $null
    }
    return @{ Exit = $code; Out = $out }
}

function Reset-Copy {
    Remove-Item -LiteralPath $copy -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -Path $realDocs -Destination $work -Recurse -Force
}

# Writes a line into a fresh scratch document inside the copy, so the mutation cannot
# be affected by anything already in the real files (and cannot leave residue).
function Set-Probe {
    param([string] $Line)
    $probe = Join-Path $copy 'ZZ_SELFTEST_PROBE.md'
    WriteText $probe "# probe`n`n$Line`n"
}

$selfTestFailures = New-Object 'System.Collections.Generic.List[string]'

function Assert-Caught {
    param([string] $Name, [string] $Expect, [hashtable] $Result)
    if ($Result.Exit -eq 0) {
        $selfTestFailures.Add("mutation '$Name' PASSED - that check is asleep")
        return
    }
    if ($Result.Out -notmatch [regex]::Escape($Expect)) {
        $selfTestFailures.Add("mutation '$Name' failed but not for the reason (wanted '$Expect'); child said: $($Result.Out.Trim())")
        return
    }
    Write-Host "[PASS] self-test: mutation '$Name' was caught"
}

try {
    # 0. the untampered copy must PASS, or nothing below proves anything
    $baseline = Invoke-Check
    if ($baseline.Exit -ne 0) {
        $selfTestFailures.Add("the unmodified copy FAILED: $($baseline.Out.Trim())")
    }
    else {
        Write-Host '[PASS] self-test: unmodified copy passes'
    }

    # 1. wrong NAME for a number
    Set-Probe 'Failure path: 0x490 ERROR_NOT_READY was returned.'
    Assert-Caught 'wrong name for number' 'but ERROR_NOT_READY is' (Invoke-Check)
    Reset-Copy

    # 2. wrong NUMBER for a name (the reverse direction)
    Set-Probe 'The constant ERROR_NOT_FOUND = 0x15 in this build.'
    Assert-Caught 'wrong number for name' 'but ERROR_NOT_FOUND is' (Invoke-Check)
    Reset-Copy

    # 3. THE CONTROL: a correct pair must stay silent. Without this, a checker that
    #    fails on every input would pass 1 and 2 and look perfect.
    Set-Probe 'Verified: 0x490 ERROR_NOT_FOUND is what the driver returns.'
    $r = Invoke-Check
    if ($r.Exit -ne 0) {
        $selfTestFailures.Add("control: a CORRECT pair was reported as wrong: $($r.Out.Trim())")
    }
    else {
        Write-Host '[PASS] self-test: a correct number/name pair stays silent'
    }
    Reset-Copy

    # 4. a [HIST] citation of the old wrong pairing must stay silent too -- otherwise
    #    the correction cannot quote the mistake it corrects.
    Set-Probe 'Recorded at the time: 0x490 ERROR_NOT_READY [HIST] and it was wrong.'
    $r = Invoke-Check
    if ($r.Exit -ne 0) {
        $selfTestFailures.Add("control: a [HIST] citation was reported as wrong: $($r.Out.Trim())")
    }
    else {
        Write-Host '[PASS] self-test: a [HIST] citation stays silent'
    }
    Reset-Copy

    # 5. BLINDNESS: strip every pair from the copy and require FAIL. A checker whose
    #    regex stops matching reports PASS forever; the only way to see that is to
    #    take away everything it could have matched and demand a complaint.
    Remove-Item -LiteralPath $copy -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $copy -Force | Out-Null
    WriteText (Join-Path $copy 'ONLY.md') "# nothing to check`n`nNo error codes here.`n"
    Assert-Caught 'blind checker' 'this check is blind' (Invoke-Check)
}
finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
    $env:YGHV_DOCS_ROOT = $null
}

if ($selfTestFailures.Count -gt 0) {
    foreach ($f in $selfTestFailures) { Write-Host "[FAIL] $f" }
    throw 'win32_error_names_selftest: self-test failed'
}

Write-Host '[PASS] win32_error_names self-test: 2 planted mislabels caught, 3 controls silent (correct pair, [HIST] quote, and the blindness guard)'
