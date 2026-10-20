/* hooks.S — trace stubs for jpvr diagnostics (i686, AT&T syntax).
 *
 * jpvr_hook_stub_N: entry detour for traced game function N. Saves all
 * registers and flags, calls jpvr_diag_record(N, args) where args points at
 * the caller's first stack argument, restores everything and continues in
 * the trampoline (stolen prologue bytes + jmp back), so the game function
 * runs unmodified.
 *
 * jpvr_call(fn, n, args): calls fn with n dword arguments in cdecl order and
 * restores esp from ebp afterwards, so it is safe whether fn is cdecl or
 * stdcall (the GLEW pointers come from TeknoParrot; their convention is
 * not known).
 */
    .text

    .macro HOOK_STUB n
    .globl _jpvr_hook_stub_\n
_jpvr_hook_stub_\n:
    pushal
    pushfl
    leal  40(%esp), %eax          /* 32 pushal + 4 pushfl + 4 return addr */
    pushl %eax
    pushl $\n
    call  _jpvr_diag_record
    addl  $8, %esp
    popfl
    popal
    jmp   *_jpvr_tramp+(\n*4)
    .endm

    HOOK_STUB 0
    HOOK_STUB 1
    HOOK_STUB 2
    HOOK_STUB 3
    HOOK_STUB 4
    HOOK_STUB 5
    HOOK_STUB 6
    HOOK_STUB 7

    .globl _jpvr_call
_jpvr_call:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    pushl %edi
    movl  12(%ebp), %ecx          /* n */
    movl  16(%ebp), %esi          /* args */
    movl  %ecx, %eax
    shll  $2, %eax
    subl  %eax, %esp
    andl  $-16, %esp
    movl  %esp, %edi
    cld
    rep movsl
    call  *8(%ebp)
    leal  -8(%ebp), %esp
    popl  %edi
    popl  %esi
    popl  %ebp
    ret

    .data
    .globl _jpvr_tramp
_jpvr_tramp:
    .fill 8, 4, 0
