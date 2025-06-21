#include <switch.h>

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define DEFAULT_NRO "sdmc:/switch/.overlays/ovlmenu.ovl"

// Use compiler builtin instead of library call
#define FAST_MEMCPY(dst, src, size) __builtin_memcpy(dst, src, size)
#define FAST_MEMSET(ptr, val, size) __builtin_memset(ptr, val, size)

#if BUILD_LOADER_PLUS_DIRECTIVE
const char g_noticeText[] =
    "nx-ovlloader+ " VERSION "\0"
    "What's the most resilient parasite? A bacteria? A virus? An intestinal worm? An idea. Resilient, highly contagious.";
#else
const char g_noticeText[] =
    "nx-ovlloader " VERSION "\0"
    "What's the most resilient parasite? A bacteria? A virus? An intestinal worm? An idea. Resilient, highly contagious.";
#endif

// Align to cache line boundaries for better performance
static char g_argv[512] __attribute__((aligned(64)));
static char g_nextArgv[512] __attribute__((aligned(64)));
static char g_nextNroPath[256] __attribute__((aligned(64)));

u64 g_nroAddr = 0;
static u64 g_nroSize = 0;
static NroHeader g_nroHeader;

static u64 g_appletHeapSize = 0;
static u64 g_appletHeapReservationSize = 0;

static u128 g_userIdStorage;

static u8 g_savedTls[0x100] __attribute__((aligned(16)));

// Minimize fs resource usage
u32 __nx_fs_num_sessions = 1;
u32 __nx_fsdev_direntry_cache_size = 1;
bool __nx_fsdev_support_cwd = false;

// Used by trampoline.s
Result g_lastRet = 0;

extern void* __stack_top;
#define STACK_SIZE 0x10000

// Cache file system handle globally to avoid repeated initialization
static FsFileSystem g_sdmc;
static bool g_sdmc_initialized = false;

void __libnx_initheap(void)
{
    static char g_innerheap[0x4000] __attribute__((aligned(16)));

    extern char* fake_heap_start;
    extern char* fake_heap_end;

    fake_heap_start = &g_innerheap[0];
    fake_heap_end   = &g_innerheap[sizeof g_innerheap];
}

void __appInit(void)
{
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 1));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
    #if BUILD_LOADER_PLUS_DIRECTIVE
        g_appletHeapSize = 0x800000;
    #else
        g_appletHeapSize = 0x600000;
    #endif
        g_appletHeapReservationSize = 0x00;
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 2));

    smExit();
}

void __wrap_exit(void)
{
    if (g_sdmc_initialized) {
        fsFsClose(&g_sdmc);
        g_sdmc_initialized = false;
    }
    svcExitProcess();
    __builtin_unreachable();
}

static void* g_heapAddr;
static size_t g_heapSize;

static inline void setupHbHeap(void)
{
    void* addr = NULL;
    u64 size = g_appletHeapSize;

    Result rc = svcSetHeapSize(&addr, size);

    if (R_FAILED(rc) || addr == NULL)
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 9));

    g_heapAddr = addr;
    g_heapSize = size;
}

static Handle g_procHandle;

static void procHandleReceiveThread(void* arg)
{
    Handle session = (Handle)(uintptr_t)arg;
    Result rc;

    void* base = armGetTls();
    hipcMakeRequestInline(base);

    s32 idx;
    rc = svcReplyAndReceive(&idx, &session, 1, INVALID_HANDLE, UINT64_MAX);
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 15));

    HipcParsedRequest r = hipcParseRequest(base);
    if (r.meta.num_copy_handles != 1)
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 17));

    g_procHandle = r.data.copy_handles[0];
    svcCloseHandle(session);
}

static inline void getOwnProcessHandle(void)
{
    Result rc;

    Handle server_handle, client_handle;
    rc = svcCreateSession(&server_handle, &client_handle, 0, 0);
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 12));

    Thread t;
    rc = threadCreate(&t, &procHandleReceiveThread, (void*)(uintptr_t)server_handle, NULL, 0x1000, 0x20, 0);
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 10));

    rc = threadStart(&t);
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 13));

    hipcMakeRequestInline(armGetTls(),
        .num_copy_handles = 1,
    ).copy_handles[0] = CUR_PROCESS_HANDLE;

    svcSendSyncRequest(client_handle);
    svcCloseHandle(client_handle);

    threadWaitForExit(&t);
    threadClose(&t);
}

