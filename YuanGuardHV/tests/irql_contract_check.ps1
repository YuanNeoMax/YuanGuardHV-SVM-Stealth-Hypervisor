$ErrorActionPreference = 'Stop'

# irql_contract_check.ps1 — 9.347 (v3)
#
# WHY THIS CHECK EXISTS
# ---------------------
# The 15th crash was an API used in the wrong context:
#
#   yghv_protect_npt_remap_flags() begins with
#       if (KeAreAllApcsDisabled()) return 0;
#
#   An NPF handler runs at DISPATCH_LEVEL with GIF=0, which IS "all APCs
#   disabled". So the guard's ALLOW branch got 0 every time: the page was never
#   restored, the decoy was never installed, and the fall-through access
#   violation was unresolvable — 0x3B, and a reboot.
#
# The function is correct in isolation and returns a plausible value, so a
# caller in the wrong context cannot tell "refused, wrong context" from
# "nothing to do". That is the class of error a review misses.
#
# This check then found a SECOND instance of the same mistake, latent since
# Tier 4.4 was written: yghv_bp_rearm_bare() was called from the #DB island path
# yet gated on KeAreAllApcsDisabled(), so a breakpoint page was never actually
# re-armed after an ALLOWed write. The name said "_bare"; the body said
# "PASSIVE-only". That defect was fixed because this check reported it.
#
# HOW IT WORKS (v3 — why v1 and v2 were thrown away)
# -------------------------------------------------
# v1 matched a definition with a single-line regex and never matched
# SvHandleVmExit, whose declaration spans several lines. It reported PASS
# against a file containing a literal copy of the crashing line.
#
# v2 tried to parse every body generically and found only 13 of the functions in
# svm_simplevm206.cpp — it lost the thread across that file's `static VOID` /
# name / parameters / `)` / `{` layout.
#
# v3 does not try to be a parser. The island entry names are INPUT: a known,
# short, hand-maintained list, which is exactly what "this is an island entry"
# means. For each name it finds the `{` by scanning forward for a line that is
# just `{`, then walks braces to the matching close. That survives every
# declaration layout this codebase uses.
#
# It also FAILS LOUDLY when it cannot locate a body it was told to police: an
# unfound body means the PASS would be meaningless.

$root = Split-Path -Parent $PSScriptRoot
$hv = Join-Path $root 'hv'

if (-not (Test-Path $hv)) {
    throw "irql contract check: $hv not found"
}

$sources = Get-ChildItem $hv -Recurse -Include '*.c', '*.cpp', '*.h', '*.hpp'
if (-not $sources) {
    throw 'irql contract check: no sources found'
}

