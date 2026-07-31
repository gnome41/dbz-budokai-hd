/*
 * SPU interpreter — faithful decode of the Cell BE SPU ISA.
 *
 * REWRITTEN (S2 milestone, see docs/SPU_DISPATCH_PLAN.md): the original
 * interpreter had a systematically wrong opcode table (verified against the
 * binutils/RPCS3 canonical table) plus three core semantic defects:
 *   1. RRR-format destination/RC fields swapped (selb/shufb/fma/fms/fnms/mpya
 *      wrote results to the wrong register), and selb/shufb opcodes swapped.
 *   2. lqd/stqd loaded raw big-endian LS bytes but arithmetic treated u32
 *      lanes as host-native — every value loaded from LS data arrived
 *      byte-swapped.
 *   3. shufb control-byte semantics wrong (special ranges + byte order).
 *
 * Register storage convention (see spu_interp.h): raw SPU byte order.
 * u8[0] = SPU byte 0 (MSB of the preferred word).  Loads/stores/DMA are raw
 * byte copies; word/halfword element ops go through GW/SW/GH/SH accessors.
 *
 * Opcode table source: RPCS3/binutils SPU instruction table (canonical).
 */
#include "spu_interp.h"
#include <math.h>

#ifdef _MSC_VER
# define sqrtf_impl sqrtf
#else
# define sqrtf_impl __builtin_sqrtf
#endif

