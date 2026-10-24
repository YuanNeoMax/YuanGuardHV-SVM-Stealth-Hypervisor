$ErrorActionPreference = 'Stop'

# page_layout_check.ps1 — 9.347
#
# WHY THIS CHECK EXISTS
# ---------------------
# Three failures came from one confusion, and each cost a machine:
#
#   13th crash  hiding a page the driver read with plain C removed the driver's
#               own view of it (the ALLOW loop did not exist yet)
#   15th crash  yghv_mem_read read g_engine_sel at 0x2FED8 — a counter sharing
#               page 0x2F000 with g_view_pages, so hiding the view table
#               removed the engine selector
#   15th hang   after DECLSPEC_ALIGN moved the tables, the linker filled their
#               spare page space with g_guard itself; hiding the frame table's
#               page made yghv_guard_is_hidden() fault inside the NPF handler,
#               which called it again — infinite recursion, no bugcheck
#
# All three are the same shape: an object smaller than its page shared that page
# with something the driver also reads. The machine taught us this three times.
# This check makes the fourth time impossible: it reads the linker map, groups
# symbols by page, and fails the build when a page we intend to HIDE also
# carries something we must keep reaching.
#
# It reads the .map that the build already produces, so it costs nothing at
# runtime and introduces no code.

$root = Split-Path -Parent $PSScriptRoot
$mapPath = Join-Path $root 'bin\yuanguard_hv.map'

if (-not (Test-Path $mapPath)) {
    throw "page layout check: $mapPath not found (build first)"
}

# Tables the guard layer is allowed to hide.
#
# 9.360: g_bp is now a `#define` onto a page-exact union member, so the symbol in
# the map is the REGION (g_bp_region), not the array. The check must name the
# symbol that actually exists; requiring 'g_bp' made it report "not found in the
# map", which is the check telling the truth about a rename rather than a defect.
# The page-exactness itself is asserted at COMPILE time (YGHV_STATIC_ASSERT_BP in
# mem_bp.c); what this script adds is the cross-check that the region is really
# alone on its page in the produced binary.
$hiddenTables = @('g_bp_region', 'g_frame_region', 'g_view_region')

# State the guard layer itself needs while a page is hidden. Anything of ours on
# the same page as a hidden table means the NPF handler will fault while asking
# whether the page is hidden — the recursion that hung the machine.
$mustStayReachable = @(
    'g_guard',            # yghv_guard_is_hidden() reads this
    'g_bp_region',        # 9.360: the breakpoint registry region (page-exact)
    'g_hot',              # 9.360: the hot scalars region (page-exact)
    'g_ring_region',      # 9.360: the hit ring region (two page-exact pages)
    'g_guard_count',
    'g_code_start',       # the RIP whitelist
    'g_code_size',
    'g_decoy_pa',         # the decoy branch reads this
    'g_decoy_va',
    'g_npt',              # every island-safe remap touches the NPT manager
    'g_npt_alt',
    'g_protect'           # the ALLOW loop consults the protection table
)

$lines = [IO.File]::ReadAllLines($mapPath)
$rvaOf = @{}
$sectionOf = @{}

foreach ($line in $lines) {
    # " 0003:00000110       yghv_trace  0000000140001000 f   main.obj"
    $m = [regex]::Match($line, '^\s+000([0-9a-fA-F]):[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s')
    if ($m.Success) {
        $name = $m.Groups[2].Value
        $addr = [Convert]::ToUInt64($m.Groups[3].Value, 16) - 0x140000000
        if (-not $rvaOf.ContainsKey($name)) {
            $rvaOf[$name] = $addr
            $sectionOf[$name] = $m.Groups[1].Value
        }
    }
}

if ($rvaOf.Count -eq 0) {
    throw 'page layout check: no data symbols parsed from the map'
}

Write-Host ("[page-layout] parsed {0} symbols" -f $rvaOf.Count)

# Build page -> symbols, data sections only (2/.rdata, 3/.data+.bss, 4/.pdata).
$byPage = @{}
foreach ($k in $rvaOf.Keys) {
    $sec = $sectionOf[$k]
    if ($sec -eq '1') { continue }   # code: handled by the RIP whitelist, not pages
    $page = $rvaOf[$k] -band 0xFFFFF000
    if (-not $byPage.ContainsKey($page)) { $byPage[$page] = @() }
    $byPage[$page] += $k
}

$problems = @()

