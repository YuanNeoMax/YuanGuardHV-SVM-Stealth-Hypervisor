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
    # 9.501: four client commands called sc.exe with the literal service name
    # 'yuanguard'; the service is 'yghvpa' and the registry has never held that
    # name (measured: sc config yuanguard -> 1060). They never worked, and no
    # checker looked at an ARGUMENT to an external tool - every existing one
    # reads a different part of the file. This one requires every sc operand to
    # be a variable, and it carries a self-test that plants the original bug.
    'service_name_check.ps1',
    # 9.564: the DOCUMENTS had no gate at all. Eleven checkers read sources; not one
    # read docs\, and that is exactly the artifact that drifted -- five stale numbers
    # found in INDEX.md on the day it was written (9.563), plus two records with no body.
    # LINK INTEGRITY + index row counts + latest number + archive counts, all
    # measured, none assumed. Its self-test plants four real defects in a throwaway
    # copy and requires each to be caught.
    # 9.567: the stub that the real-guest path would jump into must not touch RSP.
    # 9.444 cost a bugcheck (0x1E in KiSwapContext) by leaving the guest on a foreign
    # stack; the stub's own comment claims it never touches the stack, and nothing
    # enforced that sentence until now. Static, self-tested, offline.
    'stub_stack_contract.ps1',
    'docs_check.ps1',
    'docs_check_selftest.ps1',
    # 9.583: a Win32 error NAME written next to the wrong hex code sent a whole round
    # of investigation down the wrong axis. 9.582 recorded "0x490 ERROR_NOT_READY"
    # and concluded "protect-page has a special restriction on mmf pages"; 0x490 is
    # 1168 = ERROR_NOT_FOUND, whose only source on that path is protect.c:1099
    # ("pid not in the target table"), which points at a missing `set-target`.
    # The name is not a label -- it is the constant some `return` produced.
    'struct_layout_check.ps1', 'win32_error_names.ps1',
    'win32_error_names_selftest.ps1',
    'client_undefined_vars.ps1',
    # 9.586: a whitespace-only rewrite of an UNTOUCHED file makes the real change
    # invisible in review. Measured: hv/svm_simplevm206_x64.asm arrived as a 1301
    # line diff for a +37 line addition, because the editor converted its 632 LF
    # lines to CRLF on save -- 669 lines of pure churn. The diff was not wrong, it
    # was unreadable, and unreadable is what gets approved without being read.
    # Six of 192 tracked files carried both CRLF and LF inside one file; five were
    # normalised, and this check is what keeps the sixth from being joined by a seventh.
    # A file may be all-CRLF or all-LF; mixing the two inside one file may not happen.
    # Its self-test plants a mixed-EOL file AND a clean one, because a checker that
    # fails on everything also "catches" the defect.
    'eol_check.ps1',
    # 9.587: the documents cite source as "file:NNN" -- "the early-out is at
    # mem_bp.c:1139". 18 such citations were audited by hand and SEVEN pointed at
    # the wrong line while every one of them was still INSIDE its file, so no bounds
    # check could ever have seen it: svm_core.c:479 was a blank line two below the
    # real state->dr7 = 0x400, and svm_simplevm206_x64.asm:406 was a blank comment
    # line six above the real Sv206GuestStub. A citation past the end of a file is
    # loud and rare; a citation that quietly means something else is silent and
    # common, which is the worse of the two.
    # The gate checks existence, range, and NON-BLANK -- it does NOT check that the
    # line still says what the prose claims, and its header says so.
    'source_citation_check.ps1'
)

# 9.583: COUNT THE PASSES INSTEAD OF LETTING PEOPLE WRITE THE NUMBER DOWN.
#
# Every document in this tree stated "13 scripts / 24 PASS" and every one of them
# was wrong the moment a checker was added -- docs_check had already made it
# 14/30. A number a human retypes is a number that drifts, which is the exact
# finding 9.583 was opened for (a stale image hash, a stale record count, a status
# block stuck 18 records behind).
#
# So the runner writes the real figures to a file, and docs_check reads THAT file
# and compares it against what the documents claim. No human types the number.
$passCount = 0
$selfTestCount = 0
$failures = @()

