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
$hiddenTables = @('g_bp', 'g_frame', 'g_view_pages')

# State the guard layer itself needs while a page is hidden. Anything of ours on
# the same page as a hidden table means the NPF handler will fault while asking
# whether the page is hidden — the recursion that hung the machine.
$mustStayReachable = @(
    'g_guard',            # yghv_guard_is_hidden() reads this
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

# ---- Rule 1: a hidden table's page must not carry guard state ----
foreach ($t in $hiddenTables) {
    if (-not $rvaOf.ContainsKey($t)) {
        $problems += "hidden table '$t' not found in the map (renamed or optimised out?)"
        continue
    }
    $page = $rvaOf[$t] -band 0xFFFFF000
    $onPage = $byPage[$page] | Sort-Object

    $conflict = @()
    foreach ($n in $mustStayReachable) {
        if ($onPage -contains $n) { $conflict += $n }
    }

    Write-Host ("[page-layout] {0} @ 0x{1:X}  page 0x{2:X}  {3} symbol(s) on it" -f `
        $t, $rvaOf[$t], $page, $onPage.Count)

    if ($conflict.Count -gt 0) {
        $problems += ("hidden table '{0}' shares page 0x{1:X} with guard state: {2}" -f `
                      $t, $page, ($conflict -join ', '))
    }
}

# ---- Rule 2: the guard's own state must each occupy a page by itself ----
foreach ($n in $mustStayReachable) {
    if (-not $rvaOf.ContainsKey($n)) { continue }
    $page = $rvaOf[$n] -band 0xFFFFF000
    $others = @($byPage[$page] | Where-Object { $_ -ne $n })
    if ($others.Count -gt 0) {
        # Report but do not fail: this is the condition that produced the hang,
        # so it is a warning with the page named rather than silence.
        Write-Host ("[page-layout] WARN: guard state '{0}' shares page 0x{1:X} with {2}" -f `
            $n, $page, ($others -join ', '))
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
