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
    'irql_contract_check.ps1',
    # 9.469: every #VMEXIT case in SvHandleVmExit must satisfy its RIP contract.
    # VMMCALL violated it for a long time and nothing noticed, because the
    # violation has no failure mode - the machine boots, runs, unloads cleanly,
    # and a counter moves. The only symptom is that a parameter has no effect,
    # which reads as "that instruction is a no-op here" (9.464 concluded exactly
    # that about hlt, wrongly). This check is what makes such a hole visible.
    'exit_rip_contract.ps1'
)

foreach ($script in $scripts) {
    Write-Host "[RUN] $script"
    & (Join-Path $PSScriptRoot $script)
}

Write-Host 'ALL SOURCE CHECKS PASS'