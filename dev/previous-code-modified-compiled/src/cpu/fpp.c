/*
 * UAE - The Un*x Amiga Emulator
 *
 * MC68881/68882/68040/68060 FPU emulation
 *
 * Copyright 1996 Herman ten Brugge
 * Modified 2005 Peter Keunecke
 * 68040+ exceptions and more by Toni Wilen
 */

#include "main.h"
#include "hatari-glue.h"


#include "sysconfig.h"
#include "sysdeps.h"


#include "options_cpu.h"
#include "memory.h"
#include "newcpu.h"
#include "cpummu.h"
#include "cpummu030.h"

#ifdef WITH_SOFTFLOAT
#include "fpp-softfloat.h"
#else
#include "md-fpp.h"
#endif

#define DEBUG_FPP 0
#define EXCEPTION_FPP 0
#define ARITHMETIC_EXCEPTIONS 1

static int warned = 100;

struct fpp_cr_entry {
    uae_u32 val[3];
    uae_u8 inexact;
    uae_s8 rndoff[4];
};

static const struct fpp_cr_entry fpp_cr[22] = {
    { {0x40000000, 0xc90fdaa2, 0x2168c235}, 1, {0,-1,-1, 0} }, //  0 = pi
    { {0x3ffd0000, 0x9a209a84, 0xfbcff798}, 1, {0, 0, 0, 1} }, //  1 = log10(2)
    { {0x40000000, 0xadf85458, 0xa2bb4a9a}, 1, {0, 0, 0, 1} }, //  2 = e
    { {0x3fff0000, 0xb8aa3b29, 0x5c17f0bc}, 1, {0,-1,-1, 0} }, //  3 = log2(e)
    { {0x3ffd0000, 0xde5bd8a9, 0x37287195}, 0, {0, 0, 0, 0} }, //  4 = log10(e)
    { {0x00000000, 0x00000000, 0x00000000}, 0, {0, 0, 0, 0} }, //  5 = 0.0
    { {0x3ffe0000, 0xb17217f7, 0xd1cf79ac}, 1, {0,-1,-1, 0} }, //  6 = ln(2)
    { {0x40000000, 0x935d8ddd, 0xaaa8ac17}, 1, {0,-1,-1, 0} }, //  7 = ln(10)
    { {0x3fff0000, 0x80000000, 0x00000000}, 0, {0, 0, 0, 0} }, //  8 = 1e0
    { {0x40020000, 0xa0000000, 0x00000000}, 0, {0, 0, 0, 0} }, //  9 = 1e1
    { {0x40050000, 0xc8000000, 0x00000000}, 0, {0, 0, 0, 0} }, // 10 = 1e2
    { {0x400c0000, 0x9c400000, 0x00000000}, 0, {0, 0, 0, 0} }, // 11 = 1e4
    { {0x40190000, 0xbebc2000, 0x00000000}, 0, {0, 0, 0, 0} }, // 12 = 1e8
    { {0x40340000, 0x8e1bc9bf, 0x04000000}, 0, {0, 0, 0, 0} }, // 13 = 1e16
    { {0x40690000, 0x9dc5ada8, 0x2b70b59e}, 1, {0,-1,-1, 0} }, // 14 = 1e32
    { {0x40d30000, 0xc2781f49, 0xffcfa6d5}, 1, {0, 0, 0, 1} }, // 15 = 1e64
    { {0x41a80000, 0x93ba47c9, 0x80e98ce0}, 1, {0,-1,-1, 0} }, // 16 = 1e128
    { {0x43510000, 0xaa7eebfb, 0x9df9de8e}, 1, {0,-1,-1, 0} }, // 17 = 1e256
    { {0x46a30000, 0xe319a0ae, 0xa60e91c7}, 1, {0,-1,-1, 0} }, // 18 = 1e512
    { {0x4d480000, 0xc9767586, 0x81750c17}, 1, {0, 0, 0, 1} }, // 19 = 1e1024
    { {0x5a920000, 0x9e8b3b5d, 0xc53d5de5}, 1, {0,-1,-1, 0} }, // 20 = 1e2048
    { {0x75250000, 0xc4605202, 0x8a20979b}, 1, {0,-1,-1, 0} }  // 21 = 1e4096
};

#define FPP_CR_PI       0
#define FPP_CR_LOG10_2  1
#define FPP_CR_E        2
#define FPP_CR_LOG2_E   3
#define FPP_CR_LOG10_E  4
#define FPP_CR_ZERO     5
#define FPP_CR_LN_2     6
#define FPP_CR_LN_10    7
#define FPP_CR_1E0      8
#define FPP_CR_1E1      9
#define FPP_CR_1E2      10
#define FPP_CR_1E4      11
#define FPP_CR_1E8      12
#define FPP_CR_1E16     13
#define FPP_CR_1E32     14
#define FPP_CR_1E64     15
#define FPP_CR_1E128    16
#define FPP_CR_1E256    17
#define FPP_CR_1E512    18
#define FPP_CR_1E1024   19
#define FPP_CR_1E2048   20
#define FPP_CR_1E4096   21

struct fpp_cr_entry_undef {
    uae_u32 val[3];
};

#define FPP_CR_NUM_SPECIAL_UNDEFINED 10

// 68881 and 68882 have identical undefined fields
static const struct fpp_cr_entry_undef fpp_cr_undef[FPP_CR_NUM_SPECIAL_UNDEFINED+1] = {
    { {0x40000000, 0x00000000, 0x00000000} },
    { {0x40010000, 0xfe000682, 0x00000000} },
    { {0x40010000, 0xffc00503, 0x80000000} },
    { {0x20000000, 0x7fffffff, 0x00000000} },
    { {0x00000000, 0xffffffff, 0xffffffff} },
    { {0x3c000000, 0xffffffff, 0xfffff800} },
    { {0x3f800000, 0xffffff00, 0x00000000} },
    { {0x00010000, 0xf65d8d9c, 0x00000000} },
    { {0x7fff0000, 0x001e0000, 0x00000000} },
    { {0x43ff0000, 0x000e0000, 0x00000000} },
    { {0x407f0000, 0x00060000, 0x00000000} }
};

uae_u32 xhex_nan[]   ={0x7fff0000, 0xffffffff, 0xffffffff};

static bool fpu_mmu_fixup;


/* Floating Point Control Register (FPCR)
 *
 * Exception Enable Byte
 * x--- ---- ---- ----  bit 15: BSUN (branch/set on unordered)
 * -x-- ---- ---- ----  bit 14: SNAN (signaling not a number)
 * --x- ---- ---- ----  bit 13: OPERR (operand error)
 * ---x ---- ---- ----  bit 12: OVFL (overflow)
 * ---- x--- ---- ----  bit 11: UNFL (underflow)
 * ---- -x-- ---- ----  bit 10: DZ (divide by zero)
 * ---- --x- ---- ----  bit 9: INEX 2 (inexact operation)
 * ---- ---x ---- ----  bit 8: INEX 1 (inexact decimal input)
 *
 * Mode Control Byte
 * ---- ---- xx-- ----  bits 7 and 6: PREC (rounding precision)
 * ---- ---- --xx ----  bits 5 and 4: RND (rounding mode)
 * ---- ---- ---- xxxx  bits 3 to 0: all 0
 */

#define FPCR_PREC   0x00C0
#define FPCR_RND    0x0030

/* Floating Point Status Register (FPSR)
 *
 * Condition Code Byte
 * xxxx ---- ---- ---- ---- ---- ---- ----  bits 31 to 28: all 0
 * ---- x--- ---- ---- ---- ---- ---- ----  bit 27: N (negative)
 * ---- -x-- ---- ---- ---- ---- ---- ----  bit 26: Z (zero)
 * ---- --x- ---- ---- ---- ---- ---- ----  bit 25: I (infinity)
 * ---- ---x ---- ---- ---- ---- ---- ----  bit 24: NAN (not a number or unordered)
 *
 * Quotient Byte (set and reset only by FMOD and FREM)
 * ---- ---- x--- ---- ---- ---- ---- ----  bit 23: sign of quotient
 * ---- ---- -xxx xxxx ---- ---- ---- ----  bits 22 to 16: 7 least significant bits of quotient
 *
 * Exception Status Byte
 * ---- ---- ---- ---- x--- ---- ---- ----  bit 15: BSUN (branch/set on unordered)
 * ---- ---- ---- ---- -x-- ---- ---- ----  bit 14: SNAN (signaling not a number)
 * ---- ---- ---- ---- --x- ---- ---- ----  bit 13: OPERR (operand error)
 * ---- ---- ---- ---- ---x ---- ---- ----  bit 12: OVFL (overflow)
 * ---- ---- ---- ---- ---- x--- ---- ----  bit 11: UNFL (underflow)
 * ---- ---- ---- ---- ---- -x-- ---- ----  bit 10: DZ (divide by zero)
 * ---- ---- ---- ---- ---- --x- ---- ----  bit 9: INEX 2 (inexact operation)
 * ---- ---- ---- ---- ---- ---x ---- ----  bit 8: INEX 1 (inexact decimal input)
 *
 * Accrued Exception Byte
 * ---- ---- ---- ---- ---- ---- x--- ----  bit 7: IOP (invalid operation)
 * ---- ---- ---- ---- ---- ---- -x-- ----  bit 6: OVFL (overflow)
 * ---- ---- ---- ---- ---- ---- --x- ----  bit 5: UNFL (underflow)
 * ---- ---- ---- ---- ---- ---- ---x ----  bit 4: DZ (divide by zero)
 * ---- ---- ---- ---- ---- ---- ---- x---  bit 3: INEX (inexact)
 * ---- ---- ---- ---- ---- ---- ---- -xxx  bits 2 to 0: all 0
 */

#define FPSR_ZEROBITS   0xF0000007

#define FPSR_CC_N       0x08000000
#define FPSR_CC_Z       0x04000000
#define FPSR_CC_I       0x02000000
#define FPSR_CC_NAN     0x01000000

#define FPSR_QUOT_SIGN  0x00800000
#define FPSR_QUOT_LSB   0x007F0000

#define FPSR_BSUN       0x00008000
#define FPSR_SNAN       0x00004000
#define FPSR_OPERR      0x00002000
#define FPSR_OVFL       0x00001000
#define FPSR_UNFL       0x00000800
#define FPSR_DZ         0x00000400
#define FPSR_INEX2      0x00000200
#define FPSR_INEX1      0x00000100

#define FPSR_AE_IOP     0x00000080
#define FPSR_AE_OVFL    0x00000040
#define FPSR_AE_UNFL    0x00000020
#define FPSR_AE_DZ      0x00000010
#define FPSR_AE_INEX    0x00000008

struct {
    // 6888x and 68060
    uae_u32 ccr;
    uae_u32 eo[3];
    // 68060
    uae_u32 v;
    // 68040
    uae_u32 fpiarcu;
    uae_u32 cmdreg3b;
    uae_u32 cmdreg1b;
    uae_u32 stag, dtag;
    uae_u32 e1, e3, t;
    uae_u32 fpt[3];
    uae_u32 et[3];
    uae_u32 wbt[3];
    uae_u32 grs;
    uae_u32 wbte15;
    uae_u32 wbtm66;
} fsave_data;

void reset_fsave_data(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        fsave_data.eo[i] = 0;
        fsave_data.fpt[i] = 0;
        fsave_data.et[i] = 0;
        fsave_data.wbt[i] = 0;
    }
    fsave_data.ccr = 0;
    fsave_data.v = 0;
    fsave_data.fpiarcu = 0;
    fsave_data.cmdreg1b = 0;
    fsave_data.cmdreg3b = 0;
    fsave_data.stag = 0;
    fsave_data.dtag = 0;
    fsave_data.e1 = 0;
    fsave_data.e3 = 0;
    fsave_data.t = 0;
    fsave_data.wbte15 = 0;
    fsave_data.wbtm66 = 0;
    fsave_data.grs = 0;
}

static uae_u32 get_ftag(fptype *src, int size)
{
    if (fp_is_zero(src)) {
        return 1; // ZERO
    } else if (fp_is_unnormal(src) || fp_is_denormal(src)) {
        if (size == 1 || size == 5)
            return 5; // Single/double DENORMAL
        return 4; // Extended DENORMAL or UNNORMAL
    } else if (fp_is_nan(src)) {
        return 3; // NAN
    } else if (fp_is_infinity(src)) {
        return 2; // INF
    }
    return 0; // NORMAL
}

static inline bool fp_is_dyadic(uae_u16 extra)
{
    return ((extra & 0x30) == 0x20 || (extra & 0x7f) == 0x38);
}

bool fp_exception_pending(bool pre)
{
    // first check for pending arithmetic exceptions
#if ARITHMETIC_EXCEPTIONS
    if (regs.fp_exp_pend) {
        if (warned > 0) {
            write_log (_T("FPU arithmetic exception (%d) (%s)\n"), regs.fp_exp_pend, pre ? "pre" : "mid/post");
        }
        regs.fpu_exp_pre = pre;
        Exception(regs.fp_exp_pend);
        if (currprefs.fpu_model != 68882)
            regs.fp_exp_pend = 0;
        
        return true;
    }
#endif
    // no arithmetic exceptions pending, check for unimplemented datatype
    if (regs.fp_unimp_pend) {
        if (warned > 0) {
            write_log (_T("FPU unimplemented datatype exception (%s)\n"), pre ? "pre" : "mid/post");
        }
        regs.fpu_exp_pre = pre;
        Exception(55);
        regs.fp_unimp_pend = 0;

        return true;
    }
    
    return false;
}
void fp_unimp_instruction_exception_pending(void)
{
    if (regs.fp_unimp_ins) {
        if (warned > 0) {
            write_log (_T("FPU unimplemented instruction exception\n"));
        }
        regs.fpu_exp_pre = true;
        Exception(11);
        regs.fp_unimp_ins = false;
        regs.fp_unimp_pend = 0;
    }
}

