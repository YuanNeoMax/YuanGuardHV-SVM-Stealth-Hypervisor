$ErrorActionPreference = 'Stop'

# stub_stack_contract.ps1 -- 9.567
#
# WHY THIS EXISTS
#
# The remaining path for Windows-as-Guest is "after the early return, jump into
# Sv206GuestStub instead of finishing SvVirtualizeProcessor and terminating the
# thread" (WINDOWS_AS_GUEST_FINDINGS.md section 5). Four bugchecks have already
# been spent on that neighbourhood, and the most expensive of them (9.444, 0x1E
# STACKPTR_ERROR / #GP inside KiSwapContext) was not a bad offset -- it was
# leaving the guest on a stack that did not belong to the thread.
#
# That failure is decided by ONE property, and the property is visible in the
# source: the stub must not touch RSP. Its own comment says so
# ("the stub never touches the stack ... that value is never dereferenced"), and
# until now that sentence was enforced by nobody.
#
# This check makes it a rule the build can test:
#
#     inside Sv206GuestStub, no instruction may move RSP
#
# Forbidden: push, pop, call, ret, leave, enter, add/sub rsp, mov rsp, and the
# rsp-relative forms that a stack frame would need. Allowed: everything else -
# hlt, vmmcall, mov reg,imm, jmp, ud2. Those are exactly the instructions the
# stub uses today.
#
# WHAT THIS DOES AND DOES NOT PROVE
#
#   proves     : the stub cannot corrupt the stack pointer it is entered with
#   proves NOT : that the stop handshake answers after the jump (that is a live
#                test, and it is what the four bugchecks were)
#
# Being clear about that gap matters more than the check itself: a green tick
# here must not be read as "the real-guest path works".
#
# SELF-TEST: plants `push rbx` into the stub of a throwaway copy and requires a
# FAIL. A checker whose pattern cannot reach its own target is worthless (9.466).

$root = Split-Path -Parent $PSScriptRoot
$asm = Join-Path $root 'hv\svm_simplevm206_x64.asm'

if (-not (Test-Path -LiteralPath $asm)) {
    throw "stub_stack_contract: cannot find $asm"
}

# Masked source: comments (a ';' to end of line) blanked with spaces of the same
# length, so offsets survive. Comments legitimately talk ABOUT push/pop -- the
# stub's own notes mention `jmp rbx` and parked frames -- and a checker that
# flags its own documentation teaches people to ignore it (9.564).
function Get-CodeOnly {
    param([string[]] $Lines)
    $out = New-Object 'System.Collections.Generic.List[string]'
    foreach ($l in $Lines) {
        $i = $l.IndexOf(';')
        if ($i -ge 0) { $out.Add($l.Substring(0, $i)) } else { $out.Add($l) }
    }
    return $out
}

function Get-StubBody {
    param([string[]] $Lines)
    $start = -1
    $end = -1
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i] -match '^\s*Sv206GuestStub\s+proc\b') { $start = $i; continue }
        if ($start -ge 0 -and $Lines[$i] -match '^\s*Sv206GuestStub\s+endp\b') { $end = $i; break }
    }
    return @{ Start = $start; End = $end }
}

# Every one of these moves RSP, or writes the memory a stack frame would use.
$FORBIDDEN = @(
    @{ Pattern = '(?i)\bpush\b';    Why = 'push moves RSP' }
    @{ Pattern = '(?i)\bpop\b';     Why = 'pop moves RSP' }
    @{ Pattern = '(?i)\bcall\b';    Why = 'call pushes a return address' }
    @{ Pattern = '(?i)\bret\b';     Why = 'ret moves RSP' }
    @{ Pattern = '(?i)\bleave\b';   Why = 'leave restores RSP from RBP' }
    @{ Pattern = '(?i)\benter\b';   Why = 'enter builds a frame on the stack' }
    @{ Pattern = '(?i)\b(add|sub)\s+rsp\b'; Why = 'direct RSP arithmetic' }
    @{ Pattern = '(?i)\bmov\s+rsp\b';       Why = 'direct RSP assignment' }
    @{ Pattern = '(?i)\blea\s+rsp\b';       Why = 'direct RSP assignment' }
    @{ Pattern = '(?i)\b\w*x?sp\s*\]';      Why = 'stack-relative memory access' }
    @{ Pattern = '(?i)\[\s*rsp\b';          Why = 'stack-relative memory access' }
)

function Test-Stub {
    param([string] $AsmPath)
    $problems = New-Object 'System.Collections.Generic.List[string]'
    $lines = Get-Content -LiteralPath $AsmPath
    $code = Get-CodeOnly $lines
    $body = Get-StubBody $code

    if ($body.Start -lt 0 -or $body.End -lt 0) {
        $problems.Add("cannot locate the Sv206GuestStub proc/endp pair in $AsmPath")
        return $problems
    }

    for ($i = $body.Start + 1; $i -lt $body.End; $i++) {
        $text = $code[$i]
        if ($text.Trim() -eq '') { continue }
        foreach ($f in $FORBIDDEN) {
            if ($text -match $f.Pattern) {
                $problems.Add("line $($i + 1): '$($text.Trim())' - $($f.Why)")
                break
            }
        }
    }

    # The stub must also still contain its one intercepted instruction; a stub that
    # stopped ticking would pass the rule above and fail the whole design.
    $hasVmmcall = $false
    for ($i = $body.Start + 1; $i -lt $body.End; $i++) {
        if ($code[$i] -match '(?i)\bvmmcall\b') { $hasVmmcall = $true; break }
    }
    if (-not $hasVmmcall) {
        $problems.Add('the stub contains no vmmcall - its ticks are how a stop request is seen')
    }

    return $problems
}

# ---------------------------------------------------------------- self-test
function Invoke-SelfTest {
    param([string] $AsmPath)

    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("yghv_stub_st_" + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.asm')
    try {
        $lines = Get-Content -LiteralPath $AsmPath
        $code = Get-CodeOnly $lines
        $body = Get-StubBody $code
        if ($body.Start -lt 0) { throw 'self-test: cannot locate the stub to mutate' }
        $mutated = @($lines)
        $insertAt = $body.Start + 1
        $mutated = $mutated[0..($insertAt - 1)] + @('    push    rbx') + $mutated[$insertAt..($mutated.Count - 1)]
        Set-Content -LiteralPath $tmp -Value $mutated -Encoding UTF8

        $found = @(Test-Stub $tmp)
        if ($found.Count -eq 0) {
            throw 'self-test FAILED: planted `push rbx` in Sv206GuestStub was not detected'
        }
        if (-not ($found[0] -match 'push moves RSP')) {
            throw "self-test FAILED: mutation detected but not for the reason: $($found[0])"
        }
        Write-Host "[PASS] self-test: planted 'push rbx' in the stub was caught"
    }
    finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
}

$problems = Test-Stub $asm
if ($problems.Count -gt 0) {
    foreach ($p in $problems) { Write-Output "[FAIL] $p" }
    throw "stub_stack_contract: $($problems.Count) stack-touching instruction(s) inside Sv206GuestStub"
}

Invoke-SelfTest $asm

$lines = Get-Content -LiteralPath $asm
$body = Get-StubBody (Get-CodeOnly $lines)
Write-Host ("[PASS] stub stack contract: Sv206GuestStub (lines {0}-{1}) moves no RSP and still ticks" -f ($body.Start + 1), ($body.End + 1))
