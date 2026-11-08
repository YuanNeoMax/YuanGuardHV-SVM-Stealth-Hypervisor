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
    foreach ($m in [regex]::Matches($blockText, '9\.5[0-9]{2}')) {
        if (-not $n -or [double]$m.Value.Substring(2) -gt [double]$n.Value.Substring(2)) { $n = $m }
    }
    if (-not $n) { throw 'selftest: the final section carries no 9.xxx number' }
    $newBlock = $blockText.Substring(0, $n.Index) + $stale + $blockText.Substring($n.Index + $n.Length)
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
    $realHash = (Get-FileHash -LiteralPath $fakeSys -Algorithm SHA256).Hash

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
    # Check 8 compares "N scripts / M PASS" in README and QUICKSTART against
    # tests/check_stats.txt, which run_source_checks.ps1 writes. Both documents said
    # 13/24 while the tree ran 13/29 -- every document had the number wrong, because
    # it is retyped by hand. This mutation supplies a stats file and plants the old
    # wrong claim, so the comparison is exercised rather than merely present.
    $testsDir = Join-Path $work 'YuanGuardHV/tests'
    New-Item -ItemType Directory -Path $testsDir -Force | Out-Null
    WriteText (Join-Path $testsDir 'check_stats.txt') "scripts=13 passes=29 selftests=16`n"
    # The copy holds docs\ only; docs_check looks for README.md BESIDE it (the
    # repository root), so the mutation has to put one there. Without this the
    # mutation threw "no README.md in the copy" -- the check was fine, the fixture
    # was missing.
    $readme = Join-Path $work 'README.md'
    Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $realDocs) 'README.md') -Destination $readme -Force
    $rt = ReadText $readme
    $c = [regex]::Match($rt, '13\s*(?:\S*)\s*/\s*([0-9]+)\s*(?:\S*)\s*PASS')
    if (-not $c.Success) { throw 'selftest: README carries no "13 scripts / N PASS" claim to mutate' }
    $g2 = $c.Groups[1]
    $rt2 = $rt.Substring(0, $g2.Index) + '24' + $rt.Substring($g2.Index + $g2.Length)
    WriteText $readme $rt2
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
}
finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
    $env:YGHV_DOCS_ROOT = $null
}

if ($selfTestFailures.Count -gt 0) {
    foreach ($f in $selfTestFailures) { Write-Host "[FAIL] $f" }
    throw 'docs_check_selftest: self-test failed'
}

Write-Host '[PASS] docs_check self-test: all 10 planted defects were caught, and the 2 controls (code-span link, real image hash) stayed silent'