void fpsr_set_exception(uae_u32 exception)
{
    regs.fpsr |= exception;
}
uae_u32 fpsr_get_vector(uae_u32 exception)
{
    static const int vtable[8] = { 49, 49, 50, 51, 53, 52, 54, 48 };
    int i;
    exception >>= 8;
    for (i = 7; i >= 0; i--) {
        if (exception & (1 << i)) {
            return vtable[i];
        }
    }
    return 0;
}
void fpsr_check_arithmetic_exception(uae_u32 mask, fptype *src, uae_u32 opcode, uae_u16 extra, uae_u32 ea)
{
#if ARITHMETIC_EXCEPTIONS
    bool nonmaskable;
    uae_u32 exception;
    // Any exception status bit and matching exception enable bits set?
    exception = regs.fpsr & regs.fpcr & 0xff00;
    // Add 68040/68060 nonmaskable exceptions
    if (currprefs.cpu_model >= 68040 && currprefs.fpu_model)
        exception |= regs.fpsr & (FPSR_OVFL | FPSR_UNFL | mask);

    if (exception) {
        regs.fp_exp_pend = fpsr_get_vector(exception);
        nonmaskable = (regs.fp_exp_pend != fpsr_get_vector(regs.fpsr & regs.fpcr));
        if (warned > 0) {
            write_log (_T("FPU %s arithmetic exception: FPSR: %08x, FPCR: %04x (vector: %d)!\n"),
                       nonmaskable?"nonmaskable":"", regs.fpsr, regs.fpcr, regs.fp_exp_pend);
#if EXCEPTION_FPP == 0
            warned--;
#endif
        }
        
        regs.fp_opword = opcode;
        regs.fp_ea = ea;
        
        // data for FSAVE stack frame
        fptype eo;
        uae_u32 opclass = (extra >> 13) & 7;
        
        reset_fsave_data();

        if (currprefs.fpu_model == 68881 || currprefs.fpu_model == 68882) {
            // fsave data for 68881 and 68882
            regs.fpu_exp_state = 1; // 6888x IDLE frame

            if (opclass == 3) { // 011
                fsave_data.ccr = ((uae_u32)extra << 16) | extra;
            } else { // 000 or 010
                fsave_data.ccr = ((uae_u32)(opcode | 0x0080) << 16) | extra;
            }
            if (regs.fp_exp_pend == 54 || regs.fp_exp_pend == 52 || regs.fp_exp_pend == 50) { // SNAN, OPERR, DZ
                from_exten_fmovem(src, &fsave_data.eo[0], &fsave_data.eo[1], &fsave_data.eo[2]);
                if (regs.fp_exp_pend == 52 && opclass == 3) { // OPERR from move to integer or packed
                    fsave_data.eo[0] &= 0x4fff0000;
                    fsave_data.eo[1] = fsave_data.eo[2] = 0;
                }
            } else if (regs.fp_exp_pend == 53) { // OVFL
                eo = fp_get_internal_overflow();
                from_exten_fmovem(&eo, &fsave_data.eo[0], &fsave_data.eo[1], &fsave_data.eo[2]);
            } else if (regs.fp_exp_pend == 51) { // UNFL
                eo = fp_get_internal_underflow();
                from_exten_fmovem(&eo, &fsave_data.eo[0], &fsave_data.eo[1], &fsave_data.eo[2]);
            } // else INEX1, INEX2: do nothing
        } else if (currprefs.cpu_model == 68060) {
            // fsave data for 68060
            regs.fpu_exp_state = 2; // 68060 EXCP frame
            fsave_data.v = regs.fp_exp_pend & 7;
            from_exten_fmovem(src, &fsave_data.eo[0], &fsave_data.eo[1], &fsave_data.eo[2]);
        } else {
            // fsave data for 68040
            regs.fpu_exp_state = 1; // 68040 UNIMP frame

            uae_u32 reg = (extra >> 7) & 7;
            uae_u32 size = (extra >> 10) & 7;

            fsave_data.fpiarcu = regs.fpiar;
            
            if (regs.fp_exp_pend == 54) { // SNAN (undocumented)
                fsave_data.wbte15 = 1;
                fsave_data.grs = 7;
            } else {
                fsave_data.grs = 1;
            }

            if (opclass == 3) { // OPCLASS 011
                fsave_data.cmdreg1b = extra;
                fsave_data.e1 = 1;
                fsave_data.t = 1;
                fsave_data.wbte15 = (regs.fp_exp_pend == 51 || regs.fp_exp_pend == 54) ? 1 : 0; // UNFL, SNAN
                
                if (fp_is_snan(src)) {
                    fp_unset_snan(src);
                }
                from_exten_fmovem(src, &fsave_data.et[0], &fsave_data.et[1], &fsave_data.et[2]);
                fsave_data.stag = get_ftag(src, -1);
            } else { // OPCLASS 000 and 010
                fsave_data.cmdreg1b = extra;
                fsave_data.e1 = 1;
                fsave_data.wbte15 = (regs.fp_exp_pend == 54) ? 1 : 0; // SNAN (undocumented)

                if (regs.fp_exp_pend == 51 || regs.fp_exp_pend == 53 || regs.fp_exp_pend == 49) { // UNFL, OVFL, INEX
                    if ((extra & 0x30) == 0x20 || (extra & 0x3f) == 0x04) { // FADD, FSUB, FMUL, FDIV, FSQRT
                        regs.fpu_exp_state = 2; // 68040 BUSY frame
                        fsave_data.e3 = 1;
                        fsave_data.e1 = 0;
                        fsave_data.cmdreg3b = (extra & 0x3C3) | ((extra & 0x038)>>1) | ((extra & 0x004)<<3);
                        if (regs.fp_exp_pend == 51) { // UNFL
                            eo = fp_get_internal();
                        } else { // OVFL, INEX
                            eo = fp_get_internal_round();
                        }
                        fsave_data.grs = fp_get_internal_grs();
                        from_exten_fmovem(&eo, &fsave_data.wbt[0], &fsave_data.wbt[1], &fsave_data.wbt[2]);
                        fsave_data.wbte15 = (regs.fp_exp_pend == 51) ? 1 : 0; // UNFL
                        // src and dst is stored (undocumented)
                        from_exten_fmovem(src, &fsave_data.et[0], &fsave_data.et[1], &fsave_data.et[2]);
                        fsave_data.stag = get_ftag(src, (opclass == 0) ? -1 : size);
                        if (fp_is_dyadic(extra)) {
                            from_exten_fmovem(&regs.fp[reg], &fsave_data.fpt[0], &fsave_data.fpt[1], &fsave_data.fpt[2]);
                            fsave_data.dtag = get_ftag(&regs.fp[reg], -1);
                        }
                    } else { // FMOVE to register, FABS, FNEG
                        eo = fp_get_internal_round_exten();
                        fsave_data.grs = fp_get_internal_grs();
                        from_exten_fmovem(&eo, &fsave_data.fpt[0], &fsave_data.fpt[1], &fsave_data.fpt[2]);
                        eo = fp_get_internal_round_all(); // weird
                        from_exten_fmovem(&eo, &fsave_data.et[0], &fsave_data.et[1], &fsave_data.et[2]); // undocumented
                        fsave_data.stag = get_ftag(src, (opclass == 0) ? -1 : size);
                    }
                } else { // SNAN, OPERR, DZ
                    from_exten_fmovem(src, &fsave_data.et[0], &fsave_data.et[1], &fsave_data.et[2]);
                    fsave_data.stag = get_ftag(src, (opclass == 0) ? -1 : size);
                    if (fp_is_dyadic(extra)) {
                        from_exten_fmovem(&regs.fp[reg], &fsave_data.fpt[0], &fsave_data.fpt[1], &fsave_data.fpt[2]);
                        fsave_data.dtag = get_ftag(&regs.fp[reg], -1);
                    }
                }
            }
        }
    }
#endif
}
void fpsr_set_result(fptype *result)
{
    // condition code byte
    regs.fpsr &= 0x00fffff8; // clear cc
    if (fp_is_nan (result)) {
        regs.fpsr |= FPSR_CC_NAN;
    } else if (fp_is_zero(result)) {
        regs.fpsr |= FPSR_CC_Z;
    } else if (fp_is_infinity (result)) {
        regs.fpsr |= FPSR_CC_I;
    }
    if (fp_is_neg(result)) {
        regs.fpsr |= FPSR_CC_N;
    }
}
void fpsr_clear_status(void)
{
    // clear exception status byte only
    regs.fpsr &= 0x0fff00f8;
    
    // clear external status
    clear_fp_status();
}
uae_u32 fpsr_make_status(void)
{
    uae_u32 exception;
    
    // get external status
    get_fp_status(&regs.fpsr);
    
    // update accrued exception byte
    if (regs.fpsr & (FPSR_BSUN | FPSR_SNAN | FPSR_OPERR))
        regs.fpsr |= FPSR_AE_IOP;  // IOP = BSUN || SNAN || OPERR
    if (regs.fpsr & FPSR_OVFL)
        regs.fpsr |= FPSR_AE_OVFL; // OVFL = OVFL
    if ((regs.fpsr & FPSR_UNFL) && (regs.fpsr & FPSR_INEX2))
        regs.fpsr |= FPSR_AE_UNFL; // UNFL = UNFL && INEX2
    if (regs.fpsr & FPSR_DZ)
        regs.fpsr |= FPSR_AE_DZ;   // DZ = DZ
    if (regs.fpsr & (FPSR_OVFL | FPSR_INEX2 | FPSR_INEX1))
        regs.fpsr |= FPSR_AE_INEX; // INEX = INEX1 || INEX2 || OVFL
    
    // return exceptions that interrupt calculation
    exception = regs.fpsr & regs.fpcr & (FPSR_SNAN | FPSR_OPERR | FPSR_DZ);
    if (currprefs.cpu_model >= 68040 && currprefs.fpu_model)
        exception |= regs.fpsr & (FPSR_OVFL | FPSR_UNFL);
#if ARITHMETIC_EXCEPTIONS
    return exception;
#else
    return 0;
#endif
}
int fpsr_set_bsun(void)
{
    regs.fpsr |= FPSR_BSUN;
    regs.fpsr |= FPSR_AE_IOP;
    
    if (regs.fpcr & FPSR_BSUN) {
        // logging only so far
        write_log (_T("FPU exception: BSUN! (FPSR: %08x, FPCR: %04x)\n"), regs.fpsr, regs.fpcr);
#if ARITHMETIC_EXCEPTIONS
        regs.fp_exp_pend = fpsr_get_vector(FPSR_BSUN);
        fp_exception_pending(true);
        return 1;
#endif
    }
    return 0;
}
void fpsr_set_quotient(uae_u64 quot, uae_s8 sign)
{
    regs.fpsr &= 0x0f00fff8;
    regs.fpsr |= (quot << 16) & FPSR_QUOT_LSB;
    regs.fpsr |= sign ? FPSR_QUOT_SIGN : 0;
}
void fpsr_get_quotient(uae_u64 *quot, uae_s8 *sign)
{
    *quot = (regs.fpsr & FPSR_QUOT_LSB) >> 16;
    *sign = (regs.fpsr & FPSR_QUOT_SIGN) ? 1 : 0;
}


uae_u32 fpp_get_fpsr (void)
{
    return regs.fpsr;
}

void fpp_set_fpsr (uae_u32 val)
{
    regs.fpsr = val;
}

void fpp_set_fpcr (uae_u32 val)
{
    set_fp_mode(val);
    regs.fpcr = val & 0xffff;
}


static void fpnan (fptype *fp)
{
    to_exten(fp, xhex_nan[0], xhex_nan[1], xhex_nan[2]);
}
static void fpset (fptype *fp, uae_s32 val)
{
    *fp = from_int(val);
}


