$ErrorActionPreference = 'Stop'

# exit_rip_contract.ps1 — 9.469
#
# WHY THIS EXISTS: VMMCALL 漏了 `Rip = NRip` 很久没人发现。
#
# A #VMEXIT handler that emulates an instruction MUST advance RIP to NRip, or the
# guest re-enters at the same instruction and repeats it forever. That happened
# here: VMEXIT_VMMCALL counted the exit and broke without advancing, so the guest
# stub became a repeat of one instruction instead of a loop. Three rounds of
# experiments (9.462 hlt, 9.463 pause, 9.464 hlt+pause) all measured the same
# number because none of the code they changed was ever executed.
#
# WHAT MAKES IT HARD TO SEE: nothing fails. The machine boots, runs, unloads
# cleanly, and the counter moves - fast and steadily. The only symptom is that a
# parameter has no effect, which looks like "that instruction is a no-op on this
# platform" (which is what 9.464 wrongly concluded about hlt).
#
# SO THE CONTRACT IS CHECKED STRUCTURALLY: every case in SvHandleVmExit is
# classified, and a case that must advance RIP must be able to reach something
# that does. "Must advance" = instruction-emulation. "Must NOT advance" =
# exception injection, where the guest has to RE-EXECUTE the faulting instruction.
#
# CLASSIFICATION RULE (explicit lists, not heuristics):
#
#   INJECT writers  - the handler delivers #PF/#GP/#DB/etc and the guest must
#                     retry. Advancing RIP here would skip the faulting
#                     instruction, which is a silent corruption.
#   ADVANCE writers - the handler is at an instruction boundary and must step past
#                     it.
#
# A case that appears in neither list is NEITHER claimed nor guessed: the check
# reports it as UNCLASSIFIED and fails, so a new case cannot slip in assuming
# someone else handled it. That is the whole point - VMMCALL was exactly such a
# hole, and it was invisible because nothing enumerated the cases.

$repoRoot = Split-Path -Parent $PSScriptRoot
$svm = Join-Path $repoRoot 'hv\svm_simplevm206.cpp'

# 9.470: the self-test at the end of this file plants a defect in a COPY and
# re-runs this script against it through this variable. Reading the path from an
# environment variable is what lets the self-test exercise THIS checker instead
# of a reimplementation that could drift from it.
if ($env:YGHV_RIP_CONTRACT_OVERRIDE -and (Test-Path $env:YGHV_RIP_CONTRACT_OVERRIDE)) {
    $svm = $env:YGHV_RIP_CONTRACT_OVERRIDE
}

if (-not (Test-Path $svm)) {
    throw "exit_rip_contract: cannot find $svm"
}

$text = Get-Content $svm -Raw
$lines = $text -split "`n"
$failures = @()
$unclassified = @()

# ---------------------------------------------------------------------------
# The two lists. Each entry is the exit code name as it appears in `case VMEXIT_*`.
#
# KEEPING THESE AS EXPLICIT NAMES (and not a pattern) is deliberate: the point is
# that adding a case forces someone to decide which side it belongs on.
# ---------------------------------------------------------------------------

$injectCases = @(
    'VMEXIT_NPF',              # nested page fault: inject #PF, guest retries
    'VMEXIT_VMRUN',            # guest tried to VMRUN: inject #GP
    'VMEXIT_INTR',             # hardware interrupt: re-inject via EventInj
    'VMEXIT_NMI'               # NMI: re-inject via EventInj
)

# HOLD cases: neither emulate an instruction nor inject an exception. The guest
# resumes exactly where the hardware saved it, and the case's job is something
# else entirely (re-arming a page, recording a fatal fault).
#
# Why this needs its own list instead of being folded into "inject":
#
#   #BP / #DB are TRAPS. The saved RIP already points at the next instruction, and
#   APM says NRIP for an exception VMEXIT is RIP+1 (one byte), so RIP = NRip would
#   resume the guest MID-INSTRUCTION. The tree measured that consequence in 9.254:
#   "the old RIP=NRip resumed mid-instruction -> corrupted guest stream -> the
#   stable AccessViolationException both selftest rounds died on". A trap case
#   must therefore assert the OPPOSITE of an instruction case.
#
#   SHUTDOWN never resumes at all: S206RecordFatal + yghv_fail_degrade, then break.
#   Asking it "does it inject" or "does it advance" are both the wrong question.
#
# Three categories, three different assertions. Collapsing them was how the first
# version produced false findings about code that was correct.
$holdCases = @(
    'VMEXIT_EXCEPTION_BP',
    'VMEXIT_EXCEPTION_DB',
    'VMEXIT_SHUTDOWN'
)

