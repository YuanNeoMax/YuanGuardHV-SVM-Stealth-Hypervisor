$ErrorActionPreference = 'Stop'

# eol_check.ps1 -- 9.586
#
# WHY: this tree has no .gitattributes, so a file's line endings are whatever the
# editor that last touched it decided. Measured 9.585: of 192 tracked files, 53
# were entirely CRLF, 138 entirely LF, and 6 carried BOTH inside one file.
#
# The mixed six are the problem, and one of them cost a whole review:
#
#   hv/svm_simplevm206_x64.asm   a +37 line change (yghv_write_dr) arrived as a
#                                1301 line diff, because the editor rewrote the
#                                file's 632 LF lines to CRLF on save. 669 of those
#                                1301 lines were pure churn. "git diff
#                                --ignore-all-space" showed the real change in one
#                                screen; nobody runs that first.
#
# That is the general failure: a whitespace-only rewrite of an untouched file
# makes the REAL change invisible in review. The diff is not wrong, it is
# unreadable, and unreadable is what gets approved without being read.
#
# THE CONTRACT: no tracked text file may contain both CRLF and bare LF, and none
# may contain a lone CR. A file is EITHER CRLF throughout OR LF throughout.
# Which one is a per-file decision this checker does not make -- both are legal
# here, and 53 files already chose CRLF. Consistency INSIDE a file is the invariant.
#
# ALLOW LIST: one entry, with a reason, so it is a decision and not a silent pass.
#
# SELF-TEST: plants a mixed-EOL file in a throwaway tree and requires a FAIL. The
# planted defect is the asm file's exact shape (CRLF body, LF tail), plus a clean
# file that must NOT be flagged.

$repoRoot = Split-Path -Parent $PSScriptRoot

# Directory names never scanned. bin/ is build output, .release/ is a staging copy,
# thirdparty/ is an upstream clone (not ours to normalise).
$SKIP_DIRS = @('.git', 'bin', 'obj', 'thirdparty', '.release', '.vs', '.idea',
              'node_modules', 'logs_archive', '.superpowers')

# Extensions that carry source or documentation. A binary is never in scope:
# a .sys or .exe legitimately contains 0x0D 0x0A pairs that are not line endings.
$TEXT_EXT = @('.c', '.h', '.cpp', '.hpp', '.asm', '.s', '.ps1', '.psm1', '.cs',
              '.bat', '.cmd', '.java', '.md', '.txt', '.csv', '.json', '.xml',
              '.vcxproj', '.inf', '.py', '.sh', '.yml', '.yaml')

# Files with no extension that are still text.
$TEXT_NAMES = @('.gitignore', '.gitattributes', 'Makefile')

# relative path -> why mixing is tolerated.
$ALLOW = @{
    'docs/archive/_ARCHIVE_HANDOFF_9.1-9.299.md' = '1MB verbatim archive of records 9.1-9.299; its bytes are the historical artifact, and normalising it would rewrite 11056+6316 lines of history for no reader benefit'
}

function Get-TextFiles {
    param([string] $Root)
    $out = New-Object 'System.Collections.Generic.List[string]'
    $stack = New-Object 'System.Collections.Generic.Stack[string]'
    $stack.Push($Root)
    while ($stack.Count -gt 0) {
        $dir = $stack.Pop()
        foreach ($sub in [IO.Directory]::GetDirectories($dir)) {
            $name = [IO.Path]::GetFileName($sub)
            if ($SKIP_DIRS -contains $name) { continue }
            if ($name.StartsWith('.')) { continue }
            $stack.Push($sub)
        }
        foreach ($f in [IO.Directory]::GetFiles($dir)) {
            $name = [IO.Path]::GetFileName($f)
            $ext = [IO.Path]::GetExtension($f).ToLowerInvariant()
            if (($TEXT_EXT -contains $ext) -or ($TEXT_NAMES -contains $name)) {
                $out.Add($f)
            }
        }
    }
    return $out
}

function Count-Eol {
    param([string] $Path)
    $b = [IO.File]::ReadAllBytes($Path)
    $crlf = 0
    $bareLf = 0
    $loneCr = 0
    for ($i = 0; $i -lt $b.Length; $i++) {
        if ($b[$i] -eq 13) {
            if (($i + 1) -lt $b.Length -and $b[$i + 1] -eq 10) { $crlf++; $i++ }
            else { $loneCr++ }
        }
        elseif ($b[$i] -eq 10) { $bareLf++ }
    }
    return [pscustomobject]@{ Crlf = $crlf; Lf = $bareLf; LoneCr = $loneCr }
}

$files = Get-TextFiles -Root $repoRoot
if ($files.Count -eq 0) {
    throw 'eol_check: found no text file at all - the walker is stale, not the tree'
}

