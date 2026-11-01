$ErrorActionPreference = 'Stop'

# service_name_check.ps1 -- 9.501
#
# WHY: four commands in the client called
#
#     sc.exe config   yuanguard start= auto
#     sc.exe sdshow   yuanguard
#     sc.exe sdset    yuanguard <sddl>
#
# The service has always been named `yghvpa`; the registry has never held a key
# called `yuanguard` (measured: `sc config yuanguard` -> 1060, while `sc sdshow
# yghvpa` returns a normal SDDL). So those four commands never worked, and their
# failure looked like an environment problem rather than a source problem.
#
# Nothing caught it for the same reason nothing catches most of this tree's
# defects: every existing checker reads a DIFFERENT part of the file.
#   ioctl_parity        -> IOCTL codes
#   command_parity      -> command NAMES
#   client_undefined_vars -> variable names
#   ioctl_buffer_size   -> buffer lengths
# None of them looks at an ARGUMENT to an external tool.
#
# THE CONTRACT: in tools\yghv_ctl.ps1, the first non-switch argument of every
#
#     <sc invocation> <verb> <name> ...
#
# must be a VARIABLE ($...) -- which today means $YghvService, resolved once from
# -ServiceName -- and never a quoted or bare literal.
#
# Comment lines are skipped: they legitimately record the old broken spelling,
# and a checker that flags its own documentation teaches people to ignore it.
#
# A checker that cannot fail is worthless, so this one carries a self-test: it
# plants the ORIGINAL bug into a throwaway copy and requires a FAIL. The defini-
# tion of "an sc invocation" lives in ONE place (Get-ScInvocation) and the
# self-test reuses it, so the two cannot drift apart.

$root = Split-Path -Parent $PSScriptRoot
$client = Join-Path $root 'tools\yghv_ctl.ps1'

if (-not (Test-Path $client)) {
    throw "service_name_check: cannot find $client"
}

# The verbs that take the service name as their first operand. Listed explicitly
# so a new verb is a decision, not a silent pass.
$SC_VERBS = @(
    'qc', 'query', 'config', 'sdshow', 'sdset', 'start', 'stop', 'delete',
    'create', 'description', 'failure', 'triggerinfo'
)

function Get-ScInvocation {
    param([string[]] $Lines)

    $hits = @()
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $line = $Lines[$i]
        $trimmed = $line.Trim()

        # comments are skipped on purpose (see the header)
        if ($trimmed.StartsWith('#')) { continue }
        if ($trimmed.StartsWith('<#')) { continue }

        # an invocation looks like:  $sc <verb> <operand>   |   sc.exe <verb> <operand>
        $m = [regex]::Match($line, '(?i)(\$sc|\$scExe|sc\.exe)\s+(' + ($SC_VERBS -join '|') + ')\s+(\S+)')
        if (-not $m.Success) { continue }

        $hits += [pscustomobject]@{
            Line   = $i + 1
            Verb   = $m.Groups[2].Value
            Operand = $m.Groups[3].Value
            Text   = $trimmed
        }
    }
    return $hits
}

$lines = Get-Content $client
$invocations = Get-ScInvocation -Lines $lines

if ($invocations.Count -eq 0) {
    throw 'service_name_check: found no sc invocation at all - the parser is stale, not the code'
}

$bad = @()
foreach ($inv in $invocations) {
    # 9.501b: NOT -match '$'. In a regex `$` is the end-of-string anchor and
    # matches every string, so the original version passed the literal
    # 'yuanguard' as if it were a variable. A literal leading dollar sign is a
    # character test, not a pattern test.
    if ($inv.Operand.StartsWith('$')) { continue }     # a variable: resolved at runtime
    $bad += ('tools\yghv_ctl.ps1:{0} sc {1} receives the literal {2} - the service name must come from $YghvService' -f
             $inv.Line, $inv.Verb, $inv.Operand)
}

Write-Host ("  {0} sc invocation(s) checked; {1} pass the name as a variable" -f
            $invocations.Count, ($invocations.Count - $bad.Count))