# 9.586: docs_check must NOT compare the documents against the counts during the LOOP.
#
# Two ordering defects compounded here:
#
#   a. In-loop, docs_check reads check_stats.txt from the PREVIOUS run -- so the very
#      run that adds a checker judges the documents against the numbers from before
#      that checker existed, and passes documents that are already wrong. Measured
#      twice in a row (9.586): README's script count, then README's PASS count.
#   b. When a checker fails, its [PASS] lines disappear and the measured total DROPS
#      (measured: 41 -> 40 with docs_check red). A document then cannot be right for
#      both a green and a red run: the number it states is changed by its own verdict.
#
# The fix is ORDERING, not arithmetic. The count comparison runs once, at the end,
# against the counts this run measured, and only when everything else is green --
# so the numbers a document is judged against are the ones a full pass produces.
# The flag makes docs_check skip just that comparison in the loop; links, records,
# index rows, archive counts and the image hash are all still checked there.
#
# Set per script, NOT once around the loop: docs_check_selftest.ps1 plants a stale
# count in a throwaway copy and requires docs_check to CATCH it, and a flag inherited
# from the runner silenced exactly that check -- the self-test then reported the
# mutation asleep (measured, 9.586). The deferral belongs to the runner's own call,
# not to every child that happens to run docs_check.
foreach ($script in $scripts) {
    if ($script -eq 'docs_check.ps1') { $env:YGHV_DEFER_DOC_COUNTS = '1' }
    else { $env:YGHV_DEFER_DOC_COUNTS = $null }
    Write-Host "[RUN] $script"
    # A CHILD PROCESS, not `& $script`: every checker reports with Write-Host, which
    # goes to the host and NOT through stream redirection, so `& $s 2>&1` returned
    # an empty string and the first version of this block counted 0 passes for 13
    # scripts that had just printed 29 of them. docs_check.ps1 records the same
    # trap from the other side (its self-test could not read its child's reason).
    # 9.586: A CHILD THAT WRITES TO STDERR KILLS THIS SCRIPT WHILE $ErrorActionPreference
    # IS 'Stop'. powershell.exe's own error text ("docs_check: 1 documentation
    # inconsistency(ies)") arrives through the 2>&1 merge as an ErrorRecord, and under
    # 'Stop' PowerShell throws on it BEFORE the next statement can read $LASTEXITCODE.
    # Measured 9.586: the run died with a NativeCommandError at this exact line and
    # never reached the failure list -- so the exit code was 1, the reason was the
    # generic 'NativeCommandError', and the FAIL lines the child had printed were the
    # only evidence of WHAT failed. Every checker that reports a failure on stderr
    # therefore produced an unreadable suite failure.
    #
    # 'Continue' for the duration of the call: the error record is still captured into
    # $raw (and printed), but it no longer terminates the runner.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $raw = @()
    $code = 0
    try {
        $raw = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $script) 2>&1
        $code = $LASTEXITCODE
        if ($null -eq $code) { $code = 0 }
    }
    catch {
        $code = 1
        $raw = @("$script could not be run: $($_.Exception.Message)")
    }
    finally {
        $ErrorActionPreference = $prevEap
    }
    $raw | ForEach-Object { Write-Host $_ }
    # 9.586: KEEP GOING AFTER A FAILURE, then report them all at the end.
    #
    # This used to `throw` on the first non-zero child, which meant the stats file
    # below was never written -- and docs_check is one of the scripts being counted.
    # So the ONE check that compares documents against the measured numbers could
    # not be satisfied by running the suite: a document claiming a stale count made
    # the run die before the real count reached check_stats.txt, and the only way
    # forward was to guess the number and edit the document anyway. Measured 9.586:
    # README said 16 checkers, disk had 17, and fixing it required correcting the
    # document from a count the runner refused to publish.
    #
    # The measurement must land on disk whether or not a checker is unhappy with it.
    if ($code -ne 0) { $failures += "$script exited $code" }
    $passCount += @($raw | Where-Object { "$_" -match '^\[PASS\]' }).Count
    $selfTestCount += @($raw | Where-Object { "$_" -match '^\[PASS\].*self-test' }).Count
}


$env:YGHV_DEFER_DOC_COUNTS = $null
$statsPath = Join-Path $PSScriptRoot 'check_stats.txt'
$line = "scripts=$($scripts.Count) passes=$passCount selftests=$selfTestCount"
[IO.File]::WriteAllText($statsPath, $line, (New-Object Text.UTF8Encoding($false)))
Write-Host "check stats: $line  (written to $statsPath)"

# 9.586: RE-RUN docs_check NOW THAT THE NUMBERS EXIST.
#
# docs_check compares README/QUICKSTART's "N scripts / M PASS" against this file,
# and it runs INSIDE the loop above -- i.e. it reads the PREVIOUS run's stats. So
# adding a checker made the docs stale in a way the same run could never see: the
# gate compared the documents against 15/39 while measuring 16/41, and reported
# PASS on documents that were about to be wrong. The suite only failed on the NEXT
# invocation, one edit later, when the author had already moved on.
#
# Measured 9.586: exactly that happened twice in a row (16 checkers vs README's
# number, then 15/39 vs the new 16/41). A gate that reports on the run before this
# one is not a gate. This second pass costs one child process and makes the numbers
# the documents are judged against the numbers this run just produced.
#
# Not counted in passCount/selftests -- it is the same script, and adding its
# output twice would make the stats file describe a suite that does not exist.
if ($failures.Count -eq 0 -and $scripts -contains 'docs_check.ps1') {
    Write-Host '[RUN] docs_check.ps1 (second pass: against the counts just measured)'
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $raw = @()
    $code = 0
    try {
        $raw = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'docs_check.ps1') 2>&1
        $code = $LASTEXITCODE
        if ($null -eq $code) { $code = 0 }
    }
    catch {
        $code = 1
        $raw = @("docs_check.ps1 could not be run: $($_.Exception.Message)")
    }
    finally {
        $ErrorActionPreference = $prevEap
    }
    $raw | ForEach-Object { Write-Host $_ }
    if ($code -ne 0) { $failures += 'docs_check.ps1 exited ' + $code + ' on the second pass (README/QUICKSTART disagree with the counts measured by THIS run)' }
}

if ($failures.Count -gt 0) {
    foreach ($f in $failures) { Write-Host "[FAIL] $f" }
    throw "$($failures.Count) source check(s) failed"
}

Write-Host 'ALL SOURCE CHECKS PASS'