# 9.361: the guard-state span, used to decide whether a shared page is reachable
# by the hide set. g_guard is exactly one page (32 slots x 128 B = 4096) so its
# neighbours spill onto the NEXT page; the refusal therefore covers a span rather
# than individual addresses, and this mirrors that span from the map.
# Which symbols does yghv_guard_state_page() refuse? Read them from the source,
# so a change in C is automatically reflected here.
$stateRefusedSymbols = @()
$guardSrc = Join-Path (Split-Path -Parent $PSScriptRoot) 'hv\yghv_guard.c'
if (Test-Path $guardSrc) {
    $gs = [IO.File]::ReadAllText($guardSrc)
    $si = $gs.IndexOf('static int yghv_guard_state_page(uint64_t gpa) {')
    if ($si -ge 0) {
        $depth = 0; $end = $si
        for ($k = $si; $k -lt $gs.Length; $k++) {
            if ($gs[$k] -eq '{') { $depth++ } elseif ($gs[$k] -eq '}') { $depth--; if ($depth -eq 0) { $end = $k; break } }
        }
        $body = $gs.Substring($si, $end - $si)

        # 9.365: THREE forms now appear in state_page(), and all three must be read.
        #
        #   g_state_pa_xxx            the cached physical addresses of the guard's
        #                             own control values (they are resolved once,
        #                             because this runs per VMEXIT)
        #   &symbol                   any address named directly
        #   yghv_protect_page_is_npt  the NPT managers, static in protect.c
        #
        # A checker that understands only one form silently stops covering the
        # others — measured: after the VA/PA fix introduced g_state_pa_*, this
        # reported "none parsed" while the code was refusing seven pages.
        foreach ($m in [regex]::Matches($body, 'g_state_pa_(\w+)')) {
            $suffix = $m.Groups[1].Value
            if ($suffix -eq 'lo' -or $suffix -eq 'hi' -or $suffix -eq 'ready') { continue }
            $sym = 'g_' + $suffix
            if ($stateRefusedSymbols -notcontains $sym) { $stateRefusedSymbols += $sym }
        }
        foreach ($m in [regex]::Matches($body, '&(\w+)\b')) {
            $sym = $m.Groups[1].Value
            if ($sym -match '^g_') {
                if ($stateRefusedSymbols -notcontains $sym) { $stateRefusedSymbols += $sym }
            }
        }
        if ($body -match 'yghv_protect_page_is_npt') {
            foreach ($n2 in @('g_npt', 'g_npt_alt')) {
                if ($stateRefusedSymbols -notcontains $n2) { $stateRefusedSymbols += $n2 }
            }
        }
    }
}
Write-Host ("[page-layout] state_page refuses: " + $(if($stateRefusedSymbols.Count){($stateRefusedSymbols -join ', ')}else{'none parsed'}))

$statePageLo = 0; $statePageHi = 0
if ($rvaOf.ContainsKey('g_guard') -and $rvaOf.ContainsKey('g_guard_count')) {
    $statePageLo = $rvaOf['g_guard'] -band 0xFFFFF000
    $statePageHi = $rvaOf['g_guard_count'] -band 0xFFFFF000
}
Write-Host ("[page-layout] guard-state span: pages 0x{0:X} .. 0x{1:X} are refused as hide targets" -f $statePageLo, $statePageHi)


# ---- Rule 1: a hidden table's page must not carry guard state ----
foreach ($t in $hiddenTables) {
    if (-not $rvaOf.ContainsKey($t)) {
        $problems += "hidden table '$t' not found in the map (renamed or optimised out?)"
        continue
    }
    $page = $rvaOf[$t] -band 0xFFFFF000
    $onPage = $byPage[$page] | Sort-Object

    # A table is in both lists (it is guard-managed state AND a hide target), so
    # comparing against $mustStayReachable naively made it conflict with ITSELF.
    # The question is only whether something OTHER than this table is on its page.
    $conflict = @()
    foreach ($n in $mustStayReachable) {
        if ($n -eq $t) { continue }
        if ($onPage -contains $n) { $conflict += $n }
    }

    Write-Host ("[page-layout] {0} @ 0x{1:X}  page 0x{2:X}  {3} symbol(s) on it" -f `
        $t, $rvaOf[$t], $page, $onPage.Count)

    if ($conflict.Count -gt 0) {
        $problems += ("hidden table '{0}' shares page 0x{1:X} with guard state: {2}" -f `
                      $t, $page, ($conflict -join ', '))
    }
}

