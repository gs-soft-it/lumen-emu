// Original assembler oracle: each eight-byte entry is one access and RET.
// These are test instructions, never executable code imported from a ROM.
.text
.arch armv8.4-a
.p2align 2
.global _LumenAtomicAcquireReads
_LumenAtomicAcquireReads:
    ldarb w0, [x0]
    ret
    ldarh w0, [x0]
    ret
    ldar w0, [x0]
    ret
    ldar x0, [x0]
    ret

.global _LumenAtomicReleaseWrites
_LumenAtomicReleaseWrites:
    stlrb w1, [x0]
    ret
    stlrh w1, [x0]
    ret
    stlr w1, [x0]
    ret
    stlr x1, [x0]
    ret

.global _LumenRcpcAcquireReads
_LumenRcpcAcquireReads:
    ldaprb w0, [x0]
    ret
    ldaprh w0, [x0]
    ret
    ldapr w0, [x0]
    ret
    ldapr x0, [x0]
    ret

.global _LumenRcpcImmediateReads
_LumenRcpcImmediateReads:
    .irp RCPC_OFFSET, -256, -1, 0, 1, 255
        ldapurb w0, [x0, #\RCPC_OFFSET]
        ret
        ldapurh w0, [x0, #\RCPC_OFFSET]
        ret
        ldapur w0, [x0, #\RCPC_OFFSET]
        ret
        ldapur x0, [x0, #\RCPC_OFFSET]
        ret
    .endr

.global _LumenRcpcImmediateWrites
_LumenRcpcImmediateWrites:
    .irp RCPC_OFFSET, -256, -1, 0, 1, 255
        stlurb w1, [x0, #\RCPC_OFFSET]
        ret
        stlurh w1, [x0, #\RCPC_OFFSET]
        ret
        stlur w1, [x0, #\RCPC_OFFSET]
        ret
        stlur x1, [x0, #\RCPC_OFFSET]
        ret
    .endr
