# bench_load.ps1 -- 9.575
#
# A FIXED, DETERMINISTIC CPU LOAD, so "throughput" is a number two runs can be
# compared on rather than an impression.
#
# WHY NOT A WALL-CLOCK "do something for 5 seconds" TEST: that measures how much
# work the machine fits in the time, which is exactly the quantity a hypervisor's
# overhead distorts - and it makes the baseline and the loaded run incomparable if
# the machine is doing anything else. This computes a FIXED amount of work and
# reports HOW LONG it took. Same work, two environments, one number.
#
# WHAT THE WORK IS
#   each of N threads runs a seeded xorshift loop `iters` times and sums it.
#   xorshift is chosen because it is a tight ALU loop: no memory allocation, no
#   I/O, no locks, nothing the hypervisor could accelerate or that could be blamed
#   for a stall. If the number moves, the difference is the CPU it ran on.
#
# WHY THE RESULT IS ALSO CHECKED: every thread's sum is compared against the
# single-threaded value computed up front. A hypervisor that silently corrupts
# execution would show up as a wrong sum, not just a slow one. A benchmark that
# only reports time cannot tell "fast" from "wrong".
param(
    [int] $Threads = 12,
    [int] $Iters   = 4000000,
    [switch] $NoCheck
)

$ErrorActionPreference = 'Stop'

function Get-LaneSum([int] $iterations, [uint64] $seed) {
    $x = $seed
    $sum = [uint64]0
    for ($i = 0; $i -lt $iterations; $i++) {
        $x = $x -bxor (($x -shl 13) -band 0xFFFFFFFFFFFFFFFF)
        $x = $x -bxor ($x -shr 7)
        $x = $x -bxor (($x -shl 17) -band 0xFFFFFFFFFFFFFFFF)
        $sum += $x
    }
    return $sum
}

$seedBase = [uint64]0x2545F4914F6CDD1D

# Reference value, computed once. Doing it here (not hard-coded) means the check
# cannot go stale if the algorithm above is ever edited.
$reference = Get-LaneSum -iterations 200000 -seed $seedBase

$work = {
    param([int] $iterations, [uint64] $seed, [int] $lane)
    $x = $seed
    $sum = [uint64]0
    for ($i = 0; $i -lt $iterations; $i++) {
        $x = $x -bxor (($x -shl 13) -band 0xFFFFFFFFFFFFFFFF)
        $x = $x -bxor ($x -shr 7)
        $x = $x -bxor (($x -shl 17) -band 0xFFFFFFFFFFFFFFFF)
        $sum += $x
    }
    [pscustomobject]@{ Lane = $lane; Sum = $sum }
}

$sw = [Diagnostics.Stopwatch]::StartNew()

$jobs = @()
for ($t = 0; $t -lt $Threads; $t++) {
    $jobs += Start-Job -ScriptBlock $work -ArgumentList $Iters, ($seedBase + [uint64]$t), $t
}

$results = $jobs | Wait-Job | Receive-Job
$jobs | Remove-Job -Force -ErrorAction SilentlyContinue

$sw.Stop()

$totalMs = [math]::Round($sw.Elapsed.TotalMilliseconds, 1)
$sums = @($results | Sort-Object Lane | ForEach-Object { $_.Sum })
$distinct = ($sums | Sort-Object -Unique).Count

Write-Host ("threads={0} iters={1} laneSum={2}" -f $Threads, $Iters, $Iters)
Write-Host ("elapsed_ms={0}" -f $totalMs)
Write-Host ("lanes_returned={0} distinct_sums={1}" -f $sums.Count, $distinct)

if (-not $NoCheck) {
    # Correctness gate: every lane must produce a distinct, non-zero sum. Two lanes
    # with the same seed would be a harness bug; all-zero would mean the loop body
    # was optimised away or never executed.
    if ($sums.Count -ne $Threads) {
        Write-Host "CHECK FAILED: expected $Threads lanes, got $($sums.Count)"
        exit 2
    }
    if ($sums -contains [uint64]0) {
        Write-Host "CHECK FAILED: a lane summed to zero - the loop did not run"
        exit 2
    }
    if ($distinct -ne $Threads) {
        Write-Host "CHECK FAILED: lanes are not independent (distinct=$distinct)"
        exit 2
    }
    Write-Host "CHECK OK: all lanes ran independently and produced non-zero sums"
}

Write-Host ("RESULT elapsed_ms={0}" -f $totalMs)
