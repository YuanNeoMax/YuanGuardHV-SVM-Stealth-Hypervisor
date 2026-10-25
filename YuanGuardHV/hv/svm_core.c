#include <ntddk.h>
#include "svm_defs.h"
#include "svm_vcpu.h"
#include "vmcb.h"
#include "debug.h"
/* YGHV_DEBUG_LOG is provided by build.bat; do not redefine locally. */

NTKERNELAPI NTSTATUS ZwYieldExecution(void);

svm_vcpu_t *g_vcpus[SVM_MAX_CORES];
ULONG g_vcpu_count;
static BOOLEAN g_svm_nested;
#ifdef YGHV_R1_EXCLUDE_PRIVATE
extern volatile BOOLEAN g_r1_diag;
extern volatile ULONG g_r1_diag_count;
extern volatile ULONG g_r1_entry_seq;
#endif

/* --- MSR helpers --- */
static uint64_t yg_read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void yg_write_msr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static void yg_svm_vmsave(uint64_t vmcb_pa) {
    __asm__ volatile(".byte 0x0F, 0x01, 0xDB" : : "a"(vmcb_pa) : "memory");
}

static void yg_svm_vmload(uint64_t vmcb_pa) {
    __asm__ volatile(".byte 0x0F, 0x01, 0xDA" : : "a"(vmcb_pa) : "memory");
}

/* --- Segment helpers --- */
static uint16_t yg_read_cs(void) {
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

static uint16_t yg_read_ss(void) {
    uint16_t ss;
    __asm__ volatile("mov %%ss, %0" : "=r"(ss));
    return ss;
}

static uint16_t yg_read_ds(void) {
    uint16_t ds;
    __asm__ volatile("mov %%ds, %0" : "=r"(ds));
    return ds;
}

static uint16_t yg_read_es(void) {
    uint16_t es;
    __asm__ volatile("mov %%es, %0" : "=r"(es));
    return es;
}

static uint16_t yg_read_gs(void) {
    uint16_t gs;
    __asm__ volatile("mov %%gs, %0" : "=r"(gs));
    return gs;
}

static uint16_t yg_read_fs(void) {
    uint16_t fs;
    __asm__ volatile("mov %%fs, %0" : "=r"(fs));
    return fs;
}

static void yg_read_gdtr(uint64_t *base, uint16_t *limit) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    *limit = gdtr.limit;
    *base = gdtr.base;
}

static void yg_read_idtr(uint64_t *base, uint16_t *limit) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr;
    __asm__ volatile("sidt %0" : "=m"(idtr));
    *limit = idtr.limit;
    *base = idtr.base;
}

static uint16_t yg_read_tr(void) {
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    return tr;
}

static uint64_t yg_read_rflags(void) {
    uint64_t rflags;
    __asm__ volatile(
        "pushfq\n"
        "pop %0\n"
        : "=r"(rflags)
    );
    return rflags;
}