# Cases that share a fallthrough group with an ADVANCE case are covered by that
# group's body; they are listed so the enumeration is complete, and the check
# verifies the GROUP reaches an advance.
$advanceCases = @(
    'VMEXIT_CPUID',
    'VMEXIT_MSR',
    'VMEXIT_CR3_WRITE',
    'VMEXIT_CR4_WRITE',
    'VMEXIT_RDTSC',
    'VMEXIT_RDTSCP',
    'VMEXIT_VMMCALL',
    'VMEXIT_INVLPG',
    'VMEXIT_INVLPGA',
    'VMEXIT_INVD',
    'VMEXIT_WBINVD',
    'VMEXIT_HLT',
    'VMEXIT_PAUSE',
    'VMEXIT_MONITOR',
    'VMEXIT_MWAIT',
    'VMEXIT_MWAIT_CONDITIONAL',
    'VMEXIT_XSETBV',
    'VMEXIT_IRET',
    'VMEXIT_SWINT',
    'VMEXIT_RSM',
    'VMEXIT_PUSHF',
    'VMEXIT_POPF',
    'VMEXIT_TASK_SWITCH',
    'VMEXIT_FERR_FREEZE',
    'VMEXIT_RDPMC',
    'VMEXIT_VMLOAD',
    'VMEXIT_VMSAVE',
    'VMEXIT_STGI',
    'VMEXIT_CLGI',
    'VMEXIT_SKINIT',
    'VMEXIT_ICEBP'
)

# ---------------------------------------------------------------------------
# Enumerate the cases actually present.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Group the cases into FALLTHROUGH GROUPS.
#
# The first version treated "next case" as the end of the body, which is wrong
# for the 25 instruction cases that share one body:
#
#     case VMEXIT_HLT:      \
#     case VMEXIT_PAUSE:     |  consecutive labels; the body belongs to all of
#     case VMEXIT_MONITOR:   |  them and starts after the LAST label
#     ...                   /
#     VpData->...Rip = NRip;
#     break;
#
# Cutting each at "the next case" gave every one of them an EMPTY body and
# reported 25 false failures - including the four this tree had already verified
# by hand. A checker that reports a known-good tree as broken is worse than no
# checker: it trains people to ignore it.
#
# So consecutive case labels (only blanks and comments between them) form ONE
# group, and the group is judged by the single body after its last label.
# ---------------------------------------------------------------------------

$groups = @()
$i = 0
while ($i -lt $lines.Count) {
    if ($lines[$i] -match '^\s+case\s+(VMEXIT_[A-Z_0-9]+)') {
        $names = @($matches[1])
        $start = $i
        $last = $i
        $j = $i + 1

        while ($j -lt $lines.Count) {
            $t = $lines[$j].Trim()
            if ($t -eq '' -or $t.StartsWith('//') -or $t.StartsWith('/*') -or
                $t.StartsWith('*') -or $t.StartsWith('*/')) {
                $j++
                continue
            }
            if ($lines[$j] -match '^\s+case\s+(VMEXIT_[A-Z_0-9]+)') {
                $names += $matches[1]
                $last = $j
                $j++
                continue
            }
            break
        }

        $bodyEnd = $lines.Count
        for ($m = $last + 1; $m -lt $lines.Count; $m++) {
            if ($lines[$m] -match '^\s+case\s+VMEXIT_') { $bodyEnd = $m; break }
            if ($lines[$m] -match '^\s+default:') { $bodyEnd = $m; break }
        }

        $groups += [pscustomobject]@{
            Names      = $names
            FirstLine  = $start
            BodyStart  = $last + 1
            BodyEnd    = $bodyEnd
        }
        $i = $last + 1
    }
    else {
        $i++
    }
}

$present = @()
foreach ($g in $groups) {
    foreach ($n in $g.Names) {
        $present += [pscustomobject]@{ Name = $n; Group = $g }
    }
}

if ($present.Count -eq 0) {
    throw 'exit_rip_contract: found no `case VMEXIT_*` at all - the parser is wrong, not the code'
}

Write-Host "  $($present.Count) case(s) in $($groups.Count) fallthrough group(s)"