function Get-BodyLines {
    <#
        Locate the body of $Name in $Lines; return a List of @{Line; Text}, or
        $null when this file does not define it.
    #>
    param([string[]]$Lines, [string]$Name)
    # Strip BLOCK COMMENTS before any scanning.
    #
    # The first version only skipped lines that START with '*' or '/*'. A block
    # comment's body can contain anything, including a call-like mention of a
    # gated function:
    #     * The first version of this used yghv_protect_npt_remap_flags(),
    # That line does not start with '*' in every layout, so it was scanned as
    # code and produced a violation attributed to the enclosing function. The
    # fix is to remove comment text entirely before looking for calls.
    $clean = New-Object 'System.Collections.Generic.List[string]'
    $inBlock = $false
    foreach ($src in $Lines) {
        $t = $src
        if ($inBlock) {
            $end = $t.IndexOf('*/')
            if ($end -ge 0) { $t = ' ' * ($end + 2) + $t.Substring($end + 2); $inBlock = $false }
            else { $t = '' }
        }
        if (-not $inBlock) {
            $start = $t.IndexOf('/*')
            while ($start -ge 0) {
                $end = $t.IndexOf('*/', $start + 2)
                if ($end -ge 0) {
                    $t = $t.Substring(0, $start) + (' ' * ($end + 2 - $start)) + $t.Substring($end + 2)
                } else {
                    $t = $t.Substring(0, $start)
                    $inBlock = $true
                    break
                }
                $start = $t.IndexOf('/*', $start + 1)
            }
            # line comments
            $lc = $t.IndexOf('//')
            if ($lc -ge 0) { $t = $t.Substring(0, $lc) }
        }
        $clean.Add($t)
    }
    $Lines = $clean.ToArray()

    # Try EVERY occurrence, not just the first.
    #
    # The first version took the first line containing the name followed by '('.
    # That line is often inside a COMMENT — e.g. yghv_guard_is_hidden is
    # discussed at yghv_guard.c:300 before being defined at :363 — so the scan
    # started in prose, found no brace, and reported the function BLIND. The
    # blindness check caught this correctly, which is the whole reason it exists.
    $candidates = @()
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $l = $Lines[$i]
        if ($l -match ('\b' + [regex]::Escape($Name) + '\s*\(')) {
            # skip obvious prose: a line inside a block comment or a // comment
            if ($l -match '^\s*(/\*|\*|//)') { continue }
            if ($l -match '^\s*\*') { continue }
            $candidates += $i
            # A DEFINITION, not a call site. yghv_guard_hiding_armed is called
            # inside hidecore() before it is defined, and starting the brace walk
            # at that call gave the walk hidecore's body — so a violation in
            # hidecore was reported as if it belonged to the predicate. Requiring
            # a definition shape removes that class: the name must be the first
            # identifier after a return type at the start of the line.
            if ($l -notmatch ('^\s*(?:static\s+)?(?:extern\s+"C"\s+)?[A-Za-z_][\w\s\*]*?\b' + [regex]::Escape($Name) + '\s*\(')) { continue }
        }
    }
    if ($candidates.Count -eq 0) { return $null }

    foreach ($nameIdx in $candidates) {
        $openIdx = -1
        for ($i = $nameIdx; $i -lt [Math]::Min($Lines.Count, $nameIdx + 40); $i++) {
            # style A: brace on its own line  ("..." ) then "{"
            if ($Lines[$i] -match '^\s*\{\s*$') { $openIdx = $i; break }
            # style B: brace on the declaration line  "int f(...) {"
            # A line ending in ';' before any '{' means this occurrence is a
            # DECLARATION, not a definition — e.g.
            #   int yghv_guard_hiding_armed(void);      in common/yghv_guard.h
            # Without this, the forward brace-scan ran past the declaration into
            # an unrelated function's body and attributed it to this name, which
            # reported a violation inside hidecore() as if the predicate had made
            # the call.
            if ($i -gt $nameIdx -and $Lines[$i] -match ';\s*$') { $openIdx = -2; break }
            # style C: the opening brace is at the END of the last parameter line
            #   yghv_protect_on_npf_write_bare(uint64_t a,
            #                                  int *out) {
            # This is neither "brace alone on a line" nor "brace on the name
            # line", so both earlier styles missed it and the function was
            # reported BLIND. The blindness check caught that — which is the
            # point of having it.
            if ($i -gt $nameIdx -and $Lines[$i] -match '\{\s*$') { $openIdx = $i; break }
            # style B: brace anywhere on the declaration line, including a
            # one-line body  'int f(void) { return x; }'
            if ($i -eq $nameIdx -and $Lines[$i] -match '\{') { $openIdx = $i; break }
        }
        if ($openIdx -lt 0) { continue }   # this occurrence had no body; try the next

        $out = New-Object 'System.Collections.Generic.List[object]'
        $depth = 0
        for ($i = $openIdx; $i -lt $Lines.Count; $i++) {
            $out.Add([pscustomobject]@{ Line = $i + 1; Text = $Lines[$i] })
            $depth += ([regex]::Matches($Lines[$i], '\{')).Count
            $depth -= ([regex]::Matches($Lines[$i], '\}')).Count
            if ($depth -le 0) { break }
        }
        return , $out
    }
    return $null
}

