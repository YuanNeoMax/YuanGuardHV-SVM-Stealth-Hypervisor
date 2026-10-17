#ifndef YGHV_PROTECT_H
#define YGHV_PROTECT_H
#include <ntddk.h>
#include "npt.h"
#include "svm_vcpu.h"

#define YGHV_PROTECT_MAX_PAGES 64
#define YGHV_PROTECT_MAX_HOOKS 4
#define YGHV_PROTECT_MAX_TARGETS 4
#define YGHV_PROTECT_PATCH_LEN 16
#define YGHV_PROTECT_PATCH_MIN 12

typedef enum {
    YGHV_PROTECT_MEM     = 0x1,
    YGHV_PROTECT_TERM    = 0x2,
    YGHV_PROTECT_HANDLE  = 0x4,
} yghv_protect_flags_t;

typedef struct {
    uint64_t gpa;
    uint64_t target_va;
    uint8_t  flags;
    uint8_t  armed;
} yghv_protect_page_t;

typedef struct {
    ULONG auto_disarm;
    ULONG deny_status;
    /* 9.303: the sync guard's settings live in the config so they can be set
     * once alongside the other options and read back in the same call, rather
     * than needing a separate config-sync round trip before arming. */
    ULONG sync_enable;        /* 0 = off (default), 1 = on */
    ULONG sync_interval_ms;   /* poll period; clamped to [1, 5000] */
} yghv_protect_config_t;

typedef struct {
    uint32_t pid;
    uint32_t flags;
    uint64_t cr3;
    uint32_t page_count;
    yghv_protect_page_t pages[YGHV_PROTECT_MAX_PAGES];
    PEPROCESS process;
} yghv_protect_target_t;

typedef struct {
    uint32_t target_count;
    yghv_protect_target_t targets[YGHV_PROTECT_MAX_TARGETS];
    yghv_protect_config_t config;
    volatile BOOLEAN active;
} yghv_protect_state_t;

typedef struct {
    uint64_t func_va;
    uint64_t func_pa;
    uint8_t  original[YGHV_PROTECT_PATCH_LEN];
    uint8_t  patch_len;
    uint8_t  installed;
    uint8_t  hook_id;
} yghv_protect_hook_t;

typedef struct {
    ULONG active;
    ULONG pid;
    ULONG page_count;
    ULONG hook_count;
    ULONG_PTR cr3;
} yghv_protect_target_info_t;

typedef struct {
    ULONG count;
    ULONG returned;
    yghv_protect_target_info_t targets[YGHV_PROTECT_MAX_TARGETS];
} yghv_protect_targets_info_t;

typedef struct {
    uint64_t gpa;
    uint64_t target_va;
    uint8_t  flags;
    uint8_t  armed;
    uint8_t  reserved[6];
} yghv_protect_page_info_t;

typedef struct {
    ULONG count;
    ULONG returned;
    yghv_protect_page_info_t pages[YGHV_PROTECT_MAX_PAGES];
} yghv_protect_pages_info_t;

typedef struct {
    uint64_t func_va;
    uint32_t hook_id;
    uint32_t installed;
    uint32_t patch_len;
    uint32_t reserved;
} yghv_protect_hook_info_t;

typedef struct {
    ULONG count;
    ULONG returned;
    yghv_protect_hook_info_t hooks[YGHV_PROTECT_MAX_HOOKS];
} yghv_protect_hooks_info_t;

typedef struct {
    ULONG hook_id;
    ULONG flags;
    ULONG_PTR func_va;
    WCHAR name[64];
} yghv_protect_install_hook_info_t;

typedef struct {
    ULONG hook_id;
    ULONG reserved;
} yghv_protect_remove_hook_info_t;

typedef enum {
    YGHV_NPF_NONE = 0,
    YGHV_NPF_ALLOW,
    YGHV_NPF_DENY,
    YGHV_NPF_FAKE = 3,   /* 9.275 C6: shadow fake-write (foreign cpl=0 write
                            redirected to a scratch page; vr ring shows 3) */
} yghv_npf_result_t;