bool fpu_get_constant(fptype *fp, int cr)
{
    uae_u32 f[3] = { 0, 0, 0 };
    int entry = 0;
    int mode = (regs.fpcr >> 4) & 3;
    int prec = (regs.fpcr >> 6) & 3;
    
    switch (cr)
    {
        case 0x00: // pi
            entry = FPP_CR_PI;
            break;
        case 0x0b: // log10(2)
            entry = FPP_CR_LOG10_2;
            break;
        case 0x0c: // e
            entry = FPP_CR_E;
            break;
        case 0x0d: // log2(e)
            entry = FPP_CR_LOG2_E;
            break;
        case 0x0e: // log10(e)
            entry = FPP_CR_LOG10_E;
            break;
        case 0x0f: // 0.0
            entry = FPP_CR_ZERO;
            break;
        case 0x30: // ln(2)
            entry = FPP_CR_LN_2;
            break;
        case 0x31: // ln(10)
            entry = FPP_CR_LN_10;
            break;
        case 0x32: // 1e0
            entry = FPP_CR_1E0;
            break;
        case 0x33: // 1e1
            entry = FPP_CR_1E1;
            break;
        case 0x34: // 1e2
            entry = FPP_CR_1E2;
            break;
        case 0x35: // 1e4
            entry = FPP_CR_1E4;
            break;
        case 0x36: // 1e8
            entry = FPP_CR_1E8;
            break;
        case 0x37: // 1e16
            entry = FPP_CR_1E16;
            break;
        case 0x38: // 1e32
            entry = FPP_CR_1E32;
            break;
        case 0x39: // 1e64
            entry = FPP_CR_1E64;
            break;
        case 0x3a: // 1e128
            entry = FPP_CR_1E128;
            break;
        case 0x3b: // 1e256
            entry = FPP_CR_1E256;
            break;
        case 0x3c: // 1e512
            entry = FPP_CR_1E512;
            break;
        case 0x3d: // 1e1024
            entry = FPP_CR_1E1024;
            break;
        case 0x3e: // 1e2048
            entry = FPP_CR_1E2048;
            break;
        case 0x3f: // 1e4096
            entry = FPP_CR_1E4096;
            break;
        default: // undefined
        {
            bool check_f1_adjust = false;
            int f1_adjust = 0;
            uae_u32 sr = 0;

            if (cr > FPP_CR_NUM_SPECIAL_UNDEFINED) {
                cr = 0; // Most undefined fields contain this
            }
            f[0] = fpp_cr_undef[cr].val[0];
            f[1] = fpp_cr_undef[cr].val[1];
            f[2] = fpp_cr_undef[cr].val[2];
            // Rounding mode and precision works very strangely here..
            switch (cr)
            {
                case 1:
                    check_f1_adjust = true;
                    break;
                case 2:
                    if (prec == 1 && mode == 3)
                        f1_adjust = -1;
                    break;
                case 3:
                    if (prec == 1 && (mode == 0 || mode == 3))
                        sr = FPSR_CC_I;
                    else
                        sr = FPSR_CC_NAN;
                    break;
                case 7:
                    sr = FPSR_CC_NAN;
                    check_f1_adjust = true;
                    break;
            }
            if (check_f1_adjust) {
                if (prec == 1) {
                    if (mode == 0) {
                        f1_adjust = -1;
                    } else if (mode == 1 || mode == 2) {
                        f1_adjust = 1;
                    }
                }
            }
            
            to_exten_fmovem(fp, f[0], f[1], f[2]);
            
            if (prec == 1) fp_round32(fp);
            if (prec >= 2) fp_round64(fp);
            
            if (f1_adjust) {
                from_exten_fmovem(fp, &f[0], &f[1], &f[2]);
                f[1] += f1_adjust * 0x80;
                to_exten_fmovem(fp, f[0], f[1], f[2]);
            }
            
            fpsr_set_result(fp);
            regs.fpsr |= sr;
        }
            return false;
    }
    
    f[0] = fpp_cr[entry].val[0];
    f[1] = fpp_cr[entry].val[1];
    f[2] = fpp_cr[entry].val[2];
    // if constant is inexact, set inexact bit and round
    // note: with valid constants, LSB never wraps
    if (fpp_cr[entry].inexact) {
        fpsr_set_exception(FPSR_INEX2);
        f[2] += fpp_cr[entry].rndoff[mode];
    }
    
    to_exten_fmovem(fp, f[0], f[1], f[2]);
    
    if (prec == 1) fp_round32(fp);
    if (prec >= 2) fp_round64(fp);
    
    fpsr_set_result(fp);

    return true;
}


static void fp_unimp_instruction(uae_u16 opcode, uae_u16 extra, uae_u32 ea, uaecptr oldpc, fptype *src, int reg, int size)
{
    if ((extra & 0x7f) == 4) // FSQRT 4->5
        extra |= 1;
    
    // data for fsave stack frame
    regs.fpu_exp_state = 1; // 68060 IDLE frame, 68040 UNIMP frame
    
    if (currprefs.cpu_model == 68060) {
        // fsave data for 68060
        reset_fsave_data();
    } else if (currprefs.cpu_model == 68040) {
        // fsave data for 68040
        fsave_data.fpiarcu = regs.fpiar;

        if (regs.fp_unimp_pend == 0) { // else data has been saved by fp_unimp_datatype
            reset_fsave_data();
            fsave_data.cmdreg3b = (extra & 0x3C3) | ((extra & 0x038) >> 1) | ((extra & 0x004) << 3);
            fsave_data.cmdreg1b = extra;
            from_exten_fmovem(src, &fsave_data.et[0], &fsave_data.et[1], &fsave_data.et[2]);
            fsave_data.stag = get_ftag(src, size);
            if (reg >= 0) {
                from_exten_fmovem(&regs.fp[reg], &fsave_data.fpt[0], &fsave_data.fpt[1], &fsave_data.fpt[2]);
                fsave_data.dtag = get_ftag(&regs.fp[reg], -1);
            }
        }
    }
    if (warned > 0) {
        write_log (_T("FPU unimplemented instruction: OP=%04X-%04X SRC=%08X-%08X-%08X EA=%08X PC=%08X\n"),
                   opcode, extra, fsave_data.et[0],fsave_data.et[1],fsave_data.et[2], ea, oldpc);
#if EXCEPTION_FPP == 0
        warned--;
#endif
    }
    
    regs.fp_ea = ea;
    regs.fp_opword = opcode;
    regs.fp_unimp_ins = true;
    fp_unimp_instruction_exception_pending();

    regs.fp_exception = true;
}

static void fp_unimp_datatype(uae_u16 opcode, uae_u16 extra, uae_u32 ea, uaecptr oldpc, fptype *src, uae_u32 *packed)
{
    uae_u32 reg = (extra >> 7) & 7;
    uae_u32 size = (extra >> 10) & 7;
    uae_u32 opclass = (extra >> 13) & 7;
    
    regs.fp_ea = ea;
    regs.fp_opword = opcode;
    regs.fp_unimp_pend = packed ? 2 : 1;

    if ((extra & 0x7f) == 4) // FSQRT 4->5
        extra |= 1;
    
    // data for fsave stack frame
    reset_fsave_data();
    regs.fpu_exp_state = 2; // 68060 EXCP frame, 68040 BUSY frame
    
    if (currprefs.cpu_model == 68060) {
        // fsave data for 68060
        if (packed) {
            regs.fpu_exp_state = 1; // 68060 IDLE frame
        } else {
            fsave_data.v = 7; // vector & 0x7
            from_exten_fmovem(src, &fsave_data.eo[0], &fsave_data.eo[1], &fsave_data.eo[2]);
        }
    } else if (currprefs.cpu_model == 68040) {
        // fsave data for 68040
        fsave_data.cmdreg1b = extra;
        fsave_data.fpiarcu = regs.fpiar;
        if (packed) {
            fsave_data.e1 = 1; // used to distinguish packed operands
        }
        if (opclass == 3) { // OPCLASS 011
            fsave_data.t = 1;
            from_exten_fmovem(src, &fsave_data.et[0], &fsave_data.et[1], &fsave_data.et[2]);
            fsave_data.stag = get_ftag(src, -1);
            from_exten_fmovem(src, &fsave_data.fpt[0], &fsave_data.fpt[1], &fsave_data.fpt[2]); // undocumented
            fsave_data.dtag = get_ftag(src, -1);
        } else { // OPCLASS 000 and 010
            if (packed) {
                fsave_data.fpt[2] = packed[0]; // yes, this is correct.
                fsave_data.fpt[1] = packed[1]; // undocumented
                fsave_data.et[1] = packed[1];
                fsave_data.et[2] = packed[2];
                fsave_data.stag = 7; // undocumented
            } else {
                from_exten_fmovem(src, &fsave_data.et[0], &fsave_data.et[1], &fsave_data.et[2]);
                fsave_data.stag = get_ftag(src, (opclass == 0) ? -1 : size);
                if (fsave_data.stag == 5) {
                    fsave_data.et[0] = (size == 1) ? 0x3f800000 : 0x3c000000; // exponent for denormalized single and double
                }
                if (fp_is_dyadic(extra)) {
                    from_exten_fmovem(&regs.fp[reg], &fsave_data.fpt[0], &fsave_data.fpt[1], &fsave_data.fpt[2]);
                    fsave_data.dtag = get_ftag(&regs.fp[reg], -1);
                }
            }
        }
    }
    if (warned > 0) {
        write_log (_T("FPU unimplemented datatype (%s): OP=%04X-%04X SRC=%08X-%08X-%08X EA=%08X PC=%08X\n"),
                   packed ? "packed" : "denormal", opcode, extra,
                   packed ? fsave_data.fpt[2] : fsave_data.et[0], fsave_data.et[1], fsave_data.et[2], ea, oldpc);
#if EXCEPTION_FPP == 0
        warned--;
#endif
    }
    regs.fp_exception = true;
}

static void fpu_op_illg (uae_u16 opcode, uae_u32 ea, uaecptr oldpc)
{
    if ((currprefs.cpu_model == 68060 && (currprefs.fpu_model == 0 || (regs.pcr & 2)))
        || (currprefs.cpu_model == 68040 && currprefs.fpu_model == 0)) {
        regs.fp_unimp_ins = true;
        regs.fp_ea = ea;
        fp_unimp_instruction_exception_pending();
        return;
    }
    regs.fp_exception = true;
    m68k_setpc (oldpc);
    op_illg (opcode);
}

static void fpu_noinst (uae_u16 opcode, uaecptr pc)
{
#if EXCEPTION_FPP
    write_log (_T("Unknown FPU instruction %04X %08X\n"), opcode, pc);
#endif
    regs.fp_exception = true;
    m68k_setpc (pc);
    op_illg (opcode);
}

static bool if_no_fpu(void)
{
    return (regs.pcr & 2) || currprefs.fpu_model <= 0;
}

static bool fault_if_no_fpu (uae_u16 opcode, uae_u16 extra, uaecptr ea, uaecptr oldpc)
{
    if (if_no_fpu()) {
#if EXCEPTION_FPP
        write_log (_T("no FPU: %04X-%04X PC=%08X\n"), opcode, extra, oldpc);
#endif
        if (fpu_mmu_fixup) {
            m68k_areg (regs, mmufixup[0].reg) = mmufixup[0].value;
            mmufixup[0].reg = -1;
        }
        fpu_op_illg (opcode, ea, oldpc);
        return true;
    }
    return false;
}

static bool fault_if_unimplemented_680x0 (uae_u16 opcode, uae_u16 extra, uaecptr ea, uaecptr oldpc, fptype *src, int reg)
{
    if (fault_if_no_fpu (opcode, extra, ea, oldpc))
        return true;
    if (currprefs.cpu_model >= 68040 && currprefs.fpu_model) {
        if ((extra & (0x8000 | 0x2000)) != 0)
            return false;
        if ((extra & 0xfc00) == 0x5c00) {
            // FMOVECR
            fp_unimp_instruction(opcode, extra, ea, oldpc, src, reg, -1);
            return true;
        }
        uae_u16 v = extra & 0x7f;
        switch (v)
        {
            case 0x00: /* FMOVE */
            case 0x40: /* FSMOVE */
            case 0x44: /* FDMOVE */
            case 0x04: /* FSQRT */
            case 0x41: /* FSSQRT */
            case 0x45: /* FDSQRT */
            case 0x18: /* FABS */
            case 0x58: /* FSABS */
            case 0x5c: /* FDABS */
            case 0x1a: /* FNEG */
            case 0x5a: /* FSNEG */
            case 0x5e: /* FDNEG */
            case 0x20: /* FDIV */
            case 0x60: /* FSDIV */
            case 0x64: /* FDDIV */
            case 0x22: /* FADD */
            case 0x62: /* FSADD */
            case 0x66: /* FDADD */
            case 0x23: /* FMUL */
            case 0x63: /* FSMUL */
            case 0x67: /* FDMUL */
            case 0x24: /* FSGLDIV */
            case 0x27: /* FSGLMUL */
            case 0x28: /* FSUB */
            case 0x68: /* FSSUB */
            case 0x6c: /* FDSUB */
            case 0x38: /* FCMP */
            case 0x3a: /* FTST */
                return false;
            case 0x01: /* FINT */
            case 0x03: /* FINTRZ */
                // Unimplemented only in 68040.
                if (currprefs.cpu_model != 68040) {
                    return false;
                }
            default:
                fp_unimp_instruction(opcode, extra, ea, oldpc, src, reg, -1);
                return true;
        }
    }
    return false;
}

static bool fault_if_unimplemented_6888x (uae_u16 opcode, uae_u16 extra, uaecptr oldpc)
{
    if ((currprefs.fpu_model == 68881 || currprefs.fpu_model == 68882)) {
        uae_u16 v = extra & 0x7f;
        /* 68040/68060 only variants. 6888x = F-line exception. */
        switch (v)
        {
            case 0x00: /* FMOVE */
            case 0x01: /* FINT */
            case 0x02: /* FSINH */
            case 0x03: /* FINTRZ */
            case 0x04: /* FSQRT */
            case 0x06: /* FLOGNP1 */
            case 0x08: /* FETOXM1 */
            case 0x09: /* FTANH */
            case 0x0a: /* FATAN */
            case 0x0c: /* FASIN */
            case 0x0d: /* FATANH */
            case 0x0e: /* FSIN */
            case 0x0f: /* FTAN */
            case 0x10: /* FETOX */
            case 0x11: /* FTWOTOX */
            case 0x12: /* FTENTOX */
            case 0x14: /* FLOGN */
            case 0x15: /* FLOG10 */
            case 0x16: /* FLOG2 */
            case 0x18: /* FABS */
            case 0x19: /* FCOSH */
            case 0x1a: /* FNEG */
            case 0x1c: /* FACOS */
            case 0x1d: /* FCOS */
            case 0x1e: /* FGETEXP */
            case 0x1f: /* FGETMAN */
            case 0x20: /* FDIV */
            case 0x21: /* FMOD */
            case 0x22: /* FADD */
            case 0x23: /* FMUL */
            case 0x24: /* FSGLDIV */
            case 0x25: /* FREM */
            case 0x26: /* FSCALE */
            case 0x27: /* FSGLMUL */
            case 0x28: /* FSUB */
            case 0x30: /* FSINCOS */
            case 0x31:
            case 0x32:
            case 0x33:
            case 0x34:
            case 0x35:
            case 0x36:
            case 0x37:
            case 0x38: /* FCMP */
            case 0x3a: /* FTST */
                return false;
            default:
                fpu_noinst (opcode, oldpc);
                return true;
        }
    }
    return false;
}

