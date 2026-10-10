// enc.S - Instruction encodings of sicc's AArch64 assembler: each line's
// comment is the word the instruction must assemble to (taken once from a
// reference assembler; checked by enc.py). Weighted towards what a kernel
// needs: system registers, TLB and cache maintenance, barriers, exceptions.
//
// Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
    nop                              // d503201f
    wfi                              // d503207f
    wfe                              // d503205f
    sev                              // d503209f
    sevl                             // d50320bf
    yield                            // d503203f
    eret                             // d69f03e0
    isb                              // d5033fdf
    dsb sy                           // d5033f9f
    dsb ish                          // d5033b9f
    dsb ishst                        // d5033a9f
    dsb nsh                          // d503379f
    dsb osh                          // d503339f
    dmb ish                          // d5033bbf
    dmb ishld                        // d50339bf
    dmb ishst                        // d5033abf
    dmb sy                           // d5033fbf
    dmb ld                           // d5033dbf
    dmb st                           // d5033ebf
    svc #0                           // d4000001
    hvc #0                           // d4000002
    smc #0                           // d4000003
    brk #1                           // d4200020
    hlt #0                           // d4400000
    msr sctlr_el1, x0                // d5181000
    mrs x1, sctlr_el1                // d5381001
    msr ttbr0_el1, x2                // d5182002
    msr ttbr1_el1, x3                // d5182023
    msr tcr_el1, x4                  // d5182044
    msr mair_el1, x5                 // d518a205
    msr vbar_el1, x6                 // d518c006
    mrs x7, elr_el1                  // d5384027
    msr elr_el1, x8                  // d5184028
    mrs x9, spsr_el1                 // d5384009
    msr spsr_el1, x10                // d518400a
    mrs x11, esr_el1                 // d538520b
    mrs x12, far_el1                 // d538600c
    msr sp_el0, x13                  // d518410d
    mrs x14, sp_el0                  // d538410e
    msr tpidr_el1, x15               // d518d08f
    mrs x16, tpidr_el0               // d53bd050
    msr tpidr_el0, x17               // d51bd051
    msr tpidrro_el0, x19             // d51bd073
    mrs x20, currentel               // d5384254
    mrs x21, mpidr_el1               // d53800b5
    mrs x22, midr_el1                // d5380016
    mrs x23, cntfrq_el0              // d53be017
    mrs x24, cntvct_el0              // d53be058
    mrs x25, cntpct_el0              // d53be039
    msr cntv_ctl_el0, x26            // d51be33a
    msr cntv_cval_el0, x27           // d51be35b
    msr cntv_tval_el0, x28           // d51be31c
    msr cntp_ctl_el0, x0             // d51be220
    msr cntp_cval_el0, x1            // d51be241
    msr cntkctl_el1, x2              // d518e102
    msr cpacr_el1, x3                // d5181043
    mrs x4, par_el1                  // d5387404
    mrs x5, id_aa64mmfr0_el1         // d5380705
    mrs x6, id_aa64pfr0_el1          // d5380406
    mrs x7, ctr_el0                  // d53b0027
    mrs x8, dczid_el0                // d53b00e8
    msr contextidr_el1, x9           // d518d029
    msr hcr_el2, x10                 // d51c110a
    msr sctlr_el2, x11               // d51c100b
    msr spsr_el2, x12                // d51c400c
    msr elr_el2, x13                 // d51c402d
    msr vbar_el2, x14                // d51cc00e
    msr cnthctl_el2, x15             // d51ce10f
    msr cntvoff_el2, x16             // d51ce070
    msr cptr_el2, x17                // d51c1151
    msr hstr_el2, xzr                // d51c117f
    msr sp_el1, x0                   // d51c4100
    msr vmpidr_el2, x1               // d51c00a1
    msr vpidr_el2, x2                // d51c0002
    mrs x3, daif                     // d53b4223
    msr daif, x4                     // d51b4224
    msr nzcv, x5                     // d51b4205
    mrs x6, fpcr                     // d53b4406
    msr fpsr, x7                     // d51b4427
    msr daifset, #2                  // d50342df
    msr daifclr, #2                  // d50342ff
    msr daifset, #15                 // d5034fdf
    msr spsel, #1                    // d50041bf
    msr icc_sre_el1, x0              // d518cca0
    msr icc_pmr_el1, x1              // d5184601
    mrs x2, icc_iar1_el1             // d538cc02
    msr icc_eoir1_el1, x3            // d518cc23
    msr icc_igrpen1_el1, x4          // d518cce4
    mrs x5, s3_0_c15_c2_1            // d538f225
    msr s3_1_c15_c2_1, x6            // d519f226
    tlbi vmalle1                     // d508871f
    tlbi vmalle1is                   // d508831f
    tlbi vae1, x0                    // d5088720
    tlbi vae1is, x1                  // d5088321
    tlbi aside1is, x2                // d5088342
    tlbi vaae1is, x3                 // d5088363
    tlbi vale1is, x4                 // d50883a4
    tlbi alle2                       // d50c871f
    dc civac, x0                     // d50b7e20
    dc cvac, x1                      // d50b7a21
    dc ivac, x2                      // d5087622
    dc zva, x3                       // d50b7423
    dc cvau, x4                      // d50b7b24
    dc isw, x5                       // d5087645
    dc csw, x6                       // d5087a46
    dc cisw, x7                      // d5087e47
    ic iallu                         // d508751f
    ic ialluis                       // d508711f
    ic ivau, x8                      // d50b7528
    at s1e1r, x9                     // d5087809
    at s1e1w, x10                    // d508782a
    at s1e0r, x11                    // d508784b
    ldaxr x0, [x1]                   // c85ffc20
    stlxr w2, x3, [x4]               // c802fc83
    ldxr w5, [x6]                    // 885f7cc5
    stxr w7, w8, [x9]                // 88077d28
    ldaxrb w0, [x1]                  // 085ffc20
    stlxrh w2, w3, [x4]              // 4802fc83
    ldar x5, [sp]                    // c8dfffe5
    stlr w6, [x7]                    // 889ffce6
    ldp x29, x30, [sp], #16          // a8c17bfd
    stp x29, x30, [sp, #-16]!        // a9bf7bfd
    stp q0, q1, [x0, #32]            // ad010400
    ldp d8, d9, [sp, #-512]          // 6d6027e8
    ldpsw x0, x1, [x2, #8]           // 69410440
    ldr x0, [x1, #32760]             // f97ffc20
    ldr w0, [x1, #-256]              // b8500020
    ldrb w2, [x3, x4]                // 38646862
    ldrsh x5, [x6, x7, lsl #1]       // 78a778c5
    ldr x8, [x9, w10, sxtw #3]       // f86ad928
    ldr x8, [x9, w10, uxtw]          // f86a4928
    str q0, [sp, #-16]!              // 3c9f0fe0
    ldr d1, [x2], #8                 // fc408441
    ldur x3, [x4, #-1]               // f85ff083
    adr x0, .                        // 10000000
    adrp x1, .                       // 90000001
    mov x0, sp                       // 910003e0
    mov sp, x1                       // 9100003f
    mov x2, #0x12340000              // d2a24682
    mov w3, #-1                      // 12800003
    mov x4, #0xff00ff00ff00ff00      // b2089fe4
    movk x5, #0xbeef, lsl #48        // f2f7dde5
    movn w6, #0                      // 12800006
    add x0, x1, #4095                // 913ffc20
    add x0, x1, #1, lsl #12          // 91400420
    sub sp, sp, #0x10                // d10043ff
    adds w2, w3, w4, lsl #3          // 2b040c62
    subs xzr, x5, x6, asr #63        // eb86fcbf
    cmp x0, #0                       // f100001f
    cmn w1, #1                       // 3100043f
    add x0, sp, x1                   // 8b2163e0
    add x2, x3, w4, uxtb #2          // 8b240862
    sub x5, x6, w7, sxtw             // cb27c0c5
    and x0, x1, #0xff                // 92401c20
    orr w2, w3, #0x80000000          // 32010062
    eor x4, x5, #0x5555555555555555  // d200f0a4
    tst x6, #7                       // f24008df
    bic x7, x8, x9, lsl #4           // 8a291107
    orn w10, w11, w12                // 2a2c016a
    eon x13, x14, x15, ror #8        // caef21cd
    ands w0, w1, w2                  // 6a020020
    bics x3, x4, x5                  // ea250083
    lsl x0, x1, #3                   // d37df020
    lsr w2, w3, #31                  // 531f7c62
    asr x4, x5, #63                  // 937ffca4
    ror w6, w7, #8                   // 138720e6
    lslv x8, x9, x10                 // 9aca2128
    udiv w11, w12, w13               // 1acd098b
    sdiv x14, x15, x16               // 9ad00dee
    mul x0, x1, x2                   // 9b027c20
    madd w3, w4, w5, w6              // 1b051883
    msub x7, x8, x9, x10             // 9b09a907
    smulh x11, x12, x13              // 9b4d7d8b
    umulh x14, x15, x16              // 9bd07dee
    smull x17, w19, w20              // 9b347e71
    umaddl x0, w1, w2, x3            // 9ba20c20
    smaddl x4, w5, w6, x7            // 9b261ca4
    clz x0, x1                       // dac01020
    cls w2, w3                       // 5ac01462
    rbit x4, x5                      // dac000a4
    rev w6, w7                       // 5ac008e6
    rev16 x8, x9                     // dac00528
    rev32 x10, x11                   // dac0096a
    csel x0, x1, x2, eq              // 9a820020
    csinc w3, w4, w5, ne             // 1a851483
    csinv x6, x7, x8, lt             // da88b0e6
    csneg w9, w10, w11, ge           // 5a8ba549
    cset x12, hi                     // 9a9f97ec
    csetm w13, ls                    // 5a9f83ed
    cinc x14, x15, cc                // 9a8f25ee
    cneg w16, w17, mi                // 5a915630
    ccmp x0, x1, #4, ne              // fa411004
    ccmn w2, #3, #0, eq              // 3a430840
    ubfx x0, x1, #4, #8              // d3442c20
    sbfx w2, w3, #1, #31             // 13017c62
    ubfiz x4, x5, #2, #10            // d37e24a4
    bfi w6, w7, #3, #4               // 331d0ce6
    bfxil x8, x9, #0, #16            // b3403d28
    sxtb x0, w1                      // 93401c20
    sxth w2, w3                      // 13003c62
    sxtw x4, w5                      // 93407ca4
    uxtb w6, w7                      // 53001ce6
    uxth w8, w9                      // 53003d28
    extr x10, x11, x12, #13          // 93cc356a
    adc x0, x1, x2                   // 9a020020
    sbcs w3, w4, w5                  // 7a050083
    neg x6, x7                       // cb0703e6
    negs w8, w9, lsl #2              // 6b090be8
    mvn x10, x11                     // aa2b03ea
    fmov d0, x1                      // 9e670020
    fmov x2, d3                      // 9e660062
    fmov s4, w5                      // 1e2700a4
    fmov d6, d7                      // 1e6040e6
    fadd d0, d1, d2                  // 1e622820
    fsub s3, s4, s5                  // 1e253883
    fmul d6, d7, d8                  // 1e6808e6
    fdiv s9, s10, s11                // 1e2b1949
    fnmul d12, d13, d14              // 1e6e89ac
    fmax d15, d16, d17               // 1e714a0f
    fminnm s18, s19, s20             // 1e347a72
    fmadd d0, d1, d2, d3             // 1f420c20
    fmsub s4, s5, s6, s7             // 1f069ca4
    fnmadd d8, d9, d10, d11          // 1f6a2d28
    fnmsub s12, s13, s14, s15        // 1f2ebdac
    fabs d0, d1                      // 1e60c020
    fneg s2, s3                      // 1e214062
    fsqrt d4, d5                     // 1e61c0a4
    frintm d6, d7                    // 1e6540e6
    frintz s8, s9                    // 1e25c128
    frinta d10, d11                  // 1e66416a
    frintn d12, d13                  // 1e6441ac
    frintx s14, s15                  // 1e2741ee
    fcvt d0, s1                      // 1e22c020
    fcvt s2, d3                      // 1e624062
    fcmp d0, d1                      // 1e612000
    fcmpe s2, #0.0                   // 1e202058
    fccmp d3, d4, #0, eq             // 1e640460
    fcsel d5, d6, d7, gt             // 1e67ccc5
    scvtf d0, x1                     // 9e620020
    ucvtf s2, w3                     // 1e230062
    fcvtzs x4, d5                    // 9e7800a4
    fcvtzu w6, s7                    // 1e3900e6
    fcvtns x8, d9                    // 9e600128
    fcvtms w10, d11                  // 1e70016a
    fcvtps x12, s13                  // 9e2801ac
    fcvtau x14, d15                  // 9e6501ee
    b .                              // 14000000
    bl .                             // 94000000
    b.eq .                           // 54000000
    cbz x0, .                        // b4000000
    cbnz w1, .                       // 35000001
    tbz x2, #63, .                   // b6f80002
    tbnz w3, #0, .                   // 37000003
    br x16                           // d61f0200
    blr x17                          // d63f0220
    ret                              // d65f03c0
    ret x1                           // d65f0020
    ldaddal w0, w1, [x2]             // b8e00041
    ldaddal x3, x4, [sp]             // f8e303e4
    ldaddalb w5, w6, [x7]            // 38e500e6
    ldaddh w8, w9, [x10]             // 78280149
    ldadda x11, x12, [x13]           // f8ab01ac
    ldaddl w14, w15, [x16]           // b86e020f
    ldclral x0, x1, [x2]             // f8e01041
    ldeoral w3, w4, [x5]             // b8e320a4
    ldsetal x6, x7, [x8]             // f8e63107
    swpal w9, w10, [x11]             // b8e9816a
    swpalb w12, w13, [x14]           // 38ec81cd
    swp x15, x16, [x17]              // f82f8230
    casal w0, w1, [x2]               // 88e0fc41
    casal x3, x4, [x5]               // c8e3fca4
    casalb w6, w7, [x8]              // 08e6fd07
    casalh w9, w10, [x11]            // 48e9fd6a
    casa x12, x13, [x14]             // c8ec7dcd
    casl w15, w16, [x17]             // 88affe30
    cas x19, x20, [x21]              // c8b37eb4
// NEON (Advanced SIMD): the code generator's vector operations and arm_neon.h
    add v0.16b, v1.16b, v2.16b           // 4e228420
    add v3.8h, v4.8h, v5.8h              // 4e658483
    add v0.4s, v1.4s, v31.4s             // 4ebf8420
    add v0.2d, v1.2d, v2.2d              // 4ee28420
    add v0.8b, v1.8b, v2.8b              // 0e228420
    add v0.2s, v1.2s, v2.2s              // 0ea28420
    sub v0.4s, v1.4s, v2.4s              // 6ea28420
    mul v0.4s, v1.4s, v2.4s              // 4ea29c20
    mul v0.8h, v1.8h, v2.8h              // 4e629c20
    mla v0.4s, v1.4s, v2.4s              // 4ea29420
    mls v0.8h, v1.8h, v2.8h              // 6e629420
    cmgt v0.4s, v1.4s, v2.4s             // 4ea23420
    cmhi v0.16b, v1.16b, v2.16b          // 6e223420
    cmge v0.2d, v1.2d, v2.2d             // 4ee23c20
    cmhs v0.8h, v1.8h, v2.8h             // 6e623c20
    cmeq v0.4s, v1.4s, v2.4s             // 6ea28c20
    cmtst v0.16b, v1.16b, v2.16b         // 4e228c20
    smax v0.4s, v1.4s, v2.4s             // 4ea26420
    umax v0.16b, v1.16b, v2.16b          // 6e226420
    smin v0.8h, v1.8h, v2.8h             // 4e626c20
    umin v0.4s, v1.4s, v2.4s             // 6ea26c20
    sqadd v0.16b, v1.16b, v2.16b         // 4e220c20
    uqadd v0.8h, v1.8h, v2.8h            // 6e620c20
    sqsub v0.4s, v1.4s, v2.4s            // 4ea22c20
    uqsub v0.16b, v1.16b, v2.16b         // 6e222c20
    srhadd v0.16b, v1.16b, v2.16b        // 4e221420
    urhadd v0.8h, v1.8h, v2.8h           // 6e621420
    sshl v0.4s, v1.4s, v2.4s             // 4ea24420
    ushl v0.2d, v1.2d, v2.2d             // 6ee24420
    addp v0.4s, v1.4s, v2.4s             // 4ea2bc20
    addp v0.2s, v1.2s, v2.2s             // 0ea2bc20
    smaxp v0.4s, v1.4s, v2.4s            // 4ea2a420
    uminp v0.8h, v1.8h, v2.8h            // 6e62ac20
    sabd v0.16b, v1.16b, v2.16b          // 4e227420
    and v0.16b, v1.16b, v2.16b           // 4e221c20
    bic v0.16b, v1.16b, v2.16b           // 4e621c20
    orr v0.8b, v1.8b, v2.8b              // 0ea21c20
    orn v0.16b, v1.16b, v2.16b           // 4ee21c20
    eor v0.16b, v1.16b, v2.16b           // 6e221c20
    bsl v0.16b, v1.16b, v2.16b           // 6e621c20
    bit v0.16b, v1.16b, v2.16b           // 6ea21c20
    bif v0.16b, v1.16b, v2.16b           // 6ee21c20
    mov v0.16b, v1.16b                   // 4ea11c20
    mov v3.8b, v4.8b                     // 0ea41c83
    fadd v0.4s, v1.4s, v2.4s             // 4e22d420
    fadd v0.2d, v1.2d, v2.2d             // 4e62d420
    fadd v0.2s, v1.2s, v2.2s             // 0e22d420
    fsub v0.4s, v1.4s, v2.4s             // 4ea2d420
    fmul v0.4s, v1.4s, v2.4s             // 6e22dc20
    fdiv v0.2d, v1.2d, v2.2d             // 6e62fc20
    fmla v0.4s, v1.4s, v2.4s             // 4e22cc20
    fmls v0.2d, v1.2d, v2.2d             // 4ee2cc20
    fmax v0.4s, v1.4s, v2.4s             // 4e22f420
    fmin v0.4s, v1.4s, v2.4s             // 4ea2f420
    fmaxnm v0.4s, v1.4s, v2.4s           // 4e22c420
    fminnm v0.2d, v1.2d, v2.2d           // 4ee2c420
    fcmeq v0.4s, v1.4s, v2.4s            // 4e22e420
    fcmge v0.4s, v1.4s, v2.4s            // 6e22e420
    fcmgt v0.2d, v1.2d, v2.2d            // 6ee2e420
    faddp v0.4s, v1.4s, v2.4s            // 6e22d420
    fmaxp v0.4s, v1.4s, v2.4s            // 6e22f420
    fminp v0.4s, v1.4s, v2.4s            // 6ea2f420
    fabd v0.4s, v1.4s, v2.4s             // 6ea2d420
    fmla v0.4s, v1.4s, v2.s[0]           // 4f821020
    fmla v0.4s, v1.4s, v2.s[3]           // 4fa21820
    fmla v0.4s, v1.4s, v17.s[1]          // 4fb11020
    fmla v0.2d, v1.2d, v2.d[1]           // 4fc21820
    fmul v0.4s, v1.4s, v2.s[2]           // 4f829820
    fmls v0.4s, v1.4s, v2.s[1]           // 4fa25020
    neg v0.4s, v1.4s                     // 6ea0b820
    neg v0.16b, v1.16b                   // 6e20b820
    abs v0.8h, v1.8h                     // 4e60b820
    abs v0.2d, v1.2d                     // 4ee0b820
    not v0.16b, v1.16b                   // 6e205820
    mvn v0.8b, v1.8b                     // 2e205820
    cnt v0.16b, v1.16b                   // 4e205820
    rev64 v0.4s, v1.4s                   // 4ea00820
    rev32 v0.8h, v1.8h                   // 6e600820
    rev16 v0.16b, v1.16b                 // 4e201820
    cls v0.4s, v1.4s                     // 4ea04820
    clz v0.16b, v1.16b                   // 6e204820
    saddlp v0.8h, v1.16b                 // 4e202820
    uaddlp v0.4s, v1.8h                  // 6e602820
    saddlp v0.2d, v1.4s                  // 4ea02820
    sadalp v0.4s, v1.8h                  // 4e606820
    uadalp v0.8h, v1.16b                 // 6e206820
    saddlp v0.4h, v1.8b                  // 0e202820
    xtn v0.8b, v1.8h                     // 0e212820
    xtn2 v0.16b, v1.8h                   // 4e212820
    xtn v0.4h, v1.4s                     // 0e612820
    sqxtn v0.8b, v1.8h                   // 0e214820
    uqxtn v0.4h, v1.4s                   // 2e614820
    sqxtun v0.8b, v1.8h                  // 2e212820
    sqxtn2 v0.16b, v1.8h                 // 4e214820
    cmgt v0.4s, v1.4s, #0                // 4ea08820
    cmge v0.16b, v1.16b, #0              // 6e208820
    cmeq v0.8h, v1.8h, #0                // 4e609820
    cmle v0.4s, v1.4s, #0                // 6ea09820
    cmlt v0.2d, v1.2d, #0                // 4ee0a820
    fneg v0.4s, v1.4s                    // 6ea0f820
    fabs v0.2d, v1.2d                    // 4ee0f820
    fsqrt v0.4s, v1.4s                   // 6ea1f820
    scvtf v0.4s, v1.4s                   // 4e21d820
    ucvtf v0.2d, v1.2d                   // 6e61d820
    fcvtzs v0.4s, v1.4s                  // 4ea1b820
    fcvtzu v0.4s, v1.4s                  // 6ea1b820
    fcvtns v0.4s, v1.4s                  // 4e21a820
    fcvtnu v0.2d, v1.2d                  // 6e61a820
    fcvtms v0.4s, v1.4s                  // 4e21b820
    fcvtps v0.4s, v1.4s                  // 4ea1a820
    fcvtas v0.4s, v1.4s                  // 4e21c820
    frintn v0.4s, v1.4s                  // 4e218820
    frintm v0.4s, v1.4s                  // 4e219820
    frintp v0.2d, v1.2d                  // 4ee18820
    frintz v0.4s, v1.4s                  // 4ea19820
    frinta v0.4s, v1.4s                  // 6e218820
    frintx v0.4s, v1.4s                  // 6e219820
    frecpe v0.4s, v1.4s                  // 4ea1d820
    frsqrte v0.4s, v1.4s                 // 6ea1d820
    fcvtl v0.4s, v1.4h                   // 0e217820
    fcvtl2 v0.4s, v1.8h                  // 4e217820
    fcvtl v0.2d, v1.2s                   // 0e617820
    fcvtn v0.4h, v1.4s                   // 0e216820
    fcvtn2 v0.8h, v1.4s                  // 4e216820
    fcvtn v0.2s, v1.2d                   // 0e616820
    saddl v0.8h, v1.8b, v2.8b            // 0e220020
    uaddl2 v0.4s, v1.8h, v2.8h           // 6e620020
    saddw v0.8h, v1.8h, v2.8b            // 0e221020
    uaddw2 v0.4s, v1.4s, v2.8h           // 6e621020
    ssubl v0.2d, v1.2s, v2.2s            // 0ea22020
    smlal v0.8h, v1.8b, v2.8b            // 0e228020
    smlal2 v0.8h, v1.16b, v2.16b         // 4e228020
    umlal v0.4s, v1.4h, v2.4h            // 2e628020
    umlal2 v0.2d, v1.4s, v2.4s           // 6ea28020
    smlsl v0.4s, v1.4h, v2.4h            // 0e62a020
    smull v0.8h, v1.8b, v2.8b            // 0e22c020
    smull2 v0.8h, v1.16b, v2.16b         // 4e22c020
    umull v0.4s, v1.4h, v2.4h            // 2e62c020
    umull2 v0.2d, v1.4s, v2.4s           // 6ea2c020
    sabdl v0.8h, v1.8b, v2.8b            // 0e227020
    sabal v0.8h, v1.8b, v2.8b            // 0e225020
    sdot v0.4s, v1.16b, v2.16b           // 4e829420
    udot v0.4s, v1.16b, v2.16b           // 6e829420
    sdot v0.2s, v1.8b, v2.8b             // 0e829420
    sdot v0.4s, v1.16b, v2.4b[0]         // 4f82e020
    sdot v0.4s, v1.16b, v2.4b[1]         // 4fa2e020
    sdot v0.4s, v1.16b, v2.4b[2]         // 4f82e820
    sdot v0.4s, v1.16b, v18.4b[3]        // 4fb2e820
    udot v3.4s, v4.16b, v5.4b[3]         // 6fa5e883
    sshr v0.4s, v1.4s, #3                // 4f3d0420
    ushr v0.16b, v1.16b, #4              // 6f0c0420
    ushr v0.2d, v1.2d, #63               // 6f410420
    sshr v0.8h, v1.8h, #16               // 4f100420
    ssra v0.4s, v1.4s, #1                // 4f3f1420
    usra v0.4s, v1.4s, #8                // 6f381420
    srshr v0.4s, v1.4s, #2               // 4f3e2420
    urshr v0.8h, v1.8h, #2               // 6f1e2420
    shl v0.4s, v1.4s, #3                 // 4f235420
    shl v0.16b, v1.16b, #7               // 4f0f5420
    shl v0.2d, v1.2d, #0                 // 4f405420
    sli v0.4s, v1.4s, #4                 // 6f245420
    sri v0.16b, v1.16b, #4               // 6f0c4420
    sqshl v0.4s, v1.4s, #3               // 4f237420
    uqshl v0.8h, v1.8h, #3               // 6f137420
    sshll v0.8h, v1.8b, #2               // 0f0aa420
    ushll2 v0.4s, v1.8h, #1              // 6f11a420
    sxtl v0.8h, v1.8b                    // 0f08a420
    sxtl2 v0.8h, v1.16b                  // 4f08a420
    uxtl v0.4s, v1.4h                    // 2f10a420
    uxtl2 v0.2d, v1.4s                   // 6f20a420
    shrn v0.8b, v1.8h, #4                // 0f0c8420
    shrn2 v0.16b, v1.8h, #4              // 4f0c8420
    rshrn v0.4h, v1.4s, #8               // 0f188c20
    sqshrn v0.8b, v1.8h, #1              // 0f0f9420
    uqshrn v0.4h, v1.4s, #3              // 2f1d9420
    sqshrun v0.8b, v1.8h, #2             // 2f0e8420
    uzp1 v0.4s, v1.4s, v2.4s             // 4e821820
    uzp2 v0.16b, v1.16b, v2.16b          // 4e025820
    trn1 v0.8h, v1.8h, v2.8h             // 4e422820
    trn2 v0.2d, v1.2d, v2.2d             // 4ec26820
    zip1 v0.4s, v1.4s, v2.4s             // 4e823820
    zip2 v0.8b, v1.8b, v2.8b             // 0e027820
    ext v0.16b, v1.16b, v2.16b, #8       // 6e024020
    ext v0.8b, v1.8b, v2.8b, #3          // 2e021820
    tbl v0.16b, {v1.16b}, v2.16b         // 4e020020
    tbl v0.8b, {v1.16b}, v2.8b           // 0e020020
    tbl v0.16b, {v1.16b, v2.16b}, v3.16b // 4e032020
    tbx v0.16b, {v1.16b}, v2.16b         // 4e021020
    dup v0.4s, v1.s[1]                   // 4e0c0420
    dup v0.16b, v1.b[15]                 // 4e1f0420
    dup v0.2d, v1.d[1]                   // 4e180420
    dup v0.8h, v1.h[3]                   // 4e0e0420
    dup v0.4s, w1                        // 4e040c20
    dup v0.2d, x1                        // 4e080c20
    dup v0.16b, w2                       // 4e010c40
    umov w0, v1.s[1]                     // 0e0c3c20
    umov w0, v1.b[5]                     // 0e0b3c20
    umov x0, v1.d[1]                     // 4e183c20
    smov w0, v1.h[2]                     // 0e0a2c20
    smov x0, v1.b[3]                     // 4e072c20
    mov w0, v1.s[2]                      // 0e143c20
    mov x0, v1.d[0]                      // 4e083c20
    mov v0.s[1], w1                      // 4e0c1c20
    mov v0.d[1], x1                      // 4e181c20
    mov v0.b[3], w1                      // 4e071c20
    mov v0.s[2], v1.s[0]                 // 6e140420
    ins v0.h[5], v1.h[1]                 // 6e161420
    movi v0.16b, #0                      // 4f00e400
    movi v0.8b, #255                     // 0f07e7e0
    movi v0.16b, #15                     // 4f00e5e0
    movi v0.4s, #1                       // 4f000420
    movi v0.2d, #0                       // 6f00e400
    addv s0, v1.4s                       // 4eb1b820
    addv b0, v1.16b                      // 4e31b820
    addv h0, v1.8h                       // 4e71b820
    addv b0, v1.8b                       // 0e31b820
    saddlv h0, v1.16b                    // 4e303820
    uaddlv s0, v1.8h                     // 6e703820
    smaxv s0, v1.4s                      // 4eb0a820
    uminv b0, v1.16b                     // 6e31a820
    addp d0, v1.2d                       // 5ef1b820
    faddp s0, v1.2s                      // 7e30d820
    faddp d0, v1.2d                      // 7e70d820
    ld1 {v0.16b}, [x0]                   // 4c407000
    ld1 {v0.4s, v1.4s}, [x1]             // 4c40a820
    st1 {v2.2d}, [x3]                    // 4c007c62
    st1 {v4.16b, v5.16b, v6.16b, v7.16b}, [x9]// 4c002124
    ld1 {v1.8b}, [x2]                    // 0c407041
