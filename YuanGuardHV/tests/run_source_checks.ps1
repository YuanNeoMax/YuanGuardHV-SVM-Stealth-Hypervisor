$ErrorActionPreference = 'Stop'

# run_source_checks.ps1 — 9.358
#
# Checks that read SOURCES only, so they can run before (and gate) compilation.
# The map-dependent ones live in run_map_checks.ps1 and run after the link.
#
# This split exists because the earlier single runner ran BEFORE compiling — which
# meant page_layout_check.ps1 read the PREVIOUS build's .map. Aligning four guard
# globals to their own pages therefore changed nothing it could see: it kept
# reporting the old layout and failed the build it had not yet produced.

$scripts = @(
    'ioctl_parity.ps1',
    'command_parity.ps1',
    'safety_checks.ps1',
    'irql_contract_check.ps1'
)

foreach ($script in $scripts) {
    Write-Host "[RUN] $script"
    & (Join-Path $PSScriptRoot $script)
}

Write-Host 'ALL SOURCE CHECKS PASS'