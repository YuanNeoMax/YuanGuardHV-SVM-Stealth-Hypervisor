$ErrorActionPreference = 'Stop'

# irql_contract_check.ps1 — 9.353
#
# WHY THIS CHECK EXISTS
# ---------------------
# The 15th crash was an API used in the wrong context:
#
#   yghv_protect_npt_remap_flags() begins with
#       if (KeAreAllApcsDisabled()) return 0;
#
#   An NPF handler runs at DISPATCH + GIF=0, which IS "all APCs disabled". So the
#   guard's ALLOW branch got 0 every time: the page was never restored, the decoy
#   was never installed, and the fall-through violation was unresolvable — 0x3B,
#   and a reboot.
#
# The function is correct in isolation and returns a plausible value, so a caller
# in the wrong context cannot tell "refused, wrong context" from "nothing to do".
# That is the class of error a review misses and a machine teaches.
#
# This check then found a SECOND instance (yghv_bp_rearm_bare, latent in Tier 4.4
# since it was written), a THIRD (the #DB re-hide), and a fourth class became a
# rule (unpaired hides, 9.350).
#
# HOW THE ISLAND SET IS DETERMINED (9.353 — the list is gone entirely)
# -------------------------------------------------------------------
# Earlier versions listed the island entry points by hand, which has one failure
# mode: a new island function is written, not added to the list, and the check
# quietly stops policing it. "Remember to register it" is not a mechanism.
#
# The set is now COMPUTED by reachability, the way a compiler decides which code
# can run in which context:
#
#   1. start at the VMEXIT entry points (the Sv* handlers)
#   2. follow every call edge transitively
#   3. everything reachable IS island code — by construction, not by declaration
#
# A new helper called from the NPF handler becomes island code the moment it is
# called, with nobody having to remember anything.
#
# The explicit list is GONE, not shortened. If the walk cannot find the roots, the
# check FAILS rather than passing by seeing nothing.

$root = Split-Path -Parent $PSScriptRoot
$hv = Join-Path $root 'hv'

if (-not (Test-Path $hv)) {
    throw "irql contract check: $hv not found"
}

$sources = Get-ChildItem $hv -Recurse -Include '*.c', '*.cpp', '*.h', '*.hpp'
if (-not $sources) {
    throw 'irql contract check: no sources found'
}

# ---------------------------------------------------------------------------
# Extractor. Comments are stripped first, because a function discussed in a
# comment was found (twice) to be mistaken for a definition, and a call mentioned
# in prose to be mistaken for a call.
# ---------------------------------------------------------------------------

function Strip-Comments {
    param([string]$Text)
    $t = [regex]::Replace($Text, '/\*[\s\S]*?\*/', ' ')
    $t = [regex]::Replace($t, '//[^\r\n]*', ' ')
    return $t
}

$bodies = @{}
$callees = @{}
$definedIn = @{}

foreach ($f in $sources) {
    $code = Strip-Comments ([IO.File]::ReadAllText($f.FullName))

    foreach ($m in [regex]::Matches($code, '(?m)^[\w\*\s]*?\b(\w+)\s*\([^;{]*?\)\s*\{')) {
        $name = $m.Groups[1].Value
        if ($name -match '^(if|for|while|switch|return|else|sizeof)$') { continue }
        if ($definedIn.ContainsKey($name)) { continue }

        $start = $m.Index
        $depth = 0
        $end = -1
        for ($k = $start; $k -lt $code.Length; $k++) {
            if ($code[$k] -eq '{') { $depth++ }
            elseif ($code[$k] -eq '}') { $depth--; if ($depth -eq 0) { $end = $k; break } }
        }
        if ($end -lt 0) { continue }

        $body = $code.Substring($start, $end - $start)
        $definedIn[$name] = $f.Name
        $bodies[$name] = $body

        $set = @{}
        foreach ($c in [regex]::Matches($body, '\b(\w+)\s*\(')) { $set[$c.Groups[1].Value] = $true }
        $callees[$name] = $set
    }
}

Write-Host ("[irql] functions parsed: {0}" -f $definedIn.Count)

# ---------------------------------------------------------------------------
# 1. VMEXIT roots, discovered rather than listed.
# ---------------------------------------------------------------------------
$roots = @($definedIn.Keys | Where-Object { $_ -match '^Sv' })

if ($roots.Count -lt 3) {
    throw ("IRQL contract FAIL: only {0} Sv* VMEXIT root(s) found. The reachability " -f $roots.Count) +
          'walk cannot start, so a PASS would mean it saw nothing.'
}

# ---------------------------------------------------------------------------
# 2. Transitive closure.
# ---------------------------------------------------------------------------
$island = @{}
$queue = New-Object 'System.Collections.Generic.Queue[string]'
foreach ($r in $roots) { $island[$r] = $true; $queue.Enqueue($r) }

