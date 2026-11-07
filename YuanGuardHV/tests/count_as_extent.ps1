$ErrorActionPreference = 'Stop'

# count_as_extent.ps1 -- 9.486, rewritten 9.498
#
# A TABLE THAT IS NOT COMPACTED MUST NOT BE SCANNED UP TO ITS LIVE COUNT.
#
# The shape this looks for:
#
#     int n = (int)InterlockedCompareExchange(&g_xxx_count, 0, 0);
#     ...
#     for (i = 0; i < n && i < XXX_MAX; i++)
#
# The counter is incremented on install and decremented on remove, but the array
# is never compacted, so removing a slot in the middle leaves a hole:
#
#     armed 3:  active = [1,1,1]  count = 3
#     removed:  active = [1,0,1]  count = 2
#     scan i<n: looks at slots 0,1 only -> slot 2 is LIVE and UNREACHABLE
#
# Measured consequence (9.485): arm three breakpoints, remove the middle one, and
# the third stops firing - while the count and every list still agree.
#
# THE RULE: a loop whose bound is a LIVE COUNT must either
#   (a) not exist - scan the full array and test `active` per slot, or
#   (b) be proven safe because that file compacts the table on removal.
#
# ---------------------------------------------------------------------------
# 9.498: WHY THIS FILE WAS REWRITTEN
#
# The 9.486 version printed, on a healthy tree:
#
#     [WARN] matched no live-count-bounded loop at all - parser may be stale
#     [PASS] live-count scans: no un-compacted table is bounded by its counter
#
# It had found NOTHING and PASSED anyway. The reason was a hard-coded window: it
# looked for the loop only 5 lines after the census read. The real distances in
# mem_bp.c are 18 and 19 lines, so every site fell outside the window and the
# check became vacuous. This tree already paid for that class of mistake once:
# exit_rip_contract's first version searched a guessed window and reported false
# findings about correct code. A guessed window can fail either way - it can miss
# a defect, or invent one.
#
# So this version derives the FUNCTION BODY by brace matching and looks for the
# loop anywhere inside it. No window.
#
# And, per the tree's methodology #4 ("a checker must be able to fail"), it now
# carries a SELF-TEST: every run plants the original defect into a throwaway copy
# of hv\ and requires THIS checker to fail on that copy. The real sources are
# never touched.
#
# WHAT THE SELF-TEST FOUND THE MOMENT THE PARSER WORKED: a fourth scan that 9.485
# missed - yghv_bp_on_dr still bounded its DR6 scan by g_bp_count. Same defect,
# same silence, a different exit path. Fixed in 9.498 (see the note there).
#
# Explicitly allowed today, and why:
#   hv/mem_bp.c      scans the whole array (9.485, plus 9.498 for on_dr)
#                    9.507 note: on_dr itself is now UNREACHABLE - DR arming is
#                    refused (-12) because the mechanism cannot work. The source
#                    still carries the fixed scan, so this checker and its
#                    self-test keep their meaning; they judge CODE, not reachability.
#   hv/mem_cache.c   fixed in 9.486
#   hv/mem_time.c    fixed in 9.486
#   hv/mem_view.c    fixed in 9.486
#   hv/mem_watch.c   fixed in 9.486
#   hv/protect.c     compacts on removal - its count is a true extent
#   hv/yghv_guard.c  g_state_obj_count is never decremented: the table is
#                    append-only, so its count IS the extent

$repoRoot = Split-Path -Parent $PSScriptRoot
$hvDir = Join-Path $repoRoot 'hv'

# 9.498: the self-test re-runs this script against a mutated COPY through this
# variable. Reading the directory from the environment is what lets the self-test
# exercise THIS checker instead of a reimplementation that could drift from it.
if ($env:YGHV_COUNT_EXTENT_HV_DIR -and (Test-Path $env:YGHV_COUNT_EXTENT_HV_DIR)) {
    $hvDir = $env:YGHV_COUNT_EXTENT_HV_DIR
}

if (-not (Test-Path $hvDir)) { throw "count_as_extent: cannot find $hvDir" }

# ---------------------------------------------------------------------------
# Find every function body by brace matching.
#
# A "body" starts at the line that opens a brace while depth is 0 and is not a
# declaration-only line, and ends at the matching close. For this tree - plain C
# with no macros that open braces - brace depth is enough, and it is derived
# rather than guessed.
# ---------------------------------------------------------------------------

function Get-FunctionBodies {
    param([string[]] $Lines)

    $bodies = @()
    $depth = 0
    $start = -1

    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $line = $Lines[$i]
        $opens = ([regex]::Matches($line, '{')).Count
        $closes = ([regex]::Matches($line, '}')).Count

        if ($start -lt 0 -and $depth -eq 0 -and $opens -gt 0 -and
            $line -notmatch '^\s*(//|/\*|\*|#)' ) {
            $start = $i
        }

        $depth += $opens - $closes

        if ($start -ge 0 -and $depth -le 0) {
            $bodies += [pscustomobject]@{ Start = $start; End = $i }
            $start = -1
            $depth = 0
        }

        if ($depth -lt 0) { $depth = 0 }
    }

    if ($start -ge 0) {
        $bodies += [pscustomobject]@{ Start = $start; End = $Lines.Count - 1 }
    }
    return $bodies
}

