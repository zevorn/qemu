    .option    norvc

    .text
    .global _start
    .global crt_abort

_start:
    lla     t0, trap
    csrw    mtvec, t0

    # sp size 0x400000
    csrr    a0, mhartid
    slli    a1, a0, 22
    # sp base 0x84000000
    li      sp, 0x84000000
    # TODO: initiation bsp
    call    _init_bsp
    # jump main function
    call    main
    # Success!
    li      a0, 0
    j       _exit

_init_bsp:
    addi    sp, sp, -32
    sd      t0, 16(sp)
    sd      t1, 8(sp)
    sd      t2, 0(sp)
    li      t2, 0x800000000024112d
    csrw    0x301, t2
    li      t0, 0x2200
    csrr    t1, mstatus
    or      t1, t1, t0
    csrw    mstatus, t1
    ld      t2, 0(sp)
    ld      t1, 8(sp)
    ld      t0, 16(sp)
    addi    sp, sp, 32
    ret

trap:
    # When an instruction traps, compare it to the insn in memory.
    csrr    t0, mepc
    csrr    t1, mtval
    lwu     t2, 0(t0)
    bne     t1, t2, fail

    # Skip the insn and continue.
    addi    t0, t0, 4
    csrw    mepc, t0
    mret

crt_abort:
fail:
    li    a0, 1

# Exit code in a0
_exit:
    lla    a1, semiargs
    li    t0, 0x20026    # ADP_Stopped_ApplicationExit
    sd    t0, 0(a1)
    sd    a0, 8(a1)
    li    a0, 0x20    # TARGET_SYS_EXIT_EXTENDED

    # Semihosting call sequence
    .balign    16
    slli    zero, zero, 0x1f
    ebreak
    srai    zero, zero, 0x7
    j    .

    .data
    .balign    16
semiargs:
    .space     16