/* Forward-declare RSX edge-write notifier from runtime_glue.cpp */
extern "C" void rsx_on_edge_write(uint32_t put_end_ea, uint32_t ls_src);

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static inline uint32_t ls_read32(const spu_ctx_t *ctx, uint32_t addr) {
    addr &= (SPU_LS_SIZE - 1) & ~3u;
    const uint8_t *p = ctx->ls + addr;
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static inline void ls_write32(spu_ctx_t *ctx, uint32_t addr, uint32_t v) {
    addr &= (SPU_LS_SIZE - 1) & ~3u;
    uint8_t *p = ctx->ls + addr;
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
/* Quadword LS access: raw byte copy — register storage is SPU byte order. */
static inline void ls_read128(const spu_ctx_t *ctx, uint32_t addr, spu_reg_t *dst) {
    addr &= (SPU_LS_SIZE - 1) & ~15u;
    memcpy(dst, ctx->ls + addr, 16);
}
static inline void ls_write128(spu_ctx_t *ctx, uint32_t addr, const spu_reg_t *src) {
    addr &= (SPU_LS_SIZE - 1) & ~15u;
    memcpy(ctx->ls + addr, src, 16);
}

static inline uint32_t sign_ext7(uint32_t v)  { return (v & 0x40)    ? (v | 0xFFFFFF80u) : v; }
static inline uint32_t sign_ext10(uint32_t v) { return (v & 0x200)   ? (v | 0xFFFFFC00u) : v; }
static inline uint32_t sign_ext16(uint32_t v) { return (v & 0x8000)  ? (v | 0xFFFF0000u) : v; }
static inline uint32_t sign_ext18(uint32_t v) { return (v & 0x20000) ? (v | 0xFFFC0000u) : v; }

/* Extract instruction fields */
#define F_RT(i)   ((i) & 0x7F)
#define F_RA(i)   (((i)>>7) & 0x7F)
#define F_RB(i)   (((i)>>14) & 0x7F)
#define F_RC(i)   (((i)>>21) & 0x7F)   /* RRR: this field is the DESTINATION */
#define F_I7(i)   (((i)>>14) & 0x7F)
#define F_I8(i)   (((i)>>14) & 0xFF)   /* conversion scale immediate */
#define F_I10(i)  sign_ext10(((i)>>14)&0x3FF)
#define F_I10U(i) (((i)>>14)&0x3FF)
#define F_I16(i)  sign_ext16(((i)>>7)&0xFFFF)
#define F_I16U(i) (((i)>>7)&0xFFFF)
#define F_I18(i)  (((i)>>7)&0x3FFFF)
#define F_CH(i)   (((i)>>7) & 0x1F)    /* channel address */
#define F_OP11(i) (((i)>>21)&0x7FF)
#define F_OP9(i)  (((i)>>23)&0x1FF)
#define F_OP8(i)  (((i)>>24)&0xFF)
#define F_OP7(i)  (((i)>>25)&0x7F)
#define F_OP4(i)  (((i)>>28)&0xF)

/* Register element accessors (see spu_interp.h storage convention) */
#define GW(r,i)    spu_reg_get_w(&(r), (i))
#define SW(r,i,v)  spu_reg_set_w(&(r), (i), (v))
#define GH(r,i)    spu_reg_get_h(&(r), (i))
#define SH(r,i,v)  spu_reg_set_h(&(r), (i), (v))

static inline float f32_from_bits(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline uint32_t bits_from_f32(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static inline double f64_from_bits(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static inline uint64_t bits_from_f64(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
/* double element i = words 2i (high) and 2i+1 (low) */
static inline double get_d(const spu_reg_t *r, int i) {
    return f64_from_bits(((uint64_t)spu_reg_get_w(r,2*i)<<32) | spu_reg_get_w(r,2*i+1));
}
static inline void set_d(spu_reg_t *r, int i, double d) {
    uint64_t b = bits_from_f64(d);
    spu_reg_set_w(r, 2*i,   (uint32_t)(b>>32));
    spu_reg_set_w(r, 2*i+1, (uint32_t)b);
}
static inline float get_f(const spu_reg_t *r, int i) { return f32_from_bits(spu_reg_get_w(r,i)); }
static inline void  set_f(spu_reg_t *r, int i, float f) { spu_reg_set_w(r, i, bits_from_f32(f)); }

/* SPU channel numbers (IBM Cell BE ISA) */
#define CH_SPU_RdEventStat   0
#define CH_SPU_WrEventMask   1
#define CH_SPU_WrEventAck    2
#define CH_SPU_RdSigNotify1  3
#define CH_SPU_RdSigNotify2  4
#define CH_SPU_WrDec         7
#define CH_SPU_RdDec         8
#define CH_MFC_WrMSSyncReq   9
#define CH_SPU_RdEventMask  11
#define CH_SPU_RdMachStat   13
#define CH_SPU_WrSRR0       14
#define CH_SPU_RdSRR0       15
#define CH_MFC_LSA          16
#define CH_MFC_EAH          17
#define CH_MFC_EAL          18
#define CH_MFC_Size         19
#define CH_MFC_TagID        20
#define CH_MFC_Cmd          21
#define CH_MFC_WrTagMask    22
#define CH_MFC_WrTagUpdate  23
#define CH_MFC_RdTagStat    24
#define CH_MFC_RdListStallStat 25
#define CH_MFC_WrListStallAck  26
#define CH_MFC_RdAtomicStat    27
#define CH_SPU_WrOutMbox    28
#define CH_SPU_RdInMbox     29
#define CH_SPU_WrOutIntrMbox 30

/* MFC DMA command codes */
#define MFC_PUT  0x20
#define MFC_PUTB 0x22
#define MFC_PUTF 0x24
#define MFC_GET  0x40
#define MFC_GETB 0x42
#define MFC_GETF 0x44

/* --------------------------------------------------------------------------
 * DMA
 * -------------------------------------------------------------------------- */
static void mfc_execute(spu_ctx_t *ctx, uint32_t cmd) {
    if (!ctx->vm_base) return;
    uint64_t ea = ((uint64_t)ctx->mfc_eah << 32) | ctx->mfc_eal;
    uint32_t lsa = ctx->mfc_lsa & (SPU_LS_SIZE - 1);
    uint32_t sz  = ctx->mfc_size;
    if (sz > 16384) sz = 16384;

    uint32_t ea32 = (uint32_t)ea;

    switch (cmd & 0xFF) {
        case MFC_GET: case MFC_GETB: case MFC_GETF:
            if (ctx->verbose)
                fprintf(stderr, "[SPU%d] DMA GET  ea=0x%08X ls=0x%05X sz=0x%X tag=%u\n",
                        ctx->id, ea32, lsa, sz, ctx->mfc_tag);
            /* LS-mapped DMA: EDGE maps its own LS into PPU address space at 0xFE000000.
             * DMA GET from that range copies LS[ea-0xFE000000] → LS[lsa]. */
            if (ea32 >= 0xFE000000u && ea32 + sz <= 0xFF000000u) {
                uint32_t ls_src = ea32 - 0xFE000000u;
                if (ls_src + sz <= SPU_LS_SIZE) {
                    if (ctx->verbose)
                        fprintf(stderr, "[SPU%d]   LS→LS GET ls_src=0x%X\n", ctx->id, ls_src);
                    memcpy(ctx->ls + lsa, ctx->ls + ls_src, sz);
                }
            } else if (ctx->vm_base && ea32 >= 0x10000 && ea32 + sz < 0x40000000u)
                memcpy(ctx->ls + lsa, ctx->vm_base + ea32, sz);
            break;
        case MFC_PUT: case MFC_PUTB: case MFC_PUTF:
            if (ctx->verbose)
                fprintf(stderr, "[SPU%d] DMA PUT  ea=0x%08X ls=0x%05X sz=0x%X tag=%u\n",
                        ctx->id, ea32, lsa, sz, ctx->mfc_tag);
            /* LS-mapped DMA: PUT writes from LS[lsa] → LS[ea-0xFE000000]. */
            if (ea32 >= 0xFE000000u && ea32 + sz <= 0xFF000000u) {
                uint32_t ls_dst = ea32 - 0xFE000000u;
                if (ls_dst + sz <= SPU_LS_SIZE) {
                    if (ctx->verbose)
                        fprintf(stderr, "[SPU%d]   LS→LS PUT ls_dst=0x%X\n", ctx->id, ls_dst);
                    memcpy(ctx->ls + ls_dst, ctx->ls + lsa, sz);
                }
            } else {
                if (ctx->vm_base && ea32 >= 0x10000 && ea32 + sz < 0x40000000u)
                    memcpy(ctx->vm_base + ea32, ctx->ls + lsa, sz);
                /* Commit EDGE geometry output to RSX IO region (render thread reads at 0xD0180000+) */
                if (ea32 >= 0xD0100000u && ea32 + sz <= 0xD0200000u) {
                    if (ctx->vm_base)
                        memcpy(ctx->vm_base + ea32, ctx->ls + lsa, sz);
                    /* rsx_on_edge_write not called here — render thread owns the rasterizer */
                }
            }
            break;
        default:
            break;
    }
}

/* --------------------------------------------------------------------------
 * Channel read/write
 * -------------------------------------------------------------------------- */
static void wrch(spu_ctx_t *ctx, uint32_t ch, uint32_t val) {
    switch (ch) {
        case CH_MFC_LSA:
            if (ctx->verbose)
                fprintf(stderr, "[SPU%d] wrch MFC_LSA=0x%X\n", ctx->id, val);
            ctx->mfc_lsa  = val; break;
        case CH_MFC_EAH:
            if (ctx->verbose)
                fprintf(stderr, "[SPU%d] wrch MFC_EAH=0x%X\n", ctx->id, val);
            ctx->mfc_eah  = val; break;
        case CH_MFC_EAL:
            if (ctx->verbose)
                fprintf(stderr, "[SPU%d] wrch MFC_EAL=0x%X @pc=0x%X\n", ctx->id, val, ctx->pc);
            ctx->mfc_eal  = val; break;
        case CH_MFC_Size:
            if (ctx->verbose)
                fprintf(stderr, "[SPU%d] wrch MFC_Size=0x%X\n", ctx->id, val);
            ctx->mfc_size = val; break;
        case CH_MFC_TagID:      ctx->mfc_tag  = val; break;
        case CH_MFC_Cmd:        mfc_execute(ctx, val); break;
        case CH_MFC_WrTagMask:  ctx->mfc_tagmask = val; break;
        case CH_MFC_WrTagUpdate: ctx->tag_update_pending = 1; break;
        case CH_SPU_WrOutMbox:
            ctx->outbound_mbox       = val;
            ctx->outbound_mbox_count = 1;
            break;
        case CH_SPU_WrOutIntrMbox:
            ctx->outbound_mbox       = val;
            ctx->outbound_mbox_count = 1;
            break;
        case CH_SPU_WrEventMask: break;
        case CH_SPU_WrEventAck:  break;
        case CH_SPU_WrDec:       ctx->decr = val; break;
        case CH_SPU_WrSRR0:      ctx->pc = val; break;
        case CH_MFC_WrListStallAck: break;
        default:
            fprintf(stderr, "[SPU%d] wrch unknown ch=%u val=0x%X\n", ctx->id, ch, val);
            break;
    }
}

static uint32_t rdch(spu_ctx_t *ctx, uint32_t ch) {
    switch (ch) {
        case CH_MFC_RdTagStat:
            ctx->tag_update_pending = 0;
            return ctx->mfc_tagmask;  /* all tags done (synchronous DMA) */
        case CH_SPU_RdInMbox:
            if (ctx->inbound_mbox_count > 0) {
                ctx->inbound_mbox_count = 0;
                return ctx->inbound_mbox;
            }
            fprintf(stderr, "[SPU%d] rdch InMbox: no data (blocking would occur)\n", ctx->id);
            ctx->running = 0;   /* stop until PPU sends data */
            return 0;
        case CH_SPU_RdSigNotify1:
            if (ctx->signal_count[0]) { ctx->signal_count[0]--; return ctx->signal[0]; }
            ctx->running = 0;
            return 0;
        case CH_SPU_RdSigNotify2:
            if (ctx->signal_count[1]) { ctx->signal_count[1]--; return ctx->signal[1]; }
            ctx->running = 0;
            return 0;
        case CH_SPU_RdDec:      return ctx->decr;
        case CH_SPU_RdMachStat: return 0;
        case CH_SPU_RdEventStat:return 0;
        case CH_SPU_RdEventMask:return 0;
        case CH_SPU_RdSRR0:     return ctx->pc;
        case CH_MFC_RdListStallStat: return 0;
        case CH_MFC_RdAtomicStat:    return 0;
        default:
            fprintf(stderr, "[SPU%d] rdch unknown ch=%u\n", ctx->id, ch);
            return 0;
    }
}

static uint32_t rchcnt(spu_ctx_t *ctx, uint32_t ch) {
    switch (ch) {
        case CH_SPU_RdInMbox:     return ctx->inbound_mbox_count;
        case CH_SPU_WrOutMbox:    return (ctx->outbound_mbox_count == 0) ? 1 : 0;
        case CH_MFC_WrTagUpdate:  return 1;
        case CH_MFC_RdTagStat:    return 1;
        case CH_SPU_RdSigNotify1: return ctx->signal_count[0];
        case CH_SPU_RdSigNotify2: return ctx->signal_count[1];
        default: return 0;
    }
}

/* --------------------------------------------------------------------------
 * shufb — SPU byte numbering (byte 0 = leftmost); storage is already in SPU
 * order so indices are direct.  Control byte semantics per ISA:
 *   10xxxxxx -> 0x00,  110xxxxx -> 0xFF,  111xxxxx -> 0x80,
 *   else concat(RA,RB) byte (sel & 0x1F).
 * -------------------------------------------------------------------------- */
static spu_reg_t do_shufb(const spu_reg_t *a, const spu_reg_t *b, const spu_reg_t *c) {
    spu_reg_t d;
    for (int i = 0; i < 16; i++) {
        uint8_t sel = c->u8[i];
        if      ((sel & 0xC0) == 0x80) d.u8[i] = 0x00;
        else if ((sel & 0xE0) == 0xC0) d.u8[i] = 0xFF;
        else if ((sel & 0xE0) == 0xE0) d.u8[i] = 0x80;
        else {
            uint32_t k = sel & 0x1F;
            d.u8[i] = (k < 16) ? a->u8[k] : b->u8[k - 16];
        }
    }
    return d;
}

/* Generate-controls helper (cbd/chd/cwd/cdd, cbx/chx/cwx/cdx) */
static void gen_controls(spu_reg_t *t, uint32_t pos, int size) {
    for (int k = 0; k < 16; k++) t->u8[k] = (uint8_t)(0x10 + k);
    pos &= 0xF & ~(uint32_t)(size - 1);
    for (int k = 0; k < size; k++) t->u8[pos + k] = (uint8_t)k;
}

/* 128-bit quadword shift helpers — operate directly on SPU byte order.
 * "Left" moves bytes toward u8[0] (the MSB end). */
static spu_reg_t q_shl_bytes(const spu_reg_t *a, uint32_t n) {
    spu_reg_t t; memset(&t, 0, 16);
    if (n < 16) for (uint32_t k = 0; k + n < 16; k++) t.u8[k] = a->u8[k + n];
    return t;
}
static spu_reg_t q_shr_bytes(const spu_reg_t *a, uint32_t n) {  /* rotqmby* */
    spu_reg_t t; memset(&t, 0, 16);
    if (n < 16) for (uint32_t k = n; k < 16; k++) t.u8[k] = a->u8[k - n];
    return t;
}
static spu_reg_t q_rot_bytes(const spu_reg_t *a, uint32_t n) {
    spu_reg_t t;
    for (int k = 0; k < 16; k++) t.u8[k] = a->u8[(k + n) & 15];
    return t;
}
static spu_reg_t q_shl_bits(const spu_reg_t *a, uint32_t s) {   /* s = 0..7 */
    spu_reg_t t;
    if (!s) { t = *a; return t; }
    for (int k = 0; k < 15; k++)
        t.u8[k] = (uint8_t)((a->u8[k] << s) | (a->u8[k+1] >> (8 - s)));
    t.u8[15] = (uint8_t)(a->u8[15] << s);
    return t;
}
static spu_reg_t q_shr_bits(const spu_reg_t *a, uint32_t s) {   /* rotqmbi* */
    spu_reg_t t;
    if (!s) { t = *a; return t; }
    for (int k = 15; k > 0; k--)
        t.u8[k] = (uint8_t)((a->u8[k] >> s) | (a->u8[k-1] << (8 - s)));
    t.u8[0] = (uint8_t)(a->u8[0] >> s);
    return t;
}
static spu_reg_t q_rot_bits(const spu_reg_t *a, uint32_t s) {   /* s = 0..7 */
    spu_reg_t t;
    if (!s) { t = *a; return t; }
    for (int k = 0; k < 16; k++)
        t.u8[k] = (uint8_t)((a->u8[k] << s) | (a->u8[(k+1) & 15] >> (8 - s)));
    return t;
}

/* --------------------------------------------------------------------------
 * Single-step
 * -------------------------------------------------------------------------- */
void spu_step(spu_ctx_t *ctx) {
    uint32_t pc = ctx->pc & (SPU_LS_SIZE - 1);

    /* PUT EA fixup for EDGE geometry processor: before the load at 0x35FC,
     * patch the stream descriptor's PUT-EA field (bytes 4-7, big-endian) to
     * 0xD0100000 so EDGE writes to the RSX buffer.
     * Guard: if r49/r124 are 0 use descriptor base 0xADD0 to avoid corrupting
     * the stop-signal area at LS[0..63].
     * NOTE: byte order fixed to big-endian with the faithful interpreter. */
    if (pc == 0x35FCu && ctx->id == 2) {
        uint32_t r49    = GW(ctx->gpr[49], 0);
        uint32_t r124   = GW(ctx->gpr[124], 0);
        uint32_t lsaddr = (r49 + r124) & ~15u & (SPU_LS_SIZE - 1);
        if (lsaddr < 0x80u) {
            lsaddr = 0xADD0u;
            SW(ctx->gpr[49], 0, lsaddr);
            SW(ctx->gpr[124], 0, 0);
        }
        ctx->ls[lsaddr+4] = 0xD0;
        ctx->ls[lsaddr+5] = 0x10;
        ctx->ls[lsaddr+6] = 0x00;
        ctx->ls[lsaddr+7] = 0x00;
    }

    uint32_t insn = ls_read32(ctx, pc);
    ctx->pc = (pc + 4) & (SPU_LS_SIZE - 1);

    /* Optional per-instruction trace */
    if (ctx->trace_limit && ctx->trace_count < ctx->trace_limit) {
        ctx->trace_count++;
        fprintf(stderr, "[SPU%d:%06llu] PC=0x%04X insn=0x%08X r3=%X r4=%X r79=%X\n",
                ctx->id, (unsigned long long)ctx->trace_count,
                pc, insn,
                GW(ctx->gpr[3], 0), GW(ctx->gpr[4], 0), GW(ctx->gpr[79], 0));
        fflush(stderr);
    }

    uint32_t op4  = F_OP4(insn);
    uint32_t op7  = F_OP7(insn);
    uint32_t op8  = F_OP8(insn);
    uint32_t op9  = F_OP9(insn);
    uint32_t op11 = F_OP11(insn);
    uint32_t rt   = F_RT(insn);
    uint32_t ra   = F_RA(insn);
    uint32_t rb   = F_RB(insn);

    spu_reg_t *R  = ctx->gpr;

    /* ---- RRR format (4-bit opcode; op4 >= 0x8 is exclusively RRR) --------
     * Destination is the high field (bits 21-27); low field (bits 0-6) is RC. */
    if (op4 >= 0x8) {
        uint32_t rt4 = F_RC(insn);   /* destination */
        uint32_t rc  = F_RT(insn);   /* third source operand */
        switch (op4) {
            case 0x8: { /* selb RT4, RA, RB, RC — bitwise select */
                spu_reg_t t;
                for (int i = 0; i < 16; i++)
                    t.u8[i] = (uint8_t)((R[rc].u8[i] & R[rb].u8[i]) | (~R[rc].u8[i] & R[ra].u8[i]));
                R[rt4] = t; return; }
            case 0xB: { /* shufb */
                spu_reg_t t = do_shufb(&R[ra], &R[rb], &R[rc]);
                R[rt4] = t; return; }
            case 0xC: { /* mpya RT4 = lo16(RA)*lo16(RB) + RC (per word) */
                spu_reg_t t;
                for (int i = 0; i < 4; i++) {
                    int32_t p = (int32_t)(int16_t)(GW(R[ra],i) & 0xFFFF) *
                                (int32_t)(int16_t)(GW(R[rb],i) & 0xFFFF);
                    SW(t, i, (uint32_t)(p + (int32_t)GW(R[rc],i)));
                }
                R[rt4] = t; return; }
            case 0xD: { /* fnms — RC - RA*RB */
                spu_reg_t t;
                for (int i = 0; i < 4; i++) set_f(&t, i, get_f(&R[rc],i) - get_f(&R[ra],i)*get_f(&R[rb],i));
                R[rt4] = t; return; }
            case 0xE: { /* fma — RA*RB + RC */
                spu_reg_t t;
                for (int i = 0; i < 4; i++) set_f(&t, i, get_f(&R[ra],i)*get_f(&R[rb],i) + get_f(&R[rc],i));
                R[rt4] = t; return; }
            case 0xF: { /* fms — RA*RB - RC */
                spu_reg_t t;
                for (int i = 0; i < 4; i++) set_f(&t, i, get_f(&R[ra],i)*get_f(&R[rb],i) - get_f(&R[rc],i));
                R[rt4] = t; return; }
        }
        goto unknown;
    }

    /* ---- RI18 format (7-bit opcode) -------------------------------------- */
    switch (op7) {
        case 0x21: { /* ila RT, I18 (zero-extended, all words) */
            uint32_t v = F_I18(insn);
            for (int i=0;i<4;i++) SW(R[rt], i, v); return; }
        case 0x08:   /* hbra — branch hint, no architectural effect */
        case 0x09:   /* hbrr */
            return;
    }

    /* ---- RI16 format (9-bit opcode) -------------------------------------- */
    switch (op9) {
        case 0x040: { /* brz RT, I16 */
            if (GW(R[rt],0) == 0)
                ctx->pc = (pc + (uint32_t)(int32_t)(F_I16(insn) << 2)) & (SPU_LS_SIZE-1);
            return; }
        case 0x042: { /* brnz */
            if (GW(R[rt],0) != 0)
                ctx->pc = (pc + (uint32_t)(int32_t)(F_I16(insn) << 2)) & (SPU_LS_SIZE-1);
            return; }
        case 0x044: { /* brhz — tests the preferred halfword (low 16 of word 0) */
            if ((GW(R[rt],0) & 0xFFFF) == 0)
                ctx->pc = (pc + (uint32_t)(int32_t)(F_I16(insn) << 2)) & (SPU_LS_SIZE-1);
            return; }
        case 0x046: { /* brhnz */
            if ((GW(R[rt],0) & 0xFFFF) != 0)
                ctx->pc = (pc + (uint32_t)(int32_t)(F_I16(insn) << 2)) & (SPU_LS_SIZE-1);
            return; }
        case 0x041: { /* stqa RT, I16 */
            uint32_t ea = (F_I16U(insn) << 2) & (SPU_LS_SIZE-1) & ~15u;
            ls_write128(ctx, ea, &R[rt]); return; }
        case 0x047: { /* stqr RT, I16 (PC-relative) */
            uint32_t ea = (pc + (uint32_t)(int32_t)(F_I16(insn) << 2)) & (SPU_LS_SIZE-1) & ~15u;
            ls_write128(ctx, ea, &R[rt]); return; }
        case 0x061: { /* lqa RT, I16 */
            uint32_t ea = (F_I16U(insn) << 2) & (SPU_LS_SIZE-1) & ~15u;
            ls_read128(ctx, ea, &R[rt]); return; }
        case 0x067: { /* lqr RT, I16 (PC-relative) */
            uint32_t ea = (pc + (uint32_t)(int32_t)(F_I16(insn) << 2)) & (SPU_LS_SIZE-1) & ~15u;
            ls_read128(ctx, ea, &R[rt]); return; }
        case 0x060: { /* bra */
            ctx->pc = ((F_I16U(insn) << 2)) & (SPU_LS_SIZE-1); return; }
        case 0x062: { /* brasl */
            memset(&R[rt], 0, 16); SW(R[rt], 0, ctx->pc);
            ctx->pc = ((F_I16U(insn) << 2)) & (SPU_LS_SIZE-1); return; }
        case 0x064: { /* br */
            ctx->pc = (pc + (uint32_t)(int32_t)(F_I16(insn) << 2)) & (SPU_LS_SIZE-1); return; }
        case 0x066: { /* brsl */
            memset(&R[rt], 0, 16); SW(R[rt], 0, ctx->pc);
            ctx->pc = (pc + (uint32_t)(int32_t)(F_I16(insn) << 2)) & (SPU_LS_SIZE-1); return; }
        case 0x065: { /* fsmbi RT, I16 — bit (15-k) -> byte k */
            uint32_t m = F_I16U(insn);
            for (int k=0;k<16;k++) R[rt].u8[k] = (uint8_t)(((m >> (15-k)) & 1) ? 0xFF : 0x00);
            return; }
        case 0x081: { /* il RT, I16 (sign-extended, all words) */
            uint32_t v = sign_ext16(F_I16U(insn));
            for (int i=0;i<4;i++) SW(R[rt], i, v); return; }
        case 0x082: { /* ilhu */
            uint32_t v = F_I16U(insn) << 16;
            for (int i=0;i<4;i++) SW(R[rt], i, v); return; }
        case 0x083: { /* ilh (all 8 halfwords) */
            uint16_t v = (uint16_t)F_I16U(insn);
            for (int i=0;i<8;i++) SH(R[rt], i, v); return; }
        case 0x0C1: { /* iohl — OR I16 into all words */
            uint32_t v = F_I16U(insn);
            for (int i=0;i<4;i++) SW(R[rt], i, GW(R[rt],i) | v); return; }
    }

    /* ---- RI10 format (8-bit opcode) -------------------------------------- */
    switch (op8) {
        case 0x34: { /* lqd RT, I10(RA) */
            uint32_t ea = (GW(R[ra],0) + ((uint32_t)F_I10(insn) << 4)) & (SPU_LS_SIZE-1) & ~15u;
            ls_read128(ctx, ea, &R[rt]); return; }
        case 0x24: { /* stqd */
            uint32_t ea = (GW(R[ra],0) + ((uint32_t)F_I10(insn) << 4)) & (SPU_LS_SIZE-1) & ~15u;
            ls_write128(ctx, ea, &R[rt]); return; }
        case 0x1C: { /* ai */
            uint32_t v = F_I10(insn);
            for (int i=0;i<4;i++) SW(R[rt], i, GW(R[ra],i) + v); return; }
        case 0x0C: { /* sfi — I10 - RA */
            uint32_t v = F_I10(insn);
            for (int i=0;i<4;i++) SW(R[rt], i, v - GW(R[ra],i)); return; }
        case 0x1D: { /* ahi */
            uint16_t v = (uint16_t)F_I10(insn);
            for (int i=0;i<8;i++) SH(R[rt], i, (uint16_t)(GH(R[ra],i) + v)); return; }
        case 0x0D: { /* sfhi */
            uint16_t v = (uint16_t)F_I10(insn);
            for (int i=0;i<8;i++) SH(R[rt], i, (uint16_t)(v - GH(R[ra],i))); return; }
        case 0x14: { /* andi */
            uint32_t v = F_I10(insn);
            for (int i=0;i<4;i++) SW(R[rt], i, GW(R[ra],i) & v); return; }
        case 0x15: { /* andhi */
            uint16_t v = (uint16_t)F_I10(insn);
            for (int i=0;i<8;i++) SH(R[rt], i, (uint16_t)(GH(R[ra],i) & v)); return; }
        case 0x16: { /* andbi */
            uint8_t v = (uint8_t)F_I10(insn);
            for (int i=0;i<16;i++) R[rt].u8[i] = (uint8_t)(R[ra].u8[i] & v); return; }
        case 0x04: { /* ori */
            uint32_t v = F_I10(insn);
            for (int i=0;i<4;i++) SW(R[rt], i, GW(R[ra],i) | v); return; }
        case 0x05: { /* orhi */
            uint16_t v = (uint16_t)F_I10(insn);
            for (int i=0;i<8;i++) SH(R[rt], i, (uint16_t)(GH(R[ra],i) | v)); return; }
        case 0x06: { /* orbi */
            uint8_t v = (uint8_t)F_I10(insn);
            for (int i=0;i<16;i++) R[rt].u8[i] = (uint8_t)(R[ra].u8[i] | v); return; }
        case 0x44: { /* xori */
            uint32_t v = F_I10(insn);
            for (int i=0;i<4;i++) SW(R[rt], i, GW(R[ra],i) ^ v); return; }
        case 0x45: { /* xorhi */
            uint16_t v = (uint16_t)F_I10(insn);
            for (int i=0;i<8;i++) SH(R[rt], i, (uint16_t)(GH(R[ra],i) ^ v)); return; }
        case 0x46: { /* xorbi */
            uint8_t v = (uint8_t)F_I10(insn);
            for (int i=0;i<16;i++) R[rt].u8[i] = (uint8_t)(R[ra].u8[i] ^ v); return; }
        case 0x7C: { /* ceqi */
            uint32_t v = F_I10(insn);
            for (int i=0;i<4;i++) SW(R[rt], i, (GW(R[ra],i)==v)?~0u:0u); return; }
        case 0x7D: { /* ceqhi */
            uint16_t v = (uint16_t)F_I10(insn);
            for (int i=0;i<8;i++) SH(R[rt], i, (GH(R[ra],i)==v)?0xFFFF:0); return; }
        case 0x7E: { /* ceqbi */
            uint8_t v = (uint8_t)F_I10(insn);
            for (int i=0;i<16;i++) R[rt].u8[i] = (R[ra].u8[i]==v)?0xFF:0; return; }
        case 0x4C: { /* cgti */
            int32_t v = (int32_t)F_I10(insn);
            for (int i=0;i<4;i++) SW(R[rt], i, ((int32_t)GW(R[ra],i)>v)?~0u:0u); return; }
        case 0x4D: { /* cgthi */
            int16_t v = (int16_t)F_I10(insn);
            for (int i=0;i<8;i++) SH(R[rt], i, ((int16_t)GH(R[ra],i)>v)?0xFFFF:0); return; }
        case 0x4E: { /* cgtbi */
            int8_t v = (int8_t)F_I10(insn);
            for (int i=0;i<16;i++) R[rt].u8[i] = (R[ra].s8[i]>v)?0xFF:0; return; }
        case 0x5C: { /* clgti */
            uint32_t v = F_I10(insn);
            for (int i=0;i<4;i++) SW(R[rt], i, (GW(R[ra],i)>v)?~0u:0u); return; }
        case 0x5D: { /* clgthi */
            uint16_t v = (uint16_t)F_I10(insn);
            for (int i=0;i<8;i++) SH(R[rt], i, (GH(R[ra],i)>v)?0xFFFF:0); return; }
        case 0x5E: { /* clgtbi */
            uint8_t v = (uint8_t)F_I10(insn);
            for (int i=0;i<16;i++) R[rt].u8[i] = (R[ra].u8[i]>v)?0xFF:0; return; }
        case 0x74: { /* mpyi — lo16(RA) * sext(I10), per word */
            int32_t v = (int32_t)F_I10(insn);
            for (int i=0;i<4;i++)
                SW(R[rt], i, (uint32_t)((int32_t)(int16_t)(GW(R[ra],i)&0xFFFF) * v));
            return; }
        case 0x75: { /* mpyui */
            uint32_t v = sign_ext10(F_I10U(insn)) & 0xFFFF;
            for (int i=0;i<4;i++)
                SW(R[rt], i, (uint32_t)(GW(R[ra],i)&0xFFFF) * v);
            return; }
        case 0x4F: case 0x5F: case 0x7F: /* hgti / hlgti / heqi — halt: ignore */
            return;
    }

    /* ---- Channel instructions -------------------------------------------- */
    if (op11 == 0x00D) { /* rdch */
        uint32_t ch = F_CH(insn);
        uint32_t val = rdch(ctx, ch);
        if (ctx->verbose)
            fprintf(stderr, "[SPU%d] rdch ch=%u val=0x%X\n", ctx->id, ch, val);
        memset(&R[rt], 0, 16); SW(R[rt], 0, val);
        return;
    }
    if (op11 == 0x10D) { /* wrch */
        uint32_t ch = F_CH(insn);
        uint32_t val = GW(R[rt], 0);
        if (ctx->verbose)
            fprintf(stderr, "[SPU%d] wrch ch=%u val=0x%X\n", ctx->id, ch, val);
        wrch(ctx, ch, val);
        return;
    }
    if (op11 == 0x00F) { /* rchcnt */
        uint32_t ch = F_CH(insn);
        memset(&R[rt], 0, 16); SW(R[rt], 0, rchcnt(ctx, ch));
        return;
    }

    /* ---- RR / RI7 format (11-bit opcode) ---------------------------------- */
    switch (op11) {
        /* Control */
        case 0x000: /* stop */
        case 0x140: /* stopd */
            ctx->stop_code = insn & 0x3FFF;
            ctx->running = 0; return;
        case 0x001: /* lnop */  return;
        case 0x201: /* nop */   return;
        case 0x002: /* sync */  return;
        case 0x003: /* dsync */ return;
        case 0x00C: /* mfspr */ memset(&R[rt], 0, 16); return;
        case 0x10C: /* mtspr */ return;
        case 0x1AC: /* hbr */   return;

        /* Memory */
        case 0x1C4: { /* lqx */
            uint32_t ea = (GW(R[ra],0)+GW(R[rb],0)) & (SPU_LS_SIZE-1) & ~15u;
            ls_read128(ctx, ea, &R[rt]); return; }
        case 0x144: { /* stqx */
            uint32_t ea = (GW(R[ra],0)+GW(R[rb],0)) & (SPU_LS_SIZE-1) & ~15u;
            ls_write128(ctx, ea, &R[rt]); return; }

        /* Generate controls for insertion */
        case 0x1F4: { spu_reg_t t; gen_controls(&t, GW(R[ra],0) + sign_ext7(F_I7(insn)), 1); R[rt]=t; return; } /* cbd */
        case 0x1F5: { spu_reg_t t; gen_controls(&t, GW(R[ra],0) + sign_ext7(F_I7(insn)), 2); R[rt]=t; return; } /* chd */
        case 0x1F6: { spu_reg_t t; gen_controls(&t, GW(R[ra],0) + sign_ext7(F_I7(insn)), 4); R[rt]=t; return; } /* cwd */
        case 0x1F7: { spu_reg_t t; gen_controls(&t, GW(R[ra],0) + sign_ext7(F_I7(insn)), 8); R[rt]=t; return; } /* cdd */
        case 0x1D4: { spu_reg_t t; gen_controls(&t, GW(R[ra],0) + GW(R[rb],0), 1); R[rt]=t; return; } /* cbx */
        case 0x1D5: { spu_reg_t t; gen_controls(&t, GW(R[ra],0) + GW(R[rb],0), 2); R[rt]=t; return; } /* chx */
        case 0x1D6: { spu_reg_t t; gen_controls(&t, GW(R[ra],0) + GW(R[rb],0), 4); R[rt]=t; return; } /* cwx */
        case 0x1D7: { spu_reg_t t; gen_controls(&t, GW(R[ra],0) + GW(R[rb],0), 8); R[rt]=t; return; } /* cdx */

        /* Integer arithmetic */
        case 0x0C0: { /* a */
            for (int i=0;i<4;i++) SW(R[rt], i, GW(R[ra],i)+GW(R[rb],i)); return; }
        case 0x040: { /* sf — RB - RA */
            for (int i=0;i<4;i++) SW(R[rt], i, GW(R[rb],i)-GW(R[ra],i)); return; }
        case 0x0C8: { /* ah */
            for (int i=0;i<8;i++) SH(R[rt], i, (uint16_t)(GH(R[ra],i)+GH(R[rb],i))); return; }
        case 0x048: { /* sfh */
            for (int i=0;i<8;i++) SH(R[rt], i, (uint16_t)(GH(R[rb],i)-GH(R[ra],i))); return; }
        case 0x0C2: { /* cg — carry generate */
            for (int i=0;i<4;i++) SW(R[rt], i, (uint32_t)(((uint64_t)GW(R[ra],i)+GW(R[rb],i))>>32)); return; }
        case 0x042: { /* bg — borrow generate: 1 if RB >= RA */
            for (int i=0;i<4;i++) SW(R[rt], i, (GW(R[ra],i)<=GW(R[rb],i))?1u:0u); return; }
        case 0x340: { /* addx */
            for (int i=0;i<4;i++) SW(R[rt], i, GW(R[ra],i)+GW(R[rb],i)+(GW(R[rt],i)&1)); return; }
        case 0x341: { /* sfx — RB + ~RA + (RT&1) */
            for (int i=0;i<4;i++) SW(R[rt], i, GW(R[rb],i)+~GW(R[ra],i)+(GW(R[rt],i)&1)); return; }
        case 0x342: { /* cgx */
            for (int i=0;i<4;i++) {
                uint64_t s = (uint64_t)GW(R[ra],i)+GW(R[rb],i)+(GW(R[rt],i)&1);
                SW(R[rt], i, (uint32_t)(s>>32));
            } return; }
        case 0x343: { /* bgx */
            for (int i=0;i<4;i++) {
                uint64_t s = (uint64_t)GW(R[rb],i)+(uint64_t)(uint32_t)~GW(R[ra],i)+(GW(R[rt],i)&1);
                SW(R[rt], i, (uint32_t)(s>>32));
            } return; }
        case 0x053: { /* absdb */
            for (int i=0;i<16;i++) {
                int d = (int)R[ra].u8[i]-(int)R[rb].u8[i];
                R[rt].u8[i]=(uint8_t)(d<0?-d:d);
            } return; }
        case 0x0D3: { /* avgb */
            for (int i=0;i<16;i++) R[rt].u8[i]=(uint8_t)(((int)R[ra].u8[i]+(int)R[rb].u8[i]+1)>>1);
            return; }
        case 0x253: { /* sumb — h[2i]=sum bytes RB word i; h[2i+1]=sum bytes RA word i */
            spu_reg_t t;
            for (int i=0;i<4;i++) {
                uint16_t sa=0, sb=0;
                for (int k=0;k<4;k++) { sa=(uint16_t)(sa+R[ra].u8[4*i+k]); sb=(uint16_t)(sb+R[rb].u8[4*i+k]); }
                SH(t, 2*i, sb); SH(t, 2*i+1, sa);
            }
            R[rt]=t; return; }
        case 0x2A5: { /* clz */
            for (int i=0;i<4;i++) {
                uint32_t v=GW(R[ra],i); int n=0;
                if (!v) { SW(R[rt], i, 32); continue; }
                while (!(v & 0x80000000u)) { n++; v<<=1; }
                SW(R[rt], i, (uint32_t)n);
            } return; }
        case 0x2B4: { /* cntb */
            for (int i=0;i<16;i++) {
                uint8_t v=R[ra].u8[i]; int n=0;
                while(v){n+=v&1;v>>=1;} R[rt].u8[i]=(uint8_t)n;
            } return; }
        case 0x2B6: { /* xsbh — halfword i = sext(low byte of halfword i) */
            for (int i=0;i<8;i++) SH(R[rt], i, (uint16_t)(int16_t)(int8_t)(GH(R[ra],i)&0xFF)); return; }
        case 0x2AE: { /* xshw — word i = sext(low halfword of word i) */
            for (int i=0;i<4;i++) SW(R[rt], i, (uint32_t)(int32_t)(int16_t)(GW(R[ra],i)&0xFFFF)); return; }
        case 0x2A6: { /* xswd — dword i = sext(word 2i+1) */
            spu_reg_t t;
            for (int i=0;i<2;i++) {
                int64_t v = (int64_t)(int32_t)GW(R[ra],2*i+1);
                SW(t, 2*i,   (uint32_t)((uint64_t)v>>32));
                SW(t, 2*i+1, (uint32_t)(uint64_t)v);
            }
            R[rt]=t; return; }
        case 0x1B0: { /* gb — gather LSBs of words; word0 element 0 is MSB of nibble */
            uint32_t v=0;
            for (int i=0;i<4;i++) v |= (GW(R[ra],i)&1) << (3-i);
            memset(&R[rt],0,16); SW(R[rt],0,v); return; }
        case 0x1B1: { /* gbh */
            uint32_t v=0;
            for (int i=0;i<8;i++) v |= (uint32_t)(GH(R[ra],i)&1) << (7-i);
            memset(&R[rt],0,16); SW(R[rt],0,v); return; }
        case 0x1B2: { /* gbb */
            uint32_t v=0;
            for (int i=0;i<16;i++) v |= (uint32_t)(R[ra].u8[i]&1) << (15-i);
            memset(&R[rt],0,16); SW(R[rt],0,v); return; }
        case 0x1B4: { /* fsm — expand 4 bits to word masks */
            uint32_t s = GW(R[ra],0);
            for (int i=0;i<4;i++) SW(R[rt], i, ((s>>(3-i))&1)?~0u:0u); return; }
        case 0x1B5: { /* fsmh */
            uint32_t s = GW(R[ra],0);
            for (int i=0;i<8;i++) SH(R[rt], i, ((s>>(7-i))&1)?0xFFFF:0); return; }
        case 0x1B6: { /* fsmb */
            uint32_t s = GW(R[ra],0);
            for (int i=0;i<16;i++) R[rt].u8[i] = (uint8_t)(((s>>(15-i))&1)?0xFF:0); return; }

        /* Logic */
        case 0x0C1: { /* and */
            for (int i=0;i<16;i++) R[rt].u8[i]=(uint8_t)(R[ra].u8[i]&R[rb].u8[i]); return; }
        case 0x041: { /* or */
            for (int i=0;i<16;i++) R[rt].u8[i]=(uint8_t)(R[ra].u8[i]|R[rb].u8[i]); return; }
        case 0x241: { /* xor */
            for (int i=0;i<16;i++) R[rt].u8[i]=(uint8_t)(R[ra].u8[i]^R[rb].u8[i]); return; }
        case 0x0C9: { /* nand */
            for (int i=0;i<16;i++) R[rt].u8[i]=(uint8_t)~(R[ra].u8[i]&R[rb].u8[i]); return; }
        case 0x049: { /* nor */
            for (int i=0;i<16;i++) R[rt].u8[i]=(uint8_t)~(R[ra].u8[i]|R[rb].u8[i]); return; }
        case 0x2C1: { /* andc — RA & ~RB */
            for (int i=0;i<16;i++) R[rt].u8[i]=(uint8_t)(R[ra].u8[i]&~R[rb].u8[i]); return; }
        case 0x2C9: { /* orc — RA | ~RB */
            for (int i=0;i<16;i++) R[rt].u8[i]=(uint8_t)(R[ra].u8[i]|~R[rb].u8[i]); return; }
        case 0x249: { /* eqv */
            for (int i=0;i<16;i++) R[rt].u8[i]=(uint8_t)~(R[ra].u8[i]^R[rb].u8[i]); return; }
        case 0x1F0: { /* orx — OR across words into word 0 */
            uint32_t v = GW(R[ra],0)|GW(R[ra],1)|GW(R[ra],2)|GW(R[ra],3);
            memset(&R[rt],0,16); SW(R[rt],0,v); return; }

        /* Element shifts/rotates (count from corresponding RB element) */
        case 0x058: { /* rot */
            for (int i=0;i<4;i++){uint32_t s=GW(R[rb],i)&31,v=GW(R[ra],i);
                SW(R[rt],i,s?((v<<s)|(v>>(32-s))):v);} return; }
        case 0x059: { /* rotm — logical right shift by (0-RB)&63 */
            for (int i=0;i<4;i++){uint32_t s=(0u-GW(R[rb],i))&63;
                SW(R[rt],i,(s<32)?(GW(R[ra],i)>>s):0u);} return; }
        case 0x05A: { /* rotma — arithmetic right shift */
            for (int i=0;i<4;i++){uint32_t s=(0u-GW(R[rb],i))&63; if(s>31)s=31;
                SW(R[rt],i,(uint32_t)((int32_t)GW(R[ra],i)>>s));} return; }
        case 0x05B: { /* shl */
            for (int i=0;i<4;i++){uint32_t s=GW(R[rb],i)&63;
                SW(R[rt],i,(s<32)?(GW(R[ra],i)<<s):0u);} return; }
        case 0x05C: { /* roth */
            for (int i=0;i<8;i++){uint32_t s=GH(R[rb],i)&15;uint16_t v=GH(R[ra],i);
                SH(R[rt],i,(uint16_t)(s?((v<<s)|(v>>(16-s))):v));} return; }
        case 0x05D: { /* rothm */
            for (int i=0;i<8;i++){uint32_t s=(0u-GH(R[rb],i))&31;
                SH(R[rt],i,(uint16_t)((s<16)?(GH(R[ra],i)>>s):0));} return; }
        case 0x05E: { /* rotmah */
            for (int i=0;i<8;i++){uint32_t s=(0u-GH(R[rb],i))&31; if(s>15)s=15;
                SH(R[rt],i,(uint16_t)((int16_t)GH(R[ra],i)>>s));} return; }
        case 0x05F: { /* shlh */
            for (int i=0;i<8;i++){uint32_t s=GH(R[rb],i)&31;
                SH(R[rt],i,(uint16_t)((s<16)?(GH(R[ra],i)<<s):0));} return; }
        case 0x078: { /* roti */
            uint32_t s=F_I7(insn)&31;
            for (int i=0;i<4;i++){uint32_t v=GW(R[ra],i);
                SW(R[rt],i,s?((v<<s)|(v>>(32-s))):v);} return; }
        case 0x079: { /* rotmi */
            uint32_t s=(0u-F_I7(insn))&63;
            for (int i=0;i<4;i++) SW(R[rt],i,(s<32)?(GW(R[ra],i)>>s):0u); return; }
        case 0x07A: { /* rotmai */
            uint32_t s=(0u-F_I7(insn))&63; if(s>31)s=31;
            for (int i=0;i<4;i++) SW(R[rt],i,(uint32_t)((int32_t)GW(R[ra],i)>>s)); return; }
        case 0x07B: { /* shli */
            uint32_t s=F_I7(insn)&63;
            for (int i=0;i<4;i++) SW(R[rt],i,(s<32)?(GW(R[ra],i)<<s):0u); return; }
        case 0x07C: { /* rothi */
            uint32_t s=F_I7(insn)&15;
            for (int i=0;i<8;i++){uint16_t v=GH(R[ra],i);
                SH(R[rt],i,(uint16_t)(s?((v<<s)|(v>>(16-s))):v));} return; }
        case 0x07D: { /* rothmi */
            uint32_t s=(0u-F_I7(insn))&31;
            for (int i=0;i<8;i++) SH(R[rt],i,(uint16_t)((s<16)?(GH(R[ra],i)>>s):0)); return; }
        case 0x07E: { /* rotmahi */
            uint32_t s=(0u-F_I7(insn))&31; if(s>15)s=15;
            for (int i=0;i<8;i++) SH(R[rt],i,(uint16_t)((int16_t)GH(R[ra],i)>>s)); return; }
        case 0x07F: { /* shlhi */
            uint32_t s=F_I7(insn)&31;
            for (int i=0;i<8;i++) SH(R[rt],i,(uint16_t)((s<16)?(GH(R[ra],i)<<s):0)); return; }

        /* Quadword shifts/rotates */
        case 0x1D8: { R[rt]=q_rot_bits(&R[ra], GW(R[rb],0)&7); return; }            /* rotqbi */
        case 0x1D9: { R[rt]=q_shr_bits(&R[ra], (0u-GW(R[rb],0))&7); return; }        /* rotqmbi */
        case 0x1DB: { R[rt]=q_shl_bits(&R[ra], GW(R[rb],0)&7); return; }             /* shlqbi */
        case 0x1DC: { R[rt]=q_rot_bytes(&R[ra], GW(R[rb],0)&15); return; }           /* rotqby */
        case 0x1DD: { uint32_t n=(0u-GW(R[rb],0))&31; R[rt]=q_shr_bytes(&R[ra], n); return; } /* rotqmby */
        case 0x1DF: { R[rt]=q_shl_bytes(&R[ra], GW(R[rb],0)&31); return; }           /* shlqby */
        case 0x1CC: { R[rt]=q_rot_bytes(&R[ra], (GW(R[rb],0)>>3)&15); return; }      /* rotqbybi */
        case 0x1CD: { uint32_t n=(0u-(GW(R[rb],0)>>3))&31; R[rt]=q_shr_bytes(&R[ra], n); return; } /* rotqmbybi */
        case 0x1CF: { R[rt]=q_shl_bytes(&R[ra], (GW(R[rb],0)>>3)&31); return; }      /* shlqbybi */
        case 0x1F8: { R[rt]=q_rot_bits(&R[ra], F_I7(insn)&7); return; }              /* rotqbii */
        case 0x1F9: { R[rt]=q_shr_bits(&R[ra], (0u-F_I7(insn))&7); return; }         /* rotqmbii */
        case 0x1FB: { R[rt]=q_shl_bits(&R[ra], F_I7(insn)&7); return; }              /* shlqbii */
        case 0x1FC: { R[rt]=q_rot_bytes(&R[ra], F_I7(insn)&15); return; }            /* rotqbyi */
        case 0x1FD: { uint32_t n=(0u-F_I7(insn))&31; R[rt]=q_shr_bytes(&R[ra], n); return; } /* rotqmbyi */
        case 0x1FF: { R[rt]=q_shl_bytes(&R[ra], F_I7(insn)&31); return; }            /* shlqbyi */

        /* Compare */
        case 0x3C0: { /* ceq */
            for (int i=0;i<4;i++) SW(R[rt],i,(GW(R[ra],i)==GW(R[rb],i))?~0u:0u); return; }
        case 0x3C8: { /* ceqh */
            for (int i=0;i<8;i++) SH(R[rt],i,(GH(R[ra],i)==GH(R[rb],i))?0xFFFF:0); return; }
        case 0x3D0: { /* ceqb */
            for (int i=0;i<16;i++) R[rt].u8[i]=(R[ra].u8[i]==R[rb].u8[i])?0xFF:0; return; }
        case 0x240: { /* cgt */
            for (int i=0;i<4;i++) SW(R[rt],i,((int32_t)GW(R[ra],i)>(int32_t)GW(R[rb],i))?~0u:0u); return; }
        case 0x248: { /* cgth */
            for (int i=0;i<8;i++) SH(R[rt],i,((int16_t)GH(R[ra],i)>(int16_t)GH(R[rb],i))?0xFFFF:0); return; }
        case 0x250: { /* cgtb */
            for (int i=0;i<16;i++) R[rt].u8[i]=(R[ra].s8[i]>R[rb].s8[i])?0xFF:0; return; }
        case 0x2C0: { /* clgt */
            for (int i=0;i<4;i++) SW(R[rt],i,(GW(R[ra],i)>GW(R[rb],i))?~0u:0u); return; }
        case 0x2C8: { /* clgth */
            for (int i=0;i<8;i++) SH(R[rt],i,(GH(R[ra],i)>GH(R[rb],i))?0xFFFF:0); return; }
        case 0x2D0: { /* clgtb */
            for (int i=0;i<16;i++) R[rt].u8[i]=(R[ra].u8[i]>R[rb].u8[i])?0xFF:0; return; }

        /* Halts — treat as nop (condition ignored) */
        case 0x258: case 0x2D8: case 0x3D8: return; /* hgt / hlgt / heq */

        /* Multiply (per-word 16x16) */
        case 0x3C4: { /* mpy — lo(a)*lo(b) signed */
            for (int i=0;i<4;i++)
                SW(R[rt],i,(uint32_t)((int32_t)(int16_t)(GW(R[ra],i)&0xFFFF)*(int32_t)(int16_t)(GW(R[rb],i)&0xFFFF)));
            return; }
        case 0x3CC: { /* mpyu */
            for (int i=0;i<4;i++)
                SW(R[rt],i,(uint32_t)(GW(R[ra],i)&0xFFFF)*(uint32_t)(GW(R[rb],i)&0xFFFF));
            return; }
        case 0x3C5: { /* mpyh — (hi(a)*lo(b))<<16 */
            for (int i=0;i<4;i++)
                SW(R[rt],i,(uint32_t)(((int32_t)(int16_t)(GW(R[ra],i)>>16)*(int32_t)(int16_t)(GW(R[rb],i)&0xFFFF))<<16));
            return; }
        case 0x3C7: { /* mpys — (lo*lo signed)>>16 */
            for (int i=0;i<4;i++)
                SW(R[rt],i,(uint32_t)(((int32_t)(int16_t)(GW(R[ra],i)&0xFFFF)*(int32_t)(int16_t)(GW(R[rb],i)&0xFFFF))>>16));
            return; }
        case 0x3C6: { /* mpyhh — hi*hi signed */
            for (int i=0;i<4;i++)
                SW(R[rt],i,(uint32_t)((int32_t)(int16_t)(GW(R[ra],i)>>16)*(int32_t)(int16_t)(GW(R[rb],i)>>16)));
            return; }
        case 0x3CE: { /* mpyhhu */
            for (int i=0;i<4;i++)
                SW(R[rt],i,(uint32_t)(GW(R[ra],i)>>16)*(uint32_t)(GW(R[rb],i)>>16));
            return; }
        case 0x346: { /* mpyhha — RT += hi*hi signed */
            for (int i=0;i<4;i++)
                SW(R[rt],i,GW(R[rt],i)+(uint32_t)((int32_t)(int16_t)(GW(R[ra],i)>>16)*(int32_t)(int16_t)(GW(R[rb],i)>>16)));
            return; }
        case 0x34E: { /* mpyhhau */
            for (int i=0;i<4;i++)
                SW(R[rt],i,GW(R[rt],i)+(uint32_t)(GW(R[ra],i)>>16)*(uint32_t)(GW(R[rb],i)>>16));
            return; }

        /* Branches (indirect) — target from RA preferred word */
        case 0x1A8: { /* bi */
            ctx->pc = GW(R[ra],0) & (SPU_LS_SIZE-1) & ~3u; return; }
        case 0x1A9: { /* bisl */
            uint32_t link = ctx->pc;
            ctx->pc = GW(R[ra],0) & (SPU_LS_SIZE-1) & ~3u;
            memset(&R[rt],0,16); SW(R[rt],0,link); return; }
        case 0x1AA: { /* iret — no interrupt state; stop */
            ctx->running = 0; return; }
        case 0x1AB: { /* bisled — branch if external event; we have none: no-op link */
            memset(&R[rt],0,16); SW(R[rt],0,ctx->pc); return; }
        case 0x128: { /* biz */
            if (GW(R[rt],0)==0) ctx->pc=GW(R[ra],0)&(SPU_LS_SIZE-1)&~3u; return; }
        case 0x129: { /* binz */
            if (GW(R[rt],0)!=0) ctx->pc=GW(R[ra],0)&(SPU_LS_SIZE-1)&~3u; return; }
        case 0x12A: { /* bihz */
            if ((GW(R[rt],0)&0xFFFF)==0) ctx->pc=GW(R[ra],0)&(SPU_LS_SIZE-1)&~3u; return; }
        case 0x12B: { /* bihnz */
            if ((GW(R[rt],0)&0xFFFF)!=0) ctx->pc=GW(R[ra],0)&(SPU_LS_SIZE-1)&~3u; return; }

        /* Single-precision float */
        case 0x2C4: { for (int i=0;i<4;i++) set_f(&R[rt],i,get_f(&R[ra],i)+get_f(&R[rb],i)); return; } /* fa */
        case 0x2C5: { for (int i=0;i<4;i++) set_f(&R[rt],i,get_f(&R[ra],i)-get_f(&R[rb],i)); return; } /* fs */
        case 0x2C6: { for (int i=0;i<4;i++) set_f(&R[rt],i,get_f(&R[ra],i)*get_f(&R[rb],i)); return; } /* fm */
        case 0x3C2: { for (int i=0;i<4;i++) SW(R[rt],i,(get_f(&R[ra],i)==get_f(&R[rb],i))?~0u:0u); return; } /* fceq */
        case 0x3CA: { for (int i=0;i<4;i++) SW(R[rt],i,(fabsf(get_f(&R[ra],i))==fabsf(get_f(&R[rb],i)))?~0u:0u); return; } /* fcmeq */
        case 0x2C2: { for (int i=0;i<4;i++) SW(R[rt],i,(get_f(&R[ra],i)>get_f(&R[rb],i))?~0u:0u); return; } /* fcgt */
        case 0x2CA: { for (int i=0;i<4;i++) SW(R[rt],i,(fabsf(get_f(&R[ra],i))>fabsf(get_f(&R[rb],i)))?~0u:0u); return; } /* fcmgt */
        case 0x3D4: { R[rt]=R[rb]; return; } /* fi — frest/frsqest return exact values, so pass through */
        case 0x1B8: { /* frest — exact reciprocal (paired with fi above) */
            for (int i=0;i<4;i++){float v=get_f(&R[ra],i); set_f(&R[rt],i,(v!=0.f)?1.f/v:0.f);} return; }
        case 0x1B9: { /* frsqest */
            for (int i=0;i<4;i++){float v=fabsf(get_f(&R[ra],i)); set_f(&R[rt],i,(v>0.f)?1.f/sqrtf_impl(v):0.f);} return; }
        case 0x398: { memset(&R[rt],0,16); return; } /* fscrrd */
        case 0x3BA: { return; }                       /* fscrwr */

        /* Double-precision float */
        case 0x2CC: { for (int i=0;i<2;i++) set_d(&R[rt],i,get_d(&R[ra],i)+get_d(&R[rb],i)); return; } /* dfa */
        case 0x2CD: { for (int i=0;i<2;i++) set_d(&R[rt],i,get_d(&R[ra],i)-get_d(&R[rb],i)); return; } /* dfs */
        case 0x2CE: { for (int i=0;i<2;i++) set_d(&R[rt],i,get_d(&R[ra],i)*get_d(&R[rb],i)); return; } /* dfm */
        case 0x35C: { for (int i=0;i<2;i++) set_d(&R[rt],i,get_d(&R[ra],i)*get_d(&R[rb],i)+get_d(&R[rt],i)); return; } /* dfma */
        case 0x35D: { for (int i=0;i<2;i++) set_d(&R[rt],i,get_d(&R[ra],i)*get_d(&R[rb],i)-get_d(&R[rt],i)); return; } /* dfms */
        case 0x35E: { for (int i=0;i<2;i++) set_d(&R[rt],i,get_d(&R[rt],i)-get_d(&R[ra],i)*get_d(&R[rb],i)); return; } /* dfnms */
        case 0x35F: { for (int i=0;i<2;i++) set_d(&R[rt],i,-(get_d(&R[ra],i)*get_d(&R[rb],i)+get_d(&R[rt],i))); return; } /* dfnma */
        case 0x3C3: { /* dfceq */
            for (int i=0;i<2;i++){uint32_t m=(get_d(&R[ra],i)==get_d(&R[rb],i))?~0u:0u; SW(R[rt],2*i,m); SW(R[rt],2*i+1,m);} return; }
        case 0x3CB: { /* dfcmeq */
            for (int i=0;i<2;i++){uint32_t m=(fabs(get_d(&R[ra],i))==fabs(get_d(&R[rb],i)))?~0u:0u; SW(R[rt],2*i,m); SW(R[rt],2*i+1,m);} return; }
        case 0x2C3: { /* dfcgt */
            for (int i=0;i<2;i++){uint32_t m=(get_d(&R[ra],i)>get_d(&R[rb],i))?~0u:0u; SW(R[rt],2*i,m); SW(R[rt],2*i+1,m);} return; }
        case 0x2CB: { /* dfcmgt */
            for (int i=0;i<2;i++){uint32_t m=(fabs(get_d(&R[ra],i))>fabs(get_d(&R[rb],i)))?~0u:0u; SW(R[rt],2*i,m); SW(R[rt],2*i+1,m);} return; }
        case 0x3BF: { return; } /* dftsv — test special value: ignore */
        case 0x3B8: { /* fesd — double i = (double)float element 2i */
            double d0=(double)get_f(&R[ra],0), d1=(double)get_f(&R[ra],2);
            set_d(&R[rt],0,d0); set_d(&R[rt],1,d1); return; }
        case 0x3B9: { /* frds */
            float f0=(float)get_d(&R[ra],0), f1=(float)get_d(&R[ra],1);
            set_f(&R[rt],0,f0); SW(R[rt],1,0); set_f(&R[rt],2,f1); SW(R[rt],3,0); return; }

        /* Conversions (10-bit opcode: pairs of op11 values; scale in I8) */
        case 0x3B0: case 0x3B1: { /* cflts — s32 = f * 2^(173-I8), clamped */
            double sc = ldexp(1.0, 173 - (int)F_I8(insn));
            for (int i=0;i<4;i++) {
                double v = (double)get_f(&R[ra],i) * sc;
                int32_t r = v >= 2147483647.0 ? 0x7FFFFFFF : v <= -2147483648.0 ? (int32_t)0x80000000 : (int32_t)v;
                SW(R[rt],i,(uint32_t)r);
            } return; }
        case 0x3B2: case 0x3B3: { /* cfltu */
            double sc = ldexp(1.0, 173 - (int)F_I8(insn));
            for (int i=0;i<4;i++) {
                double v = (double)get_f(&R[ra],i) * sc;
                uint32_t r = v >= 4294967295.0 ? 0xFFFFFFFFu : v < 0.0 ? 0u : (uint32_t)v;
                SW(R[rt],i,r);
            } return; }
        case 0x3B4: case 0x3B5: { /* csflt — f = (float)s32 * 2^(I8-155) */
            double sc = ldexp(1.0, (int)F_I8(insn) - 155);
            for (int i=0;i<4;i++) set_f(&R[rt],i,(float)((double)(int32_t)GW(R[ra],i)*sc));
            return; }
        case 0x3B6: case 0x3B7: { /* cuflt */
            double sc = ldexp(1.0, (int)F_I8(insn) - 155);
            for (int i=0;i<4;i++) set_f(&R[rt],i,(float)((double)GW(R[ra],i)*sc));
            return; }
    }

unknown:
    /* Fallback: unknown instruction */
    if (ctx->verbose)
        fprintf(stderr, "[SPU%d] UNIMPL PC=0x%04X insn=0x%08X op11=0x%03X op8=0x%02X op7=0x%02X\n",
                ctx->id, pc, insn, op11, op8, op7);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void spu_ctx_init(spu_ctx_t *ctx, int id, uint8_t *vm) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->id      = id;
    ctx->vm_base = vm;
    ctx->running = 0;
}

int spu_load_elf(spu_ctx_t *ctx, const uint8_t *elf, uint32_t length) {
    if (length < 52) return -1;
    /* ELF32 big-endian header */
    if (elf[0]!=0x7F||elf[1]!='E'||elf[2]!='L'||elf[3]!='F') return -1;
    uint32_t e_entry  = ((uint32_t)elf[24]<<24)|((uint32_t)elf[25]<<16)|((uint32_t)elf[26]<<8)|elf[27];
    uint32_t e_phoff  = ((uint32_t)elf[28]<<24)|((uint32_t)elf[29]<<16)|((uint32_t)elf[30]<<8)|elf[31];
    uint16_t e_phnum  = ((uint16_t)elf[44]<<8)|elf[45];
    uint16_t e_phentsz= ((uint16_t)elf[42]<<8)|elf[43];

    for (int i = 0; i < e_phnum; i++) {
        uint32_t ph = e_phoff + (uint32_t)i * e_phentsz;
        if (ph + 32 > length) break;
        uint32_t p_type  = ((uint32_t)elf[ph+0]<<24)|((uint32_t)elf[ph+1]<<16)|((uint32_t)elf[ph+2]<<8)|elf[ph+3];
        uint32_t p_off   = ((uint32_t)elf[ph+4]<<24)|((uint32_t)elf[ph+5]<<16)|((uint32_t)elf[ph+6]<<8)|elf[ph+7];
        uint32_t p_vaddr = ((uint32_t)elf[ph+8]<<24)|((uint32_t)elf[ph+9]<<16)|((uint32_t)elf[ph+10]<<8)|elf[ph+11];
        uint32_t p_filesz= ((uint32_t)elf[ph+16]<<24)|((uint32_t)elf[ph+17]<<16)|((uint32_t)elf[ph+18]<<8)|elf[ph+19];
        uint32_t p_memsz = ((uint32_t)elf[ph+20]<<24)|((uint32_t)elf[ph+21]<<16)|((uint32_t)elf[ph+22]<<8)|elf[ph+23];

        if (p_type != 1 /*PT_LOAD*/) continue;
        if (p_vaddr + p_memsz > SPU_LS_SIZE) continue;
        if (p_off + p_filesz > length) continue;

        memset(ctx->ls + p_vaddr, 0, p_memsz);
        if (p_filesz) memcpy(ctx->ls + p_vaddr, elf + p_off, p_filesz);
    }
    ctx->pc = e_entry & (SPU_LS_SIZE - 1);
    ctx->running = 1;
    return 0;
}

/* SPURS dispatch trace (S2 diagnostic — see docs/SPU_DISPATCH_PLAN.md):
 * dumps exact dataflow of the SPURS kernel's workload-select code
 * (LS 0x100-0x400) plus every instruction that writes r79, with pre-exec
 * RA/RB and post-exec RT quadwords (SPU-logical word values).
 * Enable by defining SPURS_DISPATCH_TRACE. */
/* #define SPURS_DISPATCH_TRACE 1 */

uint32_t spu_run(spu_ctx_t *ctx, uint32_t max_insns) {
    uint32_t n = 0;
    while (ctx->running && n < max_insns) {
#ifdef SPURS_DISPATCH_TRACE
        static int s_dt_lines = 0;
        int dt_on = 0;
        uint32_t dt_pc = 0, dt_insn = 0, dt_rt = 0, dt_ra = 0, dt_rb = 0;
        uint32_t dt_rav[4] = {0}, dt_rbv[4] = {0};
        if (ctx->id == 0 && s_dt_lines < 400) {
            dt_pc   = ctx->pc & (SPU_LS_SIZE - 1);
            dt_insn = ls_read32(ctx, dt_pc);
            dt_rt = F_RT(dt_insn); dt_ra = F_RA(dt_insn); dt_rb = F_RB(dt_insn);
            if (dt_pc >= 0x19C28 && dt_pc < 0x19F00) {  /* kernel main entry, first pass */
                dt_on = 1;
                for (int i=0;i<4;i++) { dt_rav[i]=GW(ctx->gpr[dt_ra],i); dt_rbv[i]=GW(ctx->gpr[dt_rb],i); }
            }
        }
#endif
        spu_step(ctx);
#ifdef SPURS_DISPATCH_TRACE
        if (dt_on) {
            s_dt_lines++;
            fprintf(stderr,
                "[DT] %04X %08X rt%u=%08X_%08X_%08X_%08X ra%u=%08X_%08X_%08X_%08X rb%u=%08X_%08X_%08X_%08X\n",
                dt_pc, dt_insn,
                dt_rt, GW(ctx->gpr[dt_rt],0), GW(ctx->gpr[dt_rt],1),
                       GW(ctx->gpr[dt_rt],2), GW(ctx->gpr[dt_rt],3),
                dt_ra, dt_rav[0], dt_rav[1], dt_rav[2], dt_rav[3],
                dt_rb, dt_rbv[0], dt_rbv[1], dt_rbv[2], dt_rbv[3]);
            fflush(stderr);
        }
#endif
        ctx->insn_count++;
        n++;
    }
    return n;
}
