
.subst mmio_write_addr '0x3FF << 22'
.subst mmio_trigger_addr '0x3FF << 22 + 1'
.subst mmio_serial_write '0x100 << 22 + 0'
.subst stack_addr '0x002 << 22'

.global entry
.section "code"
entry:
        # initialize basic segmentation
        lda %r1, @mmio_write_addr
        lda %r2, @mmio_trigger_addr
        lda %r0, segment_table # table address
        stw (%r1), %r0
        mov %r0, 0x00 # refresh command id
        stb (%r2), %r0
        mov %r0, 0x01 # TLB invalidation id
        stb (%r2), %r0

        lda %st, @stack_addr # set the stack

        # we can now use procedure call formats 
        lda %s0, string
        lda %s1, print_serial
        prc %s1
        hlt

# s0 in pointer
# s1 char holding from str
# s2 mmio serial addr
print_serial:
        lda %s2, @mmio_serial_write

print_serial_L0:
        ldb %s1, (%s0)
        tst %s1, 0
        mov eq %pc, %lf
        stb (%s2), %s1
        add %s0, %s0, 1
        jmp print_serial_L0

.section "data"
string:
        .align 4
        .asciz "hello, world!"

# we need to override the default segment table
# so that we can have a stack 
segment_table:
        .align 0x10
        .dw 0x04
        .align  0x10
        # code segment 
        .dw 0b0101 # flags, active execute 
        .dw 0x000  # segment ID 
        .dw 0x0000 # offset 
        .dw 0x1000 # size 

        # data segment 
        .dw 0b0011 # flags, active write 
        .dw 0x001  # segment ID 
        .dw 0x1000 # offset 
        .dw 0x1000 # size 

        # stack segment 
        .dw 0b0011 # flags, active write 
        .dw 0x002  # segment ID 
        .dw 0x2000 # offset 
        .dw 0x1000 # size 

        # mmu segment 
        .dw 0b100011 # flags, active write MMIO 
        .dw 0x3FF    # segment ID 
        .dw 0x0000   # hardware ID, 0 = MMU 
        .dw 0x0000   # unused 

        # serial segment 
        .dw 0b100011 # flags, active write MMIO 
        .dw 0x100    # segment ID 
        .dw 0x0010   # hardware ID, 16 = serial out (not standardized) 
        .dw 0x0000   # unused 

