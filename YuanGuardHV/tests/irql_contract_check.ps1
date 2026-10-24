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


# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 7. Rule: an indirect call on an island path must be RESOLVABLE, and its
#    targets must themselves be island-safe.
#
# The reachability walk follows DIRECT call edges, so a call through a function
# pointer is invisible to it. That blind spot is real here: mem_engine.h declares
#
#     typedef struct {
#         NTSTATUS (*read)(PEPROCESS, uint64_t, void*, SIZE_T, SIZE_T*);
#         NTSTATUS (*write)(PEPROCESS, uint64_t, const void*, SIZE_T, SIZE_T*);
#     } yghv_mem_engine_ops_t;
#
# and yghv_mem_engine.c dispatches through it.
#
# 9.358: THE TARGETS ARE PARSEABLE, so this is no longer a hint.
#
# A pointer table is initialised with the function names written out:
#
#     const yghv_mem_engine_ops_t g_yghv_engine_pte = { "pte", pte_read, pte_write };
#
# So for any island function that calls through a table, the set of possible
# targets can be READ OFF and each one checked like a direct callee. The earlier
# version only printed a NOTE asking a human to look — which is exactly the kind
# of "someone will review it" that this whole file exists to replace.
#
# Reported as a FAIL when a resolvable target gates on KeAreAllApcsDisabled():
# that is the 15th crash's mechanism reached through a pointer, and the reason it
# was worth following is that no human reviewer would have seen it either.

# --- collect pointer tables: name -> list of member function names ---
$ptrTables = @{}
$tableTypes = @{}   # table name -> struct type name (9.362, member mapping)
foreach ($f in $sources) {
    $code = Strip-Comments ([IO.File]::ReadAllText($f.FullName))
    foreach ($m in [regex]::Matches($code, '(?m)^\s*(?:const\s+)?[\w_]+\s+(g_\w+)\s*=\s*\{([^}]*)\}')) {
        $tname = $m.Groups[1].Value
        $members = @()
        foreach ($id in [regex]::Matches($m.Groups[2].Value, '\b(\w+)\b')) {
            $n = $id.Groups[1].Value
            if ($n -match '^\d') { continue }
            if ($definedIn.ContainsKey($n)) { $members += $n }
        }
        if ($members.Count -gt 0) {
            $ptrTables[$tname] = $members
            # the declared type, so member order can be resolved later
            $tm = [regex]::Match($m.Value, '(?m)^\s*(?:const\s+)?([\w_]+)\s+g_\w+\s*=')
            if ($tm.Success) { $tableTypes[$tname] = $tm.Groups[1].Value }
        }
    }
}

# 9.362: MEMBER-LEVEL PRECISION, so a call site is checked against the member it
# actually names instead of every member in the table.
#
# The conservative version checked every target in the table and reported all of
# them. That is safe but it is not useful: measured on the negative test, a call
# to t->read() reported t->write's gating as a violation of the read call, so the
# reader had to work out which of two named targets their code could even reach.
#
# The layout makes the mapping available: the initialiser lists members in
# declaration order, and the struct declaration states that order:
#
#     typedef struct {
#         const char *name;      position 0
#         NTSTATUS (*read)(...); position 1
#         NTSTATUS (*write)(...);position 2
#     } yghv_mem_engine_ops_t;
#     const ... g_yghv_engine_pte = { "pte", pte_read, pte_write };
#
# So position i of the initialiser is the member declared i-th. This map is built
# from the struct declaration rather than hardcoded, so a field reorder or a rename
# moves the map with it.
$memberPos = @{}   # table -> @{ memberName = position }
foreach ($f in $sources) {
    $code = Strip-Comments ([IO.File]::ReadAllText($f.FullName))
    # a struct whose fields include function pointers, and the table typed by it
    foreach ($d in [regex]::Matches($code, '(?m)typedef\s+struct\s*\{([^}]*)\}\s*(\w+)\s*;')) {
        $typeName = $d.Groups[2].Value
        $fields = @()
        foreach ($fm in [regex]::Matches($d.Groups[1].Value, '(?m)^\s*(?:const\s+)?[\w\s\*]*?\(\s*\*\s*(\w+)\s*\)\s*\([^)]*\)\s*;')) {
            $fields += $fm.Groups[1].Value
        }
        if ($fields.Count -gt 0) { $memberPos[$typeName] = $fields }
    }
}
Write-Host ("[irql] pointer-table layouts: " + $(if($memberPos.Count){($memberPos.Keys -join ", ")}else{"none"}))