# ---- 1. collect every yghv_* name, then test each body for the IRQL gate ----
$candidates = @{}
foreach ($f in $sources) {
    foreach ($l in [IO.File]::ReadAllLines($f.FullName)) {
        foreach ($m in [regex]::Matches($l, '\b(yghv_\w+)\s*\(')) {
            $candidates[$m.Groups[1].Value] = $true
        }
    }
}

$gated = @{}
foreach ($f in $sources) {
    $lines = [IO.File]::ReadAllLines($f.FullName)
    foreach ($name in $candidates.Keys) {
        if ($gated.ContainsKey($name)) { continue }
        $body = Get-BodyLines -Lines $lines -Name $name
        if (-not $body) { continue }
        foreach ($pair in $body) {
            if ($pair.Text -match 'KeAreAllApcsDisabled\(\)') {
                $gated[$name] = $f.Name
                break
            }
        }
    }
}

Write-Host ("[irql] PASSIVE-gated functions: {0}" -f $gated.Count)

# ---- 2. island entry points (DISPATCH + GIF=0) ----
$islandEntries = @(
    'SvHandleVmExit',        # the dispatcher: NPF, MSR, CR, DB, BP all live here
    'SvHandleMsrAccess',
    'SvHandleCpuid',
    'SvHandleVmrun',
    'SvFakeNcr3Resync',
    'yghv_bp_rearm_bare',
    'yghv_protect_arm_page_bare',
    'yghv_protect_reopen_page_bare',
    'yghv_protect_on_npf_write_bare',
    'yghv_protect_fake_bare',
    'yghv_guard_remap_bare',
    'yghv_guard_decoy_bare',
    'yghv_guard_is_hidden',
    'yghv_guard_hiding_armed'
)

$islandHelpers = $islandEntries + @(
    'yghv_guard_fault_is_ours', 'yghv_guard_state_page', 'yghv_guard_target_page',
    'yghv_guard_find', 'yghv_guard_owns_page', 'yghv_guard_probes',
    'yghv_bp_on_npf', 'yghv_bp_on_int3', 'yghv_bp_on_dr',
    'yghv_protect_find_page_bare', 'yghv_protect_fake_restore',
    'yghv_fail_degrade', 'yghv_sentry_escape',
    'yghv_sentry_exit_begin', 'yghv_sentry_exit_end',
    'npt_set_page_perm', 'npt_split_2mb_to_4kb', 'npt_map_page', 'npt_get_entry',
    'yghv_protect_guest_va_to_pa', 'yghv_pa_write', 'yghv_pa_read'
)

# ---- 3. scan island bodies for calls to gated functions ----
$violations = @()
$scanned = 0

