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
# Seven mutations, each aimed at a different check:
#
#   1. index row number     the newest row's count changed -> check 2 (index rows)
#   2. record emptied       9.554's body deleted          -> check 3 (empty record)
#   3. dangling link        link to a file that isn't     -> check 1 (links)
#   4. wrong latest number  INDEX's newest -> 9.500       -> check 4 (latest number)
#   5. flat archive count   archive/*.md row +1           -> check 5 (archive counts)
#   6. announced count      index heading's "N tiao" +1   -> check 2 (heading count)
#   7. stale status block   the handoff's final section   -> check 6 (self-consistency)
#   8. stale image hash     a manufactured .sys + wrong claim -> check 7 (image hash)
#   9. checker-count claim  README's "N / M PASS" vs the runner -> check 8 (checker counts)
#  10. README tree count   hv/ file count +1               -> check 9 (tree counts)
#  11. stale QUICKSTART number   its "latest record" claim -> check 10 (entry-doc number)
#  12. wrong main-doc line count INDEX's "compressed to N" -> check 11 (narrative count)
#  13. duplicate count, 2nd wrong  first correct, second 99 -> check 9 (Matches, not Match)
#
# 5-7 were added in 9.583, after five checks reported ALL GREEN on a tree that
# still carried three wrong numbers. Each of the three new mutators was itself
# wrong on its first run and reported its check "asleep" -- the mutator, not the
# checker, was broken all three times (a lazy regex that matched the FIRST section
# instead of the last; an offset relative to a submatch used against the whole
# file; a replacement aimed at the first number in a block whose prose deliberately
# quotes the old value). That is the same failure mode this file exists to catch,
# one level up: a test that cannot reach the text it claims to mutate.
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

