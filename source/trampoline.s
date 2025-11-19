.section .text.nroEntrypointTrampoline, "ax", %progbits
.global nroEntrypointTrampoline
.type   nroEntrypointTrampoline, %function
.align 2
.global __libnx_exception_entry
.type   __libnx_exception_entry, %function
.cfi_startproc
nroEntrypointTrampoline:
    // Reset stack pointer.
    adrp x8, __stack_top
    ldr  x8, [x8, #:lo12:__stack_top]
    mov  sp, x8
    // Call NRO.
    blr  x2
    // Save retval
    adrp x1, g_lastRet
    str  w0, [x1, #:lo12:g_lastRet]
    
    // Check if exit was requested
    adrp x8, __stack_top
    ldr  x8, [x8, #:lo12:__stack_top]
    mov  sp, x8
    bl   checkExitRequested
    cbnz w0, __exit_requested
    
    // Check if heap size changed (fast check between overlays)
    adrp x8, __stack_top
    ldr  x8, [x8, #:lo12:__stack_top]
    mov  sp, x8
    bl   checkHeapSizeChange
    cbnz w0, __exit_for_heap_change
    
    // No heap change - reset stack pointer and load next NRO.
    adrp x8, __stack_top
    ldr  x8, [x8, #:lo12:__stack_top]
    mov  sp, x8
    b    loadNro

__exit_requested:
    // Exit WITHOUT restarting the loader - just terminate cleanly
    bl   svcExitProcess
    b    .

__exit_for_heap_change:
    // Heap size changed - exit to restart with new size
    b    __wrap_exit
    
.cfi_endproc
.section .text.__libnx_exception_entry, "ax", %progbits
.align 2
.cfi_startproc
__libnx_exception_entry:
    adrp x7, g_nroAddr
    ldr  x7, [x7, #:lo12:g_nroAddr]
    cbz  x7, __libnx_exception_entry_fail
    br   x7
__libnx_exception_entry_fail:
    mov w0, #0xf801
    bl svcReturnFromException
    b .
.cfi_endproc