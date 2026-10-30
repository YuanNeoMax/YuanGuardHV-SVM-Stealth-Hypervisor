import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

public class YghvCtl {
    static {
        System.loadLibrary("yghv_ctl_jni");
    }

    private static final int FN_SET_TARGET = 0x800;
    private static final int FN_ADD_PAGE = 0x801;
    private static final int FN_REMOVE_PAGE = 0x802;
    private static final int FN_START_PROTECT = 0x803;
    private static final int FN_STOP_PROTECT = 0x804;
    private static final int FN_GET_STATE = 0x805;
    private static final int FN_GET_TARGET = 0x806;
    private static final int FN_GET_PAGES = 0x807;
    private static final int FN_GET_HOOKS = 0x808;
    private static final int FN_CLEAR = 0x809;
    private static final int FN_SET_CONFIG = 0x80A;
    private static final int FN_GET_CONFIG = 0x80B;
    private static final int FN_INSTALL_HOOK = 0x80C;
    private static final int FN_REMOVE_HOOK = 0x80D;
    private static final int FN_GET_TARGETS = 0x80E;
    private static final int FN_GET_LASTHIT = 0x80F;
    private static final int FN_ADD_PAGE_FOR_PID = 0x810;
    private static final int FN_REMOVE_PAGE_FOR_PID = 0x811;
    private static final int FN_SET_FAKE_MODE = 0x812;
    /* 9.297 (A'): runtime TLB-flush strategy for the fake-write path.
       PowerShell-only command (config-fake-tlb); the constant is declared
       here to keep the C/PS/Java IOCTL parity check green. */
    private static final int FN_SET_FAKE_TLB = 0x813;
    /* 9.299 (C line): sync guard — snapshot + poll, no NPT flip.
       PowerShell-only command (config-sync); declared for IOCTL parity. */
    private static final int FN_SET_SYNC = 0x814;
    /* Phase 1 (2026-10-11): page-table walk probe (Engine B foundation).
       PowerShell-only command (mem-pte); declared for IOCTL parity. */
    private static final int FN_PTE_PROBE = 0x815;
    /* Phase 3 (2026-10-11): typed cross-process read/write, dual engine.
       PowerShell-only commands (mem-read / mem-write / mem-engine). */
    private static final int FN_MEM_READ = 0x816;
    private static final int FN_MEM_WRITE = 0x817;
    private static final int FN_MEM_ENGINE = 0x818;
    /* Phase 4 (2026-10-11): page-table driven signature scan (mem-scan). */
    private static final int FN_MEM_SCAN = 0x819;
    /* Phase 2b (2026-10-11): self-built physical mapping window
       (mem-selfmap); declared for IOCTL parity. */
    private static final int FN_SELFMAP = 0x81A;
    /* Phase 7 (2026-10-11): guard cooperation (mem-coop). */
    private static final int FN_MEM_COOP = 0x81B;
    /* Phase 4b (2026-10-11): reverse scan (mem-scan-rev). */
    private static final int FN_MEM_SCAN_REV = 0x81C;
    /* Phase 6 (2026-10-11): island direct-map access (mem-island). */
    private static final int FN_ISLAND = 0x81D;
    /* Tier 4 prerequisite (2026-10-11): NPT remap experiment. */
    private static final int FN_NPT_REMAP = 0x81E;
    /* Tier 4.1/4.3 (2026-10-11): split view + aliasing. */
    private static final int FN_VIEW = 0x81F;
    /* Tier 4.6 (2026-10-17): NPF watchpoints. */
    private static final int FN_WATCH = 0x820;
    /* Tier 4.2 (2026-10-17): O(1) snapshot/rollback. */
    private static final int FN_TIME = 0x821;
    /* Tier 4.7 (2026-10-17): cache behaviour control. */
    private static final int FN_CACHE = 0x822;
    /* Tier 4.4 (2026-10-17): instruction-level breakpoints. */
    private static final int FN_BP = 0x823;
    /* Tier 4.5 (2026-10-17): frame ownership takeover. */
    private static final int FN_FRAME = 0x824;
    /* Tier 7.2/7.7 (2026-10-18): graceful degradation counters. */
    private static final int FN_FAIL = 0x825;
    /* Tier 7.1/7.3 (2026-10-18): self-hiding + integrity self-check. */
    private static final int FN_GUARD = 0x826;
    /* 9.338: direct physical-frame access. */
    private static final int FN_PA = 0x827;
    /* Tier 7.4/7.5/7.6: exit timing, escape attempts, attack surface. */
    private static final int FN_SENTRY = 0x828;
    private static final int FN_AUTH   = 0x829;
    /* 9.448: exit-coverage control and measurement. The 206 handler covers 35
     * exit codes; which of them are armed decides which can fire, so this is
     * the only way to turn "35 codes implemented" into a measurement. */
    private static final int FN_EXITMASK = 0x82A;
    /* 9.449 (Tier 1.5): Engine A vs Engine B on the same range. The plan claimed
     * B is faster on large ranges; this is the command that times it instead of
     * asserting it. */
    private static final int FN_MEM_BENCH = 0x82B;
    /* 9.450 (Tier 7.1): what a guest sees at one physical page. The claim "no
     * guest scan finds our pages" was unreadable until this existed - a page can
     * look hidden in every counter while its NPT entry still says present. */
    private static final int FN_NPT_VIEW = 0x82C;

