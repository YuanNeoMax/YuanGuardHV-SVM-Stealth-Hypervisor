$ErrorActionPreference = 'Stop'

# docs_check.ps1 -- 9.564
#
# WHY THIS EXISTS
#
# Two rounds of documentation work (9.560-9.562) fixed SIZE and fixed STRUCTURE.
# The tree already carried the lesson "measure before you act, and measure again
# afterwards" -- and still, 9.563 found five stale numbers in INDEX.md on the day
# the index was written:
#
#   latest number        9.561   vs  the handoff's own 9.562 generated / 9.559 last
#   archive\2026-11-08   24      vs  25 files
#   docs\superpowers\    absent  vs  2 files, with no other entry point
#   record 9.559         18 rows  vs  a heading and nothing else
#   QUICKSTART           9.537 / 9.558 in the SAME file; HEAD 31568c8 vs aebcc33
#
# Every one of those is a claim a machine can check. Nobody checked them, because
# the eleven existing checkers all read SOURCES and none read DOCUMENTS. The
# documents were the one artifact in this tree with no gate at all, and they
# drifted exactly the way an ungated artifact drifts.
#
# WHAT IT CHECKS (read-only; no driver, no machine state)
#
#   1. LINK INTEGRITY   every `](relative-path)` under docs\ resolves
#   2. INDEX ROWS       each row's line count equals the record's real span
#   3. EMPTY RECORDS    a `#### 9.x` heading must carry a body
#   4. LATEST NUMBER    INDEX.md's claim == the highest number in the log
#   5. ARCHIVE COUNTS   counts INDEX.md prints == counts on disk
#
# WHY 2 MATTERS MOST: the index only pays for itself if its numbers are real. A
# reader jumps "18 lines down" on a promise the document makes; if that promise
# was never measured they land at the wrong record and conclude they misread the
# file. 9.559 is the proof: an empty shell the index credited with 18 lines.
#
# WHY 5 IS NOT PEDANTIC: 9.500 had the same shape -- four client commands that
# never worked because every checker read a different part of the file and none
# looked at that operand. A count in a document is that kind of value: true when
# measured, fiction when typed.
#
# SELF-TEST lives in docs_check_selftest.ps1 and must plant a real defect.
#
# ENCODING: this file is deliberately ASCII-ONLY. On Windows, powershell.exe
# reads a BOM-less UTF-8 script as ANSI, and a stray backslash in the wrong
# place then reports a parser error hundreds of lines earlier than the real
# problem. Eleven other checkers in this tree are ASCII for the same reason.

$docsRoot = if ($env:YGHV_DOCS_ROOT) { $env:YGHV_DOCS_ROOT } else {
    Join-Path (Split-Path -Parent $PSScriptRoot) '..\docs'
}
$docsRoot = (Resolve-Path -LiteralPath $docsRoot).Path

# 9.606: the numbered record log was ARCHIVED when v0.4.3 shipped -- docs/ was
# consolidated into RELEASE_0.4.3.md (235 lines replacing 4928), because the log
# had grown into the thing a reader could not use. The log itself is still real
# data with real internal consistency, so the checks that read it still run --
# against the archive. Resolving ACTIVE-OR-ARCHIVE rather than deleting the
# checks keeps four gates alive (index rows, empty records, announced count,
# status block) instead of silently retiring them with the file they read.
# A tree that has neither is a SKIP with a printed reason, not a crash: a checker
# that fails on a clean fixture gets deleted within a week (same rule as check 7).
$handoff = Join-Path $docsRoot 'YUANMOD_HANDOFF_CURRENT.md'
if (-not (Test-Path -LiteralPath $handoff)) {
    $handoff = Join-Path $docsRoot 'archive/v0.4.2/YUANMOD_HANDOFF_CURRENT.md'
}
$haveHandoff = Test-Path -LiteralPath $handoff
$index = Join-Path $docsRoot 'INDEX.md'

