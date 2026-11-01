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
    'exit_rip_contract.ps1',
    # 9.483: a reply-size constant that grows while a client call site does not
    # follow is the shape behind three separate defects in this tree:
    #   sys-guard list  256 bytes vs 8 + 32*80 = 2568
    #   exit-mask        40 on the SET path vs 88 on the read-back path
    #   sys-guard why    40 vs the driver's 64-byte gate
    # All three reported FAILURE while the driver was healthy - the worst way to
    # be wrong, because the user concludes the feature does not work. The two
    # numbers live in different files, in different languages, and neither
    # compiler can see the other side.
    'ioctl_buffer_size.ps1',
    # 9.486: a table that is NOT COMPACTED must not be scanned up to its live
    # count. Five tables in hv/ shared this shape - mem_bp, mem_cache, mem_time,
    # mem_view, mem_watch - and removing a slot in the MIDDLE made every entry
    # above it unreachable, while the count and every list still agreed. The
    # symptom is "a breakpoint/page/cache entry silently stops working", which no
    # error code reports. protect.c is explicitly allowed: it compacts on removal
    # (`pages[i] = pages[count-1]`), so there its count IS a true extent.
    'count_as_extent.ps1',
    # 9.488: every handler that writes the reply buffer must test out_len first.
    # `case IOCTL_YGHV_SECTION_HIDE` wrote `out[3] = idx` unconditionally - under
    # METHOD_BUFFERED the buffer is max(in_len, out_len), so a caller sending only
    # input would have had four qwords written past the allocation. The shipped
    # client always sends 32 bytes, which is why it never fired; a polite client
    # is not an interface property.
    'ioctl_output_bounds.ps1',
    # 9.491: PowerShell turns an undefined variable into $null WITHOUT A WARNING,
    # so a typo'd name makes a guard silently dead. Measured: `mem-bench` tested
    # `$brid` where the variable is `$brd`, so `$null -eq 0` (FALSE) meant the
    # second half of the INVALID test never ran, and a meaningless ratio was
    # printed as a speedup.
    'client_undefined_vars.ps1'
)

foreach ($script in $scripts) {
    Write-Host "[RUN] $script"
    & (Join-Path $PSScriptRoot $script)
}

Write-Host 'ALL SOURCE CHECKS PASS'