// Pre-calculated segment permissions for faster setup
static const u32 segment_perms[] = { Perm_R | Perm_X, Perm_R, Perm_Rw };

// Optimized file reading with proper error handling
static inline Result readFileOptimized(const char* path, void* buffer, s64* out_size)
{
    Result rc;
    
    // Initialize SD card filesystem only once
    if (!g_sdmc_initialized) {
        rc = fsOpenSdCardFileSystem(&g_sdmc);
        if (R_FAILED(rc))
            return rc;
        g_sdmc_initialized = true;
    }

    FsFile fd;
    rc = fsFsOpenFile(&g_sdmc, path + 5, FsOpenMode_Read, &fd);
    if (R_FAILED(rc))
        return rc;

    s64 file_size;
    rc = fsFileGetSize(&fd, &file_size);
    if (R_FAILED(rc)) {
        fsFileClose(&fd);
        return rc;
    }

    u64 bytes_read;
    rc = fsFileRead(&fd, 0, buffer, file_size, FsReadOption_None, &bytes_read);
    fsFileClose(&fd);
    
    if (R_FAILED(rc) || bytes_read != file_size)
        return MAKERESULT(Module_HomebrewLoader, 4);
    
    *out_size = file_size;
    return 0;
}

void loadNro(void)
{
    NroHeader* header = NULL;
    size_t rw_size;
    Result rc;
    
    // Pre-declare loop variables outside loops for better performance
    int i;
    u64 segment_size;
    u32 file_off, size;

    FAST_MEMCPY((u8*)armGetTls() + 0x100, g_savedTls, 0x100);

    if (g_nroSize > 0)
    {
        // Unmap previous NRO with batch operations
        header = &g_nroHeader;
        rw_size = (header->segments[2].size + header->bss_size + 0xFFF) & ~0xFFF;

        // Unmap all segments at once if possible, or use optimized order
        for (i = 0; i < 3; i++) {
            segment_size = (i == 2) ? rw_size : header->segments[i].size;
            rc = svcUnmapProcessCodeMemory(
                g_procHandle, 
                g_nroAddr + header->segments[i].file_off, 
                ((u64)g_heapAddr) + header->segments[i].file_off, 
                segment_size);
            if (R_FAILED(rc))
                fatalThrow(MAKERESULT(Module_HomebrewLoader, 24 + i));
        }

        g_nroAddr = g_nroSize = 0;
    }

    // Set default paths more efficiently
    if (!g_nextNroPath[0])
        FAST_MEMCPY(g_nextNroPath, DEFAULT_NRO, sizeof(DEFAULT_NRO));

    if (!g_nextArgv[0])
        FAST_MEMCPY(g_nextArgv, DEFAULT_NRO, sizeof(DEFAULT_NRO));

    FAST_MEMCPY(g_argv, g_nextArgv, sizeof(g_argv));

    uint8_t* nrobuf = (uint8_t*)g_heapAddr;
    NroStart* start = (NroStart*)(nrobuf + 0);
    header = (NroHeader*)(nrobuf + sizeof(NroStart));

    // Optimized file reading
    s64 file_size;
    rc = readFileOptimized(g_nextNroPath, nrobuf, &file_size);
    if (R_FAILED(rc)) {
        exit(1);
    }

    // Reset NRO path
    g_nextNroPath[0] = '\0';

    // Fast structure copies
    *start = *(NroStart*)nrobuf;
    *header = *(NroHeader*)(nrobuf + sizeof(NroStart));

    if (header->magic != NROHEADER_MAGIC)
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 5));

    size_t total_size = (header->size + header->bss_size + 0xFFF) & ~0xFFF;
    rw_size = (header->segments[2].size + header->bss_size + 0xFFF) & ~0xFFF;

    // Optimized segment validation
    for (i = 0; i < 3; i++) {
        file_off = header->segments[i].file_off;
        size = header->segments[i].size;
        if (file_off >= header->size || size > header->size || 
            (file_off + size) > header->size) {
            fatalThrow(MAKERESULT(Module_HomebrewLoader, 6));
        }
    }

    // Cache header
    g_nroHeader = *header;
    header = &g_nroHeader;

    // Improved memory mapping with systematic allocation
    static u64 s_nextMapAddr = 0x8000000000ull;
    u64 map_addr = s_nextMapAddr;
    u64 attempts = 0;
    
    rc = svcMapProcessCodeMemory(g_procHandle, map_addr, (u64)nrobuf, total_size);
    if (R_SUCCEEDED(rc)) {
        s_nextMapAddr = (map_addr + total_size + 0x200000) & ~0x1FFFFFull;
    } else {
        // Optimized fallback
        do {
            map_addr = (randomGet64() & 0xFFFFFF000ull) + (attempts * 0x1000000);
            rc = svcMapProcessCodeMemory(g_procHandle, map_addr, (u64)nrobuf, total_size);
            attempts++;
        } while ((rc == 0xDC01 || rc == 0xD401) && attempts < 256);
    }

    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 18));

    // Set permissions for all segments in optimized order
    for (i = 0; i < 3; i++) {
        segment_size = (i == 2) ? rw_size : header->segments[i].size;
        rc = svcSetProcessMemoryPermission(
            g_procHandle, 
            map_addr + header->segments[i].file_off, 
            segment_size, 
            segment_perms[i]);
        if (R_FAILED(rc))
            fatalThrow(MAKERESULT(Module_HomebrewLoader, 19 + i));
    }

    u64 nro_size = header->segments[2].file_off + rw_size;
    u64 nro_heap_start = ((u64)g_heapAddr) + nro_size;
    u64 nro_heap_size = g_heapSize + (u64)g_heapAddr - (u64)nro_heap_start;

    #define M EntryFlag_IsMandatory

    // Pre-filled configuration entries
    static ConfigEntry entries[] = {
        { EntryType_MainThreadHandle,     0, {0, 0} },
        { EntryType_ProcessHandle,        0, {0, 0} },
        { EntryType_AppletType,           0, {AppletType_LibraryApplet, 0} },
        { EntryType_OverrideHeap,         M, {0, 0} },
        { EntryType_Argv,                 0, {0, 0} },
        { EntryType_NextLoadPath,         0, {0, 0} },
        { EntryType_LastLoadResult,       0, {0, 0} },
        { EntryType_SyscallAvailableHint, 0, {0xffffffffffffffff, 0x9fc1fff0007ffff} },
        { EntryType_RandomSeed,           0, {0, 0} },
        { EntryType_UserIdStorage,        0, {(u64)(uintptr_t)&g_userIdStorage, 0} },
        { EntryType_HosVersion,           0, {0, 0} },
        { EntryType_EndOfList,            0, {(u64)(uintptr_t)g_noticeText, sizeof(g_noticeText)} }
    };
    
    // Batch fill entry values
    entries[0].Value[0] = envGetMainThreadHandle();
    entries[1].Value[0] = g_procHandle;
    entries[3].Value[0] = nro_heap_start;
    entries[3].Value[1] = nro_heap_size;
    entries[4].Value[1] = (u64)&g_argv[0];
    entries[5].Value[0] = (u64)&g_nextNroPath[0];
    entries[5].Value[1] = (u64)&g_nextArgv[0];
    entries[6].Value[0] = g_lastRet;
    entries[8].Value[0] = randomGet64();
    entries[8].Value[1] = randomGet64();
    entries[10].Value[0] = hosversionGet();

    g_nroAddr = map_addr;
    g_nroSize = nro_size;

    // Fast stack clear
    FAST_MEMSET(__stack_top - STACK_SIZE, 0, STACK_SIZE);

    extern NX_NORETURN void nroEntrypointTrampoline(u64 entries_ptr, u64 handle, u64 entrypoint);
    nroEntrypointTrampoline((u64)entries, -1, map_addr);
}

int main(int argc, char **argv)
{
    FAST_MEMCPY(g_savedTls, (u8*)armGetTls() + 0x100, 0x100);

    setupHbHeap();
    getOwnProcessHandle();
    loadNro();

    fatalThrow(MAKERESULT(Module_HomebrewLoader, 8));
    return 0;
}