while ($queue.Count -gt 0) {
    $n = $queue.Dequeue()
    if (-not $callees.ContainsKey($n)) { continue }
    foreach ($c in $callees[$n].Keys) {
        if ($island.ContainsKey($c)) { continue }
        if (-not $definedIn.ContainsKey($c)) { continue }
        $island[$c] = $true
        $queue.Enqueue($c)
    }
}

$islandNames = @($island.Keys | Sort-Object)
$bareInIsland = @($islandNames | Where-Object { $_ -match '_bare$' })
Write-Host ("[irql] island set: {0} functions reachable from {1} VMEXIT root(s), {2} of them *_bare" -f `
    $islandNames.Count, $roots.Count, $bareInIsland.Count)

if ($bareInIsland.Count -lt 3) {
    throw ("IRQL contract FAIL: only {0} *_bare function(s) reachable. Island code " -f $bareInIsland.Count) +
          'exists, so the closure is not following the call graph correctly.'
}

# ---------------------------------------------------------------------------
# 3. Rule: no island function may gate on KeAreAllApcsDisabled().
# ---------------------------------------------------------------------------
$violations = @()
foreach ($n in $islandNames) {
    if ($bodies[$n] -match 'KeAreAllApcsDisabled\(\)') {
        $violations += ("{0} is reachable from a VMEXIT root but gates on KeAreAllApcsDisabled() ({1})" -f `
                        $n, $definedIn[$n])
    }
}

if ($violations.Count -gt 0) {
    foreach ($v in $violations) { Write-Host ("[irql] FAIL: $v") }
    throw ("IRQL contract FAIL ({0} island function(s) gated). An NPF/VMEXIT handler " -f $violations.Count) +
          'runs at DISPATCH with GIF=0, which IS "all APCs disabled", so such a function ' +
          'returns 0 on every island call and its caller cannot distinguish that from ' +
          '"nothing to do". Use the *_bare counterpart.'
}

# ---------------------------------------------------------------------------
# 4. Rule: a *_bare must not gate anywhere (the name is a promise), including
#    when it is not currently reachable.
# ---------------------------------------------------------------------------
$lying = @()
foreach ($n in $definedIn.Keys) {
    if ($n -notmatch '_bare$') { continue }
    if ($bodies[$n] -match 'KeAreAllApcsDisabled\(\)') {
        $lying += ("{0} is named island-safe but gates on KeAreAllApcsDisabled() ({1})" -f `
                   $n, $definedIn[$n])
    }
}

if ($lying.Count -gt 0) {
    foreach ($v in $lying) { Write-Host ("[irql] FAIL: $v") }
    throw ("IRQL contract FAIL ({0} self-gated *_bare function(s)). The name says " -f $lying.Count) +
          'island-safe; the body must not disagree.'
}

# ---------------------------------------------------------------------------
# 5. Rule: every hide goes through yghv_guard_do_hide().
# ---------------------------------------------------------------------------
$badHide = @()
foreach ($n in $definedIn.Keys) {
    if ($n -eq 'yghv_guard_do_hide') { continue }
    if ($bodies[$n] -match '\.hidden\s*=\s*1') {
        $badHide += ("{0} assigns .hidden directly ({1})" -f $n, $definedIn[$n])
    }
}

if ($badHide.Count -gt 0) {
    foreach ($b in $badHide) { Write-Host ("[irql] FAIL: $b") }
    throw ("IRQL contract FAIL ({0} unpaired hide site(s)). A hide that sets " -f $badHide.Count) +
          '.hidden without recording its mechanism leaves the page present-but-zeroed, ' +
          'which silently empties whatever lived there (9.350). Route it through ' +
          'yghv_guard_do_hide().'
}

# ---------------------------------------------------------------------------
# 6. Rule: the hide MECHANISM must match the caller's situation.
#
# do_hide takes the mode as a parameter, which makes "set the flag" and "record
# the mechanism" inseparable. It does not make the mechanism CORRECT. The rule:
# a page carrying our own bookkeeping must be hidden with UNMAP, because we have
# to read it back and DECOY would hand us zeros.
# ---------------------------------------------------------------------------
$wrongMode = @()
foreach ($n in $definedIn.Keys) {
    if (-not $bodies.ContainsKey($n)) { continue }
    $body = $bodies[$n]
    # a call site that passes UNMAP while the guard entry it touches is state
    foreach ($m in [regex]::Matches($body, 'yghv_guard_do_hide\([^)]*YGHV_HIDE_DECOY[^)]*\)')) {
        $wrongMode += ("{0} hides with DECOY ({1}) — verify the page is not our own state" -f `
                       $n, $definedIn[$n])
    }
}

if ($wrongMode.Count -gt 0) {
    foreach ($w in $wrongMode) { Write-Host ("[irql] NOTE: $w") }
    # A NOTE, not a FAIL: DECOY is correct for foreign pages and the check cannot
    # tell from the call site alone which kind of page is being hidden. Named so
    # the reader can confirm, rather than silently assumed either way.
}

Write-Host '[PASS] IRQL contract: reachability-derived island set; no gate, no unpaired hide, no lying *_bare'