static bool fault_if_60 (void)
{
    if (currprefs.cpu_model == 68060 && currprefs.fpu_model) {
        Exception(60);
        return true;
    }
    return false;
}

static bool fault_if_no_fpu_u (uae_u16 opcode, uae_u16 extra, uaecptr ea, uaecptr oldpc)
{
    if (fault_if_no_fpu (opcode, extra, ea, oldpc))
        return true;
    if (currprefs.cpu_model == 68060 && currprefs.fpu_model) {
        // 68060 FTRAP, FDBcc or FScc are not implemented.
        regs.fp_unimp_ins = true;
        regs.fp_ea = ea;
        fp_unimp_instruction_exception_pending();
        return true;
    }
    return false;
}

static bool fault_if_no_6888x (uae_u16 opcode, uae_u16 extra, uaecptr oldpc)
{
    if (currprefs.cpu_model < 68040 && currprefs.fpu_model <= 0) {
#if EXCEPTION_FPP
        write_log (_T("6888x no FPU: %04X-%04X PC=%08X\n"), opcode, extra, oldpc);
#endif
        m68k_setpc (oldpc);
        regs.fp_exception = true;
        op_illg (opcode);
        return true;
    }
    return false;
}


static int get_fpu_version (void)
{
    int v = 0;
    
    switch (currprefs.fpu_model)
    {
        case 68881:
        case 68882:
            v = 0x1f;
            break;
        case 68040:
            if (currprefs.fpu_revision == 0x40)
                v = 0x40;
            else
                v = 0x41;
            break;
    }
    return v;
}

static void fpu_null (void)
{
    int i;
    regs.fpu_state = 0;
    regs.fpu_exp_state = 0;
    regs.fpcr = 0;
    regs.fpsr = 0;
    regs.fpiar = 0;
    for (i = 0; i < 8; i++)
        fpnan (&regs.fp[i]);
}


/* single   : S  8*E 23*F */
/* double   : S 11*E 52*F */
/* extended : S 15*E 64*F */
/* E = 0 & F = 0 -> 0 */
/* E = MAX & F = 0 -> Infin */
/* E = MAX & F # 0 -> NotANumber */
/* E = biased by 127 (single) ,1023 (double) ,16383 (extended) */

// 68040/060 does not support denormals
static bool normalize_or_fault_if_no_denormal_support(uae_u16 opcode, uae_u16 extra, uaecptr ea, uaecptr oldpc, fptype *src)
{
    if (fp_is_unnormal(src) || fp_is_denormal(src)) {
        if (currprefs.cpu_model >= 68040 && currprefs.fpu_model) {
            if (fp_is_zero(src)) {
                fp_normalize(src); // 68040/060 can only fix unnormal zeros
            } else {
                fp_unimp_datatype(opcode, extra, ea, oldpc, src, NULL);
                return true;
            }
        } else {
            fp_normalize(src);
        }
    }
    return false;
}
static bool normalize_or_fault_if_no_denormal_support_dst(uae_u16 opcode, uae_u16 extra, uaecptr ea, uaecptr oldpc, fptype *dst, fptype *src)
{
    if (fp_is_unnormal(dst) || fp_is_denormal(dst)) {
        if (currprefs.cpu_model >= 68040 && currprefs.fpu_model) {
            if (fp_is_zero(dst)) {
                fp_normalize(dst); // 68040/060 can only fix unnormal zeros
            } else {
                fp_unimp_datatype(opcode, extra, ea, oldpc, src, NULL);
                return true;
            }
        } else {
            fp_normalize(dst);
        }
    }
    return false;
}
// 68040/060 does not support packed decimal format
static bool fault_if_no_packed_support(uae_u16 opcode, uae_u16 extra, uaecptr ea, uaecptr oldpc, fptype *src, uae_u32 *packed)
{
    if (currprefs.cpu_model >= 68040 && currprefs.fpu_model) {
        fp_unimp_datatype(opcode, extra, ea, oldpc, src, packed);
        return true;
    }
    return false;
}
// 68040 does not support move to integer format
static bool fault_if_68040_integer_nonmaskable(uae_u16 opcode, uae_u16 extra, uaecptr ea, uaecptr oldpc, fptype *src)
{
    if (currprefs.cpu_model == 68040 && currprefs.fpu_model) {
        fpsr_make_status();
        if (regs.fpsr & (FPSR_SNAN | FPSR_OPERR)) {
            fpsr_check_arithmetic_exception(FPSR_SNAN | FPSR_OPERR, src, opcode, extra, ea);
            fp_exception_pending(false); // post
#if ARITHMETIC_EXCEPTIONS
            return true;
#endif
        }
    }
    return false;
}

static int get_fp_value (uae_u32 opcode, uae_u16 extra, fptype *src, uaecptr oldpc, uae_u32 *adp)
{
    int size, mode, reg;
    uae_u32 ad = 0;
    static const int sz1[8] = { 4, 4, 12, 12, 2, 8, 1, 0 };
    static const int sz2[8] = { 4, 4, 12, 12, 2, 8, 2, 0 };
    uae_u32 exts[3];
    int doext = 0;
    
    if (!(extra & 0x4000)) {
        if (fault_if_no_fpu (opcode, extra, 0, oldpc))
            return -1;
        *src = regs.fp[(extra >> 10) & 7];
        normalize_or_fault_if_no_denormal_support(opcode, extra, 0, oldpc, src);
        return 1;
    }
    mode = (opcode >> 3) & 7;
    reg = opcode & 7;
    size = (extra >> 10) & 7;
    
    switch (mode) {
        case 0:
            if ((size == 0 || size == 1 ||size == 4 || size == 6) && fault_if_no_fpu (opcode, extra, 0, oldpc))
                return -1;
            
            switch (size)
        {
            case 6:
                fpset(src, (uae_s8) m68k_dreg (regs, reg));
                break;
            case 4:
                fpset(src, (uae_s16) m68k_dreg (regs, reg));
                break;
            case 0:
                fpset(src, (uae_s32) m68k_dreg (regs, reg));
                break;
            case 1:
                to_single (src, m68k_dreg (regs, reg));
                normalize_or_fault_if_no_denormal_support(opcode, extra, 0, oldpc, src);
                break;
            default:
                return 0;
        }
            return 1;
        case 1:
            return 0;
        case 2:
            ad = m68k_areg (regs, reg);
            break;
        case 3:
            // Also needed by fault_if_no_fpu
            mmufixup[0].reg = reg;
            mmufixup[0].value = m68k_areg (regs, reg);
            fpu_mmu_fixup = true;
            ad = m68k_areg (regs, reg);
            m68k_areg (regs, reg) += reg == 7 ? sz2[size] : sz1[size];
            break;
        case 4:
            // Also needed by fault_if_no_fpu
            mmufixup[0].reg = reg;
            mmufixup[0].value = m68k_areg (regs, reg);
            fpu_mmu_fixup = true;
            m68k_areg (regs, reg) -= reg == 7 ? sz2[size] : sz1[size];
            ad = m68k_areg (regs, reg);
            // 68060 no fpu -(an): EA points to -4, not -12 if extended precision
            if (currprefs.cpu_model == 68060 && if_no_fpu() && sz1[size] == 12)
                ad += 8;
            break;
        case 5:
            ad = m68k_areg (regs, reg) + (uae_s32) (uae_s16) x_cp_next_iword ();
            break;
        case 6:
            ad = x_cp_get_disp_ea_020 (m68k_areg (regs, reg), 0);
            break;
        case 7:
            switch (reg)
        {
            case 0: // (xxx).W
                ad = (uae_s32) (uae_s16) x_cp_next_iword ();
                break;
            case 1: // (xxx).L
                ad = x_cp_next_ilong ();
                break;
            case 2: // (d16,PC)
                ad = m68k_getpc ();
                ad += (uae_s32) (uae_s16) x_cp_next_iword ();
                break;
            case 3: // (d8,PC,Xn)+
                ad = x_cp_get_disp_ea_020 (m68k_getpc (), 0);
                break;
            case 4: // #imm
                doext = 1;
                switch (size)
            {
                case 0: // L
                case 1: // S
                    exts[0] = x_cp_next_ilong ();
                    break;
                case 2: // X
                case 3: // P
                    // 68060 and immediate X or P: unimplemented effective address
                    if (fault_if_60())
                        return -1;
                    exts[0] = x_cp_next_ilong ();
                    exts[1] = x_cp_next_ilong ();
                    exts[2] = x_cp_next_ilong ();
                    break;
                case 4: // W
                    exts[0] = x_cp_next_iword ();
                    break;
                case 5: // D
                    exts[0] = x_cp_next_ilong ();
                    exts[1] = x_cp_next_ilong ();
                    break;
                case 6: // B
                    exts[0] = x_cp_next_iword ();
                    break;
            }
                break;
            default:
                return 0;
        }
    }
    
    if (fault_if_no_fpu (opcode, extra, ad, oldpc))
        return -1;
    
    *adp = ad;
    uae_u32 adold = ad;
    
    if (currprefs.fpu_model == 68060) {
        // Skip if 68040 because FSAVE frame can store both src and dst
        if (fault_if_unimplemented_680x0(opcode, extra, ad, oldpc, src, -1)) {
            return -1;
        }
    }
    
    switch (size)
    {
        case 0:
            fpset(src, (uae_s32) (doext ? exts[0] : x_cp_get_long (ad)));
            break;
        case 1:
            to_single (src, (doext ? exts[0] : x_cp_get_long (ad)));
            normalize_or_fault_if_no_denormal_support(opcode, extra, adold, oldpc, src);
            break;
        case 2:
        {
            uae_u32 wrd1, wrd2, wrd3;
            wrd1 = (doext ? exts[0] : x_cp_get_long (ad));
            ad += 4;
            wrd2 = (doext ? exts[1] : x_cp_get_long (ad));
            ad += 4;
            wrd3 = (doext ? exts[2] : x_cp_get_long (ad));
            to_exten (src, wrd1, wrd2, wrd3);
            normalize_or_fault_if_no_denormal_support(opcode, extra, adold, oldpc, src);
        }
            break;
        case 3:
        {
            uae_u32 wrd[3];
            if (currprefs.cpu_model == 68060) {
                if (fault_if_no_packed_support (opcode, extra, adold, oldpc, NULL, wrd))
                    return 1;
            }
            wrd[0] = (doext ? exts[0] : x_cp_get_long (ad));
            ad += 4;
            wrd[1] = (doext ? exts[1] : x_cp_get_long (ad));
            ad += 4;
            wrd[2] = (doext ? exts[2] : x_cp_get_long (ad));
            if (fault_if_no_packed_support (opcode, extra, adold, oldpc, NULL, wrd))
                return 1;
            to_pack (src, wrd);
            fp_normalize(src);
            return 1;
        }
            break;
        case 4:
            fpset(src, (uae_s16) (doext ? exts[0] : x_cp_get_word (ad)));
            break;
        case 5:
        {
            uae_u32 wrd1, wrd2;
            wrd1 = (doext ? exts[0] : x_cp_get_long (ad));
            ad += 4;
            wrd2 = (doext ? exts[1] : x_cp_get_long (ad));
            to_double (src, wrd1, wrd2);
            normalize_or_fault_if_no_denormal_support(opcode, extra, adold, oldpc, src);
        }
            break;
        case 6:
            fpset(src, (uae_s8) (doext ? exts[0] : x_cp_get_byte (ad)));
            break;
        default:
            return 0;
    }
    return 1;
}

