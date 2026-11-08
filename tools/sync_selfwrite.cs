using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

// sync_selfwrite.cs -- 9.582 (ISSUES P1-3)
//
// THE QUESTION THIS ANSWERS
//
// The sync guard restores a page whenever its content differs from the snapshot.
// It polls from a PASSIVE thread and therefore CANNOT see WHO wrote: the verdict
// is "the page changed", never "the target changed it" or "a foreign process
// changed it". That is the design trade that bought independence from the NPT
// TLB (9.297-9.304), and P1-3 records it as a limitation.
//
// What was never measured directly is HOW OFAR the ambiguity reaches: does a
// target's OWN legitimate write get rolled back, and how often? A note says
// "70% of samples read the same value" but that was a side observation, not a
// measurement with a criterion.
//
// THIS PROGRAM IS THE CRITERION. It writes a page in a tight loop and counts how
// many of its own writes survive:
//
//   survived = the value the process reads back equals the value it just wrote
//   rolled   = the read-back is the OLD value (the guard restored it)
//
// The distinction matters because "the guard restored our own write" and "the
// guard restored a foreign write" are indistinguishable to the driver, and this
// program is the target's side of that story: if the target cannot keep its own
// writes, the guard is protecting the page from its owner.
//
// USAGE
//   sync_selfwrite.exe <hex_va> [iterations] [interval_ms]
//   prints a line per sample and a summary the harness can parse.
public static class SyncSelfWrite
{
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualLock(IntPtr addr, UIntPtr size);

    private static long Read(IntPtr p) { return Marshal.ReadInt64(p); }
    private static void Write(IntPtr p, long v) { Marshal.WriteInt64(p, v); }

    public static int Main(string[] args)
    {
        if (args.Length < 1)
        {
            Console.WriteLine("usage: sync_selfwrite <hex_va> [iterations] [interval_ms]");
            return 2;
        }

        long va = Convert.ToInt64(args[0].Replace("0x", "").Replace("0X", ""), 16);
        int iterations = args.Length > 1 ? int.Parse(args[1]) : 200;
        int intervalMs = args.Length > 2 ? int.Parse(args[2]) : 20;

        IntPtr p = new IntPtr(va);

        // Touch the page first so it is committed and resident; an unmapped page
        // would make every read throw and the whole run meaningless.
        try
        {
            Write(p, 0x5EED000000000001L);
        }
        catch (Exception e)
        {
            Console.WriteLine("FAILED: cannot write the target page: " + e.Message);
            return 3;
        }

        long survived = 0;
        long rolled = 0;
        long other = 0;

        for (int i = 0; i < iterations; i++)
        {
            long want = unchecked((long)0x5EED000000000000UL) | (long)(i + 2);

            Write(p, want);
            Thread.Sleep(intervalMs);          // let the poll run
            long got = Read(p);

            if (got == want) survived++;
            else if ((got & unchecked((long)0xFFFF000000000000UL)) == unchecked((long)0x5EED000000000000UL)) rolled++;
            else other++;

            if (i < 10 || i % 50 == 0)
                Console.WriteLine("  [{0}] wrote=0x{1:X16} read=0x{2:X16} {3}",
                    i, want, got, got == want ? "survived" : "ROLLED BACK");
        }

        Console.WriteLine("SUMMARY iterations={0} survived={1} rolled={2} other={3}",
            iterations, survived, rolled, other);
        Console.WriteLine("RESULT survived_pct={0:F1}",
            iterations == 0 ? 0.0 : (100.0 * survived / iterations));

        return 0;
    }
}
