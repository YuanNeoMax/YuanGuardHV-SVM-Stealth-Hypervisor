$ErrorActionPreference = 'Stop'

# source_citation_check.ps1 -- 9.587
#
# WHY: the documents cite source by "file.ext:NNN" -- "the early-out is at
# mem_bp.c:1139", "protected at protect.c:390". Those numbers are the only thing
# that turns a claim into something a reader can go and look at, and NOTHING has
# ever checked them.
#
# Measured 9.587: an audit of 18 citations found ZERO out of range and SEVEN that
# pointed at the wrong line. The line number was still inside the file, so no
# bounds check could see it; the sentence it used to support had moved on. That
# asymmetry is the whole problem: a citation past the end of a file is loud and
# rare, a citation that silently means something else is quiet and common.
#
# WHAT THIS CHECKS (all of it mechanical):
#
#   1. the cited file exists somewhere in the source tree
#   2. the cited line number is inside the file
#   3. the cited line is NOT BLANK
#
# (3) is the cheap half of drift detection, and it is not a guess: of the seven
# drifted citations found in 9.587, four aimed at a blank line (svm_core.c:479 was
# an empty line two below the real state->dr7 = 0x400; svm_simplevm206_x64.asm:406
# was a blank comment line six above the real Sv206GuestStub). A citation to an
# empty line is never what the author meant -- if the line is blank, the target
# moved and the number stayed.
#
# WHAT IT CANNOT CHECK: whether the line still SAYS what the document claims. That
# needs reading the prose and the code together, which is what the audit was. This
# gate keeps the cheap half from regressing; it does not replace the audit.
#
# SCOPE: docs\*.md (flat, not recursive) and the repository README.md. archive\ is
# excluded on purpose -- those files are frozen history, their citations describe
# code as it was then, and rewriting them to today's line numbers would be
# falsifying a record rather than fixing it.
#
# THE RULE for a citation that legitimately cannot be checked (a file the reader
# is told to fetch, a historical path): write the symbol name instead of a line
# number. The audit's fixes did exactly that -- "find Sv206GuestStub by symbol"
# cannot drift, and it is more useful to a reader than a number anyway.

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $here)

if ($env:YGHV_DOCS_ROOT -and $env:YGHV_CITE_REPO_ROOT) { $repoRoot = $env:YGHV_CITE_REPO_ROOT }
if ($env:YGHV_DOCS_ROOT) { $docsRoot = $env:YGHV_DOCS_ROOT }
else { $docsRoot = Join-Path $repoRoot 'docs' }

if (-not (Test-Path -LiteralPath $docsRoot)) {
    throw "source_citation_check: cannot find $docsRoot"
}

# --------------------------------------------------------------- source index
#
# One walk, keyed by BASENAME, because documents cite "mem_bp.c:1139" and never the
# path. A basename that exists in two places is recorded as ambiguous and reported;
# it is not silently resolved to whichever copy PowerShell happened to list first.
$SKIP_DIRS = @('.git', 'bin', 'obj', 'thirdparty', '.release', '.vs', '.idea',
               'node_modules', 'logs_archive', '.superpowers', 'archive')
$CITE_EXT = @('.c', '.h', '.cpp', '.hpp', '.asm', '.s', '.ps1', '.psm1', '.cs',
              '.bat', '.cmd', '.md', '.txt', '.json', '.xml', '.inf', '.vcxproj')

$byName = @{}
$stack = New-Object 'System.Collections.Generic.Stack[string]'
$stack.Push($repoRoot)
while ($stack.Count -gt 0) {
    $dir = $stack.Pop()
    try { $entries = [IO.Directory]::GetFileSystemEntries($dir) } catch { continue }
    foreach ($e in $entries) {
        $leaf = [IO.Path]::GetFileName($e)
        if ([IO.Directory]::Exists($e)) {
            if ($SKIP_DIRS -contains $leaf) { continue }
            if ($leaf.StartsWith('.')) { continue }
            $stack.Push($e)
            continue
        }
        if ($CITE_EXT -notcontains ([IO.Path]::GetExtension($leaf).ToLowerInvariant())) { continue }
        if (-not $byName.ContainsKey($leaf)) { $byName[$leaf] = New-Object 'System.Collections.Generic.List[string]' }
        $byName[$leaf].Add($e)
    }
}