$findings   = @()
$censusSites = 0
$appendOnly = 0
$allowed    = 0
$files      = 0

foreach ($file in Get-ChildItem -Path $hvDir -Filter *.c) {
    $files += 1
    $lines = Get-Content $file.FullName
    $text = $lines -join "`n"

    # Does this file compact any table on removal? If so, that file's counts are
    # extents. (Per-file, not per-table: the regex cannot prove which table a
    # compaction belongs to. That limitation is stated rather than hidden.)
    $compacts = ($text -match '\[\s*\w+\s*\]\s*=\s*\w+\[\s*\w+\s*->?\s*\w*count\s*-\s*1\s*\]')

    foreach ($body in (Get-FunctionBodies -Lines $lines)) {
        # Census reads inside this body: `NAME = (int)InterlockedCompareExchange(&g_X, 0, 0)`
        # with an optional type prefix (`int n = ...`).
        $censusVars = @{}
        for ($i = $body.Start; $i -le $body.End; $i++) {
            if ($lines[$i] -match '(\w+)\s*=\s*\(int\)\s*InterlockedCompareExchange\s*\(\s*&(g_[A-Za-z0-9_]+)\s*,') {
                $censusVars[$matches[1]] = $matches[2]
            }
            elseif ($lines[$i] -match '(\w+)\s*=\s*InterlockedCompareExchange\s*\(\s*&(g_[A-Za-z0-9_]+)\s*,') {
                $censusVars[$matches[1]] = $matches[2]
            }
        }
        if ($censusVars.Count -eq 0) { continue }

        foreach ($var in $censusVars.Keys) {
            $counter = $censusVars[$var]
            $censusSites += 1

            # Is this counter ever decremented? If not, the table is append-only
            # and its count is a true extent - allowed by construction.
            $decremented = ($text -match ("InterlockedDecrement\s*\(\s*&" + [regex]::Escape($counter) + "\b|" +
                                          [regex]::Escape($counter) + "--"))
            if (-not $decremented) { $appendOnly += 1; continue }

            # A loop inside this SAME body bounded by that census variable.
            $loopLine = -1
            for ($i = $body.Start; $i -le $body.End; $i++) {
                if ($lines[$i] -match ("for\s*\(\s*\w+\s*=\s*0\s*;\s*\w+\s*<\s*" + [regex]::Escape($var) + "\s*(&&|;|\s)")) {
                    $loopLine = $i
                    break
                }
            }
            if ($loopLine -lt 0) { continue }

            if ($compacts) { $allowed += 1; continue }

            $findings += ('{0}:{1} scans i<{2} where {2} = live {3}, and this file does not ' -f
                          $file.Name, ($loopLine + 1), $var, $counter) +
                         ' compact its table on removal - entries above a hole are unreachable'
        }
    }
}

Write-Host ("  $files file(s); $censusSites census read(s) found; " +
            "$appendOnly append-only counter(s) (count is a true extent); " +
            "$allowed allowed by compaction")

if ($censusSites -eq 0) {
    # A parser that matched nothing is the failure mode that makes a checker
    # silently useless. 9.486 printed a WARN here and still PASSed; that is the
    # defect this rewrite exists for, so it is now a hard failure.
    throw 'count_as_extent: matched no census read at all - the parser is stale, not the code'
}

if ($findings.Count -gt 0) {
    foreach ($f in $findings) { Write-Host "[FAIL] $f" }
    throw "count_as_extent: $($findings.Count) un-compacted table scan(s)"
}

Write-Host '[PASS] live-count scans: no un-compacted table is bounded by its counter'

# ---------------------------------------------------------------------------
# SELF-TEST: prove this checker still detects the defect it was written for.
#
# Measured history: the 9.486 version could not detect anything and said PASS.
# A checker that always passes is indistinguishable from a checker that works,
# until it matters. So the ability to fail is tested on every build.
#
# WHAT IS PLANTED: the exact original defect in yghv_bp_on_dr, restored in a
# throwaway copy of hv\:
#   `int live = (int)InterlockedCompareExchange(&g_bp_count, 0, 0)` -> `int n = ...`
#   `for (i = 0; i < YGHV_BP_MAX; i++)`                            -> `for (i = 0; i < n && i < YGHV_BP_MAX; i++)`
# That reproduces the census-bounded scan at its ORIGINAL DISTANCE from the
# census read (19 lines), which is the only mutation that would have caught the
# window bug. Planting the defect one line above the loop would pass even with
# the broken 5-line window, so it would prove nothing.
#
# It is located STRUCTURALLY (inside the on_dr body), not by line number, so the
# self-test keeps working when the file shifts.
# ---------------------------------------------------------------------------

$selfTestFailures = @()

