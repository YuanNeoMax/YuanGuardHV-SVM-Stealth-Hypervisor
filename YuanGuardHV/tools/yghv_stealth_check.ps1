param(
    [string]$ServiceName = 'yghvpa',
    [string]$DeviceName  = 'YuanGuardHV'
)


<#
  9.454 (Tier 7.1): the exposure audit, parameterised by service name.

  WHY THIS FILE CHANGED, AND WHY IT NOW TAKES AN ARGUMENT

  The original version hard-coded the service name `yuanguard` and checked five
  things. Two problems, both of which made its answers useless for Tier 7.1:

    1. The service this tree actually deploys is `yghvpa` (see the deploy path in
       RESUME_PROMPT). Against the real machine the script reported "CLEAN" for
       every service-related row because it was looking up a name that does not
       exist - the classic shape of a monitor whose scope does not contain the
       thing it claims to monitor.

    2. It had a fixed, hand-typed list. Tier 7.1 lists SIX surfaces, and a list
       that must be edited every time a surface is added is a list that silently
       under-reports. The surfaces below are named once, here, and the script
       reports each one as VISIBLE or CLEAN with the evidence attached.

  WHAT IT CANNOT SEE, SAID UP FRONT
  PsLoadedModuleList is not readable from user mode. This script therefore
  reports the module-list row as UNKNOWN rather than CLEAN - "I could not look"
  and "I looked and it was clean" are different answers, and only one of them is
  honest here. The kernel-side check for that row is `loader_stealth` plus a kd
  `!driver`, which is 9.453's open item.
#>

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class YghvStealthNative
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFile(
        string lpFileName,
        uint dwDesiredAccess,
        uint dwShareMode,
        IntPtr lpSecurityAttributes,
        uint dwCreationDisposition,
        uint dwFlagsAndAttributes,
        IntPtr hTemplateFile);

    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr hObject);
}
'@

$visible = 0
$unknown = 0

function Report {
    param([string]$Name, [string]$State, [string]$Detail)
    switch ($State) {
        'VISIBLE' {
            $script:visible++
            Write-Host ("[VISIBLE] {0} : {1}" -f $Name, $Detail)
        }
        'UNKNOWN' {
            $script:unknown++
            Write-Host ("[UNKNOWN] {0} : {1}" -f $Name, $Detail)
        }
        default {
            Write-Host ("[CLEAN]   {0}" -f $Name)
        }
    }
}

$service = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'" `
    -ErrorAction SilentlyContinue
Report -Name ("service {0}" -f $ServiceName) `
    -State $(if ($null -ne $service) { 'VISIBLE' } else { 'CLEAN' }) `
    -Detail ("State={0} Start={1} Path={2}" -f $service.State, $service.StartMode,
        $service.PathName)

$driver = Get-CimInstance Win32_SystemDriver -Filter "Name='$ServiceName'" `
    -ErrorAction SilentlyContinue
Report -Name ("kernel driver {0}" -f $ServiceName) `
    -State $(if ($null -ne $driver) { 'VISIBLE' } else { 'CLEAN' }) `
    -Detail ("State={0} Path={1}" -f $driver.State, $driver.PathName)

$regPath = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
Report -Name 'registry service key' `
    -State $(if (Test-Path -LiteralPath $regPath) { 'VISIBLE' } else { 'CLEAN' }) `
    -Detail $regPath

# Tier 7.1's module-list row. Not readable from user mode - see the header note.
Report -Name 'PsLoadedModuleList entry' -State 'UNKNOWN' `
    -Detail 'not observable from user mode; needs loader_stealth + kd !driver'

$files = @(
    'C:\yuanguard_hv.sys',
    'C:\Windows\yghv_progress.log',
    'C:\Windows\yghv_ioctl.log',
    'C:\Windows\yghv_hook.log'
)
foreach ($file in $files) {
    Report -Name ("file {0}" -f $file) -Found (Test-Path -LiteralPath $file) `
        -Detail $file
}

$h = [YghvStealthNative]::CreateFile(
    ("\\.\{0}" -f $DeviceName), 0, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
if ($h -ne [IntPtr]::Zero -and $h -ne [IntPtr]::new(-1)) {
    [YghvStealthNative]::CloseHandle($h) | Out-Null
    Report -Name ("control device \\.\{0}" -f $DeviceName) -State 'VISIBLE' `
        -Detail 'CreateFile opened successfully'
} else {
    Report -Name ("control device \\.\{0}" -f $DeviceName) -State 'CLEAN' -Detail ''
}

# Tier 7.1: the shared window is DELIBERATELY guest-visible (see mem_share.c).
# Audited here so it is a stated exception rather than a contradiction that
# somebody finds later while comparing 7.1's exclusion list against reality.
Report -Name 'shared window page (Tier 2.4)' -State 'VISIBLE' `
    -Detail 'by design: not a leak, but must be in the 7.1 exclusion diff'

Write-Host ("Stealth check complete: visible={0} unknown={1}" -f $visible, $unknown)

# REV-033: expose a non-zero exit code when traces are found so callers can
# gate on the result (e.g. CI/rollout checks) instead of parsing the text.
if ($visible -gt 0) {
    Write-Host "[FAIL] $visible visible trace(s) found"
    exit 1
}
if ($unknown -gt 0) {
    Write-Host "[PARTIAL] $unknown surface(s) could not be observed from user mode"
    exit 2
}
exit 0