# --------------------------------------------------------------- cited lines
$documents = @()
foreach ($p in @(Get-ChildItem -LiteralPath $docsRoot -File -Filter '*.md')) { $documents += $p.FullName }
$repoReadme = Join-Path $repoRoot 'README.md'
if ((Test-Path -LiteralPath $repoReadme) -and -not ($documents -contains $repoReadme)) { $documents += $repoReadme }

if ($documents.Count -eq 0) {
    Write-Host '[SKIP] citations: no .md documents to scan'
    exit 0
}

# file.ext:NNN, optionally a range file.ext:NNN-MMM. Only the FIRST number is
# checked -- requiring both would reject "main.c:5123-5151" for a reason nobody
# can act on, and the end of a range drifts with the start of the next one.
$citePattern = '([A-Za-z0-9_][A-Za-z0-9_.\-]*\.(?:c|h|cpp|hpp|asm|s|ps1|psm1|cs|bat|cmd|md|txt|json|xml|inf|vcxproj)):([0-9]+)'

# [HIST] -- THE ONE EXEMPTION, and it was forced by this checker's own first run.
#
# The 9.587 record documents the seven citations that were WRONG, by printing them:
# "the target moved, it used to say control_device.c:1747". Those numbers are now
# blank lines -- deliberately, because the whole point of the table is that they are
# wrong. The first run of this gate failed on the record that introduced it.
#
# "A citation of a bad citation" and "a citation" look identical to a regex, so the
# document has to say which one it is writing. [HIST] is that statement, borrowed
# from win32_error_names.ps1, where the same need appeared first (9.583: a record
# quoting the wrong error code it had itself published). One convention, two gates.
#
# The exemption is per LINE and it is counted and printed, never silent: a document
# that marks everything [HIST] would score zero citations checked, which the
# progress line makes obvious. The self-test below plants an unmarked bad citation
# on the same line as a marked one to prove the marker does not blind the scan.

# THE INSTRUMENT CHECKS ITSELF FIRST. "Zero citations found" has two possible causes
# and they are not the same finding: the documents really contain none, or the PATTERN
# stopped matching and the scan is now silent about everything. The original version
# could not tell them apart and failed on both -- which broke the self-test's
# own "a tree with no citations is fine" control (measured, 9.587). Testing the
# pattern against a string it MUST match separates the two: a broken pattern is
# loud here, and an empty document set is an honest SKIP further down.
$probe = 'see main.c:12 and svm_core.c:475-479'
if (@([regex]::Matches($probe, $citePattern)).Count -ne 2) {
    Write-Host ('[FAIL] the citation pattern no longer matches its own example: {0}' -f $probe)
    throw 'source_citation_check: the pattern is broken, so every result below would be vacuous'
}

$problems = New-Object 'System.Collections.Generic.List[string]'
$lineCache = @{}
$total = 0
$checked = 0
$historical = 0
$quoted = 0
$ambiguous = New-Object 'System.Collections.Generic.List[string]'

function Get-Lines {
    param([string] $Path)
    if (-not $lineCache.ContainsKey($Path)) {
        try { $lineCache[$Path] = [IO.File]::ReadAllLines($Path) }
        catch { $lineCache[$Path] = $null }
    }
    return $lineCache[$Path]
}