function ReadUtf8 {
    param([string] $Path)
    return [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
}

$problems = New-Object 'System.Collections.Generic.List[string]'

# ---------------------------------------------------------------- 1. links
$mdFiles = @(Get-ChildItem -LiteralPath $docsRoot -Recurse -File -Filter '*.md')
$linkCount = 0

# Masked prose: fenced blocks and inline code spans are blanked with spaces of the
# SAME LENGTH, so every offset still points at the same place in the original and
# a link's line number can be computed from $text.
#
# This is not hypothetical. 9.564 wrote a literal link-shaped fragment into a
# record while explaining this very check, and the check reported a dangling link
# in the document that documented it. A checker that flags its own documentation
# teaches people to ignore it -- the same reason service_name_check.ps1 skips
# comments even though they legitimately record the old broken spelling.
#
# WHY ' ' * Length AND NOT a character-class replace: the first version used
#   $m.Value -replace '[^<LF>]', ' '
# and it silently ATE 195 characters out of a 5704-character file (measured: the
# masked copy came back shorter), so masked regions overlapped real text and the
# link scanner reported a link that the masking was supposed to have covered.
# Rebuilding by length is exact by construction and cannot drift -- the same
# lesson as `count_as_extent`: do not derive an extent you can measure.
function Get-MaskedProse {
    param([string] $Text)
    $pre = [regex]::Replace($Text, '(?s)```.*?```', { param($m) (' ' * $m.Length) })
    return [regex]::Replace($pre, '`[^`' + [char]10 + ']*`', { param($m) (' ' * $m.Length) })
}

foreach ($f in $mdFiles) {
    $text = ReadUtf8 $f.FullName
    $prose = Get-MaskedProse $text
    foreach ($m in [regex]::Matches($prose, '\]\(([^)#\s]+)')) {
        $target = $m.Groups[1].Value
        if ($target -match '^(https?:|mailto:)') { continue }
        $linkCount++
        $resolved = Join-Path $f.DirectoryName $target
        if (-not (Test-Path -LiteralPath $resolved)) {
            $line = ($text.Substring(0, $m.Index) -split "`n").Count
            $problems.Add("dangling link at $($f.Name):$line -> $target")
        }
    }
}

if (-not $haveHandoff) {
    Write-Host '[SKIP] docs: no record log (active or archive/v0.4.2/), index rows and status block unverified'
}
else {
    # ------------------------------------- 2 and 3. handoff index rows and bodies
    $handoffText = ReadUtf8 $handoff
    $hLines = $handoffText -split "`n"
    $lineCount = $hLines.Count

    $heads = New-Object 'System.Collections.Generic.List[object]'
    for ($i = 0; $i -lt $lineCount; $i++) {
        if ($hLines[$i] -match '^####\s+(9\.[0-9]{3}(?:-9\.[0-9]{3})?)') {
            $heads.Add([pscustomobject]@{ Index = $i; Key = $Matches[1] })
        }
    }

    $spans = @{}
    $bodyCounts = @{}
    for ($k = 0; $k -lt $heads.Count; $k++) {
        $start = $heads[$k].Index
        if ($k + 1 -lt $heads.Count) {
            $end = $heads[$k + 1].Index
        }
        else {
            $j = $start + 1
            while ($j -lt $lineCount -and $hLines[$j] -notmatch '^##[^#]') { $j++ }
            $end = $j
        }
        $key = $heads[$k].Key
        $spans[$key] = $end - $start
        $body = 0
        for ($j = $start + 1; $j -lt $end; $j++) {
            $t = $hLines[$j].Trim()
            if ($t -ne '' -and $t -ne '---') { $body++ }
        }
        $bodyCounts[$key] = $body
    }

    $idxStart = -1
    # The index heading is located STRUCTURALLY, not by its text: it is the first `##`
    # section that is followed by a table whose rows start with a 9.x key. Matching on
    # the Chinese heading would hard-code a translation into a checker, and the first
    # version of this file did exactly that -- with a `\u8BB0\u5F55\u7D22\u5F15`
    # escape inside a SINGLE-quoted PowerShell string, which is not an escape at all.
    # The comparison could never be true, so the branch was dead code that read as if
    # it worked. Structural matching has no such failure mode: rename the heading and
    # this still finds it.
    if ($idxStart -lt 0) {
        # Search forward from each `##` heading for a table row keyed 9.xxx, allowing
        # for the blockquote and blank lines between the heading and the table. The
        # first version only looked 3 lines ahead, which was enough for a heading with
        # no prose under it and not for this file (measured: the table starts 6 lines
        # down, after a two-line blockquote). A guessed window is wrong in BOTH
        # directions -- exactly what count_as_extent.ps1 records about its own first
        # version, whose 5-line window missed every real site.
        for ($i = 0; $i -lt $lineCount - 1; $i++) {
            if ($hLines[$i] -notmatch '^##[^#]') { continue }
            $limit = [Math]::Min($lineCount - 1, $i + 15)
            for ($j = $i + 1; $j -le $limit; $j++) {
                if ($hLines[$j] -match '^\|\s*9\.[0-9]{3}') { $idxStart = $i; break }
                if ($hLines[$j] -match '^##') { break }
            }
            if ($idxStart -ge 0) { break }
        }
    }
    if ($idxStart -lt 0) {
        $problems.Add('handoff: no index section found (no ## heading followed by a 9.x table)')
    }
    else {
        $i = $idxStart
        $seen = New-Object 'System.Collections.Generic.List[string]'
        while (($i + 1) -lt $lineCount -and $hLines[$i + 1] -notmatch '^#{2,4}\s') {
            $i++
            if ($hLines[$i] -match '^\|\s*(9\.[0-9]{3}(?:-9\.[0-9]{3})?)[^|]*\|\s*([0-9]+)\s*\|\s*$') {
                $key = $Matches[1]
                $claimed = [int]$Matches[2]
                $seen.Add($key)
                if (-not $spans.ContainsKey($key)) {
                    $problems.Add("index row $key has no matching record heading")
                }
                elseif ($spans[$key] -ne $claimed) {
                    $problems.Add("index row $key claims $claimed lines, record spans $($spans[$key])")
                }
            }
        }
        if ($seen.Count -ne $heads.Count) {
            $problems.Add("index has $($seen.Count) rows for $($heads.Count) records")
        }
        $missing = @($spans.Keys | Where-Object { $seen -notcontains $_ })
        foreach ($k2 in $missing) { $problems.Add("record $k2 has no index row") }

        # The heading of the index section prints its own record count ("69 tiao").
        # It said 68 while the table held 69 -- the row count was checked, the number
        # the heading ANNOUNCED was not. Same shape as the status block in check 6:
        # a number a human reads, printed by the document, never compared to reality.
        # The heading is Chinese ("... , 69 tiao )"), and this file is ASCII-only, so
        # the pattern is anchored on the DIGITS-BEFORE-THE-CLOSING-PAREN shape rather
        # than on the words. The first version demanded an ASCII ')' and the heading
        # ends with a fullwidth one -- it matched nothing and the check was silent
        # (measured: a copy with 68 planted in it still reported PASS).
        $heading = $hLines[$idxStart]
        $countClaim = [regex]::Match($heading, '([0-9]+)[^0-9]*$')
        if ($countClaim.Success -and [int]$countClaim.Groups[1].Value -ne $heads.Count) {
            $problems.Add("the index heading announces $($countClaim.Groups[1].Value) records, there are $($heads.Count)")
        }
    }

    foreach ($h in $heads) {
        # ZERO non-blank body lines is the defect (9.559 was exactly that: a heading,
        # then `---`). One-liners are legitimate -- compact on purpose -- so the
        # threshold is 0, not a stylistic minimum.
        if ($bodyCounts[$h.Key] -eq 0) {
            $problems.Add("record $($h.Key) has a heading but no body")
        }
    }

    # --------------------------------------------------------- 4. latest number
    #
    # $highest is the highest RECORD -- taken from `#### 9.x` headings only, never from
    # free text. It used to scan the whole file, which made check 6 circular: the status
    # block's own number would define the maximum it was then compared against, so the
    # block could say anything and agree with itself. Measured 9.583.
    $declared = $null
    foreach ($m in [regex]::Matches((ReadUtf8 $index), '9\.[0-9]{3}')) {
        if (-not $declared -or [double]$m.Value.Substring(2) -gt [double]$declared.Substring(2)) {
            $declared = $m.Value
        }
    }
    $highest = $null
    foreach ($h in $heads) {
        if (-not $highest -or [double]$h.Key.Substring(2) -gt [double]$highest.Substring(2)) {
            $highest = $h.Key
        }
    }
    if (-not $declared) { $problems.Add('INDEX.md carries no 9.xxx number at all') }
    elseif ($declared -ne $highest) {
        $problems.Add("INDEX.md highest number is $declared, the log's is $highest")
    }

    # ------------------------------------------------------- 5. archive counts
    $indexText = ReadUtf8 $index
    # Recurse defaults to true; archive/*.md is the flat row and must stay flat,
    # otherwise it would silently start counting archive/2026-11-08/ and
    # archive/design/ as well (they have their own rows).
    $countChecks = @(
        @{ Label = 'archive/2026-11-08/'; Path = 'archive/2026-11-08' },
        @{ Label = 'archive/design/';     Path = 'archive/design' },
        @{ Label = 'superpowers/';        Path = 'superpowers' },
        @{ Label = 'archive/*.md';        Path = 'archive'; Recurse = $false }
    )
    foreach ($c in $countChecks) {
        $dir = Join-Path $docsRoot $c.Path
        if (-not (Test-Path -LiteralPath $dir)) {
            $problems.Add("INDEX.md indexes $($c.Label) but the directory does not exist")
            continue
        }
        $recurse = -not ($c.ContainsKey('Recurse') -and -not $c.Recurse)
        $actual = @(Get-ChildItem -LiteralPath $dir -File -Filter '*.md' -Recurse:$recurse).Count

        # The count is the FIRST line that both mentions this directory and carries a
        # bold number. Later lines point at individual documents inside the directory
        # (the "worth opening" table); those carry no number and must not be mistaken
        # for the authoritative count. Scoping to the first match keeps the two kinds
        # of line apart.
        $claimed = $null
        foreach ($line in ($indexText -split "`n")) {
            if ($line -notmatch [regex]::Escape($c.Label)) { continue }
            $m = [regex]::Match($line, '\*\*([0-9]+)\*\*')
            if ($m.Success) { $claimed = [int]$m.Groups[1].Value; break }
        }
        if (-not $claimed) {
            $problems.Add("INDEX.md states no file count for $($c.Label)")
        }
        elseif ($claimed -ne $actual) {
            $problems.Add("INDEX.md claims $claimed files in $($c.Label), disk has $actual")
        }
    }

    # ------------------------------------------- 6. handoff's own status block
    #
    # The handoff ENDS with a status block that restates the state. It sat at 9.564
    # while the body had already reached 9.582 -- two contradictory statements in ONE
    # file. Check 4 compares INDEX against the log; it cannot see the log disagreeing
    # with itself.
    #
    # Located by STRUCTURE (the last "## " section), not by its Chinese heading: this
    # script is ASCII-only on purpose (see the header), and powershell.exe decodes a
    # BOM-less UTF-8 file as ANSI, so a Chinese literal here is a parser error several
    # lines away from the real cause. Measured, 9.583 -- the first version of this
    # check wrote the heading out and the whole file stopped parsing.
    $sections = $handoffText -split "`n## "
    $lastSection = $sections[$sections.Count - 1]
    $blockMax = $null
    foreach ($m in [regex]::Matches($lastSection, '9\.[0-9]{3}')) {
        if (-not $blockMax -or [double]$m.Value.Substring(2) -gt [double]$blockMax.Substring(2)) {
            $blockMax = $m.Value
        }
    }
    if (-not $blockMax) {
        $problems.Add('the handoff status block carries no 9.xxx number')
    }
    elseif ($blockMax -ne $highest) {
        $problems.Add("the handoff status block tops out at $blockMax, the log's highest record is $highest")
    }

}

# ------------------------------------------------ 7. INDEX's image hash claim
#
# INDEX prints "bin == <deployment> = <hash>". It read 90C6E804994F595C against a
# binary whose real hash was F9A56562... -- stale by a whole rebuild, and it is the
# one line a reader would use to answer "is the driver on disk the one the docs
# describe?". Anchored on the ASCII "bin ==" for the same encoding reason as above.
#
# bin/ is gitignored (the binary is a build artifact), so a fresh checkout has no
# file to hash. That is a SKIP with a printed reason, not a failure: a checker that
# fails on every clean clone gets deleted within a week.
$indexText = ReadUtf8 $index
$imgClaim = [regex]::Match($indexText, '(?m)^.*bin ==.*?([0-9A-Fa-f]{8,64})\s*$')
$sysPath = Join-Path (Split-Path -Parent $docsRoot) 'YuanGuardHV/bin/yuanguard_hv.sys'
if (-not $imgClaim.Success) {
    $problems.Add('INDEX.md states no image hash on its "bin ==" line')
}
elseif (-not (Test-Path -LiteralPath $sysPath)) {
    Write-Host "[SKIP] docs: no bin\yuanguard_hv.sys to hash (fresh checkout), image claim unverified"
}
else {
    $claimed = $imgClaim.Groups[1].Value.ToUpperInvariant()
    # Get-FileHash is not available in every PowerShell host (the build script's
    # child powershell.exe lacked it, measured 9.584). Fall back to .NET's SHA256
    # so the check runs everywhere; a hash check that crashes is worse than one
    # that reports SKIP.
    $realHash = $null
    try {
        $realHash = (Get-FileHash -LiteralPath $sysPath -Algorithm SHA256).Hash
    }
    catch {
        try {
            $sha = [System.Security.Cryptography.SHA256]::Create()
            $bytes = [IO.File]::ReadAllBytes($sysPath)
            $realHash = ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '')
        }
        catch {
            Write-Host "[SKIP] docs: cannot hash $([IO.Path]::GetFileName($sysPath)) (no Get-FileHash), image claim unverified"
            $realHash = $null
        }
    }
    if ($null -eq $realHash) {
        # already reported SKIP above
    }
    elseif (-not $realHash.StartsWith($claimed)) {
        $problems.Add("INDEX.md claims image $claimed, the built .sys is $($realHash.Substring(0, $claimed.Length))")
    }
}

