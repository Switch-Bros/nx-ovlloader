#include <switch.h>

#define OVLLOADER_TID 0x420000000007E51AULL
#define CHECK_INTERVAL_NS 10000000ULL    // 10ms - check frequently
#define RESPAWN_DELAY_NS 50000000ULL    // 100ms - wait before respawn
#define TIMEOUT_NS 2000000000ULL         // 2 seconds - give up if process doesn't die
#define INNER_HEAP_SIZE 0x4000

#ifdef __cplusplus
extern "C" {
#endif

// Sysmodules should not use applet*.
u32 __nx_applet_type = AppletType_None;

// Sysmodules will normally only want to use one FS session.
u32 __nx_fs_num_sessions = 1;

// Newlib heap configuration function (makes malloc/free work).
void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;
    
    // Configure the newlib heap.
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

// Service initialization.
void __appInit(void) {
    Result rc;
    
    // Initialize SM
    rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
    
    // Get firmware version for hosversionSet
    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    
    // Initialize PM services
    rc = pmdmntInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
    
    rc = pmshellInitialize();
    if (R_FAILED(rc)) {
        pmdmntExit();
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
    }
    
    // Close SM now that we've initialized everything
    smExit();
}

// Service deinitialization.
void __appExit(void) {
    pmshellExit();
    pmdmntExit();
}

#ifdef __cplusplus
}
#endif

int main(int argc, char **argv) {
    
    Result rc;
    #if !BUILDING_NRO_DIRECTIVE
    
    u64 startTick = armGetSystemTick();
    
    // Phase 1: Wait for nx-ovlloader to exit (it should be exiting when we start)
    bool processExited = false;
    while (true) {
        const u64 now = armGetSystemTick();
        const u64 elapsed = armTicksToNs(now - startTick);
        
        // Timeout check
        if (elapsed >= TIMEOUT_NS) {
            // Process didn't exit in time, just exit ourselves
            return 1;
        }
        
        u64 pid = 0;
        rc = pmdmntGetProcessId(&pid, OVLLOADER_TID);
        bool isRunning = R_SUCCEEDED(rc) && pid != 0;
        
        if (!isRunning) {
            // Process has exited, proceed to respawn phase
            processExited = true;
            break;
        }
        
        svcSleepThread(CHECK_INTERVAL_NS);
    }
    
    // Phase 2: Wait the respawn delay
    if (processExited) {
        svcSleepThread(RESPAWN_DELAY_NS);
        
        // Phase 3: Relaunch nx-ovlloader
        NcmProgramLocation programLocation = {
            .program_id = OVLLOADER_TID,
            .storageID = NcmStorageId_None,
        };
        
        u64 newPid = 0;
        pmshellLaunchProgram(0, &programLocation, &newPid);
        
        //if (R_FAILED(rc) || newPid == 0) {
        //    // Failed to launch, but we tried
        //    return 2;
        //}
        
        // Successfully relaunched - wait a moment to ensure it starts properly
        //svcSleepThread(200000000ULL); // 200ms
        
        // Verify it's actually running
        //u64 checkPid = 0;
        //rc = pmdmntGetProcessId(&checkPid, OVLLOADER_TID);
        //if (R_FAILED(rc) || checkPid == 0) {
        //    // Launch failed or process died immediately
        //    return 3;
        //}
    }
    #else

    u64 pid = 0;
    rc = pmdmntGetProcessId(&pid, OVLLOADER_TID);
    bool isRunning = R_SUCCEEDED(rc) && pid != 0;
    
    if (!isRunning) {
        // Relaunch nx-ovlloader
        NcmProgramLocation programLocation = {
            .program_id = OVLLOADER_TID,
            .storageID = NcmStorageId_None,
        };
        
        u64 newPid = 0;
        pmshellLaunchProgram(0, &programLocation, &newPid);
    }

    
    //if (R_FAILED(rc) || newPid == 0) {
    //    // Failed to launch, but we tried
    //    return 2;
    //}
    
    // Verify it's actually running
    //u64 checkPid = 0;
    //rc = pmdmntGetProcessId(&checkPid, OVLLOADER_TID);
    //if (R_FAILED(rc) || checkPid == 0) {
    //    // Launch failed or process died immediately
    //    return 3;
    //}
    #endif
    
    // Mission accomplished - exit cleanly
    return 0;
}