/* --- CR helpers --- */
static uint64_t yg_read_cr0(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}
static uint64_t yg_read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}
static uint64_t yg_read_cr4(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

/* --- SVM detection --- */
int cpu_has_svm(void) {
    int cpu_info[4];
    __cpuidex(cpu_info, CPUID_AMD_EXTENDED, 0);
    if ((uint32_t)cpu_info[0] < CPUID_AMD_SVM)
        return 0;
    __cpuidex(cpu_info, CPUID_AMD_SVM, 0);
    return (cpu_info[2] & CPUID_SVM_FEATURE_SVM) != 0;
}

static int cpu_has_npt(void) {
    int cpu_info[4];
    __cpuidex(cpu_info, CPUID_AMD_NPT, 0);
    return (cpu_info[3] & CPUID_NPT_FEATURE_NPT) != 0;
}

/* --- VCPU allocation --- */
int svm_alloc_vcpu(uint32_t core_id, svm_vcpu_t **out) {
    svm_vcpu_t *vcpu;

    vcpu = (svm_vcpu_t *)ExAllocatePoolWithTag(NonPagedPool, sizeof(svm_vcpu_t), YGHV_TAG);
    if (!vcpu) goto fail;
    RtlZeroMemory(vcpu, sizeof(svm_vcpu_t));
    KeInitializeEvent(&vcpu->pause_done_event, NotificationEvent, FALSE);
    KeInitializeEvent(&vcpu->resume_event, NotificationEvent, FALSE);

    /* VMCB — 4KB aligned physical page */
    vcpu->vmcb = (vmcb_t *)MmAllocateContiguousMemory(HV_PAGE_SIZE, (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (!vcpu->vmcb) goto fail_vmcb;
    RtlZeroMemory(vcpu->vmcb, HV_PAGE_SIZE);
    vcpu->vmcb_pa = MmGetPhysicalAddress(vcpu->vmcb).QuadPart;

    /* Host VMCB — for VMLOAD after VMRUN exit (resident mode) */
    vcpu->host_vmcb = (vmcb_t *)MmAllocateContiguousMemory(HV_PAGE_SIZE, (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (!vcpu->host_vmcb) goto fail_host_vmcb;
    RtlZeroMemory(vcpu->host_vmcb, HV_PAGE_SIZE);
    vcpu->host_vmcb_pa = MmGetPhysicalAddress(vcpu->host_vmcb).QuadPart;

    /* HSave area — 4KB aligned */
    vcpu->hsave = MmAllocateContiguousMemory(HV_PAGE_SIZE, (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (!vcpu->hsave) goto fail_hsave;
    RtlZeroMemory(vcpu->hsave, HV_PAGE_SIZE);
    vcpu->hsave_pa = MmGetPhysicalAddress(vcpu->hsave).QuadPart;

    /* Host stack — 4 pages */
    vcpu->host_stack = MmAllocateContiguousMemory(SVM_HOST_STACK_PAGES * HV_PAGE_SIZE,
        (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (!vcpu->host_stack) goto fail_stack;
    RtlZeroMemory(vcpu->host_stack, SVM_HOST_STACK_PAGES * HV_PAGE_SIZE);
    vcpu->host_stack_top = (uint64_t)vcpu->host_stack + SVM_HOST_STACK_PAGES * HV_PAGE_SIZE - 8;

    /* Guest stack — 1 page, stays NPT-visible so synthetic guests have a
       valid stack even after hypervisor-private pages are excluded. */
    vcpu->guest_stack = MmAllocateContiguousMemory(
        HV_PAGE_SIZE, (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (!vcpu->guest_stack) goto fail_guest_stack;
    RtlZeroMemory(vcpu->guest_stack, HV_PAGE_SIZE);
    vcpu->guest_stack_pa = MmGetPhysicalAddress(vcpu->guest_stack).QuadPart;

    /* 9.171: dedicated guest TSS page (copy of the host TSS).  OS-as-guest
       Windows context switch writes TSS.RSP0; if the guest TR shares the host
       TSS that pollutes the host RSP0 and the host faults on the next
       interrupt -> whole-machine freeze.  The guest TR is repointed to this
       page by yghv_os_guest_tss_isolate(); synthetic residents keep the host
       TR.  Allocated here so it is per-vcpu and freed with the vcpu. */
    vcpu->guest_tss = MmAllocateContiguousMemory(
        HV_PAGE_SIZE, (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (!vcpu->guest_tss) goto fail_guest_tss;
    RtlZeroMemory(vcpu->guest_tss, HV_PAGE_SIZE);
    vcpu->guest_tss_pa = MmGetPhysicalAddress(vcpu->guest_tss).QuadPart;

    /* MSRPM — 2 pages, zero = allow MSR access (bit=1 means intercept) */
    vcpu->msrpm = MmAllocateContiguousMemory(SVM_MSRPM_PAGES * HV_PAGE_SIZE,
        (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (!vcpu->msrpm) goto fail_msrpm;
    RtlZeroMemory(vcpu->msrpm, SVM_MSRPM_PAGES * HV_PAGE_SIZE);
    vcpu->msrpm_pa = MmGetPhysicalAddress(vcpu->msrpm).QuadPart;

    /* IOPM — 3 pages, zero = allow IO (bit=1 means intercept) */
    vcpu->iopm = MmAllocateContiguousMemory(SVM_IOPM_PAGES * HV_PAGE_SIZE,
        (PHYSICAL_ADDRESS){ .QuadPart = 0xFFFFFFFF });
    if (!vcpu->iopm) goto fail_iopm;
    RtlZeroMemory(vcpu->iopm, SVM_IOPM_PAGES * HV_PAGE_SIZE);
    vcpu->iopm_pa = MmGetPhysicalAddress(vcpu->iopm).QuadPart;

    /* Per-vcpu control key: derived from boot time + private PA so stealing
       the global cookie alone does not grant control commands. */
    {
        LARGE_INTEGER now;
        KeQuerySystemTime(&now);
        vcpu->auth_key = (uint64_t)now.QuadPart ^ vcpu->vmcb_pa ^
                         vcpu->host_vmcb_pa ^ (uint64_t)core_id ^
                         0x59475648ULL;
        if (!vcpu->auth_key)
            vcpu->auth_key = 0x59475648ULL ^ (uint64_t)core_id;
    }

    LOG_ERROR("alloc vcpu core=%u: vmcb=0x%llx host_vmcb=0x%llx hsave=0x%llx host_stack=0x%llx msrpm=0x%llx iopm=0x%llx",
        core_id, vcpu->vmcb_pa, vcpu->host_vmcb_pa, vcpu->hsave_pa,
        MmGetPhysicalAddress(vcpu->host_stack).QuadPart,
        vcpu->msrpm_pa, vcpu->iopm_pa);

    vcpu->resident_state = SVM_RESIDENT_OFF;
    *out = vcpu;
    return STATUS_SUCCESS;

fail_iopm:
    MmFreeContiguousMemory(vcpu->msrpm);
fail_msrpm:
    if (vcpu->guest_tss) MmFreeContiguousMemory(vcpu->guest_tss);
fail_guest_tss:
    MmFreeContiguousMemory(vcpu->host_stack);
fail_guest_stack:
    if (vcpu->guest_stack) MmFreeContiguousMemory(vcpu->guest_stack);
fail_stack:
    MmFreeContiguousMemory(vcpu->hsave);
fail_hsave:
    MmFreeContiguousMemory(vcpu->host_vmcb);
fail_host_vmcb:
    MmFreeContiguousMemory(vcpu->vmcb);
fail_vmcb:
    ExFreePool(vcpu);
fail:
    LOG_ERROR("svm_alloc_vcpu: allocation failed for core %u", core_id);
    return STATUS_INSUFFICIENT_RESOURCES;
}

static void svm_free_vcpu(svm_vcpu_t *vcpu) {
    if (!vcpu) return;

    /*
     * 9.388: CANCEL AND DRAIN THE AVIC TIMER BEFORE FREEING THE VCPU.
     *
     * vmexit.c initialises a DPC and arms it with KeSetTimer:
     *
     *     KeInitializeDpc(&vcpu->avic_timer_dpc, yghv_avic_timer_dpc, vcpu);
     *     KeSetTimer(&vcpu->avic_timer, due, &vcpu->avic_timer_dpc);
     *
     * The DPC's context parameter IS the vcpu. A timer DPC runs at
     * DISPATCH_LEVEL, and the timer's expiry routine sits at CLOCK_LEVEL.
     *
     * This function freed the vcpu without cancelling that timer, so any armed
     * timer fired afterwards with a pointer into freed pool. That is exactly the
     * signature of five of this session's bugchecks:
     *
     *     DRIVER_IRQL_NOT_LESS_OR_EQUAL (0xD1)
     *     Arg2 = 0xE   (CLOCK_LEVEL - a timer expiry context, not an arbitrary one)
     *     Arg3 = 8     (an 8-byte field read)
     *     faulting addresses two ADJACENT pages (0x…9000 / 0x…a000)
     *
     * KeCancelTimer is not enough on its own: a DPC may already be queued when the
     * timer is cancelled, and it will still run. KeFlushQueuedDpcs waits for the
     * DPC's queue entry to drain, so the vcpu cannot be freed while a callback is
     * between "dequeued" and "returned". That ordering is the whole point: cancel,
     * then drain, then free - any other order leaves the same window.
     */
    if (vcpu->avic_timer_initialized) {
        /* Only touch a KTIMER/KDPC that was actually initialised: KeCancelTimer on
         * a zeroed KTIMER dereferences a NULL timer list head.
         *
         * The DRAIN is NOT done here: KeFlushQueuedDpcs waits for every core's DPC
         * queue, so calling it once per vcpu would cost 12 global drains on this
         * machine. svm_core_cleanup cancels all timers first and drains ONCE after
         * the loop - cancelling everywhere before draining anywhere is also the
         * safer order, because a DPC queued by vcpu N cannot fire while vcpu N is
         * still being freed. */
        KeCancelTimer(&vcpu->avic_timer);
        vcpu->avic_timer_initialized = FALSE;
        InterlockedExchange((volatile LONG *)&vcpu->avic_timer_armed, 0);
    }

    /*
     * 9.388b: THE AVIC STATE WAS NEVER RELEASED HERE AT ALL.
     *
     * main.c has yghv_avic_cleanup(vcpu), which frees avic_backing_page,
     * avic_physical_id_table, avic_logical_id_table and unmaps avic_host_apic_va.
     * That function is called from the thread-creation failure paths (3491, 3504)
     * and from yghv_avic_init_self (2199) - but NOT from the teardown that actually
     * runs on unload, which is this one.
     *
     * So on every unload those mappings and pages were simply leaked, and - more
     * importantly - the timer that points INTO avic_backing_page was left armed
     * while the page it reads (with vcpu as its DPC context) was going away.
     *
     * Declared here rather than included: yghv_avic_cleanup is static in main.c and
     * this module is compiled separately. A forward declaration keeps the ownership
     * in main.c, where the rest of the AVIC code lives.
     */
    {
        extern void yghv_avic_cleanup_release(svm_vcpu_t *v);
        yghv_avic_cleanup_release(vcpu);
    }

    if (vcpu->iopm)  MmFreeContiguousMemory(vcpu->iopm);
    if (vcpu->msrpm) MmFreeContiguousMemory(vcpu->msrpm);
    if (vcpu->guest_tss) MmFreeContiguousMemory(vcpu->guest_tss);
    if (vcpu->guest_stack) MmFreeContiguousMemory(vcpu->guest_stack);
    if (vcpu->host_stack) MmFreeContiguousMemory(vcpu->host_stack);
    if (vcpu->hsave) MmFreeContiguousMemory(vcpu->hsave);
    if (vcpu->host_vmcb) MmFreeContiguousMemory(vcpu->host_vmcb);
    if (vcpu->vmcb) MmFreeContiguousMemory(vcpu->vmcb);
    ExFreePool(vcpu);
}

/* --- Read segment descriptor attrib/limit/base from GDT --- */
static void yg_read_seg_descriptor(uint16_t selector, uint16_t *attrib, uint32_t *limit, uint64_t *base) {
    uint64_t gdt_base;
    uint16_t gdt_limit;
    yg_read_gdtr(&gdt_base, &gdt_limit);

    if ((selector & ~7) >= gdt_limit) {
        /* Invalid selector — use zero values */
        *attrib = 0;
        *limit = 0;
        *base = 0;
        return;
    }

    uint64_t *desc = (uint64_t *)(gdt_base + (selector & ~7));
    uint64_t lo = desc[0];
    uint64_t hi = 0;

    /* Code/data descriptors are 8 bytes in long mode; only system
       descriptors (S=0, e.g. TSS/LDT) occupy a second 8-byte slot. */
    if (!((lo >> 44) & 1))
        hi = desc[1];

    /* VMCB attr: bits 0-7 = GDT type/S/DPL/P (40-47), bits 8-11 = AVL/L/D/B/G (52-55) */
    *attrib = (uint16_t)((lo >> 40) & 0xFF) | (uint16_t)(((lo >> 52) & 0xF) << 8);

    /* Limit: bits 0-15 + bits 48-51, expanded by G bit (55) */
    *limit = (uint32_t)((lo & 0xFFFF) | ((lo >> 32) & 0xF0000));
    if (lo & (1ULL << 55))
        *limit = (*limit << 12) | 0xFFF;

    /* Base: low dword bits 16-39 and 56-63, plus high dword bits 0-31 */
    *base = ((lo >> 16) & 0xFFFF)
          | (((lo >> 32) & 0xFF) << 16)
          | (((lo >> 56) & 0xFF) << 24)
          | ((hi & 0xFF) << 32)
          | (((hi >> 8) & 0xFF) << 40)
          | ((hi >> 16) << 48);

    LOG_ERROR("seg sel=0x%x lo=0x%llx hi=0x%llx attrib=0x%x limit=0x%x base=0x%llx",
        selector, lo, hi, *attrib, *limit, *base);
}

/* --- VCPU preparation --- */
void svm_prepare_vcpu(svm_vcpu_t *vcpu, uint64_t guest_rip) {
    vmcb_control_t *ctrl = &vcpu->vmcb->control;
    vmcb_state_t *state = &vcpu->vmcb->state;
    uint64_t gdt_base, idt_base;
    uint16_t gdt_limit, idt_limit;

    /* — Control area — */
    ctrl->general1_intercepts = 0;
    ctrl->general1_intercepts |= INTERCEPT_CPUID;
    ctrl->general2_intercepts = INTR_GEN2(SVM_INTERCEPT_VMRUN) |
                                INTR_GEN2(SVM_INTERCEPT_VMMCALL);
    ctrl->cr_read_intercepts = 0;
    ctrl->cr_write_intercepts = 0;
    ctrl->dr_read_intercepts = 0;
    ctrl->dr_write_intercepts = 0;
    ctrl->exception_intercepts = (1ULL << 1); /* #DB: single-step re-arm for NPT write protection */

    /* Bitmap base addresses */
    ctrl->msrpm_base_pa = vcpu->msrpm_pa;
    ctrl->iopm_base_pa = vcpu->iopm_pa;

    /* v93: intercept VM_CR so the guest cannot observe SVM as enabled.
       bhyve/illumos: Windows ignores CPUID SVM bits and reads 0xC0010114. */
    {
        uint8_t *msrpm = (uint8_t *)vcpu->msrpm;
        uint32_t vmcr_delta = MSR_VM_CR - 0xC0010000u;
        uint32_t vmcr_byte = (vmcr_delta / 4u) + 2u * 2048u;
        uint32_t vmcr_bit = (vmcr_delta & 3u) * 2u;
        msrpm[vmcr_byte] |= (uint8_t)(0x3u << vmcr_bit);
    }
    /* 9.174: intercept guest rdmsr/wrmsr of MSR_GS_BASE (0xC0000101) and
       MSR_KERNEL_GS_BASE (0xC0000102).  Windows context switch saves/restores
       per-CPU GS state via these MSRs; without interception the guest's wrmsr
       writes straight through to the PHYSICAL MSR, polluting the host's GS
       state (and vice versa on VMEXIT).  By intercepting and updating the VMCB
       state (svm_handle_msr) the guest's GS/KERNEL_GS base lives entirely in
       the VMCB, isolated from the host — the standard KVM approach, and the
       missing half of the 9.168 GS-base virtualization plan (selector mirroring
       was 9.170).  swapgs cannot be intercepted on SVM, but wrmsr can. */
    {
        uint8_t *msrpm = (uint8_t *)vcpu->msrpm;
        uint32_t i;
        for (i = 0; i < 2; i++) {
            uint32_t msr = 0xC0000101u + i;   /* GS_BASE, KERNEL_GS_BASE */
            uint32_t delta = msr - 0xC0000000u;
            uint32_t byte = (delta / 4u) + 1u * 2048u;  /* 0xC0000000 bank */
            uint32_t bit = (delta & 3u) * 2u;
            msrpm[byte] |= (uint8_t)(0x3u << bit);       /* read+write */
        }
    }

    /* NPT disabled for minimal test */
    ctrl->np_enable = 0;
    /* Guest ASID — ASID 0 reserved for host, use core_id+1 in real multi-core */
    ctrl->guest_asid = 1;
    ctrl->tlb_control = 0;     /* Do nothing — 3 is a known VMware nested-SVM INVALID trigger */

    /* VMCB clean bits — 0 = force re-read all fields on VMRUN */
    ctrl->vmcb_clean_bits = 0;

    /* — State area — */
    /* Segment selectors (mirror host) */
    state->cs_selector = yg_read_cs();
    state->ss_selector = yg_read_ss();
    state->ds_selector = yg_read_ds();
    state->es_selector = yg_read_es();

    /* Read segment descriptors for attrib/limit/base */
    yg_read_seg_descriptor(state->cs_selector, &state->cs_attrib, &state->cs_limit, &state->cs_base);
    yg_read_seg_descriptor(state->ss_selector, &state->ss_attrib, &state->ss_limit, &state->ss_base);
    yg_read_seg_descriptor(state->ds_selector, &state->ds_attrib, &state->ds_limit, &state->ds_base);
    yg_read_seg_descriptor(state->es_selector, &state->es_attrib, &state->es_limit, &state->es_base);
    /* FS/GS base from MSR — mirror the HOST selectors (not 0!).  A null GS
       selector makes every gs:[...] access in the guest #GP — Windows x64
       kernel uses GS (KPCR / exception stack) on every context switch, so a
       null GS selector is exactly what crashed OS-as-guest resident in
       KiAbProcessContextSwitch -> KiAbEntryGetLockedHeadEntry -> #GP ->
       KiExceptionDispatchOnExceptionStack -> 0x139 MISSING_GSFRAME_STACKPTR
       (v100/v100b dumps both show KTRAP_FRAME.GsBase=0/SegGs=0). */
    state->gs_selector = yg_read_gs();
    state->gs_base = yg_read_msr(0xC0000101); /* MSR_GS_BASE */
    state->fs_selector = yg_read_fs();
    state->fs_base = yg_read_msr(0xC0000100); /* MSR_FS_BASE */

    /* Descriptor tables — mirror host */
    yg_read_gdtr(&gdt_base, &gdt_limit);
    state->gdtr_base = gdt_base;
    state->gdtr_limit = gdt_limit;
    yg_read_idtr(&idt_base, &idt_limit);
    state->idtr_base = idt_base;
    state->idtr_limit = idt_limit;

    /* TR — mirror host */
    state->tr_selector = yg_read_tr();
    yg_read_seg_descriptor(state->tr_selector, &state->tr_attrib, &state->tr_limit, &state->tr_base);

    /* LDTR — null (x64 kernel doesn't use LDT) */
    state->ldtr_selector = 0;

    /* Control registers — mirror host */
    state->cr0 = yg_read_cr0();
    state->cr3 = yg_read_cr3();
    state->cr4 = yg_read_cr4();
    state->efer = yg_read_msr(MSR_EFER);

    /* DR6/DR7 — mirror host */
    state->dr6 = 0xFFFF0FF0;
    state->dr7 = 0x400;

    /* RFLAGS — use host rflags with IF set */
    state->rflags = yg_read_rflags() | 0x200;

    /* CPL — ring 0 */
    state->cpl = 0;

    /* Guest RIP/RSP */
    state->rip = guest_rip;
    state->rsp = vcpu->host_stack_top;

    /* Syscall MSRs — mirror host */
    state->star = yg_read_msr(0xC0000081);
    state->lstar = yg_read_msr(0xC0000082);
    state->cstar = yg_read_msr(0xC0000083);
    state->sfmask = yg_read_msr(0xC0000084);
    state->sysenter_cs = yg_read_msr(0x174);
    state->sysenter_esp = yg_read_msr(0x175);
    state->sysenter_eip = yg_read_msr(0x176);
    state->kernel_gs_base = yg_read_msr(0xC0000102); /* MSR_KERNEL_GS_BASE */
    state->cr2 = 0;
    state->g_pat = yg_read_msr(MSR_IA32_PAT);

    /* Let hardware fill FS/GS/TR/LDTR hidden state + system MSRs into the
       guest VMCB, and snapshot host state for VMLOAD after VMEXIT. */
    yg_svm_vmsave(vcpu->vmcb_pa);
    yg_svm_vmsave(vcpu->host_vmcb_pa);

    /* Full VMCB state dump for diagnosing VMEXIT_INVALID under VMware nested SVM */
    LOG_ERROR("VMCB dump: es=%x/%x/%x/0x%llx cs=%x/%x/%x/0x%llx ss=%x/%x/%x/0x%llx ds=%x/%x/%x/0x%llx",
        state->es_selector, state->es_attrib, state->es_limit, state->es_base,
        state->cs_selector, state->cs_attrib, state->cs_limit, state->cs_base,
        state->ss_selector, state->ss_attrib, state->ss_limit, state->ss_base,
        state->ds_selector, state->ds_attrib, state->ds_limit, state->ds_base);
    LOG_ERROR("VMCB dump: fs=%x/%x/%x/0x%llx gs=%x/%x/%x/0x%llx gdtr=%x/%x/%x/0x%llx ldtr=%x/%x/%x/0x%llx",
        state->fs_selector, state->fs_attrib, state->fs_limit, state->fs_base,
        state->gs_selector, state->gs_attrib, state->gs_limit, state->gs_base,
        state->gdtr_selector, state->gdtr_attrib, state->gdtr_limit, state->gdtr_base,
        state->ldtr_selector, state->ldtr_attrib, state->ldtr_limit, state->ldtr_base);
    LOG_ERROR("VMCB dump: idtr=%x/%x/%x/0x%llx tr=%x/%x/%x/0x%llx cpl=%u efer=0x%llx tlb_ctl=%u",
        state->idtr_selector, state->idtr_attrib, state->idtr_limit, state->idtr_base,
        state->tr_selector, state->tr_attrib, state->tr_limit, state->tr_base,
        state->cpl, state->efer, (unsigned)ctrl->tlb_control);

    {
        int svm_feat[4];
        __cpuidex(svm_feat, CPUID_AMD_NPT, 0);
        LOG_ERROR("CPUID 8000000A: eax=%08x ebx=%08x ecx=%08x edx=%08x (NPT=%d FLUSHBYASID=%d)",
            (unsigned)svm_feat[0], (unsigned)svm_feat[1],
            (unsigned)svm_feat[2], (unsigned)svm_feat[3],
            (svm_feat[3] & CPUID_NPT_FEATURE_NPT) ? 1 : 0,
            (svm_feat[3] & CPUID_NPT_FEATURE_FLUSHBYASID) ? 1 : 0);
    }

    LOG_INFO("svm_prepare_vcpu: guest_rip=0x%llx, cr3=0x%llx, efer=0x%llx",
        guest_rip, state->cr3, state->efer);
}

/* --- SVM core init --- */
int svm_core_init(void) {
    uint64_t efer;
    NTSTATUS status;
    ULONG core_id = KeGetCurrentProcessorNumber();

    if (!cpu_has_svm()) {
        LOG_ERROR("SVM not supported on this CPU");
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }
    LOG_INFO("SVM detected");
    {
        int cpu_info[4];
        __cpuidex(cpu_info, 1, 0);
        g_svm_nested = (cpu_info[2] & (1U << 31)) ? TRUE : FALSE;
        __cpuidex(cpu_info, 0x40000000, 0);
        if (!g_svm_nested &&
            (cpu_info[1] == 0x61774D56 ||   /* "VMwa" (VMware) */
             cpu_info[1] == 0x7269634D)) {  /* "Micr" (Hyper-V) */
            g_svm_nested = TRUE;
        }
        LOG_INFO("SVM nested (hypervisor present): %s",
            g_svm_nested ? "yes" : "no");
    }

    if (cpu_has_npt())
        LOG_INFO("NPT supported");
    else
        LOG_WARN("NPT not supported — identity map won't be available");

    if (core_id >= SVM_MAX_CORES) {
        LOG_ERROR("Core %u exceeds SVM_MAX_CORES=%u", (unsigned)core_id, SVM_MAX_CORES);
        return STATUS_UNSUCCESSFUL;
    }

    /* Enable SVM if not already set (firmware/L0 may have set it). Save old state. */
    efer = yg_read_msr(MSR_EFER);
    BOOLEAN svme_was_set = (efer & EFER_SVME) ? TRUE : FALSE;
    if (efer & EFER_SVME) {
        LOG_INFO("EFER.SVME already set");
    } else {
        efer |= EFER_SVME;
        yg_write_msr(MSR_EFER, efer);
        LOG_INFO("EFER.SVME set by hypervisor");
    }

    /* Save old VM_HSAVE for cleanup restore */
    uint64_t old_hsave = yg_read_msr(MSR_VM_HSAVE);
    (void)old_hsave;

    /* Allocate and prepare VCPU for current core */
    status = (NTSTATUS)svm_alloc_vcpu(core_id, &g_vcpus[core_id]);
    if (!NT_SUCCESS(status)) {
        /* REV-016: alloc failed -> g_vcpus[core_id] is NULL so the cleanup
           IPI cannot restore EFER.SVME on this core; restore it here (only
           if we are the ones who set it). */
        if (!svme_was_set) {
            uint64_t now = yg_read_msr(MSR_EFER);
            if (now & EFER_SVME) {
                now &= ~EFER_SVME;
                yg_write_msr(MSR_EFER, now);
            }
        }
        svm_core_cleanup();
        return status;
    }
    g_vcpu_count++;
    /* Save state for cleanup restore */
    g_vcpus[core_id]->old_hsave = yg_read_msr(MSR_VM_HSAVE);
    g_vcpus[core_id]->old_efer = yg_read_msr(MSR_EFER);

    svm_prepare_vcpu(g_vcpus[core_id], (uint64_t)svm_trampoline_test_guest);
    /* Resident test: heartbeat loops in the guest; vmmcall.c auto-stops
       after a bounded number of exits so DriverEntry can return. */
    g_vcpus[core_id]->regs.rax = YGHV_CMD_HEARTBEAT;
    g_vcpus[core_id]->vmcb->state.rax = YGHV_CMD_HEARTBEAT;
    LOG_INFO("svm_core_init: VCPU[%u] ready", (unsigned)core_id);
    return STATUS_SUCCESS;
}

/*
 * 9.380: THE EFER WRITE IS GONE, AND THAT IS THE FIX - NOT A WORKAROUND.
 *
 * WHAT THE DOCUMENTATION SAYS (AMD APM, EFER.SVME, bit 12):
 *
 *   "The effect of turning off EFER.SVME WHILE A GUEST IS RUNNING IS UNDEFINED;
 *    therefore the VMM should ..."
 *
 * What this tree already does correctly: svm_simplevm206.cpp:3094 clears
 * EFER.SVME INSIDE the ExitVm path, after STGI and after vmload of the guest
 * state - i.e. at the one moment the CPU is no longer in guest mode. That is the
 * documented sequence and it has been running for the whole 206 line.
 *
 * What this function did, and why it crashed three times:
 *
 *   KeIpiGenericCall(svm_core_ipi_cleanup)  fires an IPI at every core
 *     -> that core runs this callback IN AN INTERRUPT, possibly still in guest
 *        mode or in the middle of leaving it
 *     -> yg_write_msr(MSR_EFER, efer & ~EFER_SVME)   UNDEFINED per the APM
 *     -> measured: 0x1E KMODE_EXCEPTION_NOT_HANDLED, P1=0xC0000096
 *        PRIVILEGED_INSTRUCTION, at exactly the `wrmsr` (svm_core_ipi_cleanup+0x5F)
 *
 * Three attempts failed to fix it by changing WHEN the ack was written, because
 * the ack was never the problem: the ack only decides whether the branch is
 * entered, and the branch should not exist in this build at all.
 *
 * Why it is safe to remove: on the 206 path, de-virtualization happens through
 * the CPUID_UNLOAD_SIMPLE_SVM backdoor (Sv206CoopUnload -> SvDevirtualizeAll-
 * Processors), and each core clears its own EFER.SVME from the ExitVm path. By
 * the time this IPI runs, SVME is either already clear (the branch would skip on
 * its own condition) or is being cleared from a context where it is legal. Either
 * way this IPI's EFER write adds nothing except a race.
 *
 * What is kept: MSR_VM_HSAVE. Restoring that is not undefined in any context -
 * it is a plain MSR write - and it is the thing that must be undone before the
 * VPD is freed.
 */
static ULONG_PTR svm_core_ipi_cleanup(ULONG_PTR arg) {
    (void)arg;
    ULONG core = KeGetCurrentProcessorNumber();
    svm_vcpu_t *vcpu = (core < SVM_MAX_CORES) ? g_vcpus[core] : NULL;

    if (vcpu) {
        if (vcpu->old_hsave)
            yg_write_msr(MSR_VM_HSAVE, vcpu->old_hsave);
        /* EFER.SVME intentionally NOT written here. See the note above: the APM
         * calls that undefined while a guest may still be running, and the 206
         * ExitVm path already does it in the only legal window. */
    }
    return 0;
}

int svm_core_cleanup(void) {
    ULONG i;

    /* Restore per-core MSR state while VMCB/hsave memory is still valid. */
    KeIpiGenericCall(svm_core_ipi_cleanup, 0);

    /*
     * 9.388c: CANCEL EVERY TIMER BEFORE FREEING ANY VCPU.
     *
     * A timer DPC carries the vcpu as its context. If freeing vcpu N is followed
     * by a DPC for vcpu M that was still armed, the DPC reads freed pool. So the
     * cancel pass runs over all of them first, then one drain, then the free pass.
     * That is three passes instead of one, and it is the only order where no DPC
     * can outlive the object it points at.
     */
    for (i = 0; i < SVM_MAX_CORES; i++) {
        if (g_vcpus[i] && g_vcpus[i]->avic_timer_initialized) {
            KeCancelTimer(&g_vcpus[i]->avic_timer);
            g_vcpus[i]->avic_timer_initialized = FALSE;
            InterlockedExchange((volatile LONG *)&g_vcpus[i]->avic_timer_armed, 0);
        }
    }
    /* One global drain: a DPC already dequeued on any core has returned by now. */
    KeFlushQueuedDpcs();

    for (i = 0; i < SVM_MAX_CORES; i++) {
        if (g_vcpus[i]) {
            svm_free_vcpu(g_vcpus[i]);
            g_vcpus[i] = NULL;
        }
    }
    g_vcpu_count = 0;
    return STATUS_SUCCESS;
}

int svm_core_prepare_probe(uint32_t count) {
    (void)count;
    return STATUS_NOT_IMPLEMENTED;
}

int svm_core_probe_current(uint32_t index, svm_probe_result_t *result) {
    (void)index;
    if (result) RtlZeroMemory(result, sizeof(*result));
    return STATUS_NOT_IMPLEMENTED;
}

int svm_core_run_trampoline_once(uint32_t core_id) {
    svm_vcpu_t *vcpu;
    uint64_t exitcode;

    if (core_id >= SVM_MAX_CORES || !g_vcpus[core_id])
        return -1;

    vcpu = g_vcpus[core_id];

    LOG_INFO("VMRUN single-shot: core=%u", core_id);

    exitcode = svm_vmrun_trampoline(vcpu);

    LOG_INFO("VMRUN exit: code=0x%llx, has_vmmcall=%d",
        exitcode, exitcode == SVM_EXIT_VMMCALL ? 1 : 0);

    return (exitcode == SVM_EXIT_VMMCALL) ? 0 : (int)exitcode;
}

int svm_core_run_trampoline_test(uint32_t core_id) {
    return svm_core_run_trampoline_once(core_id);
}

/* --- VCPU accessor --- */
svm_vcpu_t *svm_core_get_vcpu(uint32_t index) {
    if (index >= SVM_MAX_CORES || !g_vcpus[index])
        return NULL;
    return g_vcpus[index];
}

/* --- NPT configuration --- */
int svm_core_set_npt(uint32_t core_id, uint64_t ncr3) {
    svm_vcpu_t *vcpu = svm_core_get_vcpu(core_id);
    if (!vcpu) return STATUS_NOT_FOUND;
    /* enable NPT with identity-map covering all phys memory */
    vcpu->vmcb->control.np_enable = SVM_NP_ENABLE;
    vcpu->vmcb->control.ncr3 = ncr3;
    vcpu->resident_index = core_id;
    return STATUS_SUCCESS;
}

extern int svm_dispatch_exit(svm_vcpu_t *vcpu);

/* --- Resident lifecycle --- */
int svm_core_prepare_resident(uint32_t count) {
    (void)count;
    return STATUS_SUCCESS;
}

int svm_resident_try_activate(svm_vcpu_t *vcpu) {
    LONG old;

    do {
        old = InterlockedCompareExchange(
            (volatile LONG *)&vcpu->resident_state,
            SVM_RESIDENT_ACTIVE, -1);   /* probe: no change */
        if (old == SVM_RESIDENT_STOPPING) {
            InterlockedExchange((volatile LONG *)&vcpu->resident_state,
                                SVM_RESIDENT_STOPPED);
            return 0;
        }
    } while (InterlockedCompareExchange(
                 (volatile LONG *)&vcpu->resident_state,
                 SVM_RESIDENT_ACTIVE, old) != old);
    return 1;
}

int svm_core_enter_resident_current(uint32_t index) {
    svm_vcpu_t *vcpu = svm_core_get_vcpu(index);
    if (!vcpu) return STATUS_NOT_FOUND;

    if (!svm_resident_try_activate(vcpu))
        return 0;

    LOG_INFO("Resident loop starting on core %u", (unsigned)index);
    yghv_trace("resident start");
#ifdef YGHV_R1_EXCLUDE_PRIVATE
    if (vcpu->guest_stack_pa) {
        vcpu->vmcb->state.rsp =
            vcpu->guest_stack_pa + HV_PAGE_SIZE - 0x10;
    }
#endif
#ifdef YGHV_R1_EXCLUDE_PRIVATE
    g_r1_entry_seq++;
    g_r1_diag = TRUE;
    g_r1_diag_count = 0;
#endif

    /* Synthetic resident guest runs Windows code in guest mode; on bare metal
       intercept INTR/NMI/SHUTDOWN so Windows ISRs run in native host context
       and a guest triple fault cannot reset the machine.  Under a nested
       hypervisor (VMware) those intercepts break L1 interrupt delivery, so
       keep the v42 behavior there (no intercepts). */
    if (!g_svm_nested) {
        vcpu->vmcb->control.general1_intercepts |=
            INTR_GEN1(SVM_INTERCEPT_INTR) | INTR_GEN1(SVM_INTERCEPT_NMI) |
            INTR_GEN1(SVM_INTERCEPT_SHUTDOWN);
    }

    {
        uint64_t iter = 0;
    while (vcpu->resident_state == SVM_RESIDENT_ACTIVE) {
        if (vcpu->pause_requested) {
            InterlockedExchange((volatile LONG *)&vcpu->pause_ack, 1);
            KeSetEvent(&vcpu->pause_done_event, IO_NO_INCREMENT, FALSE);
            KeWaitForSingleObject(&vcpu->resume_event, Executive, KernelMode,
                                  FALSE, NULL);
            KeResetEvent(&vcpu->resume_event);
            KeResetEvent(&vcpu->pause_done_event);
            InterlockedExchange((volatile LONG *)&vcpu->pause_ack, 0);
            if (vcpu->resident_state != SVM_RESIDENT_ACTIVE)
                break;
        }
        if (vcpu->npt_flush_pending) {
            vcpu->vmcb->control.tlb_control = SVM_TLB_CONTROL_FLUSH;
            InterlockedExchange((volatile LONG *)&vcpu->npt_flush_pending, 0);
        }
        uint64_t exitcode = svm_vmrun_trampoline(vcpu);
        vcpu->vmcb->control.tlb_control = 0;
        if ((++iter & 0x7FFFULL) == 0)
            ZwYieldExecution();
        if ((vcpu->resident_exits % 100000ULL) == 0)
            yghv_trace("resident exit tick");
        (void)exitcode;
    int stop = svm_dispatch_exit(vcpu);
        if (stop) break;
    }
    }

    LOG_INFO("Resident loop exit: state=%d, exits=%llu",
        vcpu->resident_state, vcpu->resident_exits);
#ifdef YGHV_R1_EXCLUDE_PRIVATE
    g_r1_diag = FALSE;
#endif

    /* Devirtualize */
    InterlockedExchange((volatile LONG *)&vcpu->resident_state,
                        SVM_RESIDENT_STOPPED);
    vcpu->vmcb->control.general1_intercepts = 0;
    vcpu->vmcb->control.general2_intercepts = 0;
    vcpu->vmcb->control.np_enable = 0;

    return 0;
}

int svm_core_stop_resident_current(uint32_t index) {
    svm_vcpu_t *vcpu = svm_core_get_vcpu(index);
    if (!vcpu) return STATUS_NOT_FOUND;
    vcpu->resident_state = SVM_RESIDENT_STOPPING;
    return STATUS_SUCCESS;
}

int svm_core_resident_state(uint32_t index) {
    if (index >= SVM_MAX_CORES || !g_vcpus[index])
        return SVM_RESIDENT_OFF;
    return g_vcpus[index]->resident_state;
}

uint64_t svm_core_resident_exit_count(uint32_t index) {
    if (index >= SVM_MAX_CORES || !g_vcpus[index])
        return 0;
    return g_vcpus[index]->resident_exits;
}

/* --- Multi-core IPI callbacks --- */

int svm_core_prepare_vcpu_other(uint32_t core_id) {
    uint64_t efer;

    if (core_id >= SVM_MAX_CORES || !g_vcpus[core_id])
        return STATUS_NOT_FOUND;

    g_vcpus[core_id]->old_hsave = yg_read_msr(MSR_VM_HSAVE);
    g_vcpus[core_id]->old_efer = yg_read_msr(MSR_EFER);

    efer = yg_read_msr(MSR_EFER);
    if (!(efer & EFER_SVME)) {
        efer |= EFER_SVME;
        yg_write_msr(MSR_EFER, efer);
    }

    svm_prepare_vcpu(g_vcpus[core_id], (uint64_t)svm_trampoline_test_guest);
    /* New system threads may report SS=0 under nested SVM; the test guest
       only needs a canonical ring-0 long-mode SS. */
    g_vcpus[core_id]->vmcb->state.ss_selector = 0x18;
    g_vcpus[core_id]->vmcb->state.ss_attrib = 0x493;
    g_vcpus[core_id]->vmcb->state.ss_limit = 0;
    g_vcpus[core_id]->vmcb->state.ss_base = 0;
    g_vcpus[core_id]->regs.rax = YGHV_CMD_HEARTBEAT;
    g_vcpus[core_id]->vmcb->state.rax = YGHV_CMD_HEARTBEAT;

    LOG_INFO("svm_core_prepare_vcpu_other: VCPU[%u] ready", core_id);
    return STATUS_SUCCESS;
}

ULONG_PTR svm_core_ipi_prepare_vcpu(ULONG_PTR arg) {
    (void)arg;
    uint32_t core = (uint32_t)KeGetCurrentProcessorNumber();
    if (core == 0) return 0; /* core 0 already prepared by svm_core_init */
    svm_core_prepare_vcpu_other(core);
    return 0;
}

ULONG_PTR svm_core_ipi_set_npt(ULONG_PTR arg) {
    npt_mgr_t *npt = (npt_mgr_t *)arg;
    uint32_t core = (uint32_t)KeGetCurrentProcessorNumber();
    if (!npt || core >= SVM_MAX_CORES || !g_vcpus[core]) return 1;
    svm_core_set_npt(core, npt->pml4_pa);
    LOG_INFO("NPT set on core %u", core);
    return 0;
}

void svm_core_stop_all_residents(void) {
    ULONG i;
    /* Only ACTIVE residents need a stop signal.  Setting STOPPING on idle vcpus
       (STOPPED/OFF) leaves them stuck in STOPPING forever — no resident loop
       runs on them to consume STOPPING -> STOPPED — so every later
       svm_resident_try_activate on that core hits the STOPPING branch and bails,
       silently degrading to single-core persistent (observed 9.143, root cause
       found 9.147). */
    for (i = 0; i < SVM_MAX_CORES; i++) {
        if (g_vcpus[i] &&
            g_vcpus[i]->resident_state == SVM_RESIDENT_ACTIVE) {
            InterlockedExchange((volatile LONG *)&g_vcpus[i]->resident_state,
                                SVM_RESIDENT_STOPPING);
            if (g_vcpus[i]->pause_requested) {
                /* Wake a resident parked in the pause wait so it can observe
                   STOPPING and exit; otherwise svm_core_wait_all_stopped
                   deadlocks on unload. */
                KeSetEvent(&g_vcpus[i]->resume_event, IO_NO_INCREMENT, FALSE);
            }
            LOG_INFO("Core %u marked STOPPING", i);
        }
    }
}

NTSTATUS svm_core_pause_residents_for_patch(void) {
    ULONG i;
    ULONG requested = 0;
    LARGE_INTEGER timeout;

    for (i = 0; i < SVM_MAX_CORES; i++) {
        svm_vcpu_t *v = g_vcpus[i];
        if (!v || svm_core_resident_state(i) != SVM_RESIDENT_ACTIVE)
            continue;
        InterlockedExchange((volatile LONG *)&v->pause_requested, 1);
        requested++;
    }
    if (!requested)
        return STATUS_SUCCESS;

    for (i = 0; i < SVM_MAX_CORES; i++) {
        svm_vcpu_t *v = g_vcpus[i];
        NTSTATUS st;
        if (!v || !v->pause_requested)
            continue;
        timeout.QuadPart = -5 * 10000000LL;
        st = KeWaitForSingleObject(&v->pause_done_event, Executive, KernelMode,
                                   FALSE, &timeout);
        if (st != STATUS_SUCCESS) {
            LOG_ERROR("pause resident core %u timed out 0x%x", i, st);
            svm_core_resume_residents();
            return STATUS_TIMEOUT;
        }
    }
    return STATUS_SUCCESS;
}

void svm_core_resume_residents(void) {
    ULONG i;
    for (i = 0; i < SVM_MAX_CORES; i++) {
        svm_vcpu_t *v = g_vcpus[i];
        if (!v || !v->pause_requested)
            continue;
        InterlockedExchange((volatile LONG *)&v->pause_requested, 0);
        KeSetEvent(&v->resume_event, IO_NO_INCREMENT, FALSE);
    }
}

