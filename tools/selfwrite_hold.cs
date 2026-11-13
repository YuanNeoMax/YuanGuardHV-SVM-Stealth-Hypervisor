using System;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Threading;

// selfwrite_hold.cs -- 9.584 (P1-3c)
//
// THE QUESTION, STATED SO IT CAN BE WRONG
//
// The sync guard restores a page whenever its content differs from the snapshot it
// took when the page was armed. It polls from a PASSIVE thread, so it cannot see WHO
// wrote -- "the page changed" is the whole verdict. P1-3c records that as a design
// limitation.
//
// What was never measured is how far the ambiguity reaches in practice: when the
// TARGET ITSELF writes its own page, does the guard roll that write back too? If it
// does, the guard is not "protecting a page from foreign writers" -- it is "freezing
// a page against everyone, including its owner", and any product use on a live data
// page would corrupt the target's own state.
//
// WHY THIS OWNS ITS OWN PAGE
//
// The first version took a VA on the command line, which requires the caller to be
// the target process -- and a separate process cannot touch another process's VA at
// all (measured: the run failed with an unmapped-access exception). So this program
// MAPS THE FILE ITSELF and publishes pid + VA: the harness reads those, arms that
// page, and this same process then writes it. Target and writer are the same process
// by construction, which is the only way "the target's own write" can mean anything.
//
// Each iteration is classified by READING BACK what it wrote:
//
//   survived : readback == the value just written   -> the write stuck
//   rolled   : readback == the value written BEFORE -> the guard restored the page
//   other    : anything else -> something wrote that this program did not
//
// USAGE
//   selfwrite_hold.exe <path> <seconds> <iterations> <interval_ms> <info_path>
//
// Writes "pid=N va=0x... seed=0x..." to info_path as soon as the page is mapped and
// seeded, then waits 8 s for the harness to arm it before writing.
public static class SelfWriteHold
{
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FlushViewOfFile(IntPtr addr, UIntPtr size);

    private static long Read(IntPtr p) { return Marshal.ReadInt64(p); }
    private static void Write(IntPtr p, long v) { Marshal.WriteInt64(p, v); }

    private const int PageSize = 4096;

    public static int Main(string[] args)
    {
        if (args.Length < 5)
        {
            Console.WriteLine("usage: selfwrite_hold <path> <seconds> <iterations> <interval_ms> <info_path>");
            return 2;
        }

        string path = args[0];
        int seconds = int.Parse(args[1]);
        int iterations = int.Parse(args[2]);
        int intervalMs = int.Parse(args[3]);
        string infoPath = args[4];

        if (!File.Exists(path))
        {
            using (var fs = new FileStream(path, FileMode.Create, FileAccess.Write))
            {
                fs.SetLength(PageSize);
            }
        }

        using (var mmf = MemoryMappedFile.CreateFromFile(
                   path, FileMode.Open, null, PageSize, MemoryMappedFileAccess.ReadWrite))
        using (var view = mmf.CreateViewAccessor(0, PageSize, MemoryMappedFileAccess.ReadWrite))
        {
            // C# 5: AcquirePointer takes `ref byte*`, not `ref byte[]`. The earlier
            // form compiled in my head and not in csc (4 errors, all the same mistake).
            // `unsafe` + a local pointer is the C# 5 spelling.
            unsafe
            {
                byte* buf = null;
                view.SafeMemoryMappedViewHandle.AcquirePointer(ref buf);
                try
                {
                    IntPtr p = new IntPtr(buf + view.PointerOffset);
                    return Run(p, seconds, iterations, intervalMs, infoPath);
                }
                finally
                {
                    view.SafeMemoryMappedViewHandle.ReleasePointer();
                }
            }
        }
    }

    private static int Run(IntPtr p, int seconds, int iterations, int intervalMs, string infoPath)
    {
        // Seed a known baseline. The guard snapshots the page when it arms it, so this
        // value is what a later roll-back restores to. A distinct seed (not zero)
        // makes "restored" unmistakable.
        long seed = unchecked((long)0x00C4C40000000000UL);
        Write(p, seed);
        FlushViewOfFile(p, (UIntPtr)PageSize);

        int pid = System.Diagnostics.Process.GetCurrentProcess().Id;
        string info = string.Format("pid={0} va=0x{1:X} seed=0x{2:X}", pid, p.ToInt64(), seed);
        Console.WriteLine("holder: " + info);
        File.WriteAllText(infoPath, info);

        // Give the harness time to arm the page and switch the guard on. This wait is
        // part of the protocol, not padding: writing before the page is armed measures
        // an unguarded page and would report 100% survived for the wrong reason.
        Console.WriteLine("holder: waiting 60s for the harness to arm this page...");
        Thread.Sleep(60000);

        Console.WriteLine("target va=0x{0:X} pid={1} starting {2} writes every {3}ms",
            p.ToInt64(), pid, iterations, intervalMs);

        int survived = 0, rolled = 0, other = 0;
        for (int i = 0; i < iterations; i++)
        {
            // Different every iteration AND recognisable: low 32 bits carry the
            // counter. A constant value would make "the guard restored the previous
            // value" and "the write never landed" indistinguishable -- both read back
            // as the same number.
            long want = unchecked((long)0x5E1F000000000000UL) | (uint)(i + 1);

            long before = Read(p);
            Write(p, want);

            // Sleep PAST the poll interval so the guard has a real chance to act.
            // Without this the loop outruns the poller and measures nothing but cache
            // bandwidth.
            Thread.Sleep(intervalMs);

            long after = Read(p);
            string verdict;
            if (after == want) { verdict = "survived"; survived++; }
            else if (after == before) { verdict = "rolled"; rolled++; }
            else { verdict = "other"; other++; }

            Console.WriteLine("  [{0}] wrote=0x{1:X16} read=0x{2:X16} {3}",
                i, want, after, verdict);
        }

        Console.WriteLine("SUMMARY iterations={0} survived={1} rolled={2} other={3}",
            iterations, survived, rolled, other);
        Console.WriteLine("RESULT survived_pct={0:F1}",
            iterations == 0 ? 0.0 : (100.0 * survived / iterations));

        // Keep the mapping alive so the guard still has a page to guard while the
        // harness reads its counters. Disappearing immediately after the summary would
        // tear down the page mid-measurement.
        int spent = 8 + (iterations * intervalMs / 1000);
        int remain = seconds - spent;
        if (remain > 0)
        {
            Console.WriteLine("holder: holding {0}s more", remain);
            Thread.Sleep(remain * 1000);
        }
        return 0;
    }
}
