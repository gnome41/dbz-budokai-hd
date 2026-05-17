#pragma once
/*
 * Minimal SPU (Synergistic Processing Unit) interpreter for PS3 static recomp.
 *
 * Handles enough instructions to run the SPURS kernel and game workloads.
 * DMA is implemented as memcpy from/to vm_base (main memory).
 * Channel I/O wires up to the PPU-side SPURS/mailbox protocol.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- SPU constants -------------------------------------------------------- */
#define SPU_LS_SIZE     (256 * 1024)   /* 256 KB local store */
#define SPU_MAX_THREADS 8              /* max concurrent virtual SPUs */

/* ---- 128-bit register ---------------------------------------------------- */
typedef union {
    uint8_t  u8[16];
    uint16_t u16[8];
    uint32_t u32[4];
    uint64_t u64[2];
    int8_t   s8[16];
    int16_t  s16[8];
    int32_t  s32[4];
    int64_t  s64[2];
    float    f32[4];
    double   f64[2];
} spu_reg_t;

/* ---- SPU context ---------------------------------------------------------- */
typedef struct spu_ctx {
    spu_reg_t gpr[128];           /* 128 × 128-bit registers              */
    uint8_t   ls[SPU_LS_SIZE];    /* 256 KB local store (big-endian)       */
    uint32_t  pc;                  /* program counter (local store offset)  */
    int       running;             /* 1 = running, 0 = stopped              */
    uint32_t  stop_code;           /* code from STOP/STOPD instruction       */

    /* MFC staging registers (loaded by wrch before issuing a command) */
    uint32_t mfc_lsa;
    uint32_t mfc_eah;
    uint32_t mfc_eal;
    uint32_t mfc_size;
    uint32_t mfc_tag;
    uint32_t mfc_tagmask;

    /* Mailbox channels */
    uint32_t inbound_mbox;         /* PPU→SPU: written by PPU, read by rdch */
    int      inbound_mbox_count;   /* 1 if data available, 0 if empty       */
    uint32_t outbound_mbox;        /* SPU→PPU: written by wrch, read by PPU */
    int      outbound_mbox_count;

    /* Signal notification (sig1/sig2) */
    uint32_t signal[2];
    int      signal_count[2];

    /* Tag status: simplified — MFC_WrTagUpdate sets pending=1, we complete
       immediately and clear it on rdch MFC_RdTagStat                       */
    int      tag_update_pending;

    /* Decrementer */
    uint32_t decr;

    /* SPU ID for identification */
    int      id;

    /* vm_base pointer (set at init from runtime_glue) */
    uint8_t *vm_base;
} spu_ctx_t;

/* ---- Public API ----------------------------------------------------------- */

/* Initialise a context; vm must point to the PPU vm_base 4GB reservation.
   The SPU ELF bytes are loaded into ls[] before calling spu_run().          */
void spu_ctx_init(spu_ctx_t *ctx, int id, uint8_t *vm);

/* Load a big-endian ELF32 SPU binary (raw bytes, length bytes) into ctx->ls.
   Sets ctx->pc to the ELF entry point.  Returns 0 on success, -1 on error.  */
int spu_load_elf(spu_ctx_t *ctx, const uint8_t *elf, uint32_t length);

/* Execute up to max_insns instructions. Returns when ctx->running==0 or
   max_insns is exhausted.  Returns number of instructions executed.         */
uint32_t spu_run(spu_ctx_t *ctx, uint32_t max_insns);

/* Execute a single instruction at ctx->pc, advancing pc.                   */
void spu_step(spu_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