# ---------------------------------------------------------------------------
# Which handler functions advance RIP? Locate each definition, walk to its
# closing brace at column 0, and look for a write to StateSaveArea.Rip.
#
# NOTE the window: the earlier ad-hoc audit searched [2430,2520) for
# SvHandleMsrAccess and found nothing, concluding a second defect existed - the
# write is at 2537, seventeen lines past the window. A checker with a guessed
# window produces exactly that class of false finding, so this one derives the
# end of the function instead of assuming it.
# ---------------------------------------------------------------------------

$handlerAdvance = @{}

# Handlers that EMULATE an instruction: they must advance RIP.
# SvHandleVmrun is deliberately NOT here: it injects #GP and the guest retries,
# so advancing would skip the offending instruction. The first version listed it
# and reported "never writes Rip - repeat-forever hole", which was a false
# finding about correct code: a nested VMRUN is a REFUSAL, not an emulation.
foreach ($fn in @('SvHandleCpuid', 'SvHandleMsrAccess')) {
    $defLine = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match "^$fn\s*\(" -or $lines[$i] -match "^$fn \(") {
            $defLine = $i
            break
        }
    }
    if ($defLine -lt 0) {
        $failures += "handler ${fn}: definition not found"
        continue
    }
    # Walk to the closing brace at column 0.
    $advances = $false
    for ($j = $defLine; $j -lt $lines.Count; $j++) {
        if ($lines[$j] -match 'StateSaveArea\.Rip\s*=' -and $lines[$j] -notmatch '==') {
            $advances = $true
        }
        if ($lines[$j] -match '^\}') { break }
    }
    $handlerAdvance[$fn] = $advances
    if (-not $advances) {
        $failures += "handler ${fn} never writes StateSaveArea.Rip - every case that calls it is a repeat-forever hole"
    }
}

for ($k = 0; $k -lt $groups.Count; $k++) {
    $g = $groups[$k]
    $body = ($lines[$g.BodyStart..($g.BodyEnd - 1)] -join "`n")

    # A group is judged ONCE, by every name it carries. A group can legitimately
    # mix classifications (an injection case can fall through into an advance
    # case) - what it must not do is carry an instruction name with no advance.
    $groupHasAdvance = $false
    $groupHasInject = $false
    $groupHasHold = $false
    $groupInstructionNames = @()
    $groupInjectNames = @()
    $groupHoldNames = @()

    foreach ($name in $g.Names) {
        $isInject = $injectCases -contains $name
        $isAdvance = $advanceCases -contains $name
        $isHold = $holdCases -contains $name

        $listed = @()
        if ($isInject) { $listed += 'inject' }
        if ($isAdvance) { $listed += 'advance' }
        if ($isHold) { $listed += 'hold' }

        if ($listed.Count -gt 1) {
            $failures += "$name is listed as $($listed -join ' AND ') - the classification is contradictory"
            continue
        }
        if ($listed.Count -eq 0) {
            $unclassified += "$name (line $($g.FirstLine + 1))"
            continue
        }

        if ($isInject) { $groupHasInject = $true; $groupInjectNames += $name }
        elseif ($isHold) { $groupHasHold = $true; $groupHoldNames += $name }
        else { $groupInstructionNames += $name }
    }

    # HOLD assertion: the guest resumes where the hardware left it, so this case
    # must NOT write RIP. That is the opposite assertion from an instruction case,
    # and it is the one the tree needs for traps (#BP/#DB) and for SHUTDOWN.
    if ($groupHasHold) {
        if ($body -match 'StateSaveArea\.Rip\s*=' -and $body -notmatch 'StateSaveArea\.Rip\s*==') {
            $failures += "$($groupHoldNames -join '/') is a HOLD case but writes StateSaveArea.Rip - a trap's saved RIP already points past the instruction, so advancing resumes the guest mid-instruction (9.254 measured exactly that corruption)"
        }
    }

    if ($groupHasInject) {
        # An injection case may inject DIRECTLY or by calling a handler that does.
        # The first version only looked for the words EventInj/SvInject/ExitVm in
        # the case body, so VMEXIT_VMRUN failed while looking perfectly correct:
        # its body is `SvHandleVmrun(...)` and the injection is inside that
        # function. A checker that cannot follow one call is not checking the
        # code, it is checking the spelling.
        $injects = ($body -match 'EventInj|SvInject|ExitVm')

        # FATAL cases do not inject: they record and degrade, and the guest never
        # resumes, so RIP is irrelevant. Recognise them explicitly rather than
        # asking an injection question they cannot answer.
        $fatal = ($body -match 'S206RecordFatal|svm_fail_degrade|yghv_fail_degrade')

        if (-not $injects -and -not $fatal) {
            # Follow each handler called from this body and look inside it.
            foreach ($fn in @('SvHandleCpuid', 'SvHandleMsrAccess', 'SvHandleVmrun',
                              'SvHandleShutdown', 'SvHandleExceptionDb')) {
                if ($body -match "$fn\s*\(") {
                    $defLine = -1
                    for ($i = 0; $i -lt $lines.Count; $i++) {
                        if ($lines[$i] -match "^$fn\s*\(" -or $lines[$i] -match "^$fn \(") {
                            $defLine = $i
                            break
                        }
                    }
                    if ($defLine -ge 0) {
                        for ($j = $defLine; $j -lt $lines.Count; $j++) {
                            if ($lines[$j] -match 'EventInj|SvInject|ExitVm') { $injects = $true }
                            if ($lines[$j] -match '^\}') { break }
                        }
                    }
                }
            }
        }

        if (-not $injects) {
            $failures += "$($groupInjectNames -join '/') is classified INJECT but the body (and the handlers it calls) neither injects nor sets ExitVm"
        }
    }

    if ($groupInstructionNames.Count -eq 0) {
        continue   # pure injection group, no advance required
    }

    # The group carries at least one instruction name, so its body must advance.
    $reaches = $false
    if ($body -match 'StateSaveArea\.Rip\s*=' -and $body -notmatch 'StateSaveArea\.Rip\s*==') {
        $reaches = $true
    }
    foreach ($fn in $handlerAdvance.Keys) {
        if ($body -match "$fn\s*\(" -and $handlerAdvance[$fn]) {
            $reaches = $true
        }
    }

    if (-not $reaches) {
        $failures += "$($groupInstructionNames -join '/') (line $($g.FirstLine + 1)) is an instruction case but its body cannot reach StateSaveArea.Rip = NRip - the guest will repeat this instruction forever"
    }
}

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