# 9.606: the record log moved to archive/v0.4.2/ when v0.4.3 shipped (docs were
# consolidated into RELEASE_0.4.3.md). The mutations below still target it --
# the log's internal consistency is still checked by docs_check -- so resolve the
# path the same way the checker does instead of assuming it sits at docs\ root.
$handoffPath = Join-Path $copy 'YUANMOD_HANDOFF_CURRENT.md'
if (-not (Test-Path -LiteralPath $handoffPath)) {
    $handoffPath = Join-Path $copy 'archive/v0.4.2/YUANMOD_HANDOFF_CURRENT.md'
}
if (-not (Test-Path -LiteralPath $handoffPath)) { throw "selftest: cannot find the record log in the copy ($handoffPath)" }
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
    $m = [regex]::Match($t, '\|\s*(9\.[56][0-9]{2}[^|]*)\|\s*([0-9]+)\s*\|')
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
    # THE MAXIMUM, not the first match. The checker compares the HIGHEST number it
    # can find in INDEX against the highest record heading, so a mutation has to
    # remove every occurrence of that maximum -- replacing some other 9.5xx number
    # leaves the real claim standing and the check reports "asleep" while being
    # perfectly awake. Measured 9.586: the earlier version took the FIRST match
    # (9.583, a changelog line) and INDEX meanwhile carried 9.585 in two places, so
    # the mutation never touched the number under test.
    $idxNums = @([regex]::Matches($t, '9\.[0-9]{3}') | ForEach-Object { $_.Value })
    if ($idxNums.Count -eq 0) { throw 'selftest: cannot locate a 9.xxx number in INDEX.md to mutate' }
    $newest = ($idxNums | Sort-Object { [double]$_.Substring(2) })[-1]
    $idxStale = '9.' + ([double]$newest.Substring(2) - 1)
    # String.Replace (not -replace) replaces EVERY occurrence, which is the point.
    $t2 = $t.Replace($newest, $idxStale)
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

    # 6. the FLAT archive row (9.583)
    #
    # archive/*.md is counted non-recursively on purpose: recursing would fold
    # archive/2026-11-08/ and archive/design/ into it (they have their own rows).
    # A count check with no mutation behind it is a check nobody has ever seen
    # work, so plant a wrong number in that exact row and require it to be caught.
    # If the row is ever counted recursively the real value jumps to 47 and this
    # mutation stops landing -- which is itself the signal that the scoping broke.
    $t = ReadText $indexPath
    $row = [regex]::Match($t, '\[`archive/\*\.md`\]\([^)]*\)\s*\|\s*\*\*([0-9]+)\*\*')
    if (-not $row.Success) { throw 'selftest: cannot locate the archive/*.md count row to mutate' }
    $wrong = [int]$row.Groups[1].Value + 1
    $t2 = $t.Substring(0, $row.Groups[1].Index) + "$wrong" + $t.Substring($row.Groups[1].Index + $row.Groups[1].Length)
    WriteText $indexPath $t2
    Assert-Caught 'flat archive count' 'archive/*.md' (Invoke-Check)
    Reset-Copy

    # 7. the record count ANNOUNCED by the index heading (9.583)
    #
    # The heading of the index section prints how many records follow. It said 68
    # while the table held 69: the ROWS were checked, the number the heading
    # announced was not. Mutate that number -- the heading's own trailing digits,
    # whatever they currently are -- and require the checker to notice.
    $t = ReadText $handoffPath
    # The window must be wide enough for the blockquote and blank lines between the
    # heading and the table, and the row must be a THREE-DIGIT record key: the
    # '## bianhao tixi' section also carries '| 9.1 - 9.499 |', which a looser
    # '9\.' matched, and that heading has no trailing count (measured, first run).
    $head = [regex]::Match($t, '(?m)^##[^\n]*\n(?:[^\n]*\n){0,15}?\|\s*9\.[0-9]{3}')
    if (-not $head.Success) { throw 'selftest: cannot locate the handoff index heading to mutate' }
    $line = [regex]::Match($head.Value, '(?m)^##[^\n]*$')
    $digits = [regex]::Match($line.Value, '([0-9]+)[^0-9]*$')
    if (-not $digits.Success) { throw 'selftest: the index heading carries no trailing count' }
    $wrong = [int]$digits.Groups[1].Value + 1
    $newLine = $line.Value.Substring(0, $digits.Groups[1].Index) + "$wrong" + $line.Value.Substring($digits.Groups[1].Index + $digits.Groups[1].Length)
    # $line.Index is relative to $head.Value -- the FIRST version used it directly
    # on $t and spliced the heading into the wrong offset (measured: the mutation
    # landed in the middle of a record, the heading kept its count, and the check
    # was reported asleep when it was the mutator that was broken).
    $off = $head.Index + $line.Index
    $t2 = $t.Substring(0, $off) + $newLine + $t.Substring($off + $line.Length)
    WriteText $handoffPath $t2
    Assert-Caught 'announced record count' 'index heading announces' (Invoke-Check)
    Reset-Copy

    # 8. the handoff status block disagreeing with its own body (9.583)
    #
    # The block sat at 9.564 while the body had reached 9.582. Take the LAST
    # record heading's number and write one below it into the block: the block and
    # the body then contradict each other, which check 6 exists to catch.
    $t = ReadText $handoffPath
    $nums = @([regex]::Matches($t, '(?m)^####\s+(9\.[0-9]{3})') | ForEach-Object { $_.Groups[1].Value })
    if ($nums.Count -eq 0) { throw 'selftest: no record headings to derive a stale number from' }
    $highest = ($nums | Sort-Object { [double]$_.Substring(2) })[-1]
    $stale = '9.' + ([double]$highest.Substring(2) - 1)
    # The LAST "## " section. A lazy quantifier here matched from the FIRST one --
    # measured: it started at '## wen dang dao hang', 100 KB before the end, and the
    # mutation landed in a navigation table instead of the status block. The
    # negative lookahead is what makes it the last section rather than the first.
    $block = [regex]::Match($t, '(?s)\n## [^\n]*\n(?:(?!\n## )[^\n]*\n)*\z')
    if (-not $block.Success) { throw 'selftest: cannot locate the final section of the handoff' }
    $blockText = $block.Value
    # The MAXIMUM in the block, not its first number: the block opens with prose
    # that mentions the OLD stale value on purpose ("it once said 9.564"), so
    # replacing the first occurrence left the real claim untouched and the check
    # was reported asleep (measured, second run).
    $n = $null
    foreach ($m in [regex]::Matches($blockText, '9\.[0-9]{3}')) {
        if (-not $n -or [double]$m.Value.Substring(2) -gt [double]$n.Value.Substring(2)) { $n = $m }
    }
    if (-not $n) { throw 'selftest: the final section carries no 9.xxx number' }
    # EVERY occurrence of the maximum, for the same reason as mutation 4: the block
    # may legitimately name the current record more than once, and one surviving
    # copy keeps blockMax at the true value so the check looks asleep while working.
    # Measured 9.586: the block named 9.585 twice ("record count" and "latest
    # number"), the single-site replacement left the second one, and this mutation
    # had been reporting PASS on a working check.
    $newBlock = $blockText.Replace($n.Value, $stale)
    $t2 = $t.Substring(0, $block.Index) + $newBlock
    WriteText $handoffPath $t2
    Assert-Caught 'stale status block' 'status block' (Invoke-Check)
    Reset-Copy

    # 8. the image hash (9.583)
    #
    # Check 7 hashes YuanGuardHV/bin/yuanguard_hv.sys -- which is gitignored, so the
    # throwaway copy has none and the check takes its SKIP branch on every run. A
    # branch nobody has ever entered is a branch nobody knows works, so this
    # mutation MANUFACTURES the file (64 known bytes in the copy's own tree) and
    # then runs the pair:
    #
    #   a. INDEX claims the hash that file really has  -> must PASS
    #   b. INDEX claims a different hash              -> must FAIL
    #
    # (a) is not decoration. The first version of this mutation only did (b) and
    # wrote the CORRECT prefix into INDEX by mistake, so it "passed" -- a mutation
    # that plants the right value and then wonders why nothing was caught.
    $binDir = Join-Path $work 'YuanGuardHV/bin'
    New-Item -ItemType Directory -Path $binDir -Force | Out-Null
    $fakeSys = Join-Path $binDir 'yuanguard_hv.sys'
    [IO.File]::WriteAllBytes($fakeSys, [byte[]](1..64))
    # Same Get-FileHash fallback as docs_check.ps1 (measured 9.584: the build
    # script's child powershell.exe lacked the cmdlet).
    $realHash = $null
    try { $realHash = (Get-FileHash -LiteralPath $fakeSys -Algorithm SHA256).Hash }
    catch {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $realHash = ([BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($fakeSys))) -replace '-', '')
    }

    $t = ReadText $indexPath
    $claim = [regex]::Match($t, '(?m)^.*bin ==.*?([0-9A-Fa-f]{8,64})\s*$')
    if (-not $claim.Success) { throw 'selftest: INDEX carries no image hash line to mutate' }
    $g = $claim.Groups[1]
    $len = $g.Length

    $setClaim = {
        param([string] $Value)
        $cur = ReadText $indexPath
        $m = [regex]::Match($cur, '(?m)^.*bin ==.*?([0-9A-Fa-f]{8,64})\s*$')
        $gg = $m.Groups[1]
        WriteText $indexPath ($cur.Substring(0, $gg.Index) + $Value + $cur.Substring($gg.Index + $gg.Length))
    }

    & $setClaim $realHash.Substring(0, $len)
    $ok = Invoke-Check
    if ($ok.Exit -ne 0) {
        $selfTestFailures.Add("control: INDEX claims the .sys's REAL hash and the checker still failed: $($ok.Out.Trim())")
    }
    else {
        Write-Host '[PASS] self-test: INDEX claiming the real image hash passes'
    }

    & $setClaim 'DEADBEEFDEADBEEF'.Substring(0, [Math]::Min($len, 16))
    Assert-Caught 'stale image hash' 'INDEX.md claims image' (Invoke-Check)

    Remove-Item -LiteralPath $binDir -Recurse -Force -ErrorAction SilentlyContinue
    Reset-Copy

    # 9. the checker-count claim (9.583)
    #
    # Check 8 compares "N scripts / M PASS" in README against tests/check_stats.txt,
    # which run_source_checks.ps1 writes. Every document had that number wrong at
    # some point because it is retyped by hand. This mutation supplies a stats file
    # and plants a wrong PASS count, so the comparison is exercised, not just present.
    #
    # The script count is DERIVED from the stats file this mutation writes, and the
    # README claim is located by that number -- never by a literal. The first version
    # pinned '13' and silently stopped mutating the day the tree reached 15 checkers.
    # The copy holds docs\ only; docs_check looks for README.md BESIDE it (the
    # repository root), so the mutation has to put one there.
    $readme = Join-Path $work 'README.md'
    Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $realDocs) 'README.md') -Destination $readme -Force
    $rt = ReadText $readme

    # TWO DIFFERENT NUMBERS LIVE IN README, and they measure different things:
    #
    #   "15 scripts / 39 PASS"     check 8 -- what the runner reported
    #   "tests/ ... (16 scripts)"  check 9 -- how many .ps1 exist on disk
    #
    # They are not equal and must not be conflated: 16 files, 15 of them registered
    # in run_source_checks.ps1's list. The first version of this mutation used one
    # number for both, so its fixture made check 9 complain and the assertion below
    # was satisfied by the wrong complaint (measured, 9.583).
    # The word between the number and the slash is REQUIRED. Without it this matched
    # README's "G25 ... 25 17/17 ALL PASS" row first (a functional test count) and
    # built the fixture around 25/17 instead of 15/39, so nothing was contradicted
    # and the check was reported asleep. Same trap as check 8 itself.
    $c = [regex]::Match($rt, '([0-9]+)\s+[^\s/0-9]\S*\s*/\s*([0-9]+)\s*[^\s/0-9]\S*\s*PASS')
    if (-not $c.Success) { throw "selftest: README carries no matching scripts/PASS claim to mutate" }
    $claimScripts = [int]$c.Groups[1].Value
    $claimPasses = [int]$c.Groups[2].Value

    $treeCount = $null
    $tm = [regex]::Match($rt, 'tests/\s*[^0-9
]*([0-9]+)')
    if ($tm.Success) { $treeCount = [int]$tm.Groups[1].Value }

    $testsDir = Join-Path $work 'YuanGuardHV/tests'
    New-Item -ItemType Directory -Path $testsDir -Force | Out-Null
    # Same script count as the claim, one fewer PASS -> ONLY check 8 can catch this.
    WriteText (Join-Path $testsDir 'check_stats.txt') "scripts=$claimScripts passes=$($claimPasses - 1) selftests=0`n"
    # Satisfy check 9 separately, using ITS number, so it stays silent and the
    # failure below can only come from check 8.
    if ($treeCount) {
        1..$treeCount | ForEach-Object { WriteText (Join-Path $testsDir "fixture$_.ps1") "# fixture`n" }
    }
    Assert-Caught 'checker-count claim' 'check_stats.txt says' (Invoke-Check)
    Remove-Item -LiteralPath (Join-Path $work 'YuanGuardHV') -Recurse -Force -ErrorAction SilentlyContinue
    Reset-Copy

    # 10. README's repository tree counts (9.583)
    #
    # Same fixture as 9 (README + a manufactured tests/ tree beside the docs copy),
    # aimed at check 9: hv/ and tests/ file counts. Both were wrong in README -- 41
    # vs 40 and 15 vs 14 -- and the first version of check 9 could not even find the
    # line, because it anchored on an ASCII ')' while README writes a fullwidth one.
    $binDir2 = Join-Path $work 'YuanGuardHV'
    New-Item -ItemType Directory -Path (Join-Path $binDir2 'hv') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $binDir2 'tests') -Force | Out-Null
    1..3 | ForEach-Object { WriteText (Join-Path $binDir2 "hv/f$_.c") "x`n" }
    1..2 | ForEach-Object { WriteText (Join-Path $binDir2 "tests/c$_.ps1") "x`n" }
    $readme2 = Join-Path $work 'README.md'
    $rt = ReadText $readme2
    $m2 = [regex]::Match($rt, 'hv/\s*[^0-9
]*([0-9]+)')
    if (-not $m2.Success) { throw 'selftest: README carries no hv/ file count to mutate' }
    $g3 = $m2.Groups[1]
    # The real tree has 3 files, so plant a number that is certainly wrong.
    $rt2 = $rt.Substring(0, $g3.Index) + '99' + $rt.Substring($g3.Index + $g3.Length)
    WriteText $readme2 $rt2
    Assert-Caught 'README tree count' 'hv/ file count' (Invoke-Check)
    Remove-Item -LiteralPath $binDir2 -Recurse -Force -ErrorAction SilentlyContinue
    Reset-Copy
    # 11. QUICKSTART announcing a stale latest number (9.607)
    #
    # Check 4 only ever looked at INDEX.md. QUICKSTART carries the same claim TWICE
    # (the 30-second line and the status table) and both said 9.585/9.588 while the
    # log had reached 9.605 -- in the file a new session reads first. Mutate the
    # number on ONE announcement line and require the new check to catch it.
    #
    # The old value is DERIVED from the log, not written as a literal: a pinned number
    # stops mutating the day the log advances, which is the defect shape this file
    # exists to prevent.
    $qsPath = Join-Path $copy 'QUICKSTART.md'
    if (-not (Test-Path -LiteralPath $qsPath)) { throw 'selftest: cannot find QUICKSTART.md in the copy' }
    $handoffNums = @([regex]::Matches((ReadText $handoffPath), '(?m)^####\s+(9\.[0-9]{3})') | ForEach-Object { $_.Groups[1].Value })
    if ($handoffNums.Count -eq 0) { throw 'selftest: no record headings to derive the current number from' }
    $cur = ($handoffNums | Sort-Object { [double]$_.Substring(2) })[-1]
    $qsText = ReadText $qsPath
    $kLatest = [string]([char]0x6700 + [char]0x65B0 + [char]0x7F16 + [char]0x53F7)
    $qsMatch = [regex]::Match($qsText, [regex]::Escape($kLatest) + '[^0-9' + [char]10 + ']*?(9\.[0-9]{3})')
    if (-not $qsMatch.Success) { throw 'selftest: QUICKSTART announces no latest number to mutate' }
    $qsGroup = $qsMatch.Groups[1]
    $qsStale = '9.' + ([double]$cur.Substring(2) - 7)
    $qsText2 = $qsText.Substring(0, $qsGroup.Index) + $qsStale + $qsText.Substring($qsGroup.Index + $qsGroup.Length)
    WriteText $qsPath $qsText2
    Assert-Caught 'stale QUICKSTART number' 'announces latest number' (Invoke-Check)
    Reset-Copy

    # 12. INDEX stating a wrong line count for the main document (9.607)
    #
    # INDEX said the handoff was compressed to 235 lines; the file is 254. Nothing
    # compared that number to the file, so a reader judging "is this readable" had one
    # unchecked number to go on. Plant +9 on whatever the real line count is.
    $mainDocPath = Join-Path $copy 'RELEASE_0.4.3.md'
    if (-not (Test-Path -LiteralPath $mainDocPath)) { throw 'selftest: cannot find RELEASE_0.4.3.md in the copy' }
    $realLines = @([IO.File]::ReadAllLines($mainDocPath)).Count
    $kSqueeze = [string]([char]0x538B + [char]0x6210)
    $kLines = [string]([char]0x884C)
    $ixText = ReadText $indexPath
    $lineClaim = [regex]::Match($ixText, [regex]::Escape($kSqueeze) + '\s*([0-9]+)\s*' + [regex]::Escape($kLines))
    if (-not $lineClaim.Success) { throw 'selftest: INDEX states no compressed-to-N-lines claim to mutate' }
    $lcGroup = $lineClaim.Groups[1]
    $ixText2 = $ixText.Substring(0, $lcGroup.Index) + ('' + ($realLines + 9)) + $ixText.Substring($lcGroup.Index + $lcGroup.Length)
    WriteText $indexPath $ixText2
    Assert-Caught 'wrong main-doc line count' 'compressed to' (Invoke-Check)
    Reset-Copy    # 13. the tree count stated TWICE, only the SECOND one wrong (9.607)
    #
    # Check 9 used [regex]::Match, which returns ONE match. README states the tests/
    # count twice -- once in the repository tree, once in the testing section -- and
    # one of them said 19 while disk had 20. The check passed for a whole release
    # because it only ever looked at the first line.
    #
    # This mutation is aimed at exactly that: a purpose-built README where the FIRST
    # tests/ count is CORRECT and only the SECOND is wrong. A Match-based checker
    # reports PASS here; only a Matches-based one can catch it. Mutation 10 already
    # covers "notices a wrong number", so this one covers the other axis.
    $dupRoot = Join-Path $work 'YuanGuardHV'
    New-Item -ItemType Directory -Path (Join-Path $dupRoot 'hv') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $dupRoot 'tests') -Force | Out-Null
    1..2 | ForEach-Object { WriteText (Join-Path $dupRoot "hv/d$_.c") "x`n" }
    1..4 | ForEach-Object { WriteText (Join-Path $dupRoot "tests/d$_.ps1") "x`n" }
    $dupReadme = Join-Path $work 'README.md'
    # Built from scratch so the two counts are the ONLY thing under test: a real
    # README here would also drag in check 8 by mentioning run_source_checks.
    $dupBody = @(
        '# fixture',
        '',
        '- hv/   ( 2 files )',
        '- tests/   ( 4 scripts )',
        '',
        '## testing',
        '',
        'Build-time checkers (tests/, 4 scripts)',
        '',
        '- see tests/   ( 99 scripts ) for the full list',
        ''
    ) -join "`n"
    WriteText $dupReadme $dupBody
    Assert-Caught 'tree count wrong in the SECOND statement only' 'claims 99' (Invoke-Check)
    Remove-Item -LiteralPath $dupRoot -Recurse -Force -ErrorAction SilentlyContinue
    Reset-Copy    # 13b. THE CONTROL for the mutation above: both statements correct -> silent.
    #
    # Mutation 13 proves the check can see the SECOND statement. It does not prove
    # the check is not simply failing on any README with two tests/ lines. Fix both
    # to the true count (4 on disk) and require PASS. Without this, a checker that
    # rejects every duplicate would "pass" mutation 13 while being unusable.
    $dupRoot = Join-Path $work 'YuanGuardHV'
    New-Item -ItemType Directory -Path (Join-Path $dupRoot 'hv') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $dupRoot 'tests') -Force | Out-Null
    1..2 | ForEach-Object { WriteText (Join-Path $dupRoot "hv/d$_.c") "x`n" }
    1..4 | ForEach-Object { WriteText (Join-Path $dupRoot "tests/d$_.ps1") "x`n" }
    $dupReadme2 = Join-Path $work 'README.md'
    $dupOk = @(
        '# fixture',
        '',
        '- hv/   ( 2 files )',
        '- tests/   ( 4 scripts )',
        '',
        '## testing',
        '',
        'Build-time checkers (tests/, 4 scripts)',
        '',
        '- see tests/   ( 4 scripts ) for the full list',
        ''
    ) -join "`n"
    WriteText $dupReadme2 $dupOk
    $r13b = Invoke-Check
    if ($r13b.Exit -ne 0) {
        $selfTestFailures.Add("control: two CORRECT duplicate counts were reported as a problem (the check rejects duplicates instead of comparing them): $($r13b.Out.Trim())")
    }
    else {
        Write-Host '[PASS] self-test: two correct duplicate counts stay silent'
    }
    Remove-Item -LiteralPath $dupRoot -Recurse -Force -ErrorAction SilentlyContinue
    Reset-Copy}
finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
    $env:YGHV_DOCS_ROOT = $null
}

if ($selfTestFailures.Count -gt 0) {
    foreach ($f in $selfTestFailures) { Write-Host "[FAIL] $f" }
    throw 'docs_check_selftest: self-test failed'
}

Write-Host '[PASS] docs_check self-test: all 13 planted defects were caught, and the 3 controls (code-span link, real image hash, correct-first-duplicate-count) stayed silent'