static int put_fp_value (fptype *value, uae_u32 opcode, uae_u16 extra, uaecptr oldpc, uae_u32 *adp)
{
    int size, mode, reg;
    uae_u32 ad = 0;
    static int sz1[8] = { 4, 4, 12, 12, 2, 8, 1, 0 };
    static int sz2[8] = { 4, 4, 12, 12, 2, 8, 2, 0 };
    
#if DEBUG_FPP
    write_log (_T("PUTFP: %04X %04X\n"), opcode, extra);
#endif
#if 0
    if (!(extra & 0x4000)) {
        if (fault_if_no_fpu (opcode, extra, 0, oldpc))
            return 1;
        regs.fp[(extra >> 10) & 7] = *value;
        return 1;
    }
#endif
    reg = opcode & 7;
    mode = (opcode >> 3) & 7;
    size = (extra >> 10) & 7;
    switch (mode)
    {
        case 0:
            if ((size == 0 || size == 1 ||size == 4 || size == 6) && fault_if_no_fpu (opcode, extra, 0, oldpc))
                return -1;
            
            switch (size)
        {
            case 6:
                if (normalize_or_fault_if_no_denormal_support(opcode, extra, 0, oldpc, value))
                    return 1;
                m68k_dreg (regs, reg) = (uae_u32)(((to_int (value, 0) & 0xff)
                                                   | (m68k_dreg (regs, reg) & ~0xff)));
                if (fault_if_68040_integer_nonmaskable(opcode, extra, ad, oldpc, value))
                    return -1;
                break;
            case 4:
                if (normalize_or_fault_if_no_denormal_support(opcode, extra, 0, oldpc, value))
                    return 1;
                m68k_dreg (regs, reg) = (uae_u32)(((to_int (value, 1) & 0xffff)
                                                   | (m68k_dreg (regs, reg) & ~0xffff)));
                if (fault_if_68040_integer_nonmaskable(opcode, extra, ad, oldpc, value))
                    return -1;
                break;
            case 0:
                if (normalize_or_fault_if_no_denormal_support(opcode, extra, 0, oldpc, value))
                    return 1;
                m68k_dreg (regs, reg) = (uae_u32)to_int (value, 2);
                if (fault_if_68040_integer_nonmaskable(opcode, extra, ad, oldpc, value))
                    return -1;
                break;
            case 1:
                if (normalize_or_fault_if_no_denormal_support(opcode, extra, 0, oldpc, value))
                    return 1;
                m68k_dreg (regs, reg) = from_single (value);
                break;
            default:
                return 0;
        }
            return 1;
        case 1:
            return 0;
        case 2:
            ad = m68k_areg (regs, reg);
            break;
        case 3:
            // Also needed by fault_if_no_fpu
            mmufixup[0].reg = reg;
            mmufixup[0].value = m68k_areg (regs, reg);
            fpu_mmu_fixup = true;
            ad = m68k_areg (regs, reg);
            m68k_areg (regs, reg) += reg == 7 ? sz2[size] : sz1[size];
            break;
        case 4:
            // Also needed by fault_if_no_fpu
            mmufixup[0].reg = reg;
            mmufixup[0].value = m68k_areg (regs, reg);
            fpu_mmu_fixup = true;
            m68k_areg (regs, reg) -= reg == 7 ? sz2[size] : sz1[size];
            ad = m68k_areg (regs, reg);
            // 68060 no fpu -(an): EA points to -4, not -12 if extended precision
            if (currprefs.cpu_model == 68060 && if_no_fpu() && sz1[size] == 12)
                ad += 8;
            break;
        case 5:
            ad = m68k_areg (regs, reg) + (uae_s32) (uae_s16) x_cp_next_iword ();
            break;
        case 6:
            ad = x_cp_get_disp_ea_020 (m68k_areg (regs, reg), 0);
            break;
        case 7:
            switch (reg)
        {
            case 0:
                ad = (uae_s32) (uae_s16) x_cp_next_iword ();
                break;
            case 1:
                ad = x_cp_next_ilong ();
                break;
            case 2:
                ad = m68k_getpc ();
                ad += (uae_s32) (uae_s16) x_cp_next_iword ();
                break;
            case 3:
                ad = x_cp_get_disp_ea_020 (m68k_getpc (), 0);
                break;
            default:
                return 0;
        }
    }
    
    *adp = ad;

    if (fault_if_no_fpu (opcode, extra, ad, oldpc))
        return -1;
    
    switch (size)
    {
        case 0:
            if (normalize_or_fault_if_no_denormal_support(opcode, extra, ad, oldpc, value))
                return 1;
            x_cp_put_long(ad, (uae_u32)to_int(value, 2));
            if (fault_if_68040_integer_nonmaskable(opcode, extra, ad, oldpc, value))
                return -1;
            break;
        case 1:
            if (normalize_or_fault_if_no_denormal_support(opcode, extra, ad, oldpc, value))
                return 1;
            x_cp_put_long(ad, from_single(value));
            break;
        case 2:
        {
            if (normalize_or_fault_if_no_denormal_support(opcode, extra, ad, oldpc, value))
                return 1;
            uae_u32 wrd1, wrd2, wrd3;
            from_exten(value, &wrd1, &wrd2, &wrd3);
            x_cp_put_long (ad, wrd1);
            ad += 4;
            x_cp_put_long (ad, wrd2);
            ad += 4;
            x_cp_put_long (ad, wrd3);
        }
            break;
        case 3: // Packed-Decimal Real with Static k-Factor
        case 7: // Packed-Decimal Real with Dynamic k-Factor (P{Dn}) (reg to memory only)
        {
            uae_u32 wrd[3];
            int kfactor;
            if (fault_if_no_packed_support (opcode, extra, ad, oldpc, value, wrd))
                return 1;
            kfactor = size == 7 ? m68k_dreg (regs, (extra >> 4) & 7) : extra;
            kfactor &= 127;
            if (kfactor & 64)
                kfactor |= ~63;
            fp_normalize(value);
            from_pack (value, wrd, kfactor);
            x_cp_put_long (ad, wrd[0]);
            ad += 4;
            x_cp_put_long (ad, wrd[1]);
            ad += 4;
            x_cp_put_long (ad, wrd[2]);
        }
            break;
        case 4:
            if (normalize_or_fault_if_no_denormal_support(opcode, extra, ad, oldpc, value))
                return 1;
            x_cp_put_word(ad, (uae_s16)to_int(value, 1));
            if (fault_if_68040_integer_nonmaskable(opcode, extra, ad, oldpc, value))
                return -1;
            break;
        case 5:
        {
            if (normalize_or_fault_if_no_denormal_support(opcode, extra, ad, oldpc, value))
                return 1;
            uae_u32 wrd1, wrd2;
            from_double(value, &wrd1, &wrd2);
            x_cp_put_long (ad, wrd1);
            ad += 4;
            x_cp_put_long (ad, wrd2);
        }
            break;
        case 6:
            if (normalize_or_fault_if_no_denormal_support(opcode, extra, ad, oldpc, value))
                return 1;
            x_cp_put_byte(ad, (uae_s8)to_int(value, 0));
            if (fault_if_68040_integer_nonmaskable(opcode, extra, ad, oldpc, value))
                return -1;
            break;
        default:
            return 0;
    }
    return 1;
}

STATIC_INLINE int get_fp_ad (uae_u32 opcode, uae_u32 * ad)
{
    int mode;
    int reg;
    
    mode = (opcode >> 3) & 7;
    reg = opcode & 7;
    switch (mode)
    {
        case 0:
        case 1:
            return 0;
        case 2:
            *ad = m68k_areg (regs, reg);
            break;
        case 3:
            *ad = m68k_areg (regs, reg);
            break;
        case 4:
            *ad = m68k_areg (regs, reg);
            break;
        case 5:
            *ad = m68k_areg (regs, reg) + (uae_s32) (uae_s16) x_cp_next_iword ();
            break;
        case 6:
            *ad = x_cp_get_disp_ea_020 (m68k_areg (regs, reg), 0);
            break;
        case 7:
            switch (reg)
        {
            case 0:
                *ad = (uae_s32) (uae_s16) x_cp_next_iword ();
                break;
            case 1:
                *ad = x_cp_next_ilong ();
                break;
            case 2:
                *ad = m68k_getpc ();
                *ad += (uae_s32) (uae_s16) x_cp_next_iword ();
                break;
            case 3:
                *ad = x_cp_get_disp_ea_020 (m68k_getpc (), 0);
                break;
            default:
                return 0;
        }
    }
    return 1;
}

int fpp_cond (int condition)
{
    int NotANumber, Z, N;
    
    NotANumber = (regs.fpsr & FPSR_CC_NAN) ? 1 : 0;
    N = (regs.fpsr & FPSR_CC_N) ? 1 : 0;
    Z = (regs.fpsr & FPSR_CC_Z) ? 1 : 0;
    
    if ((condition & 0x10) && NotANumber) {
        if (fpsr_set_bsun())
            return -2;
    }
    
    switch (condition)
    {
        case 0x00:
            return 0;
        case 0x01:
            return Z;
        case 0x02:
            return !(NotANumber || Z || N);
        case 0x03:
            return Z || !(NotANumber || N);
        case 0x04:
            return N && !(NotANumber || Z);
        case 0x05:
            return Z || (N && !NotANumber);
        case 0x06:
            return !(NotANumber || Z);
        case 0x07:
            return !NotANumber;
        case 0x08:
            return NotANumber;
        case 0x09:
            return NotANumber || Z;
        case 0x0a:
            return NotANumber || !(N || Z);
        case 0x0b:
            return NotANumber || Z || !N;
        case 0x0c:
            return NotANumber || (N && !Z);
        case 0x0d:
            return NotANumber || Z || N;
        case 0x0e:
            return !Z;
        case 0x0f:
            return 1;
        case 0x10:
            return 0;
        case 0x11:
            return Z;
        case 0x12:
            return !(NotANumber || Z || N);
        case 0x13:
            return Z || !(NotANumber || N);
        case 0x14:
            return N && !(NotANumber || Z);
        case 0x15:
            return Z || (N && !NotANumber);
        case 0x16:
            return !(NotANumber || Z);
        case 0x17:
            return !NotANumber;
        case 0x18:
            return NotANumber;
        case 0x19:
            return NotANumber || Z;
        case 0x1a:
            return NotANumber || !(N || Z);
        case 0x1b:
            return NotANumber || Z || !N;
        case 0x1c:
            return NotANumber || (N && !Z);
        case 0x1d:
            return NotANumber || Z || N;
        case 0x1e:
            return !Z;
        case 0x1f:
            return 1;
    }
    return -1;
}

static void maybe_idle_state (void)
{
    // conditional floating point instruction does not change state
    // from null to idle on 68040/060.
    if (currprefs.fpu_model == 68881 || currprefs.fpu_model == 68882)
        regs.fpu_state = 1;
}

void fpuop_dbcc (uae_u32 opcode, uae_u16 extra)
{
    uaecptr pc = m68k_getpc ();
    uae_s32 disp;
    int cc;
    
    if (fp_exception_pending(true))
        return;

    regs.fp_exception = false;
#if DEBUG_FPP
    write_log (_T("fdbcc_opp at %08x\n"), m68k_getpc ());
#endif
    if (fault_if_no_6888x (opcode, extra, pc - 4))
        return;
    
    disp = (uae_s32) (uae_s16) x_cp_next_iword ();
    if (fault_if_no_fpu_u (opcode, extra, pc + disp, pc - 4))
        return;
    regs.fpiar = pc - 4;
    maybe_idle_state ();
    cc = fpp_cond (extra & 0x3f);
    if (cc < 0) {
        if (cc == -2)
            return; // BSUN
        else
            fpu_op_illg (opcode, 0, regs.fpiar);
    } else if (!cc) {
        int reg = opcode & 0x7;
        
        m68k_dreg (regs, reg) = ((m68k_dreg (regs, reg) & 0xffff0000)
                                 | (((m68k_dreg (regs, reg) & 0xffff) - 1) & 0xffff));
        if ((m68k_dreg (regs, reg) & 0xffff) != 0xffff) {
            m68k_setpc (pc + disp);
            regs.fp_branch = true;
        }
    }
}

void fpuop_scc (uae_u32 opcode, uae_u16 extra)
{
    uae_u32 ad = 0;
    int cc;
    uaecptr pc = m68k_getpc () - 4;
    
    if (fp_exception_pending(true))
        return;

    regs.fp_exception = false;
#if DEBUG_FPP
    write_log (_T("fscc_opp at %08x\n"), m68k_getpc ());
#endif
    
    if (fault_if_no_6888x (opcode, extra, pc))
        return;
    
    if (opcode & 0x38) {
        if (get_fp_ad (opcode, &ad) == 0) {
            fpu_noinst (opcode, regs.fpiar);
            return;
        }
    }
    
    if (fault_if_no_fpu_u (opcode, extra, ad, pc))
        return;
    
    regs.fpiar = pc;
    maybe_idle_state ();
    cc = fpp_cond (extra & 0x3f);
    if (cc < 0) {
        if (cc == -2)
            return; // BSUN
        else
            fpu_op_illg (opcode, 0, regs.fpiar);
    } else if ((opcode & 0x38) == 0) {
        m68k_dreg (regs, opcode & 7) = (m68k_dreg (regs, opcode & 7) & ~0xff) | (cc ? 0xff : 0x00);
    } else {
        x_cp_put_byte (ad, cc ? 0xff : 0x00);
    }
}

void fpuop_trapcc (uae_u32 opcode, uaecptr oldpc, uae_u16 extra)
{
    int cc;
    
    if (fp_exception_pending(true))
        return;

    regs.fp_exception = false;
#if DEBUG_FPP
    write_log (_T("ftrapcc_opp at %08x\n"), m68k_getpc ());
#endif
    if (fault_if_no_fpu_u (opcode, extra, 0, oldpc))
        return;
    
    regs.fpiar = oldpc;
    maybe_idle_state ();
    cc = fpp_cond (extra & 0x3f);
    if (cc < 0) {
        if (cc == -2)
            return; // BSUN
        else
            fpu_op_illg (opcode, 0, regs.fpiar);
    } else if (cc) {
        Exception (7);
    }
}

void fpuop_bcc (uae_u32 opcode, uaecptr oldpc, uae_u32 extra)
{
    int cc;
    
    if (fp_exception_pending(true))
        return;

    regs.fp_exception = false;
#if DEBUG_FPP
    write_log (_T("fbcc_opp at %08x\n"), m68k_getpc ());
#endif
    if (fault_if_no_fpu (opcode, extra, 0, oldpc - 2))
        return;
    
    regs.fpiar = oldpc - 2;
    maybe_idle_state ();
    cc = fpp_cond (opcode & 0x3f);
    if (cc < 0) {
        if (cc == -2)
            return; // BSUN
        else
            fpu_op_illg (opcode, 0, regs.fpiar);
    } else if (cc) {
        if ((opcode & 0x40) == 0)
            extra = (uae_s32) (uae_s16) extra;
        m68k_setpc (oldpc + extra);
        regs.fp_branch = true;
    }
}