# --------------------------------------------- 8. the checker-count claims
#
# README and QUICKSTART both print "N scripts / M PASS". Both were wrong: they said
# 13/24 while the tree already ran 14/30, because the number is retyped by hand in
# prose every time a checker is added. run_source_checks.ps1 now WRITES the figures
# to tests/check_stats.txt; this compares every claim it can find against that file.
#
# A missing stats file is a SKIP, not a failure: docs_check is itself one of the
# scripts being counted, so it must be able to run standalone (and the self-test
# runs it against a throwaway docs\ with no tests\ beside it at all).
#
# RESOLVED RELATIVE TO THE DOCS ROOT, not to $PSScriptRoot. $PSScriptRoot is the
# checker's own directory -- the REAL repository -- so the first version read the
# real check_stats.txt no matter what YGHV_DOCS_ROOT said. The self-test could then
# plant any lie it liked in its copy and check 8 would compare the real file against
# the real document and pass; the mutation was reported "asleep" while the check was
# in fact reading a file the test never touched (measured, 9.583). A check that
# cannot be pointed at a fixture is a check nobody can test.
# 9.586: DEFERRED BY THE RUNNER. run_source_checks.ps1 sets YGHV_DEFER_DOC_COUNTS while
# it is still measuring, because the counts file it would read here is the PREVIOUS
# run's -- so the run that adds a checker would judge the documents against numbers
# from before that checker existed and pass on documents that are already wrong
# (measured twice in a row, 9.586). The runner clears the flag and re-runs this
# script once the real counts are on disk; that second pass is where check 8 speaks.
# Deferring, not skipping: the runner fails the build if it never gets the second pass.
$statsPath = Join-Path (Split-Path -Parent $docsRoot) 'YuanGuardHV/tests/check_stats.txt'
if ($env:YGHV_DEFER_DOC_COUNTS) {
    Write-Host '[SKIP] docs: checker counts deferred to the runner second pass (YGHV_DEFER_DOC_COUNTS)'
}
elseif (-not (Test-Path -LiteralPath $statsPath)) {
    Write-Host '[SKIP] docs: no tests/check_stats.txt yet (run run_source_checks.ps1), checker counts unverified'
}
else {
    $stats = ReadUtf8 $statsPath
    $sm2 = [regex]::Match($stats, 'scripts=([0-9]+)\s+passes=([0-9]+)')
    if (-not $sm2.Success) {
        $problems.Add("tests/check_stats.txt is malformed: $($stats.Trim())")
    }
    else {
        $realScripts = [int]$sm2.Groups[1].Value
        $realPasses = [int]$sm2.Groups[2].Value
        # Each document is scanned on its own so the message can name the file.
        foreach ($doc in @('README.md', 'docs/QUICKSTART.md')) {
            $p = Join-Path (Split-Path -Parent $docsRoot) $doc
            if (-not (Test-Path -LiteralPath $p)) { continue }
            $txt = ReadUtf8 $p
            $sawClaim = $false
            # The pattern requires a WORD between the two numbers ("N <word> / M
            # PASS"). Without it, QUICKSTART's G25 row "**15/17 PASS (T7/T9 FAIL)**" --
            # a FUNCTIONAL test count -- matches and gets compared against the checker
            # counts. Measured 9.583: that row was only ever "safe" because 15 happened
            # to equal the current script count, which is luck, not scoping.
            # "N <word> / M <word> PASS", where neither word may begin with a digit.
            # That last restriction is what separates this from README's
            # "25 17/17 ALL PASS" row (G25's functional count), which every looser
            # version of this pattern matched first (measured, 9.583).
            foreach ($m in [regex]::Matches($txt, '([0-9]+)\s+[^\s/0-9]\S*\s*/\s*([0-9]+)\s*[^\s/0-9]\S*\s*PASS')) {
                $cs = [int]$m.Groups[1].Value
                $cp = [int]$m.Groups[2].Value
                $sawClaim = $true
                # BOTH numbers are checked. The first version compared only the PASS
                # count, and only when the script count already matched -- so a
                # document claiming the WRONG SCRIPT COUNT was skipped silently, and
                # the self-test's mutation for this check could never fire. That is
                # the same shape as every other defect in this tree: a criterion that
                # only looks at the case it already agrees with.
                if ($cs -ne $realScripts) {
                    $problems.Add("$doc claims $cs scripts, check_stats.txt says $realScripts")
                }
                elseif ($cp -ne $realPasses) {
                    $problems.Add("$doc claims $cp PASS for $cs scripts, check_stats.txt says $realPasses")
                }
            }
            # A document that states checker counts at all must state them in a form
            # this check can read. Silence is not agreement.
            if (-not $sawClaim -and $txt -match 'run_source_checks') {
                $problems.Add("$doc mentions run_source_checks but states no checkable `"N <word> / M PASS`" figure")
            }
        }
    }
}

# ------------------------------------------ 9. README's repository tree counts
#
# The tree in README states file counts that are all retyped by hand and all drifted:
# hv/ said 41 files (40 at the time of the first measurement -- then the check proved
# the DOCUMENT right and my own `ls *.c *.cpp *.h` wrong, it had missed the .hpp),
# tests/ said 15 scripts (14). Each is one Get-ChildItem away from being true, which
# is exactly the kind of number nobody re-measures.
#
# Only counts with an UNAMBIGUOUS source are checked: "N commands" needs the client's
# own table, so that one stays with command_parity.ps1, which already owns it.
#
# A MISSING tree is a SKIP, not a crash. The self-test runs this against a throwaway
# docs\ with no code beside it, and Get-ChildItem on a missing path THREW -- the
# checker died instead of saying "cannot verify", which reads as "docs are broken"
# when the truth is "there is nothing here to compare against".
$repoRoot = Split-Path -Parent $docsRoot
$readmePath = Join-Path $repoRoot 'README.md'
if (-not (Test-Path -LiteralPath $readmePath)) {
    Write-Host '[SKIP] docs: no README.md beside docs/, tree counts unverified'
}
else {
    $rd = ReadUtf8 $readmePath
    $treeChecks = @(
        # Anchored on "label ... ( N" and NOT on the closing paren: README uses a
        # FULLWIDTH paren, so the first version's ASCII ')' matched nothing and the
        # check reported "carries no hv/ file count" on a line that was right there.
        @{ Pattern = 'hv/\s*[^0-9
]*([0-9]+)';   Dir = 'YuanGuardHV/hv';    Filter = '*';    What = 'hv/ file count' },
        @{ Pattern = 'tests/\s*[^0-9
]*([0-9]+)'; Dir = 'YuanGuardHV/tests'; Filter = '*.ps1'; What = 'tests/ checker count' }
    )
    foreach ($tc in $treeChecks) {
        $dir = Join-Path $repoRoot $tc.Dir
        # 9.607: EVERY occurrence, not just the first.
        #
        # This used [regex]::Match, which returns ONE match. README states the
        # tests/ count TWICE -- once in the repository tree, once in the testing
        # section -- and one of them said 19 while disk had 20. The check passed
        # for a whole release because it only ever looked at the first line. A
        # count stated twice in one file is the easiest inconsistency there is,
        # and a checker that reads one of them reports "the document agrees"
        # about a document that disagrees with itself.
        $ms = @([regex]::Matches($rd, $tc.Pattern))
        if ($ms.Count -eq 0) {
            $problems.Add("README.md carries no $($tc.What)")
            continue
        }
        if (-not (Test-Path -LiteralPath $dir)) {
            Write-Host "[SKIP] docs: no $($tc.Dir) beside docs/, $($tc.What) unverified"
            continue
        }
        $actual = @(Get-ChildItem -LiteralPath $dir -File -Filter $tc.Filter |
                Where-Object { $_.Name -notmatch '^run_' }).Count
        foreach ($m in $ms) {
            if ([int]$m.Groups[1].Value -ne $actual) {
                $line = ($rd.Substring(0, $m.Index) -split [char]10).Count
                $problems.Add(("README.md:{0} claims {1} for {2}, disk has {3}" -f $line, $m.Groups[1].Value, $tc.What, $actual))
            }
        }
    }
}

# --------------------------------- 10. the "latest record number" in every entry doc
#
# 9.607. Check 4 compares the INDEX highest 9.xxx against the log. It does not look
# at QUICKSTART.md, and QUICKSTART carries that number TWICE -- in the 30-second
# line and in the status table -- while the log had reached 9.605. Two stale
# numbers, in the file a new session is told to read first, and the checker that
# exists to catch exactly this was pointed at a different file.
#
# The Chinese anchor is built from [char] codes, not written out: this file is
# ASCII-only on purpose (see the header) and powershell.exe decodes a BOM-less
# UTF-8 script as ANSI, so a Chinese literal here is a parser error several lines
# away from the real cause -- measured, 9.583, in this very file.
if ($haveHandoff) {
    $kLatest = [string]([char]0x6700 + [char]0x65B0 + [char]0x7F16 + [char]0x53F7)
    foreach ($doc in @('docs/QUICKSTART.md')) {
        $p = Join-Path (Split-Path -Parent $docsRoot) $doc
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $txt = ReadUtf8 $p
        # Only lines that ANNOUNCE the number, not every 9.xxx mention: QUICKSTART
        # legitimately cites historical records as evidence (9.449, 9.556), and
        # those are not claims about the ceiling. The anchor word separates them.
        $rx = '(?m)^.*' + [regex]::Escape($kLatest) + '[^0-9' + [char]10 + ']*?(9\.[0-9]{3})'
        foreach ($m in [regex]::Matches($txt, $rx)) {
            if ($m.Groups[1].Value -ne $highest) {
                $line = ($txt.Substring(0, $m.Index) -split [char]10).Count
                $problems.Add(("QUICKSTART.md:{0} announces latest number {1}, the log highest is {2}" -f $line, $m.Groups[1].Value, $highest))
            }
        }
    }
}

# ------------------------- 11. a narrative line count must equal the real file
#
# 9.607. INDEX.md said the handoff was "compressed to 235 lines"; the file is 254.
# A reader judging "is this short enough to read" has exactly one number to go on,
# and nothing compared it to the file it describes. Same shape as the index row
# counts in check 2 -- a number in a document about a document -- but this one had
# no gate at all.
$mainDoc = Join-Path $docsRoot 'RELEASE_0.4.3.md'
if (Test-Path -LiteralPath $mainDoc) {
    $realLines = @([IO.File]::ReadAllLines($mainDoc)).Count
    $ix = ReadUtf8 $index
    $kSqueeze = [string]([char]0x538B + [char]0x6210)
    $kLines = [string]([char]0x884C)
    $rx2 = [regex]::Escape($kSqueeze) + '\s*([0-9]+)\s*' + [regex]::Escape($kLines)
    foreach ($m in [regex]::Matches($ix, $rx2)) {
        if ([int]$m.Groups[1].Value -ne $realLines) {
            $line = ($ix.Substring(0, $m.Index) -split [char]10).Count
            $problems.Add(("INDEX.md:{0} says the handoff was compressed to {1} lines, RELEASE_0.4.3.md is {2}" -f $line, $m.Groups[1].Value, $realLines))
        }
    }
}
# ------------------------------------------------------------ report
if ($problems.Count -gt 0) {
    # Write-Output, not Write-Host: the self-test runs this checker as a child
    # process and must read the reason, and Write-Host bypasses stream redirection
    # (measured: the parent saw only the thrown line and reported "failed but not
    # for the reason" four times over -- the checker was right, the harness was
    # blind to why).
    foreach ($p in $problems) { Write-Output "[FAIL] $p" }
    throw "docs_check: $($problems.Count) documentation inconsistency(ies)"
}

if ($haveHandoff) {
    Write-Host ('[PASS] docs: {0} md files, {1} links, {2} records, index rows agree, highest number {3}, archive counts agree' -f `
            $mdFiles.Count, $linkCount, $heads.Count, $highest)
}
else {
    Write-Host ('[PASS] docs: {0} md files, {1} links, archive counts agree (no record log to check)' -f `
            $mdFiles.Count, $linkCount)
}
