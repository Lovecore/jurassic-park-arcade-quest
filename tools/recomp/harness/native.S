/* native.S — run one original game function natively (32-bit Windows).
 *
 * void native_run(regs_t *in_out, uint32_t target)
 *   regs_t: eax ecx edx ebx esp ebp esi edi (8 dwords). Loads the guest
 *   registers (esp points at a guest stack whose top holds the return
 *   address = our landing), calls target via that stack, then stores the
 *   resulting registers back into *in_out. Host callee-saved registers and
 *   the host stack are preserved.
 */
    .text
    .globl _native_run
_native_run:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl  20(%esp), %eax            /* regs */
    movl  24(%esp), %ecx            /* target */
    movl  %esp, _native_host_esp
    movl  %eax, _native_regs
    movl  %ecx, _native_target
    /* guest stack: push our return address as the "caller" */
    movl  16(%eax), %esp
    pushl $native_back
    movl  _native_target, %ecx
    pushl %ecx                      /* jump target via ret */
    movl  4(%eax), %ecx
    movl  8(%eax), %edx
    movl  12(%eax), %ebx
    movl  20(%eax), %ebp
    movl  24(%eax), %esi
    movl  28(%eax), %edi
    movl  0(%eax), %eax
    ret                             /* -> target, return address = native_back */
    .globl _native_back
_native_back:
native_back:
    cld                             /* the guest may leave DF set */
    pushl %eax
    movl  _native_regs, %eax
    popl  0(%eax)
    movl  %ecx, 4(%eax)
    movl  %edx, 8(%eax)
    movl  %ebx, 12(%eax)
    movl  %esp, 16(%eax)
    movl  %ebp, 20(%eax)
    movl  %esi, 24(%eax)
    movl  %edi, 28(%eax)
    movl  _native_host_esp, %esp
    popl  %edi
    popl  %esi
    popl  %ebx
    popl  %ebp
    ret

    .globl _native_abort
/* jumped to from the exception handler: restore the host stack and return */
_native_abort:
    cld
    movl  _native_host_esp, %esp
    popl  %edi
    popl  %esi
    popl  %ebx
    popl  %ebp
    ret

    .data
    .globl _native_host_esp
_native_host_esp: .long 0
_native_regs:     .long 0
_native_target:   .long 0