static uaecptr fmovem2mem (uaecptr ad, uae_u32 list, int incr, int regdir)
{
    int reg;
    
    // 68030 MMU state saving is annoying!
    if (currprefs.mmu_model == 68030) {
        int idx = 0;
        int r;
        int i;
        uae_u32 wrd[3];
        mmu030_state[1] |= MMU030_STATEFLAG1_MOVEM1;
        for (r = 0; r < 8; r++) {
            if (regdir < 0)
                reg = 7 - r;
            else
                reg = r;
            if (list & 0x80) {
                from_exten_fmovem(&regs.fp[reg], &wrd[0], &wrd[1], &wrd[2]);
                if (incr < 0)
                    ad -= 3 * 4;
                for (i = 0; i < 3; i++) {
                    if (mmu030_state[0] == idx * 3 + i) {
                        if (mmu030_state[1] & MMU030_STATEFLAG1_MOVEM2) {
                            mmu030_state[1] &= ~MMU030_STATEFLAG1_MOVEM2;
                        }
                        else {
                            mmu030_data_buffer = wrd[i];
                            x_put_long(ad + i * 4, wrd[i]);
                        }
                        mmu030_state[0]++;
                    }
                }
                if (incr > 0)
                    ad += 3 * 4;
                idx++;
            }
            list <<= 1;
        }
    } else {
        int r;
        for (r = 0; r < 8; r++) {
            uae_u32 wrd1, wrd2, wrd3;
            if (regdir < 0)
                reg = 7 - r;
            else
                reg = r;
            if (list & 0x80) {
                from_exten_fmovem(&regs.fp[reg], &wrd1, &wrd2, &wrd3);
                if (incr < 0)
                    ad -= 3 * 4;
                x_put_long(ad + 0, wrd1);
                x_put_long(ad + 4, wrd2);
                x_put_long(ad + 8, wrd3);
                if (incr > 0)
                    ad += 3 * 4;
            }
            list <<= 1;
        }
    }
    return ad;
}

static uaecptr fmovem2fpp (uaecptr ad, uae_u32 list, int incr, int regdir)
{
    int reg;
    
    if (currprefs.mmu_model == 68030) {
        uae_u32 wrd[3];
        int idx = 0;
        int r;
        int i;
        mmu030_state[1] |= MMU030_STATEFLAG1_MOVEM1 | MMU030_STATEFLAG1_FMOVEM;
        if (mmu030_state[1] & MMU030_STATEFLAG1_MOVEM2)
            ad = mmu030_ad[mmu030_idx].val;
        else
            mmu030_ad[mmu030_idx].val = ad;
        for (r = 0; r < 8; r++) {
            if (regdir < 0)
                reg = 7 - r;
            else
                reg = r;
            if (list & 0x80) {
                if (incr < 0)
                    ad -= 3 * 4;
                for (i = 0; i < 3; i++) {
                    if (mmu030_state[0] == idx * 3 + i) {
                        if (mmu030_state[1] & MMU030_STATEFLAG1_MOVEM2) {
                            mmu030_state[1] &= ~MMU030_STATEFLAG1_MOVEM2;
                            wrd[i] = mmu030_data_buffer;
                        } else {
                            wrd[i] = x_get_long (ad + i * 4);
                        }
                        // save first two entries if 2nd or 3rd get_long() faults.
                        if (i == 0 || i == 1)
                            mmu030_fmovem_store[i] = wrd[i];
                        mmu030_state[0]++;
                        if (i == 2)
                            to_exten_fmovem (&regs.fp[reg], mmu030_fmovem_store[0], mmu030_fmovem_store[1], wrd[2]);
                    }
                }
                if (incr > 0)
                    ad += 3 * 4;
                idx++;
            }
            list <<= 1;
        }
    } else {
        int r;
        for (r = 0; r < 8; r++) {
            uae_u32 wrd1, wrd2, wrd3;
            if (regdir < 0)
                reg = 7 - r;
            else
                reg = r;
            if (list & 0x80) {
                if (incr < 0)
                    ad -= 3 * 4;
                wrd1 = x_get_long (ad + 0);
                wrd2 = x_get_long (ad + 4);
                wrd3 = x_get_long (ad + 8);
                if (incr > 0)
                    ad += 3 * 4;
                to_exten_fmovem (&regs.fp[reg], wrd1, wrd2, wrd3);
            }
            list <<= 1;
        }
    }
    return ad;
}

static bool fp_arithmetic(fptype *src, fptype *dst, int extra)
{
    uae_u64 q = 0;
    uae_s8 s = 0;
    
    switch (extra & 0x7f)
    {
        case 0x00: /* FMOVE */
            fp_move(dst, src);
            break;
        case 0x40: /* FSMOVE */
            fp_move_single(dst, src);
            break;
        case 0x44: /* FDMOVE */
            fp_move_double(dst, src);
            break;
        case 0x01: /* FINT */
            fp_int(dst, src);
            break;
        case 0x02: /* FSINH */
            fp_sinh(dst, src);
            break;
        case 0x03: /* FINTRZ */
            fp_intrz(dst, src);
            break;
        case 0x04: /* FSQRT */
            fp_sqrt(dst, src);
            break;
        case 0x41: /* FSSQRT */
            fp_sqrt_single(dst, src);
            break;
        case 0x45: /* FDSQRT */
            fp_sqrt_double(dst, src);
            break;
        case 0x06: /* FLOGNP1 */
            fp_lognp1(dst, src);
            break;
        case 0x08: /* FETOXM1 */
            fp_etoxm1(dst, src);
            break;
        case 0x09: /* FTANH */
            fp_tanh(dst, src);
            break;
        case 0x0a: /* FATAN */
            fp_atan(dst, src);
            break;
        case 0x0c: /* FASIN */
            fp_asin(dst, src);
            break;
        case 0x0d: /* FATANH */
            fp_atanh(dst, src);
            break;
        case 0x0e: /* FSIN */
            fp_sin(dst, src);
            break;
        case 0x0f: /* FTAN */
            fp_tan(dst, src);
            break;
        case 0x10: /* FETOX */
            fp_etox(dst, src);
            break;
        case 0x11: /* FTWOTOX */
            fp_twotox(dst, src);
            break;
        case 0x12: /* FTENTOX */
            fp_tentox(dst, src);
            break;
        case 0x14: /* FLOGN */
            fp_logn(dst, src);
            break;
        case 0x15: /* FLOG10 */
            fp_log10(dst, src);
            break;
        case 0x16: /* FLOG2 */
            fp_log2(dst, src);
            break;
        case 0x18: /* FABS */
            fp_abs(dst, src);
            break;
        case 0x58: /* FSABS */
            fp_abs_single(dst, src);
            break;
        case 0x5c: /* FDABS */
            fp_abs_double(dst, src);
            break;
        case 0x19: /* FCOSH */
            fp_cosh(dst, src);
            break;
        case 0x1a: /* FNEG */
            fp_neg(dst, src);
            break;
        case 0x5a: /* FSNEG */
            fp_neg_single(dst, src);
            break;
        case 0x5e: /* FDNEG */
            fp_neg_double(dst, src);
            break;
        case 0x1c: /* FACOS */
            fp_acos(dst, src);
            break;
        case 0x1d: /* FCOS */
            fp_cos(dst, src);
            break;
        case 0x1e: /* FGETEXP */
            fp_getexp(dst, src);
            break;
        case 0x1f: /* FGETMAN */
            fp_getman(dst, src);
            break;
        case 0x20: /* FDIV */
            fp_div(dst, src);
            break;
        case 0x60: /* FSDIV */
            fp_div_single(dst, src);
            break;
        case 0x64: /* FDDIV */
            fp_div_double(dst, src);
            break;
        case 0x21: /* FMOD */
            fpsr_get_quotient(&q, &s);
            fp_mod(dst, src, &q, &s);
            fpsr_set_quotient(q, s);
            break;
        case 0x22: /* FADD */
            fp_add(dst, src);
            break;
        case 0x62: /* FSADD */
            fp_add_single(dst, src);
            break;
        case 0x66: /* FDADD */
            fp_add_double(dst, src);
            break;
        case 0x23: /* FMUL */
            fp_mul(dst, src);
            break;
        case 0x63: /* FSMUL */
            fp_mul_single(dst, src);
            break;
        case 0x67: /* FDMUL */
            fp_mul_double(dst, src);
            break;
        case 0x24: /* FSGLDIV */
            fp_sgldiv(dst, src);
            break;
        case 0x25: /* FREM */
            fpsr_get_quotient(&q, &s);
            fp_rem(dst, src, &q, &s);
            fpsr_set_quotient(q, s);
            break;
        case 0x26: /* FSCALE */
            fp_scale(dst, src);
            break;
        case 0x27: /* FSGLMUL */
            fp_sglmul(dst, src);
            break;
        case 0x28: /* FSUB */
            fp_sub(dst, src);
            break;
        case 0x68: /* FSSUB */
            fp_sub_single(dst, src);
            break;
        case 0x6c: /* FDSUB */
            fp_sub_double(dst, src);
            break;
        case 0x30: /* FSINCOS */
        case 0x31:
        case 0x32:
        case 0x33:
        case 0x34:
        case 0x35:
        case 0x36:
        case 0x37:
            fp_cos(dst, src);
            if (fpsr_make_status())
                return false;
            regs.fp[extra & 7] = *dst;
            fp_sin(dst, src);
            break;
        case 0x38: /* FCMP */
            fp_cmp(dst, src);
            fpsr_make_status();
            fpsr_set_result(dst);
            return false;
        case 0x3a: /* FTST */
            fp_tst(dst, src);
            fpsr_make_status();
            fpsr_set_result(dst);
            return false;
            
        default:
            write_log (_T("Unknown FPU arithmetic function (%02x)\n"), extra & 0x7f);
            return false;
    }
    
    fpsr_set_result(dst);

    if (fpsr_make_status())
        return false;
    
    return true;
}