Write-Host "  enumerated $($present.Count) VMEXIT case(s); $($injectCases.Count) classified inject, $($advanceCases.Count) classified advance"

if ($unclassified.Count -gt 0) {
    Write-Host '  UNCLASSIFIED cases - decide whether each advances RIP or injects an exception:'
    foreach ($u in $unclassified) { Write-Host "    $u" }
    $failures += "$($unclassified.Count) case(s) are not classified in exit_rip_contract.ps1"
}

# Stale entries: a listed case that no longer exists is dead guidance.
foreach ($listed in ($injectCases + $advanceCases + $holdCases)) {
    if (-not ($present | Where-Object { $_.Name -eq $listed })) {
        $failures += "$listed is listed in exit_rip_contract.ps1 but no longer exists in SvHandleVmExit"
    }
}

if ($failures.Count -gt 0) {
    foreach ($f in $failures) { Write-Host "[FAIL] $f" }
    throw "exit_rip_contract: $($failures.Count) problem(s)"
}

Write-Host '[PASS] VMEXIT RIP contract: every instruction case can reach NRip, every injection case injects'

# ---------------------------------------------------------------------------
# 9.470: SELF-TEST - prove this checker still detects the defect it was written
# for, by planting that defect in a COPY and requiring the copy to FAIL.
#
# WHY A CHECKER NEEDS THIS
# Measured history in this tree: the first version of this checker reported 25
# false failures on a healthy tree (fallthrough groups cut in half), and later
# versions reported two more false failures before the classification was right.
# Then it was VERIFIED once by hand - the VMMCALL advance was deleted, the checker
# named it, the file was restored - and that verification exists only in a
# transcript. Nothing stops a future edit to the parser from silently turning the
# checker into one that always passes.
#
# A checker that always passes is indistinguishable from a checker that works,
# until it matters. So the ability to fail is now itself tested, on every build,
# against a copy - the real source is never touched.
#
# WHAT IS PLANTED: the `Rip = NRip` line inside VMEXIT_VMMCALL's body. It is
# located structurally (by the case group), not by line number, so the self-test
# keeps working when the file shifts.
# ---------------------------------------------------------------------------

