$ErrorActionPreference = 'Stop'

$scripts = @(
    'ioctl_parity.ps1',
    'command_parity.ps1',
    'safety_checks.ps1'
        # 9.347: added from the three failures paid for in crashes and a hang.
        # Both are pure analysis of artifacts the build already produces, so
        # they cost nothing at runtime.
        # page sharing: a table smaller than its page sharing that page with
        # something the driver reads — the 13th crash, 15th crash, 15th hang.
        'page_layout_check.ps1',
        # IRQL contract: a PASSIVE-gated function called from the island
        # returns a plausible 0 the caller cannot tell from 'nothing to do'.
        'irql_contract_check.ps1'
)

foreach ($script in $scripts) {
    Write-Host "[RUN] $script"
    & (Join-Path $PSScriptRoot $script)
}

Write-Host 'ALL STATIC CHECKS PASS'