    private static native long openHandle();
    private static native void closeHandle(long handle);
    private static native int lastError();
    private static native int enableSeDebug();
    private static native int ioctl(long handle, int fn, byte[] in, byte[] out);
    private static native long[] enumeratePages(int pid, int maxPages);

    private static long handle;

    private static int ctl(int fn) {
        return (0x5947 << 16) | (fn << 2);
    }

    private static void fail(String what, int err) {
        System.err.printf("yghv: %s failed, Win32 error 0x%X%n", what, err);
        System.exit(1);
    }

    private static void check(int err, String what) {
        if (err != 0) {
            fail(what, err);
        }
    }

    private static void open() {
        int err = enableSeDebug();
        if (err != 0) {
            fail("enable SeDebugPrivilege", err);
        }
        handle = openHandle();
        if (handle == 0) {
            fail("CreateFile", lastError());
        }
    }

    private static void close() {
        if (handle != 0) {
            closeHandle(handle);
            handle = 0;
        }
    }

    private static byte[] u32(long value) {
        return ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
                .putInt((int) value).array();
    }

    private static byte[] u64(long value) {
        return ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
                .putLong(value).array();
    }

    private static long[] state() {
        byte[] out = new byte[12];
        check(ioctl(handle, FN_GET_STATE, null, out), "GET_STATE");
        ByteBuffer b = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN);
        return new long[] {
                Integer.toUnsignedLong(b.getInt()),
                Integer.toUnsignedLong(b.getInt()),
                Integer.toUnsignedLong(b.getInt())
        };
    }

    private static void printState() {
        long[] s = state();
        System.out.printf("state: active=%d pid=%d page_count=%d%n",
                s[0], s[1], s[2]);
    }

    private static void printTarget() {
        byte[] out = new byte[24];
        check(ioctl(handle, FN_GET_TARGET, null, out), "GET_TARGET");
        ByteBuffer b = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN);
        long active = Integer.toUnsignedLong(b.getInt());
        long pid = Integer.toUnsignedLong(b.getInt());
        long pageCount = Integer.toUnsignedLong(b.getInt());
        long hookCount = Integer.toUnsignedLong(b.getInt());
        long cr3 = b.getLong();
        System.out.printf(
                "target: active=%d pid=%d cr3=0x%X page_count=%d hook_count=%d%n",
                active, pid, cr3, pageCount, hookCount);
    }

    private static void listPages() {
        byte[] buf = new byte[4 + 4 + 64 * 24];
        ByteBuffer in = ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN);
        in.putInt(64);
        byte[] out = new byte[buf.length];
        System.arraycopy(buf, 0, out, 0, buf.length);
        check(ioctl(handle, FN_GET_PAGES, out, out), "GET_PAGES");
        ByteBuffer b = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN);
        b.getInt(); /* count */
        int returned = b.getInt();
        System.out.printf("list-pages: returned=%d%n", returned);
        int cap = Math.min(returned, 64); /* guard against over-large returned */
        for (int i = 0; i < cap; i++) {
            long gpa = b.getLong();
            long va = b.getLong();
            int flags = b.get() & 0xFF;
            int armed = b.get() & 0xFF;
            b.get(new byte[6]);
            System.out.printf("  gpa=0x%X va=0x%X flags=%d armed=%d%n",
                    gpa, va, flags, armed);
        }
    }

    private static void listHooks() {
        byte[] buf = new byte[4 + 4 + 4 * 24];
        ByteBuffer in = ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN);
        in.putInt(4);
        byte[] out = new byte[buf.length];
        System.arraycopy(buf, 0, out, 0, buf.length);
        check(ioctl(handle, FN_GET_HOOKS, out, out), "GET_HOOKS");
        ByteBuffer b = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN);
        b.getInt(); /* count */
        int returned = b.getInt();
        System.out.printf("list-hooks: returned=%d%n", returned);
        for (int i = 0; i < returned; i++) {
            long va = b.getLong();
            long hookId = Integer.toUnsignedLong(b.getInt());
            long installed = Integer.toUnsignedLong(b.getInt());
            long patchLen = Integer.toUnsignedLong(b.getInt());
            b.getInt(); /* reserved */
            System.out.printf("  id=%d va=0x%X installed=%d patch_len=%d%n",
                    hookId, va, installed, patchLen);
        }
    }

    private static void listTargets() {
        byte[] buf = new byte[4 + 4 + 4 * 24];
        ByteBuffer in = ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN);
        in.putInt(4);
        byte[] out = new byte[buf.length];
        System.arraycopy(buf, 0, out, 0, buf.length);
        check(ioctl(handle, FN_GET_TARGETS, out, out), "GET_TARGETS");
        ByteBuffer b = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN);
        b.getInt(); /* count */
        int returned = b.getInt();
        System.out.printf("list-targets: returned=%d%n", returned);
        for (int i = 0; i < returned; i++) {
            long active = Integer.toUnsignedLong(b.getInt());
            long pid = Integer.toUnsignedLong(b.getInt());
            long pageCount = Integer.toUnsignedLong(b.getInt());
            long hookCount = Integer.toUnsignedLong(b.getInt());
            long cr3 = b.getLong();
            System.out.printf(
                    "  active=%d pid=%d cr3=0x%X pages=%d hooks=%d%n",
                    active, pid, cr3, pageCount, hookCount);
        }
    }

    private static void clear() {
        check(ioctl(handle, FN_CLEAR, null, null), "CLEAR");
        long[] s = state();
        System.out.printf("clear: OK (active=%d pid=%d page_count=%d)%n",
                s[0], s[1], s[2]);
    }

    private static void printConfig() {
        byte[] out = new byte[8];
        check(ioctl(handle, FN_GET_CONFIG, null, out), "GET_CONFIG");
        ByteBuffer b = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN);
        long autoDisarm = Integer.toUnsignedLong(b.getInt());
        long denyStatus = Integer.toUnsignedLong(b.getInt());
        System.out.printf("config: auto_disarm=%d deny_status=0x%X%n",
                autoDisarm, denyStatus);
    }

    private static void setConfigAutoDisarm(long value) {
        if (value > 1) {
            fail("SET_CONFIG auto_disarm", 87 /* ERROR_INVALID_PARAMETER */);
        }
        byte[] cfg = new byte[8];
        ByteBuffer b = ByteBuffer.wrap(cfg).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt((int) value);
        byte[] cur = new byte[8];
        check(ioctl(handle, FN_GET_CONFIG, null, cur), "GET_CONFIG");
        b.putInt(ByteBuffer.wrap(cur).order(ByteOrder.LITTLE_ENDIAN).getInt(4));
        check(ioctl(handle, FN_SET_CONFIG, cfg, null), "SET_CONFIG");
        System.out.println("config: auto_disarm=" + value + " OK");
    }

    private static void setConfigDenyStatus(long value) {
        if (value == 0) {
            fail("SET_CONFIG deny_status", 87);
        }
        byte[] cfg = new byte[8];
        ByteBuffer b = ByteBuffer.wrap(cfg).order(ByteOrder.LITTLE_ENDIAN);
        byte[] cur = new byte[8];
        check(ioctl(handle, FN_GET_CONFIG, null, cur), "GET_CONFIG");
        b.putInt(ByteBuffer.wrap(cur).order(ByteOrder.LITTLE_ENDIAN).getInt(0));
        b.putInt((int) value);
        check(ioctl(handle, FN_SET_CONFIG, cfg, null), "SET_CONFIG");
        System.out.printf("config: deny_status=0x%X OK%n", value);
    }

    private static byte[] hookInstall(String nameOrVa, long hookId) {
        ByteBuffer b = ByteBuffer.allocate(144).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt((int) hookId);
        b.putInt(0);
        if (nameOrVa.startsWith("0x")) {
            b.putLong(Long.decode(nameOrVa));
        } else {
            byte[] name = nameOrVa.getBytes(StandardCharsets.UTF_16LE);
            if (name.length > 126) {
                fail("INSTALL_HOOK name too long", 87);
            }
            b.position(16);
            b.put(name);
        }
        return b.array();
    }

    private static byte[] hookRemove(long hookId) {
        return ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
                .putInt((int) hookId).putInt(0).array();
    }

    private static void setTarget(long pid) {
        check(ioctl(handle, FN_SET_TARGET, u32(pid), null), "SET_TARGET");
        System.out.println("set-target: pid=" + pid + " OK");
    }

    private static void addPage(long va) {
        check(ioctl(handle, FN_ADD_PAGE, u64(va), null), "ADD_PAGE");
    }

    private static void removePage(long va) {
        check(ioctl(handle, FN_REMOVE_PAGE, u64(va), null), "REMOVE_PAGE");
    }

    private static void start() {
        check(ioctl(handle, FN_START_PROTECT, null, null), "START_PROTECT");
        System.out.println("start: OK");
    }

    private static void stop() {
        check(ioctl(handle, FN_STOP_PROTECT, null, null), "STOP_PROTECT");
        System.out.println("stop: OK");
    }

    private static void listJava() {
        ProcessHandle.allProcesses()
                .filter(p -> {
                    Optional<String> cmd = p.info().commandLine();
                    Optional<String> name = p.info().command();
                    String hay = cmd.orElse("") + " " + name.orElse("");
                    return hay.toLowerCase().contains("java");
                })
                .forEach(p -> System.out.printf("pid=%d cmd=%s%n",
                        p.pid(), p.info().commandLine()
                                .orElse(p.info().command().orElse(""))));
    }

    private static void protect(long pid, int maxPages) {
        open();
        try {
            ioctl(handle, FN_STOP_PROTECT, null, null);
            setTarget(pid);

            long[] pages = enumeratePages((int) pid, maxPages);
            if (pages == null) {
                fail("OpenProcess/enumerate", lastError());
            }
            System.out.printf("protect: enumerated %d pages (cap=%d)%n",
                    pages.length, maxPages);

            int ok = 0;
            int fails = 0;
            List<String> sample = new ArrayList<>();
            for (long va : pages) {
                if (ok >= maxPages) {
                    break;
                }
                int err = ioctl(handle, FN_ADD_PAGE, u64(va), null);
                if (err == 0) {
                    ok++;
                    if (sample.size() < 3) {
                        sample.add(String.format("0x%X", va));
                    }
                } else if (fails < 3) {
                    System.out.printf("protect: add fail err=0x%X va=0x%X%n",
                            err, va);
                    fails++;
                }
            }
            System.out.printf("protect: add-page ok=%d failed=%d sample=[%s]%n",
                    ok, pages.length - ok, String.join(", ", sample));
            if (ok == 0) {
                fail("ADD_PAGE", 0);
            }

            start();
            printState();
        } finally {
            close();
        }
    }

    private static void unprotect() {
        long pid = ProcessHandle.current().pid();
        ioctl(handle, FN_STOP_PROTECT, null, null);
        setTarget(pid);
        printState();
        System.out.println("unprotect: OK");
    }

    private static void scan(long pid, int maxPages) {
        int err = enableSeDebug();
        if (err != 0) {
            fail("enable SeDebugPrivilege", err);
        }
        long[] pages = enumeratePages((int) pid, maxPages);
        if (pages == null) {
            fail("OpenProcess/enumerate", lastError());
        }
        System.out.printf("scan: pid=%d enumerated=%d (cap=%d)%n",
                pid, pages.length, maxPages);
        for (long va : pages) {
            System.out.printf("  va=0x%X%n", va);
        }
    }

    private static void usage() {
        System.out.println("Usage:");
        System.out.println("  YghvCtl state");
        System.out.println("  YghvCtl set-target <pid>");
        System.out.println("  YghvCtl add-page <hex_va>");
        System.out.println("  YghvCtl remove-page <hex_va>");
        System.out.println("  YghvCtl start");
        System.out.println("  YghvCtl stop");
        System.out.println("  YghvCtl target");
        System.out.println("  YghvCtl list-targets");
        System.out.println("  YghvCtl list-pages");
        System.out.println("  YghvCtl list-hooks");
        System.out.println("  YghvCtl install-hook <name|hex_va> [hook_id]");
        System.out.println("  YghvCtl remove-hook <hook_id>");
        System.out.println("  YghvCtl clear");
        System.out.println("  YghvCtl config [auto-disarm <0|1> | deny-status <hex>]");
        System.out.println("  YghvCtl list-java");
        System.out.println("  YghvCtl protect <pid> [maxPages]");
        System.out.println("  YghvCtl unprotect");
        System.out.println("  YghvCtl scan <pid> [maxPages]");
    }

    public static void main(String[] args) {
        String cmd = args.length > 0 ? args[0].toLowerCase() : "help";
        try {
            switch (cmd) {
                case "state":
                    open();
                    try {
                        printState();
                    } finally {
                        close();
                    }
                    break;
                case "set-target":
                    open();
                    try {
                        setTarget(Long.parseLong(args[1]));
                    } finally {
                        close();
                    }
                    break;
                case "add-page":
                    open();
                    try {
                        addPage(Long.decode(args[1]));
                        System.out.println("add-page: OK");
                    } finally {
                        close();
                    }
                    break;
                case "remove-page":
                    open();
                    try {
                        removePage(Long.decode(args[1]));
                        System.out.println("remove-page: OK");
                    } finally {
                        close();
                    }
                    break;
                case "start":
                    open();
                    try {
                        start();
                    } finally {
                        close();
                    }
                    break;
                case "stop":
                    open();
                    try {
                        stop();
                    } finally {
                        close();
                    }
                    break;
                case "target":
                    open();
                    try {
                        printTarget();
                    } finally {
                        close();
                    }
                    break;
                case "list-targets":
                    open();
                    try {
                        listTargets();
                    } finally {
                        close();
                    }
                    break;
                case "list-pages":
                    open();
                    try {
                        listPages();
                    } finally {
                        close();
                    }
                    break;
                case "list-hooks":
                    open();
                    try {
                        listHooks();
                    } finally {
                        close();
                    }
                    break;
                case "install-hook": {
                    long hid = args.length > 2 ? Long.parseLong(args[2]) : 2;
                    open();
                    try {
                        check(ioctl(handle, FN_INSTALL_HOOK,
                                hookInstall(args[1], hid), null), "INSTALL_HOOK");
                        System.out.println("install-hook: id=" + hid +
                                " target=" + args[1] + " OK");
                    } finally {
                        close();
                    }
                    break;
                }
                case "remove-hook": {
                    long hid = Long.parseLong(args[1]);
                    open();
                    try {
                        check(ioctl(handle, FN_REMOVE_HOOK,
                                hookRemove(hid), null), "REMOVE_HOOK");
                        System.out.println("remove-hook: id=" + hid + " OK");
                    } finally {
                        close();
                    }
                    break;
                }
                case "clear":
                    open();
                    try {
                        clear();
                    } finally {
                        close();
                    }
                    break;
                case "config":
                    open();
                    try {
                        if (args.length == 1) {
                            printConfig();
                        } else if ("auto-disarm".equalsIgnoreCase(args[1])) {
                            setConfigAutoDisarm(Long.parseLong(args[2]));
                        } else if ("deny-status".equalsIgnoreCase(args[1])) {
                            setConfigDenyStatus(Long.decode(args[2]));
                        } else {
                            usage();
                        }
                    } finally {
                        close();
                    }
                    break;
                case "list-java":
                    listJava();
                    break;
                case "protect": {
                    long pid = Long.parseLong(args[1]);
                    int maxPages = args.length > 2
                            ? Math.min(Integer.parseInt(args[2]), 64)
                            : 64;
                    protect(pid, maxPages);
                    break;
                }
                case "unprotect":
                    open();
                    try {
                        unprotect();
                    } finally {
                        close();
                    }
                    break;
                case "scan": {
                    long pid = Long.parseLong(args[1]);
                    int maxPages = args.length > 2
                            ? Math.min(Integer.parseInt(args[2]), 64)
                            : 64;
                    scan(pid, maxPages);
                    break;
                }
                default:
                    usage();
                    break;
            }
        } catch (ArrayIndexOutOfBoundsException | NumberFormatException e) {
            System.err.println("yghv: bad arguments");
            usage();
            System.exit(2);
        }
    }
}
