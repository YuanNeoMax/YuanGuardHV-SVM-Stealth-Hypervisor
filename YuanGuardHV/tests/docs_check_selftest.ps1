$ErrorActionPreference = 'Stop'

# docs_check_selftest.ps1 -- 9.564
#
# THE RULE THIS ENFORCES: a checker that cannot fail is worthless. Three of the
# eleven checkers in this tree once reported PASS on a source file that had the
# defect planted in it, because their regex could not reach the text they claimed
# to inspect (9.466). So docs_check ships with this file, and it does not inspect
# docs_check's code -- it RUNS it against a throwaway copy of docs\ with real
# defects planted in it, one at a time, and requires each one to be caught.
#
# Four mutations, each aimed at a different check:
#
#   1. index row number   the newest row's count changed -> check 2 (index rows)
#   2. record emptied      9.554's body deleted       -> check 3 (empty record)
#   3. dangling link       link to a file that isn't  -> check 1 (links)
#   4. wrong latest number INDEX's newest -> 9.500    -> check 4 (latest number)
#
# Each mutation runs docs_check as a CHILD PROCESS with YGHV_DOCS_ROOT pointing at
# the copy, so a failure inside docs_check cannot be mistaken for a failure of the
# self-test itself, and the real docs\ is never touched.
#
# A mutation that PASSES is reported as a failure of the self-test, naming which
# check was asleep.

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$realDocs = Join-Path (Split-Path -Parent $here) '..\docs'
$realDocs = (Resolve-Path -LiteralPath $realDocs).Path
$checker = Join-Path $here 'docs_check.ps1'

if (-not (Test-Path -LiteralPath $checker)) { throw "selftest: cannot find $checker" }

$work = Join-Path ([IO.Path]::GetTempPath()) ("yghv_docs_selftest_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$copy = Join-Path $work 'docs'
New-Item -ItemType Directory -Path $copy -Force | Out-Null
Copy-Item -Path $realDocs -Destination $work -Recurse -Force

$handoffPath = Join-Path $copy 'YUANMOD_HANDOFF_CURRENT.md'
$indexPath = Join-Path $copy 'INDEX.md'

function ReadText {
    param([string] $Path)
    return [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
}

function WriteText {
    param([string] $Path, [string] $Text)
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}

# Runs docs_check against the copy. Returns @{ Exit; Out }.
function Invoke-Check {
    # This file runs with $ErrorActionPreference = 'Stop'. That is correct for the
    # mutations themselves, but it silently BREAKS this function: a child process's
    # stderr arrives as an ErrorRecord, and under 'Stop' the FIRST such record
    # terminates the call -- so the parent captured only the child's throw line and
    # reported "failed but not for the reason" for four correct mutations (measured,
    # 9.563). The checker was right; the harness was looking at half its output.
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

# Restores the copy from the real docs, so each mutation starts from a clean tree.
function Reset-Copy {
    Remove-Item -LiteralPath $copy -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -Path $realDocs -Destination $work -Recurse -Force
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
    # 0. the untampered copy must PASS -- otherwise every mutation below is meaningless
    $baseline = Invoke-Check
    if ($baseline.Exit -ne 0) {
        $selfTestFailures.Add("the unmodified copy FAILED, so nothing below proves anything: $($baseline.Out.Trim())")
    }
    else {
        Write-Host '[PASS] self-test: unmodified copy passes'
    }

    # 1. index row count
    # Targets the newest record, whatever its number is: a hard-coded number would
    # silently stop mutating anything the moment a new record was added -- exactly
    # the "test that can no longer fail" shape this file exists to prevent.
    $t = ReadText $handoffPath
    $m = [regex]::Match($t, '\|\s*(9\.5[6-9][0-9][^|]*)\|\s*([0-9]+)\s*\|')
    if (-not $m.Success) { throw 'selftest: cannot locate any 9.5xx index row to mutate' }
    $t2 = $t.Substring(0, $m.Index) + ($m.Value -replace '\|\s*[0-9]+\s*\|', '| 999 |') + $t.Substring($m.Index + $m.Length)
    WriteText $handoffPath $t2
    Assert-Caught 'index row number' 'claims' (Invoke-Check)
    Reset-Copy

    # 2. emptied record
    $t = ReadText $handoffPath
    $start = $t.IndexOf("#### 9.554")
    if ($start -lt 0) { throw 'selftest: cannot locate record 9.554 to empty' }
    $next = $t.IndexOf("#### 9.555", $start)
    if ($next -lt 0) { throw 'selftest: cannot locate the next record after 9.554' }
    $heading = $t.Substring($start, $t.IndexOf("`n", $start) - $start)
    $t2 = $t.Substring(0, $start) + $heading + "`n`n---`n`n" + $t.Substring($next)
    WriteText $handoffPath $t2
    Assert-Caught 'emptied record' 'no body' (Invoke-Check)
    Reset-Copy

    # 3. dangling link
    $t = ReadText $indexPath
    $t2 = $t -replace '\]\(QUICKSTART\.md\)', '](QUICKSTART_MISSING.md)'
    WriteText $indexPath $t2
    Assert-Caught 'dangling link' 'dangling link' (Invoke-Check)
    Reset-Copy

    # 4. wrong latest number in INDEX
    $t = ReadText $indexPath
    $newest = [regex]::Match($t, '9\.5[6-9][0-9]')
    if (-not $newest.Success) { throw 'selftest: cannot locate a 9.5xx number in INDEX.md to mutate' }
    # Every occurrence: replacing only the first would leave the real number
    # somewhere else in the file, and the checker takes the MAXIMUM it finds --
    # so a single-site mutation would prove nothing.
    $t2 = $t -replace [regex]::Escape($newest.Value), '9.500'
    WriteText $indexPath $t2
    Assert-Caught 'stale latest number' 'highest number' (Invoke-Check)
    Reset-Copy

    # 5. THE OTHER DIRECTION -- a fake link inside inline code must stay silent.
    #
    # The checker masks fenced blocks and inline code before scanning for links,
    # because prose ABOUT link syntax is not a link. That masking is itself a way
    # to become blind, so it gets a control: plant a link that does not resolve
    # inside a code span and require the checker to STILL PASS. If this ever fails
    # the masking has eaten a real region of the document.
    $t = ReadText $indexPath
    $t2 = $t + "`n`nThis inline span mentions ``](NO_SUCH_FILE_EVER.md)`` on purpose.`n"
    WriteText $indexPath $t2
    $r = Invoke-Check
    if ($r.Exit -ne 0) {
        $selfTestFailures.Add("control: a link shown inside inline code was reported as dangling (masking is wrong): $($r.Out.Trim())")
    }
    else {
        Write-Host "[PASS] self-test: a link shown inside inline code is correctly ignored"
    }
    Reset-Copy
}
finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
    $env:YGHV_DOCS_ROOT = $null
}

if ($selfTestFailures.Count -gt 0) {
    foreach ($f in $selfTestFailures) { Write-Host "[FAIL] $f" }
    throw 'docs_check_selftest: self-test failed'
}

Write-Host '[PASS] docs_check self-test: all 4 planted defects were caught, and the code-span control stayed silent'