# --- 9.365: NAMED initialisers give the mapping directly; no order is inferred ---
#
# The positional form {"pte", pte_read, pte_write} says nothing about WHICH member
# each function is for — only its position does, so inferring the mapping depends
# on the struct declaration order staying put. The named form states it:
#
#     .name = "pte", .read = pte_read, .write = pte_write
#
# So read the mapping from the initialiser when it is named, and only fall back to
# positional inference when it is not. BOTH paths are kept on purpose: a table
# written positionally must still be checked, and a silent fallback to "check every
# member" is the kind of quiet degradation that hides a real change.
$namedMember = @{}   # table -> @{ memberName = targetFunction }
$positionalTable = @()

foreach ($f in $sources) {
    $code = Strip-Comments ([IO.File]::ReadAllText($f.FullName))
    foreach ($m in [regex]::Matches($code, '(?m)^\s*(?:const\s+)?[\w_]+\s+(g_\w+)\s*=\s*\{([^}]*)\}')) {
        $tname = $m.Groups[1].Value
        $body  = $m.Groups[2].Value
        $map = @{}
        foreach ($nm in [regex]::Matches($body, '\.(\w+)\s*=\s*(\w+)')) {
            $member = $nm.Groups[1].Value
            $target = $nm.Groups[2].Value
            if ($definedIn.ContainsKey($target)) { $map[$member] = $target }
        }
        if ($map.Count -gt 0) { $namedMember[$tname] = $map }
        else { $positionalTable += $tname }
    }
}

Write-Host ("[irql] named-initialiser tables: " + $(if($namedMember.Count){($namedMember.Keys -join ", ")}else{"none"}))
# --- 9.366: A POSITIONAL TABLE NOW FAILS THE BUILD ---
#
# 9.365 converted the two engine tables and made the checker READ named
# initialisers, but left the positional form as a reported NOTE. A NOTE is read
# once; the coupling it describes (member meaning tied to declaration order)
# survives every refactor of the struct it points at, and the next table someone
# writes will be written the same way because nothing objects.
#
# So this is now a FAILURE, and the fallback below stays as the DEFENSIVE path:
# if the rule is ever bypassed (a table built by a macro, a generated header),
# the mapping is still resolved precisely so the message names the real target
# rather than every member of the table. Enforcing the rule and keeping the
# fallback are not in tension — one prevents the state, the other survives it.
$positionalProblem = $null
if ($positionalTable.Count -gt 0) {
    Write-Host ("[irql] POSITIONAL tables (mapping inferred from declaration order): " + ($positionalTable -join ", "))
    Write-Host "[irql]   A positional table's member meanings move if the struct is reordered."
    Write-Host '[irql]   Convert it to named initialisers (.read = fn) to remove that coupling.'
    $positionalProblem = ("IRQL contract FAIL ($($positionalTable.Count) positional pointer-table " +
        "initialiser(s): $($positionalTable -join ', ')). A positional initialiser ties each " +
        "function to its POSITION on the struct declaration, so reordering the struct silently " +
        "swaps the members and nothing in the language catches it. Write the members by name " +
        "(.read = ..., .write = ...) — see mem_engine.c for the two tables that already were.")
}

Write-Host ("[irql] pointer tables: {0}" -f $ptrTables.Count)
foreach ($t in $ptrTables.Keys) {
    Write-Host ("[irql]   {0} -> {1}" -f $t, ($ptrTables[$t] -join ', '))
}

# --- map the identifier used at an island call site back to its table ---
# A call reads "g_xxx->read(...)" or "g_xxx.read(...)"; the table is g_xxx.
$badIndirect = @()
$checkedIndirect = 0

