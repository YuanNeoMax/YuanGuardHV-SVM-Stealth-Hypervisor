# YuanGuardHV Java/JNI IOCTL client

Build:

```
build.bat
```

Java client usage (all commands require the yuanguard driver to be loaded):

```
run.bat state
run.bat set-target <pid>
run.bat add-page <hex_va>
run.bat remove-page <hex_va>
run.bat start
run.bat stop
run.bat target
run.bat list-targets
run.bat list-pages
run.bat list-hooks
run.bat install-hook <name|hex_va> [hook_id]
run.bat remove-hook <hook_id>
run.bat clear
run.bat config [auto-disarm <0|1> | deny-status <hex>]
run.bat list-java
run.bat protect <pid> [maxPages]
run.bat unprotect
run.bat scan <pid> [maxPages]
```

PowerShell client usage:

```
yghv_ctl.ps1 state
yghv_ctl.ps1 lasthit
yghv_ctl.ps1 set-target <pid>
yghv_ctl.ps1 add-page <hex_va>
yghv_ctl.ps1 remove-page <hex_va>
yghv_ctl.ps1 start
yghv_ctl.ps1 stop
yghv_ctl.ps1 target
yghv_ctl.ps1 list-targets
yghv_ctl.ps1 list-pages
yghv_ctl.ps1 list-hooks
yghv_ctl.ps1 install-hook <name|hex_va> [hook_id]
yghv_ctl.ps1 remove-hook <hook_id>
yghv_ctl.ps1 clear
yghv_ctl.ps1 config [auto-disarm <0|1> | deny-status <hex>]
yghv_ctl.ps1 set-auto-start
yghv_ctl.ps1 unset-auto-start
yghv_ctl.ps1 harden-service
yghv_ctl.ps1 unharden-service
yghv_ctl.ps1 selftest
yghv_ctl.ps1 selftest-abort
yghv_ctl.ps1 exit-test
yghv_ctl.ps1 protect-page <pid> <hex_va>
yghv_ctl.ps1 unprotect-page <pid> <hex_va>
yghv_ctl.ps1 mmf-open <path>
yghv_ctl.ps1 mmf-loop <path> <sec> [info]
yghv_ctl.ps1 mmf-hold <path> <sec> [info]
yghv_ctl.ps1 mmf-write <path>
yghv_ctl.ps1 scan-pid <pid> [count]
yghv_ctl.ps1 wpm-write <pid> <hex_va> <hex_val>
yghv_ctl.ps1 wpm-read <pid> <hex_va>
yghv_ctl.ps1 config-fake <0|1>
yghv_ctl.ps1 config-fake-tlb [0|1|2|3|4]
yghv_ctl.ps1 config-sync [0|1] [interval_ms]
yghv_ctl.ps1 mem-pte reset | mem-pte <pid> <hex_va>
yghv_ctl.ps1 mem-read <pid> <hex_va> <type> [count]
yghv_ctl.ps1 mem-write <pid> <hex_va> <type> <value>
yghv_ctl.ps1 mem-engine [auto|api|pte|reset]
yghv_ctl.ps1 mem-scan <pid> <hex_pattern> [max] [writable] [exec]
yghv_ctl.ps1 mem-selfmap [init|stop|reset]
yghv_ctl.ps1 mem-coop [reset]
yghv_ctl.ps1 mem-scan-rev <pid> <hex_pa> [max] [start] [end]
```

`protect` sets the target, enumerates committed pages of the real Java process,
adds up to `maxPages` (driver cap is 64), starts protection and prints state.
`unprotect` stops protection and clears the calling process's target pages
without touching hooks. `scan` only enumerates committed pages and prints them;
it does not arm or protect anything.

> REV-019: `protect <pid>` only works when `pid` is the YghvCtl process's own
> PID — the driver binds ADD_PAGE to the *caller's* CR3 (P0 self-targeting), so
> protecting an arbitrary enumerated PID returns ACCESS_DENIED on every page.
> Use `list-java` to find this process's PID, or `scan <pid>` (read-only) for
> other processes. 206-C4 supersedes this for the PowerShell client:
> `protect-page <pid> <hex_va>` (fn 0x810) resolves the target slot by pid,
> so an external controller can arm a page inside another registered target.

Boundary: the current hypervisor runs a synthetic resident guest; real process
code still executes natively, so NPT/vmmcall interception of the real process
requires the later OS-as-guest milestone. This client validates the real-target
configuration, page translation and arming path on the physical machine.