# RECURSION GUARD: the self-test re-runs this same script. Without this flag the
# child would run its own self-test, which would run its own... The flag says
# "this run IS the self-test's child; do not self-test again".
if (-not $env:YGHV_COUNT_EXTENT_SELFTEST_CHILD) {
$copyDir = $null
try {
    $copyDir = Join-Path $env:TEMP ("count_extent_selftest_" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $copyDir -Force | Out-Null
    Copy-Item -Path (Join-Path $repoRoot 'hv\*.c') -Destination $copyDir -Force
    Copy-Item -Path (Join-Path $repoRoot 'hv\*.h')  -Destination $copyDir -Force -ErrorAction SilentlyContinue
    Copy-Item -Path (Join-Path $repoRoot 'hv\*.hpp') -Destination $copyDir -Force -ErrorAction SilentlyContinue

    $bpPath = Join-Path $copyDir 'mem_bp.c'
    if (-not (Test-Path $bpPath)) {
        $selfTestFailures += 'mem_bp.c not found in the copy - cannot plant the defect'
    }
    else {
        $bpLines = Get-Content $bpPath
        $body = $null
        foreach ($b in (Get-FunctionBodies -Lines $bpLines)) {
            $head = $bpLines[$b.Start]
            if ($head -match 'yghv_bp_on_dr') { $body = $b; break }
        }
        if (-not $body) {
            $selfTestFailures += 'yghv_bp_on_dr not found in the copy - the self-test cannot locate the site'
        }
        else {
            $censusLine = -1
            $loopLine = -1
            for ($i = $body.Start; $i -le $body.End; $i++) {
                if ($censusLine -lt 0 -and
                    $bpLines[$i] -match 'int\s+live\s*=\s*\(int\)\s*InterlockedCompareExchange\s*\(\s*&g_bp_count') {
                    $censusLine = $i
                }
                if ($loopLine -lt 0 -and
                    $bpLines[$i] -match 'for\s*\(\s*\w+\s*=\s*0\s*;\s*\w+\s*<\s*YGHV_BP_MAX\s*;\s*\w+\+\+\s*\)') {
                    $loopLine = $i
                }
            }
            if ($censusLine -lt 0) {
                $selfTestFailures += 'no `int live = ...(&g_bp_count` in yghv_bp_on_dr - the code no longer uses the census-then-scan shape, update the self-test'
            }
            elseif ($loopLine -lt 0) {
                $selfTestFailures += 'no `i < YGHV_BP_MAX` loop in yghv_bp_on_dr - the code no longer uses the census-then-scan shape, update the self-test'
            }
            else {
                $mutated = @()
                $mutated += $bpLines[0..($censusLine - 1)]
                $mutated += ($bpLines[$censusLine] -replace 'int\s+live\s*=', 'int n =')
                $mutated += ($bpLines[($censusLine + 1)..($loopLine - 1)])
                $mutated += ($bpLines[$loopLine] -replace '<\s*YGHV_BP_MAX\s*;', '< n && i < YGHV_BP_MAX;')
                if ($loopLine + 1 -lt $bpLines.Count) {
                    $mutated += $bpLines[($loopLine + 1)..($bpLines.Count - 1)]
                }
                Set-Content -Path $bpPath -Value $mutated -Encoding ASCII

                $env:YGHV_COUNT_EXTENT_HV_DIR = $copyDir
                $env:YGHV_COUNT_EXTENT_SELFTEST_CHILD = '1'

                # Capture the child through a FILE, not through a pipeline: the
                # child's EXPECTED throw must not be swallowed by this scope's
                # $ErrorActionPreference='Stop'. Same shape as exit_rip_contract.
                $logPath = Join-Path $env:TEMP ("count_extent_selftest_" + [guid]::NewGuid().ToString('N') + ".log")
                $cmdLine = '/c powershell -NoProfile -ExecutionPolicy Bypass -File "' +
                           $PSCommandPath + '" > "' + $logPath + '" 2>&1'
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

                $env:YGHV_COUNT_EXTENT_HV_DIR = $null
                $env:YGHV_COUNT_EXTENT_SELFTEST_CHILD = $null

                if ($childExit -eq 0) {
                    $selfTestFailures += 'the mutated copy PASSED - the checker accepted a census-bounded scan of an un-compacted table'
                }
                elseif ($all -notmatch 'mem_bp\.c') {
                    $selfTestFailures += "the mutated copy failed but never named mem_bp.c (child output: $($all.Trim()))"
                }
                elseif ($all -notmatch 'unreachable') {
                    $selfTestFailures += "the mutated copy named a file but not the reason (child output: $($all.Trim()))"
                }
                else {
                    Write-Host '[PASS] self-test: planted census-bounded scan in yghv_bp_on_dr was caught'
                }
            }
        }
    }
}
finally {
    if ($copyDir -and (Test-Path $copyDir)) { Remove-Item $copyDir -Recurse -Force -ErrorAction SilentlyContinue }
    $env:YGHV_COUNT_EXTENT_HV_DIR = $null
    $env:YGHV_COUNT_EXTENT_SELFTEST_CHILD = $null
}
}   # end of the RECURSION GUARD

if ($selfTestFailures.Count -gt 0) {
    foreach ($f in $selfTestFailures) { Write-Host "[FAIL] $f" }
    throw 'count_as_extent: self-test failed'
}