static void fpuop_arithmetic2 (uae_u32 opcode, uae_u16 extra)
{
    int reg = -1;
    int v;
    fptype src, dst;
    uaecptr pc = m68k_getpc () - 4;
    uaecptr ad = 0;
    
#if DEBUG_FPP
    write_log (_T("FPP %04x %04x at %08x\n"), opcode & 0xffff, extra, pc);
#endif
    if (fault_if_no_6888x (opcode, extra, pc))
        return;
    
    switch ((extra >> 13) & 0x7)
    {
        case 3:
            if (fp_exception_pending(true))
                return;

            regs.fpiar = pc;
            fpsr_clear_status();
            src = regs.fp[(extra >> 7) & 7];
            v = put_fp_value (&src, opcode, extra, pc, &ad);
            if (v <= 0) {
                if (v == 0)
                    fpu_noinst (opcode, pc);
                return;
            }
            fpsr_make_status();
            fpsr_check_arithmetic_exception(0, &src, opcode, extra, ad);
            fp_exception_pending(false); // post/mid instruction
            return;
            
        case 4:
        case 5:
            if ((opcode & 0x38) == 0) {
                if (fault_if_no_fpu (opcode, extra, 0, pc))
                    return;
                if (extra & 0x2000) {
                    if (extra & 0x1000)
                        m68k_dreg (regs, opcode & 7) = regs.fpcr & 0xffff;
                    if (extra & 0x0800)
                        m68k_dreg (regs, opcode & 7) = fpp_get_fpsr ();
                    if (extra & 0x0400)
                        m68k_dreg (regs, opcode & 7) = regs.fpiar;
                } else {
                    if (extra & 0x1000)
                        fpp_set_fpcr (m68k_dreg (regs, opcode & 7));
                    if (extra & 0x0800)
                        fpp_set_fpsr (m68k_dreg (regs, opcode & 7));
                    if (extra & 0x0400)
                        regs.fpiar = m68k_dreg (regs, opcode & 7);
                }
            } else if ((opcode & 0x38) == 0x08) {
                if (fault_if_no_fpu (opcode, extra, 0, pc))
                    return;
                if (extra & 0x2000) {
                    if (extra & 0x1000)
                        m68k_areg (regs, opcode & 7) = regs.fpcr & 0xffff;
                    if (extra & 0x0800)
                        m68k_areg (regs, opcode & 7) = fpp_get_fpsr ();
                    if (extra & 0x0400)
                        m68k_areg (regs, opcode & 7) = regs.fpiar;
                } else {
                    if (extra & 0x1000)
                        fpp_set_fpcr (m68k_areg (regs, opcode & 7));
                    if (extra & 0x0800)
                        fpp_set_fpsr (m68k_areg (regs, opcode & 7));
                    if (extra & 0x0400)
                        regs.fpiar = m68k_areg (regs, opcode & 7);
                }
            } else if ((opcode & 0x3f) == 0x3c) {
                if ((extra & 0x2000) == 0) {
                    uae_u32 ext[3];
                    // 68060 FMOVEM.L #imm,more than 1 control register: unimplemented EA
                    uae_u16 bits = extra & (0x1000 | 0x0800 | 0x0400);
                    if (bits && bits != 0x1000 && bits != 0x0800 && bits != 0x400) {
                        if (fault_if_60())
                            return;
                    }
                    // fetch first, use only after all data has been fetched
                    ext[0] = ext[1] = ext[2] = 0;
                    if (extra & 0x1000)
                        ext[0] = x_cp_next_ilong ();
                    if (extra & 0x0800)
                        ext[1] = x_cp_next_ilong ();
                    if (extra & 0x0400)
                        ext[2] = x_cp_next_ilong ();
                    if (fault_if_no_fpu (opcode, extra, 0, pc))
                        return;
                    if (extra & 0x1000)
                        fpp_set_fpcr (ext[0]);
                    if (extra & 0x0800)
                        fpp_set_fpsr (ext[1]);
                    if (extra & 0x0400)
                        regs.fpiar = ext[2];
                } else {
                    // immediate as destination
                    fpu_noinst (opcode, pc);
                    return;
                }
            } else if (extra & 0x2000) {
                /* FMOVEM FPP->memory */
                uae_u32 ad;
                int incr = 0;
                
                if (get_fp_ad (opcode, &ad) == 0) {
                    fpu_noinst (opcode, pc);
                    return;
                }
                if (fault_if_no_fpu (opcode, extra, ad, pc))
                    return;
                
                if ((opcode & 0x38) == 0x20) {
                    if (extra & 0x1000)
                        incr += 4;
                    if (extra & 0x0800)
                        incr += 4;
                    if (extra & 0x0400)
                        incr += 4;
                }
                ad -= incr;
                if (extra & 0x1000) {
                    x_cp_put_long (ad, regs.fpcr & 0xffff);
                    ad += 4;
                }
                if (extra & 0x0800) {
                    x_cp_put_long (ad, fpp_get_fpsr ());
                    ad += 4;
                }
                if (extra & 0x0400) {
                    x_cp_put_long (ad, regs.fpiar);
                    ad += 4;
                }
                ad -= incr;
                if ((opcode & 0x38) == 0x18)
                    m68k_areg (regs, opcode & 7) = ad;
                if ((opcode & 0x38) == 0x20)
                    m68k_areg (regs, opcode & 7) = ad;
            } else {
                /* FMOVEM memory->FPP */
                uae_u32 ad;
                int incr = 0;
                
                if (get_fp_ad (opcode, &ad) == 0) {
                    fpu_noinst (opcode, pc);
                    return;
                }
                if (fault_if_no_fpu (opcode, extra, ad, pc))
                    return;
                
                if((opcode & 0x38) == 0x20) {
                    if (extra & 0x1000)
                        incr += 4;
                    if (extra & 0x0800)
                        incr += 4;
                    if (extra & 0x0400)
                        incr += 4;
                    ad = ad - incr;
                }
                if (extra & 0x1000) {
                    fpp_set_fpcr (x_cp_get_long (ad));
                    ad += 4;
                }
                if (extra & 0x0800) {
                    fpp_set_fpsr (x_cp_get_long (ad));
                    ad += 4;
                }
                if (extra & 0x0400) {
                    regs.fpiar = x_cp_get_long (ad);
                    ad += 4;
                }
                if ((opcode & 0x38) == 0x18)
                    m68k_areg (regs, opcode & 7) = ad;
                if ((opcode & 0x38) == 0x20)
                    m68k_areg (regs, opcode & 7) = ad - incr;
            }
            return;
            
        case 6:
        case 7:
        {
            uae_u32 ad, list = 0;
            int incr = 1;
            int regdir = 1;
            if (get_fp_ad (opcode, &ad) == 0) {
                fpu_noinst (opcode, pc);
                return;
            }
            if (fault_if_no_fpu (opcode, extra, ad, pc))
                return;
            switch ((extra >> 11) & 3)
            {
                case 0:	/* static pred */
                case 2:	/* static postinc */
                    list = extra & 0xff;
                    break;
                case 1:	/* dynamic pred */
                case 3:	/* dynamic postinc */
                    if (fault_if_60())
                        return;
                    list = m68k_dreg (regs, (extra >> 4) & 3) & 0xff;
                    break;
            }
            if ((opcode & 0x38) == 0x20) { // -(an)
                incr = -1;
                switch ((extra >> 11) & 3)
                {
                    case 0:	/* static pred */
                    case 1:	/* dynamic pred */
                        regdir = -1;
                        break;
                }
            }
            
            if (extra & 0x2000) {
                /* FMOVEM FPP->memory */
                ad = fmovem2mem (ad, list, incr, regdir);
            } else {
                /* FMOVEM memory->FPP */
                ad = fmovem2fpp (ad, list, incr, regdir);
            }
            if ((opcode & 0x38) == 0x18 || (opcode & 0x38) == 0x20) // (an)+ or -(an)
                m68k_areg (regs, opcode & 7) = ad;
        }
            return;
            
        case 0:
        case 2: /* Extremely common */
            if (fp_exception_pending(true))
                return;

            regs.fpiar = pc;
            reg = (extra >> 7) & 7;
            if ((extra & 0xfc00) == 0x5c00) {
                if (fault_if_unimplemented_680x0 (opcode, extra, ad, pc, &src, reg))
                    return;
                if (extra & 0x40) {
                    // 6888x and ROM constant 0x40 - 0x7f: f-line
                    fpu_noinst (opcode, pc);
                    return;
                }
                fpsr_clear_status();
                fpu_get_constant(&regs.fp[reg], extra & 0x3f);
                fpsr_make_status();
                fpsr_check_arithmetic_exception(0, &src, opcode, extra, ad);
                return;
            }
            
            // 6888x does not have special exceptions, check immediately
            if (fault_if_unimplemented_6888x (opcode, extra, pc))
                return;
            
            fpsr_clear_status();

            v = get_fp_value (opcode, extra, &src, pc, &ad);
            if (v <= 0) {
                if (v == 0)
                    fpu_noinst (opcode, pc);
                return;
            }
            
            dst = regs.fp[reg];
            
            if (fp_is_dyadic(extra))
                normalize_or_fault_if_no_denormal_support_dst(opcode, extra, ad, pc, &dst, &src);
            
            // check for 680x0 unimplemented instruction
            if (fault_if_unimplemented_680x0 (opcode, extra, ad, pc, &src, reg))
                return;
            
            // unimplemented datatype was checked in get_fp_value
            if (regs.fp_unimp_pend) {
                fp_exception_pending(false); // simplification: always mid/post-instruction exception
                return;
            }

            v = fp_arithmetic(&src, &dst, extra);
            
            fpsr_check_arithmetic_exception(0, &src, opcode, extra, ad);

            if (v)
                regs.fp[reg] = dst;
            
            fp_exception_pending(false); // simplification: always mid/post-instruction exception
            
            return;
        default:
            break;
    }
    fpu_noinst (opcode, pc);
}

void fpuop_arithmetic (uae_u32 opcode, uae_u16 extra)
{
    regs.fpu_state = 1;
    regs.fp_exception = false;
    fpu_mmu_fixup = false;
    fpuop_arithmetic2 (opcode, extra);
    if (fpu_mmu_fixup) {
        mmufixup[0].reg = -1;
    }
}

void fpuop_save (uae_u32 opcode)
{
    uae_u32 ad, adp;
    int incr = (opcode & 0x38) == 0x20 ? -1 : 1;
    int fpu_version = get_fpu_version ();
    uaecptr pc = m68k_getpc () - 2;
    int i;
    
    regs.fp_exception = false;
#if DEBUG_FPP
    write_log (_T("fsave_opp at %08x\n"), m68k_getpc ());
#endif
    
    if (fault_if_no_6888x (opcode, 0, pc))
        return;
    
    if (get_fp_ad (opcode, &ad) == 0) {
        fpu_op_illg (opcode, 0, pc);
        return;
    }
    
    if (fault_if_no_fpu (opcode, 0, ad, pc))
        return;
    
    if (currprefs.fpu_model == 68060) {
        /* 12 byte 68060 NULL/IDLE/EXCP frame.  */
        int frame_size = 12;
        uae_u32 frame_id;
        
        if (regs.fpu_exp_state > 1) {
            frame_id = 0x0000e000 | fsave_data.v;
            
#if 0 //EXCEPTION_FPP
            write_log(_T("68060 FSAVE EXCP %s\n"), fp_print(&fsave_data.src));
#endif
            
        } else {
            frame_id = regs.fpu_state == 0 ? 0x00000000 : 0x00006000;
        }
        if (incr < 0)
            ad -= frame_size;
        adp = ad;
        x_put_long (ad, (fsave_data.eo[0] & 0xffff0000) | frame_id);
        ad += 4;
        x_put_long (ad, fsave_data.eo[1]);
        ad += 4;
        x_put_long (ad, fsave_data.eo[2]);
        ad += 4;
        
    } else if (currprefs.fpu_model == 68040) {
        
        if (!regs.fpu_exp_state) {
            /* 4 byte 68040 NULL/IDLE frame.  */
            uae_u32 frame_id = regs.fpu_state == 0 ? 0 : fpu_version << 24;
            if (incr < 0)
                ad -= 4;
            adp = ad;
            x_put_long (ad, frame_id);
            ad += 4;
        } else {
            /* 44 (rev $40) and 52 (rev $41) byte 68040 unimplemented instruction frame */
            /* 96 byte 68040 busy frame */
            int frame_size = regs.fpu_exp_state == 2 ? 0x64 : (fpu_version >= 0x41 ? 0x34 : 0x2c);
            uae_u32 frame_id = ((fpu_version << 8) | (frame_size - 4)) << 16;
            
#if 0//EXCEPTION_FPP
            write_log(_T("68040 FSAVE %d (%d), CMDREG=%04X"), regs.fp_exp_pend, frame_size, extra);
            if (regs.fp_exp_pend == FPU_EXP_UNIMP_DATATYPE_PACKED_PRE) {
                write_log(_T(" PACKED %08x-%08x-%08x"), fsave_data.pack[0], fsave_data.pack[1], fsave_data.pack[2]);
            } else if (regs.fp_exp_pend == FPU_EXP_UNIMP_DATATYPE_PACKED_POST) {
                write_log(_T(" SRC=%s (%08x-%08x-%08x %d)"),
                          fp_print(&fsave_data.src), src1[0], src1[1], src1[2], stag);
                write_log(_T(" DST=%s (%08x-%08x-%08x %d)"),
                          fp_print(&fsave_data.dst), src2[0], src2[1], src2[2], dtag);
            }
            write_log(_T("\n"));
#endif
            
            if (incr < 0)
                ad -= frame_size;
            adp = ad;
            x_put_long (ad, frame_id);
            ad += 4;
            if (regs.fpu_exp_state == 2) {
                /* BUSY frame */
                x_put_long(ad, 0);
                ad += 4;
                x_put_long(ad, 0); // CU_SAVEPC (Software shouldn't care)
                ad += 4;
                x_put_long(ad, 0);
                ad += 4;
                x_put_long(ad, 0);
                ad += 4;
                x_put_long(ad, 0);
                ad += 4;
                x_put_long(ad, fsave_data.wbt[0]); // WBTS/WBTE
                ad += 4;
                x_put_long(ad, fsave_data.wbt[1]); // WBTM
                ad += 4;
                x_put_long(ad, fsave_data.wbt[2]); // WBTM
                ad += 4;
                x_put_long(ad, 0);
                ad += 4;
                x_put_long(ad, fsave_data.fpiarcu); // FPIARCU (same as FPU PC or something else?)
                ad += 4;
                x_put_long(ad, 0);
                ad += 4;
                x_put_long(ad, 0);
                ad += 4;
            }
            if (fpu_version >= 0x41 || regs.fpu_exp_state == 2) {
                x_put_long(ad, fsave_data.cmdreg3b<<16); // CMDREG3B
                ad += 4;
                x_put_long (ad, 0);
                ad += 4;
            }
            x_put_long (ad, (fsave_data.stag<<29) | (fsave_data.wbtm66<<26) | (fsave_data.grs<<23)); // STAG
            ad += 4;
            x_put_long (ad, fsave_data.cmdreg1b<<16); // CMDREG1B
            ad += 4;
            x_put_long (ad, (fsave_data.dtag<<29) | (fsave_data.wbte15<<20)); // DTAG
            ad += 4;
            x_put_long (ad, (fsave_data.e1<<26) | (fsave_data.e3<<25) | (fsave_data.t<<20));
            ad += 4;
            x_put_long (ad, fsave_data.fpt[0]); // FPTS/FPTE
            ad += 4;
            x_put_long (ad, fsave_data.fpt[1]); // FPTM
            ad += 4;
            x_put_long (ad, fsave_data.fpt[2]); // FPTM
            ad += 4;
            x_put_long (ad, fsave_data.et[0]); // ETS/ETE
            ad += 4;
            x_put_long (ad, fsave_data.et[1]); // ETM
            ad += 4;
            x_put_long (ad, fsave_data.et[2]); // ETM
            ad += 4;
        }
    } else { /* 68881/68882 */
        uae_u32 biu_flags = 0x540effff;
        int frame_size = currprefs.fpu_model == 68882 ? 0x3c : 0x1c;
        uae_u32 frame_id = regs.fpu_state == 0 ? ((frame_size - 4) << 16) : (fpu_version << 24) | ((frame_size - 4) << 16);
        
        regs.fp_exp_pend = 0;
        if (regs.fpu_exp_state) {
            biu_flags |= 0x20000000;
        } else {
            biu_flags |= 0x08000000;
        }
        if (regs.fpu_state == 0)
            frame_size = 4;
        
        if (currprefs.mmu_model) {
            i = 0;
            if (incr < 0)
                ad -= frame_size;
            adp = ad;
            if(mmu030_state[0] == i) {
                x_put_long(ad, frame_id); // frame id
                mmu030_state[0]++;
            }
            ad += 4;
            i++;
            if (regs.fpu_state != 0) { // idle frame
                if(mmu030_state[0] == i) {
                    x_put_long(ad, fsave_data.ccr); // command/condition register
                    mmu030_state[0]++;
                }
                ad += 4;
                i++;
                if (currprefs.fpu_model == 68882) {
                    while (i <= 9) {
                        if (mmu030_state[0] == i) {
                            x_put_long(ad, 0x00000000); // internal
                            mmu030_state[0]++;
                        }
                        ad += 4;
                        i++;
                    }
                }
                if (mmu030_state[0] == i) {
                    x_put_long (ad, fsave_data.eo[0]); // exceptional operand lo
                    mmu030_state[0]++;
                }
                ad += 4;
                i++;
                if (mmu030_state[0] == i) {
                    x_put_long (ad, fsave_data.eo[1]); // exceptional operand mid
                    mmu030_state[0]++;
                }
                ad += 4;
                i++;
                if (mmu030_state[0] == i) {
                    x_put_long (ad, fsave_data.eo[2]); // exceptional operand hi
                    mmu030_state[0]++;
                }
                ad += 4;
                i++;
                if (mmu030_state[0] == i) {
                    x_put_long(ad, 0x00000000); // operand register
                    mmu030_state[0]++;
                }
                ad += 4;
                i++;
                if (mmu030_state[0] == i) {
                    x_put_long(ad, biu_flags); // biu flags
                    mmu030_state[0]++;
                }
                ad += 4;
            }
        } else {
            if (incr < 0)
                ad -= frame_size;
            adp = ad;
            x_put_long(ad, frame_id); // frame id
            ad += 4;
            if (regs.fpu_state != 0) { // idle frame
                x_put_long(ad, fsave_data.ccr); // command/condition register
                ad += 4;
                if(currprefs.fpu_model == 68882) {
                    for(i = 0; i < 32; i += 4) {
                        x_put_long(ad, 0x00000000); // internal
                        ad += 4;
                    }
                }
                x_put_long(ad, fsave_data.eo[0]); // exceptional operand hi
                ad += 4;
                x_put_long(ad, fsave_data.eo[1]); // exceptional operand mid
                ad += 4;
                x_put_long(ad, fsave_data.eo[2]); // exceptional operand lo
                ad += 4;
                x_put_long(ad, 0x00000000); // operand register
                ad += 4;
                x_put_long(ad, biu_flags); // biu flags
                ad += 4;
            }
        }
    }
    
    if ((opcode & 0x38) == 0x20) // predecrement
        m68k_areg (regs, opcode & 7) = adp;
    regs.fpu_exp_state = 0;
}

