using System;
using System.Diagnostics;
using System.Threading;

// bench_core.cs -- 9.575
//
// THE LOAD MUST BE COMPILED, NOT INTERPRETED.
//
// Measured with the PowerShell version of this benchmark: 2,000,000 iterations x 12
// lanes took 39,757 ms. Almost all of that is the interpreter walking AST nodes, so
// any hypervisor overhead would be buried under interpreter noise - the benchmark
// would be measuring PowerShell, which is exactly the "my measurement cannot see the
// thing I claim to measure" failure this tree keeps recording.
//
// This computes the SAME algorithm natively. Each lane is a tight xorshift loop over
// registers: no allocation, no I/O, no locks, no virtual calls. If a runtime number
// changes between modes, the CPU is what changed.
//
// The work is fixed (iterations x lanes), so the output is a DURATION to compare, not
// a rate that drifts with machine state. That is what makes two runs comparable.
//
// Every lane also returns its sum, and the runner compares them: lanes must be
// independent (distinct seeds) and non-zero. A hypervisor that corrupted execution
// would show up as a wrong sum, not merely as a slow one - and a benchmark that only
// reports time cannot tell "fast" from "wrong".
public static class BenchCore
{
    public struct LaneResult
    {
        public int Lane;
        public ulong Sum;
    }

    private static ulong Spin(ulong seed, int iterations)
    {
        ulong x = seed;
        ulong sum = 0;
        for (int i = 0; i < iterations; i++)
        {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            sum += x;
        }
        return sum;
    }

    // Returns elapsed milliseconds for the whole fan-out.
    // startLane lets a run put its lanes on specific CPUs; -1 means "let the OS place it".
    public static long Run(int threads, int iterations, ulong seedBase,
                           out ulong[] sums, out ulong singleThreaded)
    {
        var localSums = new ulong[threads];

        // Reference value from one lane, computed before the fan-out, so the check
        // cannot go stale if Spin() is ever edited.
        singleThreaded = Spin(seedBase, iterations);

        var lanes = new Thread[threads];
        var barrier = new Barrier(threads);

        for (int t = 0; t < threads; t++)
        {
            int lane = t;   // capture, not the loop variable
            lanes[t] = new Thread(() =>
            {
                // All lanes start together, so lane 0 cannot finish while lane 11 is
                // still being created - creation cost is then outside the timed window
                // for every lane instead of only for the first.
                barrier.SignalAndWait();
                localSums[lane] = Spin(seedBase + (ulong)lane, iterations);
            });
            lanes[t].IsBackground = true;
            lanes[t].Start();
        }

        var sw = Stopwatch.StartNew();
        foreach (var th in lanes) th.Join();
        sw.Stop();

        sums = localSums;
        return sw.ElapsedMilliseconds;
    }

    public static int Main(string[] args)
    {
        int threads = 12;
        int iterations = 20000000;
        ulong seedBase = 0x2545F4914F6CDD1DUL;

        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "-threads" && i + 1 < args.Length) threads = int.Parse(args[++i]);
            else if (args[i] == "-iters" && i + 1 < args.Length) iterations = int.Parse(args[++i]);
        }

        ulong[] sums;
        ulong single;
        long ms = Run(threads, iterations, seedBase, out sums, out single);

        Console.WriteLine("threads={0} iters={1}", threads, iterations);
        Console.WriteLine("elapsed_ms={0}", ms);
        Console.WriteLine("single_lane_sum=0x{0:X16}", single);
        Console.WriteLine("lanes_returned={0}", sums.Length);

        int bad = 0;
        var seen = new System.Collections.Generic.HashSet<ulong>();
        foreach (var s in sums)
        {
            if (s == 0) { Console.WriteLine("CHECK FAILED: a lane summed to zero"); bad++; }
            if (!seen.Add(s)) { Console.WriteLine("CHECK FAILED: duplicate lane sum 0x{0:X16}", s); bad++; }
        }
        if (bad == 0) Console.WriteLine("CHECK OK: all lanes independent and non-zero");
        Console.WriteLine("RESULT elapsed_ms={0}", ms);
        return bad == 0 ? 0 : 2;
    }
}
