$ErrorActionPreference = 'Stop'

# run_map_checks.ps1 — 9.358
#
# Checks that read the LINKER MAP, so they must run after link.exe has written it.
#
# page_layout_check.ps1 was previously in the source-level set, which made it
# analyse a stale map: a change that moved globals onto their own pages produced
# no visible difference, so the check kept failing on the layout of the build
# before last. A check must read the artifact it is judging.

$map = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin\yuanguard_hv.map'
if (-not (Test-Path $map)) {
    throw "map-level checks: $map not found — the linker has not run"
}

$scripts = @(
    'page_layout_check.ps1'
)

foreach ($script in $scripts) {
    Write-Host "[RUN] $script"
    & (Join-Path $PSScriptRoot $script)
}

Write-Host 'ALL MAP CHECKS PASS'