NTSTATUS yghv_protect_init(void);
void yghv_protect_cleanup(void);
NTSTATUS yghv_protect_set_target(uint32_t pid);
BOOLEAN yghv_protect_is_target_cr3(uint64_t cr3);
NTSTATUS yghv_protect_add_page(uint64_t target_va);
NTSTATUS yghv_protect_remove_page(uint64_t target_va);
NTSTATUS yghv_protect_add_page_for(uint64_t cr3, uint64_t target_va);
NTSTATUS yghv_protect_remove_page_for(uint64_t cr3, uint64_t target_va);
NTSTATUS yghv_protect_add_page_for_pid(uint32_t pid, uint64_t target_va);
NTSTATUS yghv_protect_remove_page_for_pid(uint32_t pid, uint64_t target_va);
yghv_protect_page_t *yghv_protect_find_page(uint64_t gpa);
yghv_protect_page_t *yghv_protect_find_page_bare(uint64_t gpa);   /* 9.282: lock-free, island-safe */
int yghv_protect_arm_page(yghv_protect_page_t *p);
int yghv_protect_disarm_page(yghv_protect_page_t *p);
uint64_t yghv_protect_guest_va_to_pa(uint64_t cr3, uint64_t va);
NTSTATUS yghv_protect_start(void);
NTSTATUS yghv_protect_stop(void);
NTSTATUS yghv_protect_install_hook(uint8_t hook_id, uint64_t func_va);
NTSTATUS yghv_protect_remove_hook(uint8_t hook_id);
uint64_t yghv_protect_on_hook_query(uint8_t hook_id, uint64_t accessor_cr3);
uint64_t yghv_protect_find_func_pattern(PCWSTR name_hint, uint8_t *pat, SIZE_T pat_len);
uint64_t yghv_protect_get_hook_stub_va(uint8_t hook_id);
yghv_npf_result_t yghv_protect_on_npf_write(svm_vcpu_t *vcpu, uint64_t gpa);
yghv_npf_result_t yghv_protect_on_npf_write_bare(uint64_t guest_cr3, uint32_t cpl, uint64_t gpa, uint64_t *rearm_gpa_out, int *flip_out);
int yghv_protect_arm_page_bare(uint64_t gpa);
void yghv_protect_reopen_page_bare(uint64_t gpa);
void yghv_protect_fake_init(void);
int  yghv_protect_fake_mode_get(void);
void yghv_protect_fake_mode_set(int on);
int  yghv_protect_fake_bare(uint64_t gpa, uint64_t *rearm_out);
void yghv_protect_fake_restore(uint64_t gpa);
LONG yghv_protect_fake_alt_active(void);
void yghv_protect_fake_ncr3_resync(void);
void yghv_protect_fake_invpga_all(UINT32 asid);   /* 9.289 C6.1 */
void yghv_protect_fake_diag(UINT64 out[5]);
/* 9.297 (A'): runtime TLB-flush strategy for the fake-write path. */
int  yghv_protect_fake_tlb_get(void);
void yghv_protect_fake_tlb_set(int mode);
LONG yghv_protect_fake_tlb_seen(void);
int  yghv_protect_fake_tlb_plan(int *use_invlpga);
int  yghv_protect_fake_reopen_main(uint64_t gpa);
/* 9.299 (C line): snapshot + poll guard — no NPT permission flip. */
int  yghv_protect_sync_mode_get(void);
void yghv_protect_sync_mode_set(int on);
void yghv_protect_sync_diag(UINT64 out[8]);
void yghv_protect_sync_set_interval(int ms);
int  yghv_protect_sync_interval_get(void);
void yghv_protect_set_registry_path(PUNICODE_STRING registry_path);
void yghv_protect_config_save(void);
void yghv_protect_config_load(void);
int  yghv_protect_sync_arm(uint64_t gpa);
void yghv_protect_sync_disarm(uint64_t gpa);
int  yghv_protect_sync_resnapshot(uint64_t gpa);   /* Phase 7: cooperative write */
int  yghv_protect_sync_is_guarded(uint64_t gpa);   /* Phase 7: guard query */
/* Tier 4 prerequisite: remap one GPA to a different SPA (returns 1 on ok). */
int  yghv_protect_npt_remap(uint64_t gpa, uint64_t spa, int use_alt,
                            uint64_t *prev_spa);
void yghv_protect_sync_init(void);
void yghv_protect_sync_stop(void);
uint64_t yghv_protect_control_walk_gpa(void);
void yghv_protect_on_process_exit(uint32_t pid);
void yghv_protect_rearm(svm_vcpu_t *vcpu);
void yghv_protect_get_state(ULONG *active, ULONG *pid, ULONG *page_count);
void yghv_protect_get_heartbeat(uint64_t *page_va, uint64_t *hook_va);
void yghv_protect_get_target(ULONG *active, ULONG *pid, ULONG_PTR *cr3,
    ULONG *page_count, ULONG *hook_count);
void yghv_protect_get_targets_info(yghv_protect_targets_info_t *info);
void yghv_protect_get_pages_info(yghv_protect_pages_info_t *info);
void yghv_protect_get_hooks_info(yghv_protect_hooks_info_t *info);
NTSTATUS yghv_protect_clear(void);
NTSTATUS yghv_protect_set_config(const yghv_protect_config_t *cfg);
void yghv_protect_get_config(yghv_protect_config_t *out);
BOOLEAN yghv_protect_check_target_exited(void);
BOOLEAN yghv_protect_on_target_exit(ULONG pid);
int yghv_protect_validate_hook_target(uint64_t func_va);

extern yghv_protect_state_t g_protect;
extern yghv_protect_hook_t g_protect_hooks[YGHV_PROTECT_MAX_HOOKS];

#endif