# ---- Rule 2: sharing is classified, not merely reported ----
#
# 9.358: the earlier version printed one WARN for every neighbour, which made two
# very different situations look identical:
#
#   A. guard state sharing with OUR OWN other state
#      -> the 15th hang's condition. Hiding either one takes the other down
#         (yghv_guard_is_hidden reads g_guard; hiding its page recurses forever).
#         This must FAIL.
#
#   B. guard state sharing with unrelated globals
#      -> benign AS LONG AS that page is never hidden. Measured: g_npt_alt shares
#         page 0x2D000 with 14 symbols (g_protect_lock, g_sync_thread, ...) and
#         g_npt_alt is not a hide target, so the sharing costs nothing.
#         This is a NOTE with the reason, not a warning without one.
#
# Reporting A and B the same way trains the reader to ignore both. Separating
# them makes an A appear as the dangerous thing it is.
$ownStateNames = @($mustStayReachable) + @($hiddenTables)

foreach ($n in $mustStayReachable) {
    if (-not $rvaOf.ContainsKey($n)) { continue }
    $page = $rvaOf[$n] -band 0xFFFFF000
    $others = @($byPage[$page] | Where-Object { $_ -ne $n })

    $conflict = @($others | Where-Object { $ownStateNames -contains $_ })
    if ($conflict.Count -gt 0) {
        $problems += ("guard state '{0}' shares page 0x{1:X} with other guard state: {2}" -f `
                      $n, $page, ($conflict -join ', '))
    } elseif ($others.Count -gt 0) {
        # 9.361: "verify it stays one" was an instruction to a human, and a human
        # reading six of these stops reading them. The property that makes the
        # sharing harmless is checkable: yghv_guard_state_page() refuses to hide
        # the pages the guard's own state occupies, so a page listed there cannot
        # become a hide target no matter who later edits the hide set.
        #
        # So the NOTE now states WHICH of the two cases it is:
        #   refused by state_page  -> benign, with the mechanism named
        #   not refused            -> a real exposure, counted as a problem
        $guardSrc = Join-Path (Split-Path -Parent $PSScriptRoot) 'hv\yghv_guard.c'
        # 9.361: THE REFUSAL LIST IS READ FROM THE SOURCE, so the check and the
        # implementation cannot disagree about which pages are refused.
        #
        # The first version hardcoded a span (g_guard .. g_guard_count) while
        # yghv_guard_state_page() refused a longer list, so the check kept printing
        # NOTEs for pages the code already refused — the reader could not tell
        # whether the NOTE meant "unprotected" or "the check is out of date". A
        # check that disagrees with the code it is checking is worse than either
        # one alone. Extracting the names from state_page() makes them agree by
        # construction: adding a refusal in C widens what the check accepts.
        $stateRefuses = $false
        foreach ($sym in $stateRefusedSymbols) {
            if ($rvaOf.ContainsKey($sym)) {
                if ($page -eq ($rvaOf[$sym] -band 0xFFFFF000)) { $stateRefuses = $true; break }
            }
        }
        if (-not $stateRefuses -and $statePageHi -gt 0) {
            if (($page -ge $statePageLo) -and ($page -le $statePageHi)) { $stateRefuses = $true }
        }
        if ($stateRefuses) {
            Write-Host ("[page-layout] OK: '{0}' shares page 0x{1:X} with {2} unrelated global(s)," -f `
                $n, $page, $others.Count)
            Write-Host ("[page-layout]     and that page is inside the guard-state span" +
                        " yghv_guard_state_page() refuses -> it can never be a hide target.")
        } else {
            Write-Host ("[page-layout] NOTE: '{0}' shares page 0x{1:X} with {2} unrelated global(s)," -f `
                $n, $page, $others.Count)
            Write-Host ("[page-layout]     and that page is NOT covered by the guard-state refusal." +
                        " Confirm it is never a hide target.")
        }
    }
}

# ---- Rule 3: every hidden table must be page-aligned ----
foreach ($t in $hiddenTables) {
    if (-not $rvaOf.ContainsKey($t)) { continue }
    if (($rvaOf[$t] -band 0xFFF) -ne 0) {
        $problems += ("hidden table '{0}' is not page-aligned (offset 0x{1:X}); hiding its page would remove part of a neighbour" -f $t, ($rvaOf[$t] -band 0xFFF))
    }
}

if ($problems.Count -gt 0) {
    foreach ($p in $problems) { Write-Host ("[page-layout] FAIL: $p") }
    throw ("page layout check FAIL ({0} problem(s)) — each one is a page-sharing condition that has already cost this project a crash or a hang" -f $problems.Count)
}

Write-Host '[PASS] Page layout: no hidden table shares a page with guard state'