$bad = @()
$scanned = 0
$mixedCount = 0
foreach ($f in $files) {
    $rel = $f.Substring($repoRoot.Length).TrimStart('\', '/').Replace('\', '/')
    if ($ALLOW.ContainsKey($rel)) { continue }
    $scanned++
    $c = Count-Eol -Path $f
    if ($c.LoneCr -gt 0) {
        $bad += ('{0}: {1} lone CR (not followed by LF) - old-Mac line ending' -f $rel, $c.LoneCr)
    }
    if ($c.Crlf -gt 0 -and $c.Lf -gt 0) {
        $mixedCount++
        $bad += ('{0}: MIXED line endings - {1} CRLF + {2} LF in one file' -f $rel, $c.Crlf, $c.Lf)
    }
}

Write-Host ('  {0} text file(s) scanned; {1} mixed; {2} allow-listed with a reason' -f
            $scanned, $mixedCount, $ALLOW.Count)

if ($bad.Count -gt 0) {
    foreach ($b in $bad) { Write-Host "[FAIL] $b" }
    throw "eol_check: $($bad.Count) line-ending defect(s)"
}

Write-Host '[PASS] line endings: no file mixes CRLF with bare LF'

# ---------------------------------------------------------------------------
# SELF-TEST: plant a mixed-EOL file in a throwaway tree and require a FAIL.
# ---------------------------------------------------------------------------

$selfTestFailures = @()
if (-not $env:YGHV_EOL_SELFTEST_CHILD) {
    $copyRoot = $null
    try {
        $copyRoot = Join-Path $env:TEMP ('eol_selftest_' + [guid]::NewGuid().ToString('N'))
        $copyTests = Join-Path $copyRoot 'tests'
        $copyHv = Join-Path $copyRoot 'hv'
        New-Item -ItemType Directory -Path $copyTests -Force | Out-Null
        New-Item -ItemType Directory -Path $copyHv -Force | Out-Null

        $crlf = [string][char]13 + [string][char]10
        $lf = [string][char]10

        # a clean file first: the self-test must prove the checker CAN pass, or a
        # checker that fails on everything would look like it works
        [IO.File]::WriteAllText((Join-Path $copyHv 'clean.c'),
            ('int a;' + $crlf + 'int b;' + $crlf))

        # the planted defect: the asm file's exact shape -- CRLF body, one LF tail
        [IO.File]::WriteAllBytes((Join-Path $copyHv 'mixed.c'),
            [Text.Encoding]::ASCII.GetBytes('int a;' + $crlf + 'int b;' + $crlf + 'int c;' + $lf))

        Copy-Item -Path $PSCommandPath -Destination (Join-Path $copyTests 'eol_check.ps1') -Force

        $env:YGHV_EOL_SELFTEST_CHILD = '1'
        $logPath = Join-Path $env:TEMP ('eol_selftest_' + [guid]::NewGuid().ToString('N') + '.log')
        $cmdLine = '/c powershell -NoProfile -ExecutionPolicy Bypass -File "' +
                   (Join-Path $copyTests 'eol_check.ps1') + '" > "' + $logPath + '" 2>&1'
        $childExit = 0
        try {
            $null = & cmd.exe $cmdLine
            $childExit = $LASTEXITCODE
        }
        catch {
            $childExit = 1
        }

        $all = ''
        if (Test-Path $logPath) {
            $all = Get-Content $logPath -Raw
            Remove-Item $logPath -Force -ErrorAction SilentlyContinue
        }
        $env:YGHV_EOL_SELFTEST_CHILD = $null

        if ($childExit -eq 0) {
            $selfTestFailures += 'the mutated copy PASSED - the checker accepted a mixed-EOL file'
        }
        elseif ($all -notmatch 'mixed\.c') {
            $selfTestFailures += "the mutated copy failed but never named the planted file (child output: $($all.Trim()))"
        }
        elseif ($all -notmatch 'MIXED') {
            $selfTestFailures += "the mutated copy failed but not for the line-ending reason (child output: $($all.Trim()))"
        }
        elseif ($all -match 'clean\.c') {
            $selfTestFailures += 'the mutated copy flagged the CLEAN file too - the checker fails on valid input'
        }
        else {
            Write-Host '[PASS] self-test: planted mixed-EOL file was caught, clean file was not'
        }
    }
    finally {
        if ($copyRoot -and (Test-Path $copyRoot)) { Remove-Item $copyRoot -Recurse -Force -ErrorAction SilentlyContinue }
        $env:YGHV_EOL_SELFTEST_CHILD = $null
    }
}

if ($selfTestFailures.Count -gt 0) {
    foreach ($f in $selfTestFailures) { Write-Host "[FAIL] $f" }
    throw 'eol_check: self-test failed'
}