foreach ($f in $sources) {
    $lines = [IO.File]::ReadAllLines($f.FullName)
    foreach ($entry in $islandEntries) {
        $body = Get-BodyLines -Lines $lines -Name $entry
        if (-not $body) { continue }
        foreach ($pair in $body) {
            $scanned++
            foreach ($call in [regex]::Matches($pair.Text, '\b(yghv_\w+)\s*\(')) {
                $callee = $call.Groups[1].Value
                if ($callee -eq $entry) { continue }
                if ($islandHelpers -contains $callee) { continue }
                if ($gated.ContainsKey($callee)) {
                    $violations += ("{0} (island) calls {1} (PASSIVE-gated) at {2}:{3}" -f `
                        $entry, $callee, $f.Name, $pair.Line)
                }
            }
        }
    }
}

# ---- 4. blindness check: every island entry must be located ----
$missing = @()
foreach ($entry in $islandEntries) {
    $found = $false
    foreach ($f in $sources) {
        $lines = [IO.File]::ReadAllLines($f.FullName)
        if (Get-BodyLines -Lines $lines -Name $entry) { $found = $true; break }
    }
    if (-not $found) { $missing += $entry }
}

# ---- 3b. an island entry must not itself gate on the IRQL ----
#
# Rule 3 catches the island CALLING a gated function. It cannot catch the other
# half: an island entry whose OWN body gates. That is exactly what
# yghv_bp_rearm_bare did — it was named "_bare", was called from the #DB island
# path, and began with `if (KeAreAllApcsDisabled()) return 0;`, so it refused
# every call from the island it existed to serve. Rule 3 could not see it
# because the gate was in the entry, not in a callee.
#
# The name says island-safe; the body must not disagree.
$selfGated = @()
foreach ($f in $sources) {
    $lines = [IO.File]::ReadAllLines($f.FullName)
    foreach ($entry in $islandEntries) {
        $body = Get-BodyLines -Lines $lines -Name $entry
        if (-not $body) { continue }
        foreach ($pair in $body) {
            if ($pair.Text -match 'KeAreAllApcsDisabled\(\)') {
                $selfGated += ("{0} is declared island-safe but gates on " -f $entry) +
                              ("KeAreAllApcsDisabled() at {0}:{1}" -f $f.Name, $pair.Line)
                break
            }
        }
    }
}

if ($selfGated.Count -gt 0) {
    foreach ($s in $selfGated) { Write-Host ("[irql] FAIL: $s") }
    throw ("IRQL contract FAIL ({0} self-gated island entry(ies)). An entry the " -f $selfGated.Count) +
          'island calls must not refuse to run in the island: its 0 is ' +
          'indistinguishable from "nothing to do".'
}

Write-Host ("[irql] island body lines scanned: {0}" -f $scanned)

if ($missing.Count -gt 0) {
    foreach ($m in $missing) { Write-Host ("[irql] BLIND: island entry '$m' body not found") }
    throw ("IRQL contract FAIL: {0} island entry body(ies) not located — the check " -f $missing.Count) +
          'cannot see this code, so a PASS would be meaningless.'
}

if ($scanned -eq 0) {
    throw 'IRQL contract FAIL: scanned 0 island lines — the scanner is broken, not the code'
}

if ($violations.Count -gt 0) {
    foreach ($v in $violations) { Write-Host ("[irql] FAIL: $v") }
    throw ("IRQL contract FAIL ({0} violation(s)). A gated function returns a " -f $violations.Count) +
          'plausible 0 in the island, which the caller cannot distinguish from ' +
          '"nothing to do" — the 15th crash''s exact mechanism.'
}

Write-Host '[PASS] IRQL contract: island entries call no PASSIVE-gated function'
# ---- 5. every path through a non-void function must return ----
#
# hide_core() was declared uint32_t and had `return 0` on its early-exit path but
# NOTHING at the end: it counted the pages it hid and then fell off, returning
# whatever the register held. Measured on the first clean-boot run — "hid 0
# page(s)" while the counters showed hidden=3. The hiding had worked; the number
# was garbage.
#
# NOTE ON WHY THE FIRST VERSION OF THIS RULE WAS WRONG: it tested the whole body
# for the word "return", and hide_core() does contain `return 0` on its guard
# clause — so the rule reported no defect. "Has a return somewhere" is not the
# property. The property is "cannot reach the closing brace without returning",
# which is control-flow analysis, which is the COMPILER's job:
#
#     clang-cl -Wreturn-type
#
# This rule therefore does the only part that belongs here: it verifies the
# compiler is actually configured to catch it. A warning nobody enables is not a
# check, and this project compiles with /O2 where falling off a non-void function
# is undefined behaviour, not a warning to skim past.
$buildBat = Join-Path $root 'build.bat'
if (Test-Path $buildBat) {
    $bt = [IO.File]::ReadAllText($buildBat)
    $hasFatal = ($bt -match 'Werror=return-type') -or ($bt -match 'WX') -or ($bt -match 'Werror')
    if (-not $hasFatal) {
        Write-Host '[irql] WARN: build.bat does not make -Wreturn-type fatal; a function'
        Write-Host '[irql]       falling off its end compiles silently and returns garbage.'
    }
}

