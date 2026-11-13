#ifndef YGHV_CONTROL_PLANE_H
#define YGHV_CONTROL_PLANE_H

/* Internal resident/test VMMCALL protocol. No user-mode control transport. */
#define YGHV_CMD_HEARTBEAT      1ULL
#define YGHV_CMD_STOP_INTERNAL  2ULL
#define YGHV_CMD_VERSION        3ULL
#define YGHV_CMD_STATS          4ULL
#define YGHV_CMD_SET_TARGET    0x20ULL
#define YGHV_CMD_ADD_PAGE      0x21ULL
#define YGHV_CMD_REMOVE_PAGE   0x22ULL
#define YGHV_CMD_START_PROTECT 0x23ULL
#define YGHV_CMD_STOP_PROTECT  0x24ULL
#define YGHV_CMD_GET_STATE     0x25ULL
#define YGHV_CMD_HOOK_QUERY    0x60ULL
#define YGHV_CMD_PROTECT_HANDLE 0x10ULL
#define YGHV_CMD_UNPROTECT      0x11ULL
#define YGHV_CMD_SCAN_PROCESS   0x30ULL
#define YGHV_CMD_READ_MEMORY    0x31ULL
#define YGHV_CMD_GET_CONFIG     0x50ULL
#define YGHV_CMD_SET_CONFIG     0x51ULL
#define YGHV_CMD_SHUTDOWN       0xF0ULL
#define YGHV_STATUS_OK          0ULL
#define YGHV_STATUS_ERROR       1ULL
#define YGHV_STATUS_DENIED      2ULL
#define YGHV_STATUS_INVALID     3ULL

extern uint64_t g_vmmcall_auth_cookie;
extern uint64_t g_control_cr3;

#endif
