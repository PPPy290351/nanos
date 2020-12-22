#define SYSCALL_FRAME_ARG0    FRAME_RDI
#define SYSCALL_FRAME_ARG1    FRAME_RSI
#define SYSCALL_FRAME_ARG2    FRAME_RDX
#define SYSCALL_FRAME_ARG3    FRAME_R10
#define SYSCALL_FRAME_ARG4    FRAME_R8
#define SYSCALL_FRAME_ARG5    FRAME_R9
#define SYSCALL_FRAME_RETVAL1 FRAME_RAX
#define SYSCALL_FRAME_RETVAL2 FRAME_RDX
#define SYSCALL_FRAME_SP      FRAME_RSP

#define UC_FP_XSTATE            0x1
#define UC_SIGCONTEXT_SS        0x2
#define UC_STRICT_RESTORE_SS    0x4

#define MINSIGSTKSZ 2048

/*
 * Bytes 464..511 in the current 512-byte layout of the FXSAVE/FXRSTOR frame
 * are reserved for SW usage. On CPUs supporting XSAVE/XRSTOR, these bytes are
 * used to extend the fpstate pointer in the sigcontext, which now includes the
 * extended state information along with fpstate information.
 *
 * If sw_reserved.magic1 == FP_XSTATE_MAGIC1 then there's a
 * sw_reserved.extended_size bytes large extended context area present. (The
 * last 32-bit word of this extended area (at the
 * fpstate+extended_size-FP_XSTATE_MAGIC2_SIZE address) is set to
 * FP_XSTATE_MAGIC2 so that you can sanity check your size calculations.)
 *
 * This extended area typically grows with newer CPUs that have larger and
 * larger XSAVE areas.
 */
struct _fpx_sw_bytes {
    u32 magic1;
    u32 extended_size;
    u64 xfeatures;
    u32 xstate_size;
    u32 padding[7];
};

/*
 * The 64-bit FPU frame. (FXSAVE format and later)
 *
 * Note1: If sw_reserved.magic1 == FP_XSTATE_MAGIC1 then the structure is
 *        larger: 'struct _xstate'. Note that 'struct _xstate' embedds
 *        'struct _fpstate' so that you can always assume the _fpstate portion
 *        exists so that you can check the magic value.
 *
 * Note2: Reserved fields may someday contain valuable data. Always
 *	  save/restore them when you change signal frames.
 */
struct _fpstate_64 {
    u16 cwd;
    u16 swd;
    /* Note this is not the same as the 32-bit/x87/FSAVE twd: */
    u16 twd;
    u16 fop;
    u64 rip;
    u64 rdp;
    u32 mxcsr;
    u32 mxcsr_mask;
    u32 st_space[32];
    u32 xmm_space[64];
    u32 reserved[12];
    union {
        u32 reserved3[12];
        struct _fpx_sw_bytes sw_reserved;
    };
};

struct sigcontext {
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
    u64 rdi;
    u64 rsi;
    u64 rbp;
    u64 rbx;
    u64 rdx;
    u64 rax;
    u64 rcx;
    u64 rsp;
    u64 rip;
    u64 eflags; /* RFLAGS */
    u16 cs;
    /*
     * Prior to 2.5.64 ("[PATCH] x86-64 updates for 2.5.64-bk3"),
     * Linux saved and restored fs and gs in these slots.  This
     * was counterproductive, as fsbase and gsbase were never
     * saved, so arch_prctl was presumably unreliable.
     *
     * These slots should never be reused without extreme caution:
     *
     *  - Some DOSEMU versions stash fs and gs in these slots manually,
     *    thus overwriting anything the kernel expects to be preserved
     *    in these slots.
     *
     *  - If these slots are ever needed for any other purpose,
     *    there is some risk that very old 64-bit binaries could get
     *    confused.  I doubt that many such binaries still work,
     *    though, since the same patch in 2.5.64 also removed the
     *    64-bit set_thread_area syscall, so it appears that there
     *    is no TLS API beyond modify_ldt that works in both pre-
     *    and post-2.5.64 kernels.
     *
     * If the kernel ever adds explicit fs, gs, fsbase, and gsbase
     * save/restore, it will most likely need to be opt-in and use
     * different context slots.
     */
    u16 gs;
    u16 fs;
    union {
        u16 ss; /* If UC_SIGCONTEXT SS */
        u16 __pad0; /* Alias name for old (!UC_SIGCONTEXT_SS) user-space */
    };
    u64 err;
    u64 trapno;
    u64 oldmask;
    u64 cr2;
    struct _fpstate *fpstate; /* Zero when no FPU context */
    u64 reserved1[8];
};

struct rt_sigframe {
    char *pretcode;
    struct ucontext uc;
    struct siginfo info;
};

struct ucontext {
    unsigned long uc_flags;
    struct ucontext * uc_link;
    stack_t uc_stack;
    struct sigcontext uc_mcontext;
    sigset_t uc_sigmask;
};

static inline u64 page_flags_from_vmflags(u64 vmflags)
{
    u64 flags = PAGE_NO_FAT | PAGE_USER;
    if ((vmflags & VMAP_FLAG_EXEC) == 0)
        flags |= PAGE_NO_EXEC;
    if ((vmflags & VMAP_FLAG_WRITABLE))
        flags |= PAGE_WRITABLE;
    return flags;
}