$selfTestFailures = @()
#
# RECURSION GUARD: the self-test re-runs this same script. Without this flag the
# child would run its own self-test, which would run its own... The flag says
# "this run IS the self-test's child; do not self-test again".
#
if (-not $env:YGHV_RIP_CONTRACT_SELFTEST_CHILD) {
try {
    $copy = Join-Path $env:TEMP ("exit_rip_selftest_" + [guid]::NewGuid().ToString('N') + ".cpp")

    # Find VMEXIT_VMMCALL's body in the real file, then remove its advance line
    # from the copy.
    $vmcallGroup = $groups | Where-Object { $_.Names -contains 'VMEXIT_VMMCALL' } | Select-Object -First 1
    if (-not $vmcallGroup) {
        $selfTestFailures += 'VMEXIT_VMMCALL group not found - cannot plant the defect'
    }
    else {
        $advanceLine = -1
        for ($i = $vmcallGroup.BodyStart; $i -lt $vmcallGroup.BodyEnd; $i++) {
            if ($lines[$i] -match 'StateSaveArea\.Rip\s*=' -and $lines[$i] -notmatch '==') {
                $advanceLine = $i
                break
            }
        }
        if ($advanceLine -lt 0) {
            $selfTestFailures += 'VMEXIT_VMMCALL has no advance line to remove - the body already violates the contract'
        }
        else {
            # Build the mutated copy as: lines before the advance, then lines after
            # it. Written as two range expressions assigned separately because a
            # single expression mixing `..` ranges with `-join` inside one set of
            # parentheses does not parse (PowerShell reads `($a + $b -join x)` as a
            # malformed parenthesised expression).
            $head = @()
            if ($advanceLine -gt 0) {
                $head = $lines[0..($advanceLine - 1)]
            }
            $tail = @()
            if ($advanceLine + 1 -lt $lines.Count) {
                $tail = $lines[($advanceLine + 1)..($lines.Count - 1)]
            }
            $mutated = ($head + $tail) -join "`n"
            Set-Content -Path $copy -Value $mutated -Encoding UTF8 -NoNewline

            # Re-run the same logic against the mutated copy. Reusing the script
            # itself (via an env var pointing at a substitute path) is the honest
            # way: it tests THIS checker, not a reimplementation of it.
            $env:YGHV_RIP_CONTRACT_OVERRIDE = $copy
            $env:YGHV_RIP_CONTRACT_SELFTEST_CHILD = '1'

            # Capture the child through a FILE, not through a pipeline.
            #
            # Measured dead ends on the way here, both producing false alarms about
            # a checker that was actually working:
            #   `2>&1 | Out-String`          -> the exception text vanished, leaving ''
            #   `(...) 2>$null | Out-String` -> stdout vanished too (the [FAIL] lines)
            # $ErrorActionPreference='Stop' in this scope makes the child's EXPECTED
            # `throw` a terminating error here, and PowerShell's handling of that
            # inside a pipeline swallowed one stream or the other.
            # A redirect inside a cmd line has no such interaction - it is the
            # child's own shell doing the writing.
            $logPath = Join-Path $env:TEMP ("exit_rip_selftest_" + [guid]::NewGuid().ToString('N') + ".log")
            $cmdLine = '/c powershell -NoProfile -ExecutionPolicy Bypass -File "' +
                       $PSCommandPath + '" > "' + $logPath + '" 2>&1'
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

            $env:YGHV_RIP_CONTRACT_OVERRIDE = $null
            $env:YGHV_RIP_CONTRACT_SELFTEST_CHILD = $null

            if ($childExit -eq 0) {
                $selfTestFailures += 'the mutated copy PASSED - the checker accepted a tree with the VMMCALL advance removed'
            }
            elseif ($all -notmatch 'VMEXIT_VMMCALL') {
                $selfTestFailures += "the mutated copy failed but never named VMEXIT_VMMCALL (child output: $($all.Trim()))"
            }
            elseif ($all -notmatch 'repeat this instruction forever') {
                $selfTestFailures += "the mutated copy named VMEXIT_VMMCALL but not the RIP-contract reason (child output: $($all.Trim()))"
            }
            else {
                Write-Host '[PASS] self-test: planted defect in VMEXIT_VMMCALL was caught'
            }
        }
    }
}
finally {
    if ($copy -and (Test-Path $copy)) { Remove-Item $copy -Force -ErrorAction SilentlyContinue }
    $env:YGHV_RIP_CONTRACT_OVERRIDE = $null
    $env:YGHV_RIP_CONTRACT_SELFTEST_CHILD = $null
}
}   # end of the RECURSION GUARD

if ($selfTestFailures.Count -gt 0) {
    foreach ($f in $selfTestFailures) { Write-Host "[FAIL] $f" }
    throw "exit_rip_contract: self-test failed"
}