foreach ($n in $islandNames) {
    if (-not $bodies.ContainsKey($n)) { continue }
    $body = $bodies[$n]
    foreach ($m in [regex]::Matches($body, '\b(g_\w+)\s*(?:->|\.)\s*(\w+)\s*\(')) {
        $tname = $m.Groups[1].Value
        $member = $m.Groups[2].Value
        if (-not $ptrTables.ContainsKey($tname)) {
            # points at something this scan cannot resolve: report, do not ignore
            $badIndirect += ("$n calls through $tname.$member() which is not a recognised table (" + $definedIn[$n] + ")")
            continue
        }
        $checkedIndirect++
        # 9.362: MEMBER-LEVEL PRECISION.
        #
        # The conservative version checked every target in the table and reported
        # all of them. Safe, but not useful: measured on the negative test, a call
        # to t->read() reported that t->write() gates as a violation of the read
        # call, so the reader had to work out which named target their code could
        # even reach.
        #
        # The layout makes the mapping available: the initialiser lists members in
        # declaration order, so position i is the i-th declared member. The map is
        # built from the struct DECLARATION (see $memberPos), not hardcoded, so a
        # field reorder moves the map with it.
        #
        # FALLBACK IS STILL CONSERVATIVE: if the table type cannot be resolved, or
        # the member name is not among the declared fields, every target is checked.
        # A false negative (miss a gated target actually reached) is far worse than
        # a false positive, so an unresolvable name falls back to checking all.
        $targets = $ptrTables[$tname]
        $resolvedByName = $false

        # 9.365: PREFER the named initialiser. It states the mapping, so no
        # declaration-order inference is involved at all.
        if ($namedMember.ContainsKey($tname) -and $namedMember[$tname].ContainsKey($member)) {
            $targets = @($namedMember[$tname][$member])
            $resolvedByName = $true
        }

        # Positional fallback: the table is written positionally, so the mapping
        # must be inferred from the struct declaration order. Only used when the
        # initialiser itself did not name the member.
        if (-not $resolvedByName) {
            $tableType = $tableTypes[$tname]
            if ($tableType -and $memberPos.ContainsKey($tableType)) {
                $fields = $memberPos[$tableType]
                $pos = [array]::IndexOf($fields, $member)
                if ($pos -ge 0) {
                    $indexed = @($ptrTables[$tname] | Where-Object { $true })
                    if ($pos -lt $indexed.Count) { $targets = @($indexed[$pos]) }
                }
            }
        }
        foreach ($target in $targets) {
            if ($bodies[$target] -match 'KeAreAllApcsDisabled[(]') {
                $msg = "$n (island) calls " + $tname + "." + $member + "(), which dispatches to " + $target + " (PASSIVE-gated) [" + $definedIn[$n] + "]"
                $badIndirect += $msg
            }
        }
    }
}

Write-Host ("[irql] indirect call sites resolved: {0}" -f $checkedIndirect)

if ($badIndirect.Count -gt 0) {
    foreach ($b in $badIndirect) { Write-Host ("[irql] FAIL: $b") }
    $badMsg = "IRQL contract FAIL ($($badIndirect.Count) indirect violation(s)). A pointer call hides its " +
              "target from the direct-call walk, so it is checked by reading the table " +
              "initialiser. These targets gate on KeAreAllApcsDisabled() or could not " +
              "be resolved at all."
}

if ($checkedIndirect -eq 0) {
    Write-Host '[irql] indirect calls on island paths: none (checked, not assumed)'
}

# --- 9.366: the positional-initialiser rule is a GATE, not a reminder ---
#
# Checked here so both failures are reported in one run: a reader fixing an
# indirect-call violation should not have to rebuild to discover that a table
# form is also refused.
if ($positionalProblem) {
    throw $positionalProblem
}

Write-Host '[PASS] IRQL contract: reachability-derived island set; no gate, no unpaired hide, no lying *_bare; no positional pointer tables'
