# Qt's qsimd_p.h workaround for GCC bug 49001, supplied to every LTO
# assembler invocation. Top-level C++ asm alone reaches only one partition.
.ifndef qt_mingw_avx_macros
.set qt_mingw_avx_macros, 1
.macro vmovapd args:vararg
    vmovupd \args
.endm
.macro vmovaps args:vararg
    vmovups \args
.endm
.macro vmovdqa args:vararg
    vmovdqu \args
.endm
.macro vmovdqa32 args:vararg
    vmovdqu32 \args
.endm
.macro vmovdqa64 args:vararg
    vmovdqu64 \args
.endm
.endif