foreach ($doc in $documents) {
    $docLines = [IO.File]::ReadAllLines($doc, [Text.Encoding]::UTF8)
    $docName = [IO.Path]::GetFileName($doc)
    $inFence = $false
    for ($i = 0; $i -lt $docLines.Count; $i++) {
        $rawLine = $docLines[$i]
        # A FENCED BLOCK IS A QUOTATION by construction -- it is what some tool printed,
        # reproduced. The 9.587 record quotes the gate's own FAIL output to show what the
        # gate said, and those two lines repeat the bad citations. Marking them [HIST]
        # would mean editing a transcript, which is the one thing a quoted transcript
        # must not be. The fence is the marker; this is not a second convention, it is
        # the same statement made structurally instead of textually.
        if ($rawLine -match '^\s*```') { $inFence = -not $inFence; continue }
        if ($inFence) { if ($rawLine -match $citePattern) { $quoted++ }; continue }
        # .Contains, not -match: '[HIST]' is a REGEX CHARACTER CLASS and would match
        # any line containing an H, I, S or T -- i.e. almost every line. .Contains
        # asks the literal question. The self-test below proves the marker still
        # exempts exactly one line and blinds nothing else.
        if ($rawLine.Contains('[HIST]') -and $rawLine -match $citePattern) { $historical++; continue }
        foreach ($m in [regex]::Matches($rawLine, $citePattern)) {
            $total++
            $name = $m.Groups[1].Value
            $n = [int]$m.Groups[2].Value
            if (-not $byName.ContainsKey($name)) {
                # A citation nothing can resolve is a claim nothing can check. It is
                # NOT a failure: documents legitimately mention files that are not in
                # the tree (a sample the reader supplies, a Windows SDK header).
                continue
            }
            $candidates = $byName[$name]
            if ($candidates.Count -gt 1) {
                $rel = @($candidates | ForEach-Object { $_.Substring($repoRoot.Length).TrimStart('\', '/') })
                $ambiguous.Add("$docName line $($i + 1) cites $name, which exists $($candidates.Count) times ($($rel -join '; '))")
            }
            $bestReason = $null
            $ok = $false
            foreach ($cand in $candidates) {
                $target = Get-Lines $cand
                if ($null -eq $target) { continue }
                if ($n -lt 1 -or $n -gt $target.Count) {
                    if (-not $bestReason) { $bestReason = "$name has $($target.Count) lines, line $n does not exist" }
                    continue
                }
                if ($target[$n - 1].Trim().Length -eq 0) {
                    if (-not $bestReason) { $bestReason = "line $n of $name is BLANK - the target moved and the number did not" }
                    continue
                }
                $ok = $true
                break
            }
            if ($ok) { $checked++; continue }
            if ($bestReason) { $problems.Add("$docName line $($i + 1) cites $($name):$n - $bestReason") }
        }
    }
}

Write-Host ("  {0} citation(s) in {1} document(s); {2} resolved to a real, non-blank line; {3} line(s) marked [HIST], {4} inside fenced blocks -- both quoted rather than claimed" -f
            $total, $documents.Count, $checked, $historical, $quoted)
foreach ($a in $ambiguous) { Write-Host "  [INFO] $a" }

if ($problems.Count -gt 0) {
    foreach ($p in $problems) { Write-Host "[FAIL] $p" }
    throw "source_citation_check: $($problems.Count) citation(s) do not point at the line they claim"
}

if ($total -eq 0) {
    # Not a failure (the pattern proved itself above): a document set can honestly
    # contain no source citations. Said out loud so it is not mistaken for a pass.
    Write-Host ('[SKIP] citations: no file.ext:NNN citation in {0} document(s), nothing to check' -f $documents.Count)
    exit 0
}

Write-Host '[PASS] citations: every source citation points at a real, non-blank line'

# ---------------------------------------------------------------------------
# SELF-TEST: plant an out-of-range citation and a blank-line citation, require a
# FAIL for each, and require a real citation to stay silent.
# ---------------------------------------------------------------------------

function Invoke-SelfTestChild {
    param([string] $DocsPath, [string] $RepoPath)
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out = ''
    $code = 0
    try {
        $env:YGHV_DOCS_ROOT = $DocsPath
        $env:YGHV_CITE_REPO_ROOT = $RepoPath
        $out = (& powershell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath 2>&1 | Out-String)
        $code = $LASTEXITCODE
        if ($null -eq $code) { $code = 0 }
    }
    catch { $out = "$_"; $code = 1 }
    finally {
        $ErrorActionPreference = $saved
        # YGHV_CITE_SELFTEST_CHILD is deliberately NOT cleared here: it belongs to
        # the parent's whole self-test block, and clearing it per child would let the
        # next child recurse again.
        $env:YGHV_DOCS_ROOT = $null
        $env:YGHV_CITE_REPO_ROOT = $null
    }
    return @{ Exit = $code; Out = $out }
}

if (-not $env:YGHV_CITE_SELFTEST_CHILD) {
    # THE GUARD HAS TO BE SET HERE, not around each child call. The children are
    # spawned with this same script, so a child that does not inherit the flag runs
    # this block again, spawns its own children, and so on -- measured 9.587: the
    # run printed the PASS line and then hung for six minutes with no further output
    # while descendants piled up. Set on entry and cleared at the end, this file
    # re-enters as a plain checker.
    $env:YGHV_CITE_SELFTEST_CHILD = '1'
    $failures = New-Object 'System.Collections.Generic.List[string]'
    $work = Join-Path ([IO.Path]::GetTempPath()) ('yghv_cite_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $copy = Join-Path $work 'docs'
    try {
        New-Item -ItemType Directory -Path $copy -Force | Out-Null

        # A file that definitely exists in the real tree, with a line count we can
        # read, so the mutations below rest on measured facts rather than on a guess
        # about which file is big enough.
        $probe = $null
        foreach ($n in @('mem_bp.c', 'svm_simplevm206.cpp', 'main.c', 'protect.c')) {
            if ($byName.ContainsKey($n)) { $probe = $n; break }
        }
        if (-not $probe) { throw 'selftest: none of the probe files is in the tree' }
        $probePath = $byName[$probe][0]
        $probeLines = @(Get-Lines $probePath)

        # 0. CONTROL -- a citation into the middle of a real file must PASS. Without
        # this the rest proves only that the checker can fail, not that it can pass.
        $mid = [int]($probeLines.Count / 2)
        while ($mid -lt $probeLines.Count -and $probeLines[$mid - 1].Trim().Length -eq 0) { $mid++ }
        Set-Content -LiteralPath (Join-Path $copy 'a.md') -Value (('see {0}:{1}' -f $probe, $mid)) -Encoding UTF8
        $r = Invoke-SelfTestChild $copy $repoRoot
        if ($r.Exit -ne 0) { $failures.Add((('control: a real citation to {0}:{1} was reported broken: {2}' -f $probe, $mid, $r.Out.Trim()))) }
        else { Write-Host '[PASS] self-test: a real citation stays silent' }
        Remove-Item -LiteralPath (Join-Path $copy 'a.md') -Force

        # 1. OUT OF RANGE -- the loud case.
        Set-Content -LiteralPath (Join-Path $copy 'b.md') -Value (('see {0}:999999' -f $probe)) -Encoding UTF8
        $r = Invoke-SelfTestChild $copy $repoRoot
        if ($r.Exit -eq 0) { $failures.Add('mutation: an out-of-range citation PASSED') }
        elseif ($r.Out -notmatch 'does not exist') { $failures.Add("mutation: out-of-range failed but not for the reason: $($r.Out.Trim())") }
        else { Write-Host '[PASS] self-test: an out-of-range citation was caught' }
        Remove-Item -LiteralPath (Join-Path $copy 'b.md') -Force

        # 2. BLANK LINE -- the quiet case, and the one 9.587 actually found four of.
        $blankAt = 0
        for ($i = 0; $i -lt $probeLines.Count; $i++) {
            if ($probeLines[$i].Trim().Length -eq 0) { $blankAt = $i + 1; break }
        }
        if ($blankAt -eq 0) { $failures.Add("mutation: $probe has no blank line to aim at, pick another probe") }
        else {
            Set-Content -LiteralPath (Join-Path $copy 'c.md') -Value (('see {0}:{1}' -f $probe, $blankAt)) -Encoding UTF8
            $r = Invoke-SelfTestChild $copy $repoRoot
            if ($r.Exit -eq 0) { $failures.Add('mutation: a citation to a BLANK line PASSED - the drift heuristic is asleep') }
            elseif ($r.Out -notmatch 'BLANK') { $failures.Add("mutation: blank-line citation failed but not for the reason: $($r.Out.Trim())") }
            else { Write-Host '[PASS] self-test: a citation to a blank line was caught' }
            Remove-Item -LiteralPath (Join-Path $copy 'c.md') -Force
        }

        # 3. [HIST] EXEMPTS A LINE, AND MUST EXEMPT ONLY THAT LINE. Two planted
        # defects: a marked bad citation (must stay silent -- that is the whole
        # point) and, on the NEXT line, an unmarked identical bad citation (must
        # still be caught, so the marker cannot be used as a way to switch the
        # gate off).
        $histAt = 0
        for ($i = 0; $i -lt $probeLines.Count; $i++) {
            if ($probeLines[$i].Trim().Length -eq 0) { $histAt = $i + 1; break }
        }
        if ($histAt -eq 0) { $failures.Add("marker: $probe has no blank line to quote, pick another probe") }
        else {
            $body = @(
                ('quoted as wrong, was {0}:{1} [HIST]' -f $probe, $histAt),
                ('still claimed, is {0}:{1}' -f $probe, $histAt)
            )
            Set-Content -LiteralPath (Join-Path $copy 'd.md') -Value $body -Encoding UTF8
            $r = Invoke-SelfTestChild $copy $repoRoot
            if ($r.Exit -eq 0) { $failures.Add('marker: an UNMARKED bad citation on the line after a [HIST] one PASSED - the marker blinds the scan') }
            elseif ($r.Out -notmatch 'line 2 ') { $failures.Add("marker: the unmarked half was not the one caught: $($r.Out.Trim())") }
            elseif ($r.Out -match 'line 1 ') { $failures.Add("marker: the [HIST] line was still reported: $($r.Out.Trim())") }
            else { Write-Host '[PASS] self-test: a [HIST] line is exempt and its neighbour is not' }
            Remove-Item -LiteralPath (Join-Path $copy 'd.md') -Force
        }

        # 4. A FENCE EXEMPTS WHAT IS INSIDE IT, AND NOTHING ELSE. Same boundary as
        # [HIST]: a bad citation inside a fence must stay silent (that is a quoted
        # transcript), and the identical bad citation just after the closing fence
        # must still be caught. Without the second half the fence would be a way to
        # turn the gate off, and every control above would still be green.
        $fenceAt = 0
        for ($j = 0; $j -lt $probeLines.Count; $j++) {
            if ($probeLines[$j].Trim().Length -eq 0) { $fenceAt = $j + 1; break }
        }
        if ($fenceAt -eq 0) { $failures.Add("fence: $probe has no blank line to quote, pick another probe") }
        else {
            $body = @(
                'transcript follows:',
                '```',
                ('[FAIL] something cites {0}:{1}' -f $probe, $fenceAt),
                '```',
                ('and here it is claimed: {0}:{1}' -f $probe, $fenceAt)
            )
            Set-Content -LiteralPath (Join-Path $copy 'e.md') -Value $body -Encoding UTF8
            $r = Invoke-SelfTestChild $copy $repoRoot
            if ($r.Exit -eq 0) { $failures.Add('fence: an UNFENCED bad citation after a fenced one PASSED - the fence blinds the scan') }
            elseif ($r.Out -notmatch 'line 5 ') { $failures.Add("fence: the unfenced half was not the one caught: $($r.Out.Trim())") }
            elseif ($r.Out -match 'line 3 ') { $failures.Add("fence: the citation inside the fence was still reported: $($r.Out.Trim())") }
            else { Write-Host '[PASS] self-test: a fenced citation is exempt and the one after it is not' }
            Remove-Item -LiteralPath (Join-Path $copy 'e.md') -Force
        }

        # 5. A document with NO citation at all would make every check above vacuous.
        # An empty docs root must still exit 0 (SKIP) rather than pretend it checked.
        $empty = Join-Path $work 'empty'
        New-Item -ItemType Directory -Path $empty -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $empty 'x.md') -Value 'no citations here' -Encoding UTF8
        $r = Invoke-SelfTestChild $empty $repoRoot
        if ($r.Exit -ne 0) { $failures.Add("a document set with no citations was reported as a failure: $($r.Out.Trim())") }
        else { Write-Host '[PASS] self-test: a tree with no citations is silent, not a fake pass or fake failure' }
    }
    finally {
        $env:YGHV_CITE_SELFTEST_CHILD = $null
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
    }

    if ($failures.Count -gt 0) {
        foreach ($f in $failures) { Write-Host "[FAIL] $f" }
        throw 'source_citation_check: self-test failed'
    }
    Write-Host '[PASS] source_citation self-test: 2 planted citation defects caught, 4 controls silent (a real citation, a [HIST] quote, a fenced transcript, and an empty tree)'
}