void fpuop_restore (uae_u32 opcode)
{
    uaecptr pc = m68k_getpc () - 2;
    uae_u32 ad;
    uae_u32 d;
    
    int frame_version;
    int fpu_version = get_fpu_version();
    
    regs.fp_exception = false;
#if DEBUG_FPP
	write_log (_T("frestore_opp at %08x\n"), m68k_getpc ());
#endif
	
    if (fault_if_no_6888x (opcode, 0, pc))
        return;
    
    if (get_fp_ad (opcode, &ad) == 0) {
        fpu_op_illg (opcode, 0, pc);
        return;
    }
    
    if (fault_if_no_fpu (opcode, 0, ad, pc))
        return;
    regs.fpiar = pc;
    
    // FRESTORE does not support predecrement
    
    d = x_get_long (ad);
    ad += 4;
    
    frame_version = (d >> 24) & 0xff;
    
    if (currprefs.fpu_model == 68060) {
        int ff = (d >> 8) & 0xff;
        uae_u32 v = d & 0x7;
        fsave_data.eo[0] = d & 0xffff0000;
        
        fsave_data.eo[1] = x_get_long (ad);
        ad += 4;
        fsave_data.eo[2] = x_get_long (ad);
        ad += 4;
        
        if (ff == 0x60) {
            regs.fpu_state = 1;
            regs.fpu_exp_state = 0;
        } else if (ff == 0xe0) {
            regs.fpu_state = 1;
            regs.fpu_exp_state = 2;
            if (v == 7) {
                regs.fp_unimp_pend = 1;
            } else {
                regs.fp_exp_pend = 48 + v;
            }
        } else if (ff) {
            write_log (_T("FRESTORE invalid frame format %02X!\n"), ff);
            Exception(14);
            return;
        } else {
            fpu_null();
        }
    } else if (currprefs.fpu_model == 68040) {
        
        if (frame_version == fpu_version) { // not null frame
            uae_u32 frame_size = (d >> 16) & 0xff;
            
            if (frame_size == 0x60) { // busy
                fptype src, dst;
                uae_u32 tmp, v, opclass, cmdreg1b, fpte15, et15, cusavepc;
                
                ad += 0x4; // offset to CU_SAVEPC field
                tmp = x_get_long (ad);
                cusavepc = tmp >> 24;
                ad += 0x20; // offset to FPIARCU field
                regs.fpiar = x_get_long (ad);
                ad += 0x14; // offset to ET15 field
                tmp = x_get_long (ad);
                et15 = (tmp & 0x10000000) >> 28;
                ad += 0x4; // offset to CMDREG1B field
                fsave_data.cmdreg1b = x_get_long (ad);
                fsave_data.cmdreg1b >>= 16;
                cmdreg1b = fsave_data.cmdreg1b;
                ad += 0x4; // offset to FPTE15 field
                tmp = x_get_long (ad);
                fpte15 = (tmp & 0x10000000) >> 28;
                ad += 0x8; // offset to FPTE field
                fsave_data.fpt[0] = x_get_long (ad);
                ad += 0x4;
                fsave_data.fpt[1] = x_get_long (ad);
                ad += 0x4;
                fsave_data.fpt[2] = x_get_long (ad);
                ad += 0x4; // offset to ET field
                fsave_data.et[0] = x_get_long (ad);
                ad += 0x4;
                fsave_data.et[1] = x_get_long (ad);
                ad += 0x4;
                fsave_data.et[2] = x_get_long (ad);
                ad += 0x4;
                
                opclass = (cmdreg1b >> 13) & 0x7; // just to be sure
                
                if (cusavepc == 0xFE) {
                    if (opclass == 0 || opclass == 2) {
                        to_exten_fmovem(&dst, fsave_data.fpt[0], fsave_data.fpt[1], fsave_data.fpt[2]);
                        fp_denormalize(&dst, fpte15);
                        to_exten_fmovem(&src, fsave_data.et[0], fsave_data.et[1], fsave_data.et[2]);
                        fp_denormalize(&src, et15);
#if EXCEPTION_FPP
                        uae_u32 tmpsrc[3], tmpdst[3];
                        from_exten_fmovem(&src, &tmpsrc[0], &tmpsrc[1], &tmpsrc[2]);
                        from_exten_fmovem(&dst, &tmpdst[0], &tmpdst[1], &tmpdst[2]);
                        write_log (_T("FRESTORE src = %08X %08X %08X, dst = %08X %08X %08X, extra = %04X\n"),
                                   tmpsrc[0], tmpsrc[1], tmpsrc[2], tmpdst[0], tmpdst[1], tmpdst[2], cmdreg1b);
#endif
                        fpsr_clear_status();
                        
                        v = fp_arithmetic(&src, &dst, cmdreg1b);
                        
                        if (v)
                            regs.fp[(cmdreg1b>>7)&7] = dst;
                        
                        fpsr_check_arithmetic_exception(0, &src, regs.fp_opword, cmdreg1b, regs.fp_ea);
                    } else {
                        write_log (_T("FRESTORE resume of opclass %d instruction not supported!\n"), opclass);
                    }
                }
            } else if (frame_size == 0x30 || frame_size == 0x28) { // unimp
                
                
                // TODO: restore frame contents
                ad += frame_size;
                
            } else if (frame_size == 0x00) { // idle
                regs.fpu_state = 1;
                regs.fpu_exp_state = 0;
            } else {
                write_log (_T("FRESTORE invalid frame size %02X!\n"), frame_size);
                Exception(14);
                return;
            }
        } else if (frame_version == 0x00) { // null frame
            fpu_null();
        } else {
            write_log (_T("FRESTORE invalid frame version %02X!\n"), frame_version);
            Exception(14);
            return;
        }
    } else {
        // 6888x
        
        if (frame_version == fpu_version) { // not null frame
            uae_u32 biu_flags;
            uae_u32 frame_size = (d >> 16) & 0xff;
            regs.fpu_state = 1;
            
            if (frame_size == 0x18 || frame_size == 0x38) { // idle
                fsave_data.ccr = x_get_long (ad);
                ad += 4;
                // 68882 internal registers (32 bytes, unused)
                ad += frame_size - 24;
                fsave_data.eo[0] = x_get_long (ad);
                ad += 4;
                fsave_data.eo[1] = x_get_long (ad);
                ad += 4;
                fsave_data.eo[2] = x_get_long (ad);
                ad += 4;
                // operand register (unused)
                ad += 4;
                biu_flags = x_get_long (ad);
                ad += 4;
                
                if ((biu_flags & 0x08000000) == 0x00000000) {
                    regs.fpu_exp_state = 2;
                    regs.fp_exp_pend = fpsr_get_vector(regs.fpsr & regs.fpcr & 0xff00);
                } else {
                    regs.fpu_exp_state = 0;
                    regs.fp_exp_pend = 0;
                }
            } else if (frame_size == 0xB4 || frame_size == 0xD4) {
                write_log (_T("FRESTORE of busy frame not supported\n"));
                ad += frame_size;
            } else {
                write_log (_T("FRESTORE invalid frame size %02X!\n"), frame_size);
                Exception(14);
                return;
            }
        } else if (frame_version == 0x00) { // null frame
            fpu_null ();
        } else {
            write_log (_T("FRESTORE invalid frame version %02X!\n"), frame_version);
            Exception(14);
            return;
        }
    }
    
    if ((opcode & 0x38) == 0x18) // postincrement
        m68k_areg (regs, opcode & 7) = ad;
    
    fp_exception_pending(false);
}

void fpu_reset (void)
{
    regs.fpiar = 0;
    regs.fpu_exp_state = 0;
    fpp_set_fpcr (0);
    fpp_set_fpsr (0);
}

#if 0
uae_u8 *restore_fpu (uae_u8 *src)
{
    uae_u32 w1, w2, w3;
    int i;
    uae_u32 flags;
    
    fpu_reset();
    changed_prefs.fpu_model = currprefs.fpu_model = restore_u32 ();
    flags = restore_u32 ();
    for (i = 0; i < 8; i++) {
        w1 = restore_u16 () << 16;
        w2 = restore_u32 ();
        w3 = restore_u32 ();
        to_exten (&regs.fp[i], w1, w2, w3);
    }
    regs.fpcr = restore_u32 ();
    native_set_fpucw (regs.fpcr);
    regs.fpsr = restore_u32 ();
    regs.fpiar = restore_u32 ();
    if (flags & 0x80000000) {
        restore_u32 ();
        restore_u32 ();
    }
    if (flags & 0x40000000) {
        w1 = restore_u16() << 16;
        w2 = restore_u32();
        w3 = restore_u32();
        to_exten(&regs.exp_src1, w1, w2, w3);
        w1 = restore_u16() << 16;
        w2 = restore_u32();
        w3 = restore_u32();
        to_exten(&regs.exp_src2, w1, w2, w3);
        regs.exp_pack[0] = restore_u32();
        regs.exp_pack[1] = restore_u32();
        regs.exp_pack[2] = restore_u32();
        regs.exp_opcode = restore_u16();
        regs.exp_extra = restore_u16();
        regs.exp_type = restore_u16();
    }
    regs.fpu_state = (flags & 1) ? 0 : 1;
    regs.fpu_exp_state = (flags & 2) ? 1 : 0;
    if (flags & 4)
        regs.fpu_exp_state = 2;
    write_log(_T("FPU: %d\n"), currprefs.fpu_model);
    return src;
}

uae_u8 *save_fpu (int *len, uae_u8 *dstptr)
{
    uae_u32 w1, w2, w3;
    uae_u8 *dstbak, *dst;
    int i;
    
    *len = 0;
#ifndef WINUAE_FOR_HATARI
    /* Under Hatari, we save all FPU variables, even if fpu_model==0 */
    if (currprefs.fpu_model == 0)
        return 0;
#endif
    if (dstptr)
        dstbak = dst = dstptr;
    else
        dstbak = dst = xmalloc (uae_u8, 4+4+8*10+4+4+4+4+4+2*10+3*(4+2));
    save_u32 (currprefs.fpu_model);
    save_u32 (0x80000000 | 0x40000000 | (regs.fpu_state == 0 ? 1 : 0) | (regs.fpu_exp_state ? 2 : 0) | (regs.fpu_exp_state > 1 ? 4 : 0));
    for (i = 0; i < 8; i++) {
        from_exten (&regs.fp[i], &w1, &w2, &w3);
        save_u16 (w1 >> 16);
        save_u32 (w2);
        save_u32 (w3);
    }
    save_u32 (regs.fpcr);
    save_u32 (regs.fpsr);
    save_u32 (regs.fpiar);
    
    save_u32 (-1);
    save_u32 (0);
    
    from_exten(&regs.exp_src1, &w1, &w2, &w3);
    save_u16(w1 >> 16);
    save_u32(w2);
    save_u32(w3);
    from_exten(&regs.exp_src2, &w1, &w2, &w3);
    save_u16(w1 >> 16);
    save_u32(w2);
    save_u32(w3);
    save_u32(regs.exp_pack[0]);
    save_u32(regs.exp_pack[1]);
    save_u32(regs.exp_pack[2]);
    save_u16(regs.exp_opcode);
    save_u16(regs.exp_extra);
    save_u16(regs.exp_type);
    
    *len = dst - dstbak;
    return dstbak;
}
#endif