.global _asm_default_int, _asm_reserved_int, _asm_exc_DIV0, _asm_exc_DEBUG, _asm_exc_BP, _asm_exc_NOMATH, _asm_exc_MF, _asm_exc_TSS, _asm_exc_SWAP, _asm_exc_AC, _asm_exc_MC, _asm_exc_XM, _asm_exc_NMI, _asm_exc_OVRFLW, _asm_exc_BOUNDS, _asm_exc_OPCODE, _asm_exc_DOUBLEF, _asm_exc_STACKF, _asm_exc_GP, _asm_exc_PF, _asm_irq_0, _asm_irq_1, _asm_irq_7, task_switch, _asm_syscall

.macro  SAVE_REGS
    pushad
    push ds
    push es
    push fs
    push gs
    push ebx
    mov bx,0x10
    mov ds,bx
    mov es,bx       # entered from ring 3 these still hold user selectors
    pop ebx
.endm

.macro  RESTORE_REGS
    pop gs
    pop fs
    pop es
    pop ds
    popad
.endm

.intel_syntax noprefix

_asm_default_int:
    SAVE_REGS
    call isr_default_int
    mov al,0x20
    out 0x20,al
    RESTORE_REGS
    iret

_asm_reserved_int:
    SAVE_REGS
    call isr_reserved_int
    mov al,0x20
    out 0x20,al
    RESTORE_REGS
    iret

# Only vectors 8 (#DF), 10-14 (#TS/#NP/#SS/#GP/#PF) and 17 (#AC) push an error
# code; their stubs pop it with `add esp,4` before iret. The other exception
# stubs must NOT do that -- it would discard the return eip and iret into
# garbage if their handler ever returned.
#
# The four faults a user program can realistically raise (#DE, #UD, #GP, #PF)
# get a richer stub: the frame pointer is passed to the C handler so it can
# tell ring 3 apart from ring 0 and kill just the offending task. Vectors
# without a CPU error code push a dummy 0 first so all four frames share one
# layout (struct fault_frame).
.macro FAULT_STUB handler
    SAVE_REGS
    push esp
    call \handler
    add esp,4
    RESTORE_REGS
    add esp,4       # drop the (real or dummy) error code
    iret
.endm

_asm_exc_DIV0:
    push 0          # no CPU error code for #DE; normalize the frame
    FAULT_STUB isr_exc_DIV0

# NB: isr_exc_DEBUG returns (it's a no-op), so this stub's frame must be
# exact. A previous ";add esp,4" here was NOT a comment -- ';' separates
# statements in gas -- so the add assembled and would have clobbered the frame.
_asm_exc_DEBUG:
    SAVE_REGS
    call isr_exc_DEBUG
    RESTORE_REGS
    iret

_asm_exc_BP:
    SAVE_REGS
    call isr_exc_BP
    RESTORE_REGS
    iret

_asm_exc_NOMATH:
    SAVE_REGS
    call isr_exc_NOMATH
    RESTORE_REGS
    iret

_asm_exc_MF:
    SAVE_REGS
    call isr_exc_MF
    RESTORE_REGS
    iret

_asm_exc_TSS:
    SAVE_REGS
    call isr_exc_TSS
    RESTORE_REGS
    add esp,4
    iret

_asm_exc_SWAP:
    SAVE_REGS
    call isr_exc_SWAP
    RESTORE_REGS
    add esp,4
    iret

_asm_exc_AC:
    SAVE_REGS
    call isr_exc_AC
    RESTORE_REGS
    add esp,4
    iret

_asm_exc_MC:
    SAVE_REGS
    call isr_exc_MC
    RESTORE_REGS
    iret

_asm_exc_XM:
    SAVE_REGS
    call isr_exc_XM
    RESTORE_REGS
    iret

_asm_exc_NMI:
    SAVE_REGS
    call isr_exc_NMI
    RESTORE_REGS
    iret

_asm_exc_OVRFLW:
    SAVE_REGS
    call isr_exc_OVRFLW
    RESTORE_REGS
    iret

_asm_exc_BOUNDS:
    SAVE_REGS
    call isr_exc_BOUNDS
    RESTORE_REGS
    iret

_asm_exc_OPCODE:
    push 0          # no CPU error code for #UD; normalize the frame
    FAULT_STUB isr_exc_OPCODE


_asm_exc_DOUBLEF:
    SAVE_REGS
    call isr_exc_DOUBLEF
    RESTORE_REGS
    add esp,4
    iret

_asm_exc_STACKF:
    SAVE_REGS
    call isr_exc_STACKF
    RESTORE_REGS
    add esp,4
    iret

_asm_exc_GP:
    FAULT_STUB isr_exc_GP

_asm_exc_PF:
    FAULT_STUB isr_exc_PF

_asm_irq_0:
    SAVE_REGS
    inc dword ptr [timer_ticks]
    # Hand the current trap frame to the scheduler and adopt whatever stack it
    # returns. When it picks a different task, mov esp swaps us onto that task's
    # kernel stack; RESTORE_REGS + iret below then resume it. The EOI is sent
    # unconditionally, so a switch never leaves the PIC waiting (the old bug).
    push esp
    call schedule
    mov esp, eax
    mov al,0x20
    out 0x20,al
    RESTORE_REGS
    iret

_asm_irq_1:
    SAVE_REGS
    call kbd_irq
    mov al,0x20
    out 0x20,al
    RESTORE_REGS
    iret

_asm_irq_7:
    SAVE_REGS
    call isr_spurious
    RESTORE_REGS
    iret

# void task_switch(void)
#
# Cooperative yield. We hand-build the exact same trap frame the CPU + SAVE_REGS
# leave on a timer interrupt, so schedule() cannot tell a voluntary yield apart
# from a preemption and both resume identically. When this task is next picked,
# the matching RESTORE_REGS/iret (in whichever switch selects us) lands on
# .task_switch_resume, and we return to our caller with all registers intact.
task_switch:
    pushf                            # eflags (caller's IF, restored on resume)
    cli                              # make the switch itself atomic
    push cs                          # cs
    push offset .task_switch_resume  # eip to resume at
    SAVE_REGS
    push esp
    call schedule
    mov esp, eax                     # adopt next task's stack (drops the arg)
    RESTORE_REGS
    iret
.task_switch_resume:
    ret

# void _asm_syscall(void)  --  int 0x80 entry
#
# Reached through a DPL3 trap gate, so IF is left as the caller had it (enabled).
# We save the same frame the IRQ path uses and hand its address to
# syscall_dispatch, which reads eax/ebx/ecx/edx and writes the result back into
# the saved eax slot so popad returns it to the caller.
_asm_syscall:
    SAVE_REGS
    push esp                # pointer to struct regs
    call syscall_dispatch
    add esp, 4              # drop the arg; keep the (possibly modified) frame
    RESTORE_REGS
    iret