if ($bad.Count -gt 0) {
    foreach ($b in $bad) { Write-Host "[FAIL] $b" }
    throw "service_name_check: $($bad.Count) sc invocation(s) hard-code a service name"
}

Write-Host '[PASS] service names: every sc invocation takes the name from a variable'

# ---------------------------------------------------------------------------
# SELF-TEST: plant the original bug in a copy and require a FAIL.
# ---------------------------------------------------------------------------

$selfTestFailures = @()
if (-not $env:YGHV_SVCNAME_SELFTEST_CHILD) {
    $copyRoot = $null
    try {
        $copyRoot = Join-Path $env:TEMP ('svcname_selftest_' + [guid]::NewGuid().ToString('N'))
        $copyTools = Join-Path $copyRoot 'tools'
        New-Item -ItemType Directory -Path $copyTools -Force | Out-Null
        Copy-Item -Path $client -Destination (Join-Path $copyTools 'yghv_ctl.ps1') -Force

        $copyClient = Join-Path $copyTools 'yghv_ctl.ps1'
        $copyLines = Get-Content $copyClient

        # find a real `$sc <verb> $YghvService` line and swap the operand back to
        # the original literal spelling
        $target = -1
        foreach ($inv in (Get-ScInvocation -Lines $copyLines)) {
            if ($inv.Operand -match 'YghvService') { $target = $inv.Line - 1; break }
        }
        if ($target -lt 0) {
            $selfTestFailures += 'no `$sc <verb> $YghvService` line to mutate - update the self-test'
        }
        else {
            $copyLines[$target] = $copyLines[$target].Replace('$YghvService', 'yuanguard')
            Set-Content -Path $copyClient -Value $copyLines -Encoding UTF8

            # re-run THIS script against the copy, via the same entry point the
            # real run uses. The path is injected by copying the tests folder
            # structure: the child needs a 'tools\yghv_ctl.ps1' beside a 'tests'.
            $copyTests = Join-Path $copyRoot 'tests'
            New-Item -ItemType Directory -Path $copyTests -Force | Out-Null
            Copy-Item -Path $PSCommandPath -Destination (Join-Path $copyTests 'service_name_check.ps1') -Force

            $env:YGHV_SVCNAME_SELFTEST_CHILD = '1'
            $logPath = Join-Path $env:TEMP ('svcname_selftest_' + [guid]::NewGuid().ToString('N') + '.log')
            $cmdLine = '/c powershell -NoProfile -ExecutionPolicy Bypass -File "' +
                       (Join-Path $copyTests 'service_name_check.ps1') + '" > "' + $logPath + '" 2>&1'
            $childExit = 0
            try {
                $null = & cmd.exe $cmdLine
                $childExit = $LASTEXITCODE
            }
            catch {
                $childExit = 1
            }

            $all = ''
            if (Test-Path $logPath) {
                $all = Get-Content $logPath -Raw
                Remove-Item $logPath -Force -ErrorAction SilentlyContinue
            }
            $env:YGHV_SVCNAME_SELFTEST_CHILD = $null

            if ($childExit -eq 0) {
                $selfTestFailures += 'the mutated copy PASSED - the checker accepted a hard-coded service name'
            }
            elseif ($all -notmatch 'yuanguard') {
                $selfTestFailures += "the mutated copy failed but never named the literal (child output: $($all.Trim()))"
            }
            elseif ($all -notmatch 'must come from') {
                $selfTestFailures += "the mutated copy failed but not for the reason (child output: $($all.Trim()))"
            }
            else {
                Write-Host '[PASS] self-test: planted hard-coded service name was caught'
            }
        }
    }
    finally {
        if ($copyRoot -and (Test-Path $copyRoot)) { Remove-Item $copyRoot -Recurse -Force -ErrorAction SilentlyContinue }
        $env:YGHV_SVCNAME_SELFTEST_CHILD = $null
    }
}

if ($selfTestFailures.Count -gt 0) {
    foreach ($f in $selfTestFailures) { Write-Host "[FAIL] $f" }
    throw 'service_name_check: self-test failed'
}
