/*
 * K230 KPU/GNNE engine
 *
 * Copyright (c) 2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bitmap.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/core/irq.h"
#include "hw/misc/k230_kpu.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "trace.h"

#define K230_KPU_COMMAND_START          0x100
#define K230_KPU_COMMAND_END            0x104
#define K230_KPU_COMMAND_HI             0x108
#define K230_KPU_CONTROL                0x128
#define K230_KPU_STATUS                 0x130
#define K230_KPU_CONTROL_CLEAR          0x4
#define K230_KPU_CONTROL_START          0x9

#define K230_KPU_DONE                   0x0000000400000004ULL
#define K230_KPU_COMPLETE_DELAY_NS      (100 * 1000)
#define K230_KPU_FAKE_OUTPUT_BASE       0x10090000
#define K230_KPU_FAKE_OUTPUT_SIZE       0x00100000
#define K230_KPU_PAGE_SIZE              4096

#define K230_GNNE_COMMAND_BASE_OFFSET   0x003a6000
#define K230_GNNE_RUNTIME_RDATA_BASE    0x10000020
#define K230_GNNE_RUNTIME_WINDOW_SIZE   (64 * MiB)
#define K230_GNNE_RDATA_ALIAS_BASE      0xfc000000
#define K230_GNNE_RDATA_FALLBACK_BASE   0x10000000
#define K230_GNNE_MAX_COMMAND_SIZE      (16 * MiB)
#define K230_GNNE_MAX_OUTPUT_SIZE       (64 * MiB)
#define K230_GNNE_GLB_CACHE_SIZE        (4 * MiB)
#define K230_GNNE_RDATA_SHADOW_SIZE     (4 * MiB)
#define K230_GNNE_GP_COUNT              32
#define K230_GNNE_SHAPE_COUNT           8
#define K230_GNNE_MMU_COUNT             16
#define K230_GNNE_L2_LANE_WIDTH         24

#define K230_GNNE_SKIP_CONF             1
#define K230_GNNE_SKIP_SRC_GP           2
#define K230_GNNE_SKIP_DST_GP           3
#define K230_GNNE_SKIP_SHAPE            4
#define K230_GNNE_SKIP_SRC_TRANSLATE    5
#define K230_GNNE_SKIP_DST_TRANSLATE    6
#define K230_GNNE_SKIP_STRIDE           7
#define K230_GNNE_SKIP_COUNT            8
#define K230_GNNE_SKIP_OVERFLOW         9
#define K230_GNNE_SKIP_SOURCE_READ      10
#define K230_GNNE_SKIP_DEST_WRITE       11
#define K230_GNNE_SKIP_PSUM             12
#define K230_GNNE_SKIP_ACT0             13

typedef struct K230GnneScalar {
    uint32_t value;
    bool valid;
} K230GnneScalar;

typedef struct K230GnneShape {
    uint32_t n;
    uint32_t c;
    uint32_t h;
    uint32_t w;
    bool valid;
} K230GnneShape;

typedef struct K230GnneStride {
    uint32_t n;
    uint32_t c;
    uint32_t h;
    bool valid;
} K230GnneStride;

typedef struct K230GnneMmu {
    uint32_t start;
    uint32_t depth;
    bool valid;
} K230GnneMmu;

typedef struct K230GnneL2Conf {
    uint32_t rstride_d;
    uint32_t rstride_s;
    K230GnneStride stride_d;
    K230GnneStride stride_s;
    uint32_t l2_datatype;
    uint32_t ddr_datatype;
    uint32_t rlen_compressed;
    uint32_t rlen_decompressed;
    bool enable_decompress;
    bool stride_d_valid;
    bool stride_s_valid;
    bool valid;
} K230GnneL2Conf;

typedef struct K230GnneMfuAct1Src1 {
    uint32_t rslice;
    uint32_t rright_repeats;
    uint32_t rslice_repeats;
    bool slice_loc;
    bool valid;
} K230GnneMfuAct1Src1;

typedef struct K230GnneMfuAct1Src2 {
    uint32_t rleft_repeats;
    uint32_t rshape;
    bool source_type;
    bool valid;
} K230GnneMfuAct1Src2;

typedef struct K230GnneMfuAct1Deq {
    uint32_t rscale;
    uint32_t rbias;
    uint32_t quant_type;
    uint32_t rshift_bits;
    bool valid;
} K230GnneMfuAct1Deq;

typedef struct K230GnneMfuAct1Conf {
    uint32_t rstride_s1;
    uint32_t rstride_s2;
    uint32_t rstride_d1;
    bool stride_valid;
    K230GnneMfuAct1Src1 src1[2];
    K230GnneMfuAct1Src2 src2[2];
    uint32_t dest_rlen;
    uint64_t dest_len;
    uint32_t dest_rshape;
    bool dest_valid;
    bool dest_len_valid;
    K230GnneMfuAct1Deq deq[2];
    uint32_t quant_type;
    uint32_t quant_rshift_bits;
    bool quant_valid;
    uint32_t funct4;
    bool is_by_channel;
    bool is_16_segments;
    bool act_valid;
} K230GnneMfuAct1Conf;

typedef struct K230GnneMfuPdp1Conf {
    uint32_t stride_w;
    uint32_t stride_h;
    uint32_t rstride_s;
    uint32_t funct2;
    uint32_t rstride_d;
    bool conf1_valid;
    uint32_t rcount_w;
    uint32_t rcount_h;
    uint32_t rpe_h;
    uint32_t rpe_last_h;
    bool conf2_valid;
    uint32_t rpe_channels;
    uint32_t rpe_last_channels;
    uint32_t rpad_value;
    uint32_t sspad;
    bool conf3_valid;
    uint32_t rwindow_w;
    uint32_t rwindow_h;
    uint32_t rscale;
    bool enable_h2c;
    bool enable_bw;
    bool conf4_valid;
    K230GnneMfuAct1Deq deq;
    uint32_t quant_rscale;
    uint32_t quant_rbias;
    uint32_t quant_type;
    uint32_t quant_rshift_bits;
    bool quant_valid;
} K230GnneMfuPdp1Conf;

typedef struct K230GnneMfuTransposeConf {
    uint32_t rstride_d;
    uint32_t rstride_s;
    uint32_t l2_datatype;
    uint32_t permute;
    bool valid;
} K230GnneMfuTransposeConf;

typedef struct K230GnneDmLoadL1Conf {
    uint32_t rstride_s;
    uint32_t datatype;
    uint32_t l1_type;
    bool valid;
} K230GnneDmLoadL1Conf;

typedef struct K230GnneDmLoadL1 {
    uint32_t raddr_s;
    uint32_t rshape;
    bool valid;
} K230GnneDmLoadL1;

typedef struct K230GnneDmLoadWConf {
    uint32_t kernel_h;
    uint32_t kernel_w;
    uint32_t rstride_oc;
    uint32_t rgroups;
    uint32_t rgoc;
    uint32_t quant_type;
    bool valid;
} K230GnneDmLoadWConf;

typedef struct K230GnneDmLoadW {
    uint32_t raddr_s;
    uint32_t raddr_bw;
    uint32_t r_iochannels;
    bool valid;
} K230GnneDmLoadW;

typedef struct K230GnneDmLoadAct0 {
    uint32_t raddr_s;
    uint32_t rlen;
    bool is_by_channel;
    bool valid;
} K230GnneDmLoadAct0;

typedef struct K230GnneDmStoreOf {
    uint32_t raddr_d;
    uint32_t rshape;
    bool valid;
} K230GnneDmStoreOf;

typedef struct K230GnnePuConf {
    uint32_t stride_w;
    uint32_t stride_h;
    uint32_t rstride_s;
    uint32_t rgic;
    uint32_t rgic_last;
    uint32_t raddr_s;
    uint32_t rgroups;
    uint32_t rshape;
    uint32_t rpad_value;
    uint32_t sspad;
    uint32_t ric;
    uint32_t rbx;
    uint32_t quant_type;
    uint32_t kernel_h;
    uint32_t kernel_w;
    uint32_t rgoc;
    uint32_t rgoc_last;
    uint32_t rstride_d;
    uint32_t raddr_d;
    uint32_t rshape_d;
    K230GnneShape output_shape;
    K230GnneStride output_stride;
    bool load_psum;
    bool clr_psum;
    bool release_if;
    uint32_t dest_target;
    uint32_t mode;
    bool fetch1_valid;
    bool fetch2_valid;
    bool fetch3_valid;
    bool fetch4_valid;
    bool fetch_deq_valid;
    bool w_valid;
    bool of1_valid;
    bool of2_valid;
    bool output_shape_valid;
    bool output_stride_valid;
    bool compute_valid;
} K230GnnePuConf;

typedef struct K230GnnePdp0Conf {
    uint32_t mode;
    uint32_t stride_w;
    uint32_t stride_h;
    uint32_t rgic;
    uint32_t rgic_last;
    uint32_t rshape;
    K230GnneShape input_shape;
    uint32_t rpad_value;
    uint32_t sspad;
    uint32_t rbx;
    uint32_t quant_type;
    uint32_t kernel_h;
    uint32_t kernel_w;
    uint32_t rstride_d;
    uint32_t rshape_d;
    K230GnneShape output_shape;
    K230GnneStride output_stride;
    bool mode_valid;
    bool fetch1_valid;
    bool fetch2_valid;
    bool fetch3_valid;
    bool input_shape_valid;
    bool fetch4_valid;
    bool fetch_deq_valid;
    bool w_valid;
    bool of_valid;
    bool output_shape_valid;
    bool output_stride_valid;
} K230GnnePdp0Conf;

typedef struct K230GnneAct0Conf {
    uint32_t rshape;
    uint32_t rshift_bits;
    bool valid;
} K230GnneAct0Conf;

typedef struct K230GnneAct0Compute {
    uint32_t raddr_d;
    uint32_t dest_datatype;
    bool is_by_channel;
    bool valid;
} K230GnneAct0Compute;

typedef struct K230GnneFrontend {
    K230GnneScalar gp[K230_GNNE_GP_COUNT];
    K230GnneShape shape[K230_GNNE_SHAPE_COUNT];
    K230GnneStride stride[K230_GNNE_SHAPE_COUNT];
    K230GnneMmu mmu[K230_GNNE_MMU_COUNT];
    uint64_t glb_base;
    bool glb_base_valid;
    uint64_t rdata_base;
    bool rdata_base_valid;
    const uint8_t *rdata_shadow;
    uint64_t rdata_shadow_base;
    uint64_t rdata_shadow_size;
    bool rdata_shadow_valid;
    K230GnneL2Conf l2_load_conf;
    K230GnneL2Conf l2_load_w_conf;
    K230GnneL2Conf l2_store_conf;
    K230GnneMfuAct1Conf mfu_act1;
    K230GnneMfuPdp1Conf mfu_pdp1;
    K230GnneMfuTransposeConf mfu_transpose;
    K230GnneDmLoadL1Conf dm_load_l1_conf;
    K230GnneDmLoadL1 dm_load_l1;
    K230GnneDmLoadWConf dm_load_w_conf;
    K230GnneDmLoadW dm_load_w;
    K230GnneDmLoadAct0 dm_load_act0;
    K230GnneDmStoreOf dm_store_of;
    K230GnnePuConf pu_conf;
    K230GnnePdp0Conf pdp0_conf;
    K230GnneAct0Conf act0_conf;
    K230GnneAct0Compute act0_compute;
    int32_t *pu_psum;
    uint64_t pu_psum_count;
    uint64_t pu_psum_base;
    bool pu_psum_valid;
    bool pu_psum_base_valid;
    uint64_t instructions;
    uint64_t l2_loads;
    uint64_t l2_load_ws;
    uint64_t l2_stores;
    uint64_t mfu_act1s;
    uint64_t mfu_pdp1s;
    uint64_t mfu_transposes;
    uint64_t ai2d_computes;
    uint64_t pu_computes;
    uint64_t pdp0_computes;
    uint64_t input_bytes;
    uint64_t output_bytes;
    uint64_t unknown;
    uint8_t *glb_cache;
    uint64_t glb_cache_base;
    uint64_t glb_cache_size;
    bool glb_cache_dirty;
} K230GnneFrontend;

static K230GnneFrontend *k230_gnne_active_fe;

static const char *k230_kpu_name(K230KpuState *s)
{
    return object_get_canonical_path_component(OBJECT(s));
}

static bool k230_kpu_range_ok(hwaddr addr, unsigned int size)
{
    return addr <= K230_KPU_SIZE && size <= K230_KPU_SIZE - addr;
}

static bool k230_kpu_access_hits(hwaddr addr, unsigned int size,
                                 hwaddr offset)
{
    return addr <= offset && offset < addr + size;
}

static bool k230_kpu_control_has(uint64_t val, uint32_t bits)
{
    return (val & bits) == bits || (((val >> 32) & bits) == bits);
}

static bool k230_gnne_cache_access(K230GnneFrontend *fe, uint64_t addr,
                                   uint64_t size, uint8_t **ptr)
{
    uint64_t offset;

    if (!fe || !fe->glb_cache || addr < fe->glb_cache_base ||
        size > fe->glb_cache_size) {
        return false;
    }

    offset = addr - fe->glb_cache_base;
    if (offset > fe->glb_cache_size - size) {
        return false;
    }

    *ptr = fe->glb_cache + offset;
    return true;
}

static bool k230_gnne_cache_read(K230GnneFrontend *fe, uint64_t addr,
                                 void *buf, uint64_t size)
{
    uint8_t *ptr;

    if (!k230_gnne_cache_access(fe, addr, size, &ptr)) {
        return false;
    }

    memcpy(buf, ptr, size);
    return true;
}

static bool k230_gnne_cache_write(K230GnneFrontend *fe, uint64_t addr,
                                  const void *buf, uint64_t size)
{
    uint8_t *ptr;

    if (!k230_gnne_cache_access(fe, addr, size, &ptr)) {
        return false;
    }

    memcpy(ptr, buf, size);
    fe->glb_cache_dirty = true;
    return true;
}

static bool k230_gnne_rdata_shadow_read(K230GnneFrontend *fe, uint64_t addr,
                                        void *buf, uint64_t size)
{
    uint64_t offset;

    if (!fe || !fe->rdata_shadow_valid ||
        addr < fe->rdata_shadow_base ||
        size > fe->rdata_shadow_size) {
        return false;
    }

    offset = addr - fe->rdata_shadow_base;
    if (offset > fe->rdata_shadow_size - size) {
        return false;
    }

    memcpy(buf, fe->rdata_shadow + offset, size);
    return true;
}

static uint64_t k230_kpu_read_bytes(uint8_t *regs, hwaddr addr,
                                    unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_kpu_write_bytes(uint8_t *regs, hwaddr addr, uint64_t val,
                                 unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static uint32_t k230_kpu_readl_regs(K230KpuState *s, hwaddr addr)
{
    return ldl_le_p(s->regs + addr);
}

static void k230_kpu_writeq_regs(K230KpuState *s, hwaddr addr, uint64_t val)
{
    stq_le_p(s->regs + addr, val);
}

static bool k230_gnne_read_phys_u32(uint64_t addr, uint32_t *value)
{
    uint8_t buf[4];

    if (dma_memory_read(&address_space_memory, addr, buf, sizeof(buf),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return false;
    }

    *value = ldl_le_p(buf);
    return true;
}

static bool k230_gnne_base_has_alias_anchor(uint64_t base)
{
    uint32_t value;

    if (UINT64_MAX - base < sizeof(value) * 3) {
        return false;
    }

    return k230_gnne_read_phys_u32(base + sizeof(value) * 2, &value) &&
           value == K230_GNNE_RDATA_ALIAS_BASE;
}

static bool k230_gnne_command_in_runtime_window(uint64_t command_start)
{
    return command_start >= K230_GNNE_RUNTIME_RDATA_BASE &&
           command_start - K230_GNNE_RUNTIME_RDATA_BASE <
           K230_GNNE_RUNTIME_WINDOW_SIZE;
}

static void k230_gnne_select_rdata_base(K230GnneFrontend *fe)
{
    fe->rdata_base = fe->glb_base;
    fe->rdata_base_valid = fe->glb_base_valid;

    if (!fe->glb_base_valid ||
        k230_gnne_base_has_alias_anchor(fe->glb_base)) {
        return;
    }

    if (k230_gnne_base_has_alias_anchor(K230_GNNE_RDATA_FALLBACK_BASE)) {
        fe->rdata_base = K230_GNNE_RDATA_FALLBACK_BASE;
        fe->rdata_base_valid = true;
    }
}

static void k230_kpu_clear_rdata_shadow(K230KpuState *s)
{
    g_clear_pointer(&s->gnne_rdata_shadow, g_free);
    s->gnne_rdata_shadow_base = 0;
    s->gnne_rdata_shadow_size = 0;
    s->gnne_rdata_shadow_valid = false;
}

static void k230_kpu_ensure_rdata_shadow(K230KpuState *s, uint64_t base)
{
    if (s->gnne_rdata_shadow_valid &&
        s->gnne_rdata_shadow_base == base &&
        s->gnne_rdata_shadow_size == K230_GNNE_RDATA_SHADOW_SIZE) {
        return;
    }

    k230_kpu_clear_rdata_shadow(s);
    s->gnne_rdata_shadow = g_malloc(K230_GNNE_RDATA_SHADOW_SIZE);
    if (dma_memory_read(&address_space_memory, base, s->gnne_rdata_shadow,
                        K230_GNNE_RDATA_SHADOW_SIZE,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        k230_kpu_clear_rdata_shadow(s);
        return;
    }

    s->gnne_rdata_shadow_base = base;
    s->gnne_rdata_shadow_size = K230_GNNE_RDATA_SHADOW_SIZE;
    s->gnne_rdata_shadow_valid = true;
}

static uint64_t k230_kpu_command_addr(K230KpuState *s, hwaddr offset)
{
    uint64_t lo = k230_kpu_readl_regs(s, offset);
    uint64_t hi = k230_kpu_readl_regs(s, K230_KPU_COMMAND_HI);

    return (hi << 32) | lo;
}

static void k230_gnne_frontend_init(K230KpuState *s, K230GnneFrontend *fe,
                                    uint64_t command_start)
{
    memset(fe, 0, sizeof(*fe));
    fe->gp[0].valid = true;
    fe->gp[0].value = 0;
    if (k230_gnne_command_in_runtime_window(command_start)) {
        fe->glb_base = K230_GNNE_RUNTIME_RDATA_BASE;
        fe->glb_base_valid = true;
    } else if (command_start >= K230_GNNE_COMMAND_BASE_OFFSET) {
        fe->glb_base = command_start - K230_GNNE_COMMAND_BASE_OFFSET;
        fe->glb_base_valid = true;
    }

    if (fe->glb_base_valid) {
        fe->glb_cache_base = fe->glb_base;
        fe->glb_cache_size = K230_GNNE_GLB_CACHE_SIZE;
        k230_gnne_select_rdata_base(fe);
        if (fe->rdata_base_valid &&
            k230_gnne_command_in_runtime_window(command_start)) {
            k230_kpu_ensure_rdata_shadow(s, fe->rdata_base);
            if (s->gnne_rdata_shadow_valid &&
                s->gnne_rdata_shadow_base == fe->rdata_base) {
                fe->rdata_shadow = s->gnne_rdata_shadow;
                fe->rdata_shadow_base = s->gnne_rdata_shadow_base;
                fe->rdata_shadow_size = s->gnne_rdata_shadow_size;
                fe->rdata_shadow_valid = true;
            }
        }
        fe->glb_cache = g_malloc(fe->glb_cache_size);
        if (dma_memory_read(&address_space_memory, fe->glb_cache_base,
                            fe->glb_cache, fe->glb_cache_size,
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            g_clear_pointer(&fe->glb_cache, g_free);
            fe->glb_cache_size = 0;
        }
    }
}

static void k230_gnne_frontend_destroy(K230GnneFrontend *fe)
{
    if (fe->glb_cache && fe->glb_cache_dirty) {
        dma_memory_write(&address_space_memory, fe->glb_cache_base,
                         fe->glb_cache, fe->glb_cache_size,
                         MEMTXATTRS_UNSPECIFIED);
    }

    g_free(fe->glb_cache);
    g_free(fe->pu_psum);
}

static uint32_t k230_gnne_gp(K230GnneFrontend *fe, unsigned int reg,
                             bool *valid)
{
    if (reg >= K230_GNNE_GP_COUNT || !fe->gp[reg].valid) {
        *valid = false;
        return 0;
    }

    *valid = true;
    return fe->gp[reg].value;
}

static void k230_gnne_set_gp(K230GnneFrontend *fe, unsigned int reg,
                             uint32_t value, bool valid)
{
    if (reg >= K230_GNNE_GP_COUNT) {
        return;
    }

    if (reg == 0) {
        fe->gp[0].value = 0;
        fe->gp[0].valid = true;
        return;
    }

    fe->gp[reg].value = value;
    fe->gp[reg].valid = valid;
}

static bool k230_gnne_translate(K230GnneFrontend *fe, uint32_t encoded,
                                uint64_t *physical, uint64_t *logical)
{
    unsigned int mmu_id = extract32(encoded, 28, 4);
    uint64_t offset = encoded & 0x0fffffff;
    uint64_t addr;

    if (!fe->glb_base_valid || mmu_id >= K230_GNNE_MMU_COUNT ||
        !fe->mmu[mmu_id].valid) {
        return false;
    }

    addr = (uint64_t)fe->mmu[mmu_id].start * 32 + offset;
    if (UINT64_MAX - fe->glb_base < addr) {
        return false;
    }

    if (logical) {
        *logical = addr;
    }
    if (physical) {
        *physical = fe->glb_base + addr;
    }
    return true;
}

static bool k230_gnne_translate_rdata_alias(K230GnneFrontend *fe,
                                            uint32_t encoded,
                                            uint64_t *physical,
                                            uint64_t *logical)
{
    uint64_t offset;

    if (!fe->rdata_base_valid || encoded < K230_GNNE_RDATA_ALIAS_BASE) {
        return false;
    }

    offset = (uint64_t)encoded - K230_GNNE_RDATA_ALIAS_BASE;
    if (offset >= K230_GNNE_RUNTIME_WINDOW_SIZE ||
        UINT64_MAX - fe->rdata_base < offset) {
        return false;
    }

    if (logical) {
        *logical = encoded;
    }
    if (physical) {
        *physical = fe->rdata_base + offset;
    }
    return true;
}

static bool k230_gnne_translate_store_dest(K230GnneFrontend *fe,
                                           uint32_t encoded,
                                           uint64_t *physical,
                                           uint64_t *logical)
{
    return k230_gnne_translate(fe, encoded, physical, logical) ||
           k230_gnne_translate_rdata_alias(fe, encoded, physical, logical);
}

static bool k230_gnne_read_scalar(K230GnneFrontend *fe, uint32_t encoded,
                                  unsigned int size, bool sign,
                                  uint32_t *value)
{
    uint64_t logical;
    uint8_t buf[4] = {};

    if (size > sizeof(buf) ||
        !k230_gnne_translate(fe, encoded, NULL, &logical) ||
        !fe->rdata_base_valid ||
        UINT64_MAX - fe->rdata_base < logical) {
        return false;
    }

    if (k230_gnne_rdata_shadow_read(fe, fe->rdata_base + logical, buf,
                                    size)) {
        goto decode;
    }

    if (dma_memory_read(&address_space_memory, fe->rdata_base + logical,
                        buf, size,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return false;
    }

decode:
    switch (size) {
    case 4:
        *value = ldl_le_p(buf);
        return true;
    case 2:
        if (sign) {
            *value = (uint32_t)(int32_t)(int16_t)lduw_le_p(buf);
        } else {
            *value = lduw_le_p(buf);
        }
        return true;
    case 1:
        if (sign) {
            *value = (uint32_t)(int32_t)(int8_t)buf[0];
        } else {
            *value = buf[0];
        }
        return true;
    default:
        return false;
    }
}

static bool k230_gnne_source_read(K230GnneFrontend *fe, uint64_t source,
                                  void *buf, uint64_t size)
{
    uint64_t rdata_offset;
    uint64_t physical;

    if (dma_memory_read(&address_space_memory, source, buf, size,
                        MEMTXATTRS_UNSPECIFIED) == MEMTX_OK) {
        return true;
    }

    if (!fe->rdata_base_valid || source < K230_GNNE_RDATA_ALIAS_BASE) {
        return false;
    }

    rdata_offset = source - K230_GNNE_RDATA_ALIAS_BASE;
    if (UINT64_MAX - fe->rdata_base < rdata_offset) {
        return false;
    }
    physical = fe->rdata_base + rdata_offset;

    if (k230_gnne_rdata_shadow_read(fe, physical, buf, size)) {
        return true;
    }

    return dma_memory_read(&address_space_memory, physical, buf, size,
                           MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
}

static bool k230_gnne_is_short(uint32_t opcode)
{
    switch (opcode) {
    case 0x01:
    case 0x03:
    case 0x05:
    case 0x07:
    case 0x41:
    case 0x43:
    case 0x45:
    case 0x49:
    case 0x4b:
    case 0x4d:
    case 0x4f:
    case 0x51:
        return true;
    default:
        return false;
    }
}

static bool k230_gnne_opcode_known(uint32_t word)
{
    uint32_t opcode = word & 0x7f;
    uint32_t funct3_17 = extract32(word, 17, 3);
    uint32_t funct4_13 = extract32(word, 13, 4);
    uint32_t funct5_7 = extract32(word, 7, 5);
    uint32_t funct5_17 = extract32(word, 17, 5);

    switch (opcode) {
    case 0x06:
        return funct3_17 <= 4;
    case 0x08:
        return funct3_17 <= 2;
    case 0x0c:
        return funct5_17 <= 6;
    case 0x0e:
        return funct3_17 == 0;
    case 0x10:
        return funct3_17 <= 5;
    case 0x50:
        return funct4_13 <= 5;
    case 0x5a:
        return funct4_13 <= 8;
    case 0x5e:
        return funct4_13 <= 7;
    case 0x62:
        return funct5_7 <= 0x0e;
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x04:
    case 0x05:
    case 0x07:
    case 0x12:
    case 0x14:
    case 0x16:
    case 0x18:
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x48:
    case 0x49:
    case 0x4a:
    case 0x4b:
    case 0x4c:
    case 0x4d:
    case 0x4e:
    case 0x4f:
    case 0x51:
    case 0x52:
    case 0x54:
    case 0x56:
    case 0x57:
    case 0x58:
    case 0x5c:
    case 0x60:
    case 0x64:
    case 0x66:
    case 0x68:
    case 0x6a:
    case 0x72:
    case 0x74:
        return true;
    default:
        return false;
    }
}

static bool k230_gnne_direct_type_size(uint32_t datatype, unsigned int *size)
{
    switch (datatype) {
    case 0:
        *size = 1;
        return true;
    case 1:
    case 5:
        *size = 2;
        return true;
    case 2:
        *size = 4;
        return true;
    default:
        return false;
    }
}

static unsigned int k230_gnne_l2_type_size(uint32_t datatype)
{
    return datatype == 0 ? 1 : 2;
}

static bool k230_gnne_l2_datatype_size(uint32_t datatype, unsigned int *size)
{
    switch (datatype) {
    case 0:
        *size = 1;
        return true;
    case 1:
    case 2:
        *size = 2;
        return true;
    default:
        return false;
    }
}

static double k230_gnne_pow2(int64_t shift)
{
    double value = 1.0;

    if (shift > 63) {
        shift = 63;
    } else if (shift < -63) {
        shift = -63;
    }

    while (shift > 0) {
        value *= 2.0;
        shift--;
    }
    while (shift < 0) {
        value *= 0.5;
        shift++;
    }

    return value;
}

static int64_t k230_gnne_round_nearest(double value)
{
    if (value >= (double)INT64_MAX - 1.0) {
        return INT64_MAX;
    }
    if (value <= (double)INT64_MIN + 1.0) {
        return INT64_MIN;
    }

    return value >= 0.0 ? (int64_t)(value + 0.5) :
           (int64_t)(value - 0.5);
}

static int64_t k230_gnne_clamp_i64(int64_t value, int64_t low, int64_t high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }

    return value;
}

static double k230_gnne_fp16_to_double(uint16_t raw)
{
    bool sign = raw & 0x8000;
    uint32_t exp = extract32(raw, 10, 5);
    uint32_t frac = raw & 0x03ff;
    double value;

    if (exp == 0x1f) {
        value = 65504.0;
    } else if (exp == 0) {
        value = ((double)frac / 1024.0) * k230_gnne_pow2(-14);
    } else {
        value = (1.0 + (double)frac / 1024.0) *
                k230_gnne_pow2((int64_t)exp - 15);
    }

    return sign ? -value : value;
}

static uint16_t k230_gnne_double_to_fp16(double value)
{
    uint16_t sign = 0;
    int exp = 0;
    double abs_value = value;
    double normalized;
    int64_t frac;

    if (abs_value < 0.0) {
        sign = 0x8000;
        abs_value = -abs_value;
    }

    if (abs_value == 0.0) {
        return sign;
    }
    if (abs_value >= 65504.0) {
        return sign | 0x7bff;
    }
    if (abs_value < k230_gnne_pow2(-14)) {
        frac = k230_gnne_round_nearest(abs_value * k230_gnne_pow2(24));
        if (frac <= 0) {
            return sign;
        }
        return sign | MIN(frac, 0x3ff);
    }

    normalized = abs_value;
    while (normalized >= 2.0) {
        normalized *= 0.5;
        exp++;
    }
    while (normalized < 1.0) {
        normalized *= 2.0;
        exp--;
    }

    frac = k230_gnne_round_nearest((normalized - 1.0) * 1024.0);
    if (frac >= 1024) {
        frac = 0;
        exp++;
    }
    if (exp > 15) {
        return sign | 0x7bff;
    }

    return sign | ((exp + 15) << 10) | (frac & 0x3ff);
}

static bool k230_gnne_dma_read_bytes(uint64_t addr, void *buf,
                                     unsigned int size)
{
    if (k230_gnne_cache_read(k230_gnne_active_fe, addr, buf, size)) {
        return true;
    }

    return dma_memory_read(&address_space_memory, addr, buf, size,
                           MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
}

static bool k230_gnne_dma_write_bytes(uint64_t addr, const void *buf,
                                      unsigned int size)
{
    if (k230_gnne_cache_write(k230_gnne_active_fe, addr, buf, size)) {
        return true;
    }

    return dma_memory_write(&address_space_memory, addr, buf, size,
                            MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
}

static bool k230_gnne_read_fp16(uint64_t addr, double *value)
{
    uint8_t buf[2];

    if (!k230_gnne_dma_read_bytes(addr, buf, sizeof(buf))) {
        return false;
    }

    *value = k230_gnne_fp16_to_double(lduw_le_p(buf));
    return true;
}

static bool k230_gnne_write_fp16(uint64_t addr, double value)
{
    uint8_t buf[2];

    stw_le_p(buf, k230_gnne_double_to_fp16(value));
    return k230_gnne_dma_write_bytes(addr, buf, sizeof(buf));
}

static bool k230_gnne_quant_type_size(uint32_t quant_type, unsigned int *size)
{
    switch (quant_type) {
    case 0:
    case 3:
        *size = 2;
        return true;
    case 1:
    case 2:
        *size = 1;
        return true;
    default:
        return false;
    }
}

static bool k230_gnne_act0_type_size(uint32_t datatype, unsigned int *size)
{
    switch (datatype) {
    case 0:
    case 1:
        *size = 1;
        return true;
    case 2:
    case 3:
        *size = 2;
        return true;
    default:
        return false;
    }
}

static bool k230_gnne_shape_count(const K230GnneShape *shape, uint64_t *count)
{
    if (!shape->valid) {
        return false;
    }

    if (!shape->n || !shape->c || !shape->h || !shape->w) {
        *count = 0;
        return true;
    }

    if (umul64_overflow((uint64_t)shape->n, shape->c, count) ||
        umul64_overflow(*count, shape->h, count) ||
        umul64_overflow(*count, shape->w, count)) {
        return false;
    }

    return true;
}

static bool k230_gnne_shape_product(K230GnneFrontend *fe, unsigned int index,
                                    uint64_t *count)
{
    if (index >= K230_GNNE_SHAPE_COUNT) {
        return false;
    }

    return k230_gnne_shape_count(&fe->shape[index], count);
}

static bool k230_gnne_stride_value(K230GnneFrontend *fe, unsigned int index,
                                   K230GnneStride *stride)
{
    if (index >= K230_GNNE_SHAPE_COUNT || !fe->stride[index].valid) {
        return false;
    }

    *stride = fe->stride[index];
    return true;
}

static bool k230_gnne_l2_conf_sizes(K230GnneL2Conf *conf,
                                    unsigned int *src_size,
                                    unsigned int *dst_size)
{
    if (!conf->valid ||
        !k230_gnne_direct_type_size(conf->ddr_datatype, src_size)) {
        return false;
    }

    *dst_size = k230_gnne_l2_type_size(conf->l2_datatype);
    return true;
}

static void k230_gnne_l2_load(K230KpuState *s, K230GnneFrontend *fe,
                              uint32_t word)
{
    K230GnneL2Conf *conf = &fe->l2_load_conf;
    K230GnneShape *shape;
    K230GnneStride src_stride;
    K230GnneStride dst_stride;
    unsigned int raddr_d = extract32(word, 7, 5);
    unsigned int raddr_s = extract32(word, 12, 5);
    unsigned int rshape = extract32(word, 17, 3);
    unsigned int src_size;
    unsigned int dst_size;
    uint32_t src_addr;
    uint32_t dst_encoded;
    uint64_t dst_base;
    uint64_t dst_logical;
    uint64_t total_count;
    uint64_t copied = 0;
    bool valid;

    src_addr = k230_gnne_gp(fe, raddr_s, &valid);
    if (!valid) {
        return;
    }
    dst_encoded = k230_gnne_gp(fe, raddr_d, &valid);
    if (!valid || rshape >= K230_GNNE_SHAPE_COUNT ||
        !fe->shape[rshape].valid ||
        !k230_gnne_translate(fe, dst_encoded, &dst_base, &dst_logical) ||
        !k230_gnne_l2_conf_sizes(conf, &src_size, &dst_size) ||
        !conf->stride_s_valid ||
        !conf->stride_d_valid ||
        !k230_gnne_shape_product(fe, rshape, &total_count)) {
        return;
    }

    src_stride = conf->stride_s;
    dst_stride = conf->stride_d;

    if (total_count > K230_GNNE_MAX_OUTPUT_SIZE / dst_size) {
        return;
    }

    shape = &fe->shape[rshape];
    for (uint32_t n = 0; n < shape->n; n++) {
        for (uint32_t c = 0; c < shape->c; c++) {
            for (uint32_t h = 0; h < shape->h; h++) {
                uint64_t src_index;
                uint64_t dst_index;
                uint64_t src_line;
                uint64_t dst_line;
                g_autofree uint8_t *line = NULL;

                src_index = (uint64_t)n * src_stride.n +
                            (uint64_t)c * src_stride.c +
                            (uint64_t)h * src_stride.h;
                dst_index = (uint64_t)n * dst_stride.n +
                            (uint64_t)c * dst_stride.c +
                            (uint64_t)h * dst_stride.h;
                if (umul64_overflow(src_index, src_size, &src_line) ||
                    umul64_overflow(dst_index, dst_size, &dst_line) ||
                    UINT64_MAX - src_addr < src_line ||
                    UINT64_MAX - dst_base < dst_line ||
                    shape->w > K230_GNNE_MAX_OUTPUT_SIZE / src_size) {
                    return;
                }

                line = g_malloc(shape->w * src_size);
                if (!k230_gnne_source_read(fe, src_addr + src_line,
                                           line, shape->w * src_size)) {
                    return;
                }

                for (uint32_t w = 0; w < shape->w; w++) {
                    uint64_t dst_off = dst_base + dst_line +
                                       (uint64_t)w * dst_size;
                    uint8_t item[4] = {};

                    memcpy(item, line + w * src_size, MIN(src_size,
                                                          dst_size));
                    if (!k230_gnne_dma_write_bytes(dst_off, item,
                                                   dst_size)) {
                        return;
                    }
                    copied += dst_size;
                }
            }
        }
    }

    trace_k230_kpu_l2_load(k230_kpu_name(s), src_addr, dst_logical, copied);
    fe->l2_loads++;
    fe->input_bytes += copied;
}

static void k230_gnne_l2_load_w(K230KpuState *s, K230GnneFrontend *fe,
                                uint32_t word, uint64_t pc)
{
    K230GnneL2Conf *conf = &fe->l2_load_w_conf;
    unsigned int raddr_d = extract32(word, 7, 5);
    unsigned int raddr_s = extract32(word, 12, 5);
    unsigned int rvalid_c_num = extract32(word, 17, 5);
    unsigned int src_size;
    unsigned int dst_size;
    uint32_t src_addr;
    uint32_t dst_encoded;
    uint32_t rlen;
    uint32_t valid_c;
    uint64_t dst_base;
    uint64_t dst_logical = 0;
    uint64_t copied = 0;
    bool valid;

    if (conf->enable_decompress ||
        !k230_gnne_l2_conf_sizes(conf, &src_size, &dst_size)) {
        trace_k230_kpu_l2_load_w_skip(k230_kpu_name(s), pc,
                                      K230_GNNE_SKIP_CONF, 0, 0, 0, 0);
        return;
    }

    src_addr = k230_gnne_gp(fe, raddr_s, &valid);
    if (!valid) {
        trace_k230_kpu_l2_load_w_skip(k230_kpu_name(s), pc,
                                      K230_GNNE_SKIP_SRC_GP, 0, 0, 0, 0);
        return;
    }
    dst_encoded = k230_gnne_gp(fe, raddr_d, &valid);
    if (!valid ||
        !k230_gnne_translate(fe, dst_encoded, &dst_base, &dst_logical)) {
        trace_k230_kpu_l2_load_w_skip(k230_kpu_name(s), pc,
                                      K230_GNNE_SKIP_DST_TRANSLATE,
                                      src_addr, 0, 0, 0);
        return;
    }
    rlen = conf->rlen_decompressed;
    if (rlen > K230_GNNE_MAX_OUTPUT_SIZE / dst_size) {
        trace_k230_kpu_l2_load_w_skip(k230_kpu_name(s), pc,
                                      K230_GNNE_SKIP_COUNT, src_addr,
                                      dst_logical, rlen, 0);
        return;
    }
    valid_c = k230_gnne_gp(fe, rvalid_c_num, &valid);
    if (!valid || valid_c == UINT32_MAX) {
        trace_k230_kpu_l2_load_w_skip(k230_kpu_name(s), pc,
                                      K230_GNNE_SKIP_COUNT, src_addr,
                                      dst_logical, rlen, 0);
        return;
    }
    valid_c++;

    for (uint32_t index = 0; index < rlen; index++) {
        uint64_t src_off = (uint64_t)index * src_size;
        uint64_t row = index / valid_c;
        uint64_t col = index % valid_c;
        uint64_t dst_index = row * K230_GNNE_L2_LANE_WIDTH + col;
        uint64_t dst_off;
        uint8_t item[4] = {};

        if (UINT64_MAX - src_addr < src_off ||
            umul64_overflow(dst_index, dst_size, &dst_off) ||
            UINT64_MAX - dst_base < dst_off) {
            trace_k230_kpu_l2_load_w_skip(k230_kpu_name(s), pc,
                                          K230_GNNE_SKIP_OVERFLOW,
                                          src_addr, dst_logical, rlen,
                                          valid_c);
            return;
        }

        if (!k230_gnne_source_read(fe, src_addr + src_off, item, src_size)) {
            trace_k230_kpu_l2_load_w_skip(k230_kpu_name(s), pc,
                                          K230_GNNE_SKIP_SOURCE_READ,
                                          src_addr + src_off, dst_logical,
                                          rlen, valid_c);
            return;
        }
        if (!k230_gnne_dma_write_bytes(dst_base + dst_off, item,
                                       dst_size)) {
            trace_k230_kpu_l2_load_w_skip(k230_kpu_name(s), pc,
                                          K230_GNNE_SKIP_DEST_WRITE,
                                          src_addr, dst_logical, rlen,
                                          valid_c);
            return;
        }
        copied += dst_size;
    }

    trace_k230_kpu_l2_load_w(k230_kpu_name(s), src_addr, dst_logical,
                             rlen, valid_c);
    fe->l2_load_ws++;
    fe->input_bytes += copied;
}

static bool k230_gnne_store_conf_sizes(K230GnneL2Conf *conf,
                                       unsigned int *src_size,
                                       unsigned int *dst_size)
{
    if (!conf->valid ||
        !k230_gnne_direct_type_size(conf->ddr_datatype, dst_size)) {
        return false;
    }

    *src_size = k230_gnne_l2_type_size(conf->l2_datatype);
    return true;
}

static void k230_gnne_l2_store(K230KpuState *s, K230GnneFrontend *fe,
                               uint32_t word, uint64_t pc)
{
    K230GnneL2Conf *conf = &fe->l2_store_conf;
    K230GnneShape *shape;
    K230GnneStride src_stride;
    K230GnneStride dst_stride;
    unsigned int raddr_d = extract32(word, 7, 5);
    unsigned int raddr_s = extract32(word, 12, 5);
    unsigned int rshape = extract32(word, 17, 3);
    unsigned int src_size;
    unsigned int dst_size;
    uint32_t src_encoded;
    uint32_t dst_encoded;
    uint64_t src_base;
    uint64_t dst_base;
    uint64_t src_logical = 0;
    uint64_t dst_logical = 0;
    uint64_t total_count;
    uint64_t copied = 0;
    bool valid;

    src_encoded = k230_gnne_gp(fe, raddr_s, &valid);
    if (!valid) {
        trace_k230_kpu_l2_store_skip(k230_kpu_name(s), pc,
                                     K230_GNNE_SKIP_SRC_GP, 0, 0, rshape,
                                     copied);
        return;
    }
    dst_encoded = k230_gnne_gp(fe, raddr_d, &valid);
    if (!valid) {
        trace_k230_kpu_l2_store_skip(k230_kpu_name(s), pc,
                                     K230_GNNE_SKIP_DST_GP, src_encoded, 0,
                                     rshape, copied);
        return;
    }
    if (rshape >= K230_GNNE_SHAPE_COUNT || !fe->shape[rshape].valid) {
        trace_k230_kpu_l2_store_skip(k230_kpu_name(s), pc,
                                     K230_GNNE_SKIP_SHAPE, src_encoded, 0,
                                     rshape, copied);
        return;
    }
    if (!k230_gnne_translate(fe, src_encoded, &src_base, &src_logical)) {
        trace_k230_kpu_l2_store_skip(k230_kpu_name(s), pc,
                                     K230_GNNE_SKIP_SRC_TRANSLATE,
                                     src_encoded, 0, rshape, copied);
        return;
    }
    if (!k230_gnne_translate_store_dest(fe, dst_encoded, &dst_base,
                                        &dst_logical)) {
        trace_k230_kpu_l2_store_skip(k230_kpu_name(s), pc,
                                     K230_GNNE_SKIP_DST_TRANSLATE,
                                     src_logical, dst_encoded, rshape,
                                     copied);
        return;
    }
    if (!k230_gnne_store_conf_sizes(conf, &src_size, &dst_size)) {
        trace_k230_kpu_l2_store_skip(k230_kpu_name(s), pc,
                                     K230_GNNE_SKIP_CONF, src_logical,
                                     dst_logical, rshape, copied);
        return;
    }
    if (!conf->stride_s_valid || !conf->stride_d_valid) {
        trace_k230_kpu_l2_store_skip(k230_kpu_name(s), pc,
                                     K230_GNNE_SKIP_STRIDE, src_logical,
                                     dst_logical, rshape, copied);
        return;
    }
    if (!k230_gnne_shape_product(fe, rshape, &total_count)) {
        trace_k230_kpu_l2_store_skip(k230_kpu_name(s), pc,
                                     K230_GNNE_SKIP_COUNT, src_logical,
                                     dst_logical, rshape, copied);
        return;
    }

    src_stride = conf->stride_s;
    dst_stride = conf->stride_d;

    if (total_count > K230_GNNE_MAX_OUTPUT_SIZE / dst_size) {
        trace_k230_kpu_l2_store_skip(k230_kpu_name(s), pc,
                                     K230_GNNE_SKIP_COUNT, src_logical,
                                     dst_logical, rshape, copied);
        return;
    }

    shape = &fe->shape[rshape];
    for (uint32_t n = 0; n < shape->n; n++) {
        for (uint32_t c = 0; c < shape->c; c++) {
            for (uint32_t h = 0; h < shape->h; h++) {
                uint64_t src_index;
                uint64_t dst_index;
                uint64_t src_line;
                uint64_t dst_line;

                src_index = (uint64_t)n * src_stride.n +
                            (uint64_t)c * src_stride.c +
                            (uint64_t)h * src_stride.h;
                dst_index = (uint64_t)n * dst_stride.n +
                            (uint64_t)c * dst_stride.c +
                            (uint64_t)h * dst_stride.h;
                if (umul64_overflow(src_index, src_size, &src_line) ||
                    umul64_overflow(dst_index, dst_size, &dst_line) ||
                    UINT64_MAX - src_base < src_line ||
                    UINT64_MAX - dst_base < dst_line ||
                    shape->w > K230_GNNE_MAX_OUTPUT_SIZE / dst_size) {
                    trace_k230_kpu_l2_store_skip(k230_kpu_name(s), pc,
                                                 K230_GNNE_SKIP_OVERFLOW,
                                                 src_logical, dst_logical,
                                                 rshape, copied);
                    return;
                }

                for (uint32_t w = 0; w < shape->w; w++) {
                    uint64_t src_off = src_base + src_line +
                                       (uint64_t)w * src_size;
                    uint64_t dst_off = dst_base + dst_line +
                                       (uint64_t)w * dst_size;
                    uint8_t item[4] = {};

                    if (!k230_gnne_dma_read_bytes(src_off, item, src_size) ||
                        !k230_gnne_dma_write_bytes(dst_off, item,
                                                   dst_size)) {
                        trace_k230_kpu_l2_store_skip(k230_kpu_name(s), pc,
                                                     K230_GNNE_SKIP_DEST_WRITE,
                                                     src_off, dst_off,
                                                     rshape, copied);
                        return;
                    }
                    copied += dst_size;
                }
            }
        }
    }

    trace_k230_kpu_l2_store(k230_kpu_name(s), dst_logical, dst_base, copied);
    fe->l2_stores++;
    fe->output_bytes += copied;
}

static bool k230_gnne_mfu_dequant(uint64_t src, uint64_t index,
                                  uint32_t quant_type, double scale,
                                  uint32_t bias, uint32_t shift,
                                  double *value)
{
    uint64_t offset;
    uint8_t byte;
    uint8_t word[2];

    switch (quant_type) {
    case 0:
        if (umul64_overflow(index, 2, &offset) ||
            UINT64_MAX - src < offset) {
            return false;
        }
        return k230_gnne_read_fp16(src + offset, value);
    case 1:
        if (UINT64_MAX - src < index ||
            !k230_gnne_dma_read_bytes(src + index, &byte, sizeof(byte))) {
            return false;
        }
        *value = ((double)byte - bias) * scale *
                 k230_gnne_pow2(-(int64_t)shift);
        return true;
    case 2:
        if (UINT64_MAX - src < index ||
            !k230_gnne_dma_read_bytes(src + index, &byte, sizeof(byte))) {
            return false;
        }
        *value = (double)(int8_t)byte * scale *
                 k230_gnne_pow2(-(int64_t)shift);
        return true;
    case 3:
        if (umul64_overflow(index, 2, &offset) ||
            UINT64_MAX - src < offset ||
            !k230_gnne_dma_read_bytes(src + offset, word, sizeof(word))) {
            return false;
        }
        *value = (double)(int16_t)lduw_le_p(word) * scale *
                 k230_gnne_pow2(-(int64_t)shift);
        return true;
    default:
        return false;
    }
}

static bool k230_gnne_mfu_act1_value(uint64_t arg, uint32_t channel,
                                     double value, uint32_t shift,
                                     double *result)
{
    uint64_t base;
    double threshold;
    double slope;
    double bias;
    double lower;
    double upper;

    if (umul64_overflow((uint64_t)channel, 14, &base) ||
        UINT64_MAX - arg < base) {
        return false;
    }
    base += arg;

    if (!k230_gnne_read_fp16(base, &threshold)) {
        return false;
    }

    if (value < threshold) {
        if (!k230_gnne_read_fp16(base + 2, &slope) ||
            !k230_gnne_read_fp16(base + 6, &bias)) {
            return false;
        }
    } else {
        if (!k230_gnne_read_fp16(base + 4, &slope) ||
            !k230_gnne_read_fp16(base + 8, &bias)) {
            return false;
        }
    }

    if (!k230_gnne_read_fp16(base + 10, &lower) ||
        !k230_gnne_read_fp16(base + 12, &upper)) {
        return false;
    }

    value = value * slope * k230_gnne_pow2(shift) + bias;
    if (value < lower) {
        value = lower;
    } else if (value > upper) {
        value = upper;
    }

    *result = value;
    return true;
}

static bool k230_gnne_mfu_write_quant(uint64_t dst, uint64_t index,
                                      uint32_t quant_type, double value,
                                      unsigned int *written)
{
    uint64_t offset;
    int64_t rounded;
    uint8_t byte;
    uint8_t word[2];

    switch (quant_type) {
    case 0:
        if (umul64_overflow(index, 2, &offset) ||
            UINT64_MAX - dst < offset ||
            !k230_gnne_write_fp16(dst + offset, value)) {
            return false;
        }
        *written = 2;
        return true;
    case 1:
        rounded = k230_gnne_round_nearest(value);
        rounded = k230_gnne_clamp_i64(rounded, 0, 255);
        byte = rounded;
        if (UINT64_MAX - dst < index ||
            !k230_gnne_dma_write_bytes(dst + index, &byte, sizeof(byte))) {
            return false;
        }
        *written = 1;
        return true;
    case 2:
        rounded = k230_gnne_round_nearest(value);
        rounded = k230_gnne_clamp_i64(rounded, -127, 127);
        byte = rounded;
        if (UINT64_MAX - dst < index ||
            !k230_gnne_dma_write_bytes(dst + index, &byte, sizeof(byte))) {
            return false;
        }
        *written = 1;
        return true;
    case 3:
        rounded = k230_gnne_round_nearest(value);
        rounded = k230_gnne_clamp_i64(rounded, -32767, 32767);
        if (umul64_overflow(index, 2, &offset) ||
            UINT64_MAX - dst < offset) {
            return false;
        }
        stw_le_p(word, rounded);
        if (!k230_gnne_dma_write_bytes(dst + offset, word, sizeof(word))) {
            return false;
        }
        *written = 2;
        return true;
    default:
        return false;
    }
}

static uint32_t k230_gnne_mfu_channel(K230GnneFrontend *fe, uint64_t index)
{
    K230GnneMfuAct1Conf *conf = &fe->mfu_act1;
    K230GnneShape *shape;
    uint64_t plane;

    if (!conf->is_by_channel ||
        conf->dest_rshape >= K230_GNNE_SHAPE_COUNT ||
        !fe->shape[conf->dest_rshape].valid) {
        return 0;
    }

    shape = &fe->shape[conf->dest_rshape];
    if (!shape->c || !shape->h || !shape->w ||
        umul64_overflow(shape->h, shape->w, &plane) || !plane) {
        return 0;
    }

    return (index / plane) % shape->c;
}

static bool k230_gnne_mfu_act1_count(K230GnneFrontend *fe, uint64_t *count)
{
    K230GnneMfuAct1Conf *conf = &fe->mfu_act1;
    bool valid;

    if (conf->dest_len_valid && conf->dest_len) {
        *count = conf->dest_len;
        return true;
    }

    *count = k230_gnne_gp(fe, conf->dest_rlen, &valid);
    if (valid && *count) {
        return true;
    }
    return k230_gnne_shape_product(fe, conf->dest_rshape, count);
}

static bool k230_gnne_mfu_binary_source(K230GnneMfuAct1Conf *conf)
{
    return conf->deq[1].valid && conf->src2[0].valid &&
           conf->src2[1].valid;
}

static bool k230_gnne_mfu_dequant_psum(K230GnneFrontend *fe,
                                       uint64_t logical, uint64_t index,
                                       uint32_t quant_type, double scale,
                                       uint32_t shift, double *value)
{
    uint64_t psum_index;

    if (quant_type != 0 ||
        UINT64_MAX - logical < index) {
        return false;
    }

    psum_index = logical + index;
    if (psum_index >= fe->pu_psum_count) {
        return false;
    }

    *value = (double)fe->pu_psum[psum_index] * scale *
             k230_gnne_pow2(-(int64_t)shift);
    return true;
}

static bool k230_gnne_mfu_dequant_source(K230GnneFrontend *fe,
                                         uint64_t base, uint64_t logical,
                                         bool source_type, uint64_t index,
                                         uint32_t quant_type, double scale,
                                         uint32_t bias, uint32_t shift,
                                         double *value)
{
    if (source_type) {
        return k230_gnne_mfu_dequant_psum(fe, logical, index, quant_type,
                                          scale, shift, value);
    }

    return k230_gnne_mfu_dequant(base, index, quant_type, scale, bias, shift,
                                 value);
}

static void k230_gnne_mfu_act1(K230KpuState *s, K230GnneFrontend *fe,
                               uint32_t word, uint64_t pc)
{
    K230GnneMfuAct1Conf *conf = &fe->mfu_act1;
    K230GnneMfuAct1Deq *deq = &conf->deq[0];
    K230GnneMfuAct1Deq *deq2 = &conf->deq[1];
    unsigned int raddr_d1 = extract32(word, 7, 5);
    unsigned int raddr_s1 = extract32(word, 12, 5);
    unsigned int raddr_s2 = extract32(word, 17, 5);
    unsigned int raddr_arg = extract32(word, 22, 5);
    unsigned int write_size;
    uint32_t dst_encoded;
    uint32_t src_encoded;
    uint32_t src2_encoded = 0;
    uint32_t arg_encoded;
    uint32_t scale_raw;
    uint32_t scale2_raw = 0;
    uint32_t bias;
    uint32_t bias2 = 0;
    uint32_t deq_shift;
    uint32_t deq2_shift = 0;
    uint32_t quant_shift;
    uint64_t dst_base;
    uint64_t src_base;
    uint64_t src2_base = 0;
    uint64_t arg_base;
    uint64_t dst_logical;
    uint64_t src_logical;
    uint64_t src2_logical = 0;
    uint64_t arg_logical;
    uint64_t count;
    uint64_t written = 0;
    uint64_t head = 0;
    double scale;
    double scale2 = 0.0;
    bool binary_source;
    bool valid;

    if (!conf->dest_valid || !deq->valid || !conf->quant_valid ||
        !conf->act_valid ||
        !k230_gnne_quant_type_size(conf->quant_type, &write_size) ||
        !k230_gnne_mfu_act1_count(fe, &count) ||
        count > K230_GNNE_MAX_OUTPUT_SIZE / write_size ||
        conf->funct4 > 1) {
        return;
    }
    binary_source = k230_gnne_mfu_binary_source(conf);

    dst_encoded = k230_gnne_gp(fe, raddr_d1, &valid);
    if (!valid) {
        return;
    }
    src_encoded = k230_gnne_gp(fe, raddr_s1, &valid);
    if (!valid) {
        return;
    }
    arg_encoded = k230_gnne_gp(fe, raddr_arg, &valid);
    if (!valid ||
        !k230_gnne_translate(fe, dst_encoded, &dst_base, &dst_logical) ||
        !k230_gnne_translate(fe, src_encoded, conf->src2[0].source_type ?
                             NULL : &src_base, &src_logical) ||
        !k230_gnne_translate(fe, arg_encoded, &arg_base, &arg_logical)) {
        return;
    }
    if (binary_source) {
        src2_encoded = k230_gnne_gp(fe, raddr_s2, &valid);
        if (!valid ||
            !k230_gnne_translate(fe, src2_encoded,
                                 conf->src2[1].source_type ?
                                 NULL : &src2_base, &src2_logical)) {
            return;
        }
    }

    scale_raw = k230_gnne_gp(fe, deq->rscale, &valid);
    if (!valid) {
        return;
    }
    bias = k230_gnne_gp(fe, deq->rbias, &valid);
    if (!valid) {
        return;
    }
    deq_shift = k230_gnne_gp(fe, deq->rshift_bits, &valid);
    if (!valid) {
        return;
    }
    quant_shift = k230_gnne_gp(fe, conf->quant_rshift_bits, &valid);
    if (!valid) {
        return;
    }
    if (binary_source) {
        scale2_raw = k230_gnne_gp(fe, deq2->rscale, &valid);
        if (!valid) {
            return;
        }
        bias2 = k230_gnne_gp(fe, deq2->rbias, &valid);
        if (!valid) {
            return;
        }
        deq2_shift = k230_gnne_gp(fe, deq2->rshift_bits, &valid);
        if (!valid) {
            return;
        }
    }

    scale = k230_gnne_fp16_to_double(scale_raw);
    scale2 = k230_gnne_fp16_to_double(scale2_raw);
    for (uint64_t index = 0; index < count; index++) {
        double value;
        double value2;
        uint32_t channel = k230_gnne_mfu_channel(fe, index);
        unsigned int element_size;

        if (!k230_gnne_mfu_dequant_source(fe, src_base, src_logical,
                                          conf->src2[0].source_type, index,
                                          deq->quant_type, scale, bias,
                                          deq_shift, &value)) {
            return;
        }
        if (binary_source) {
            if (!k230_gnne_mfu_dequant_source(fe, src2_base, src2_logical,
                                              conf->src2[1].source_type,
                                              index, deq2->quant_type,
                                              scale2, bias2, deq2_shift,
                                              &value2)) {
                return;
            }
            if (conf->funct4 == 1) {
                value *= value2;
            } else {
                value += value2;
            }
        }
        if (!k230_gnne_mfu_act1_value(arg_base, channel, value, quant_shift,
                                      &value) ||
            !k230_gnne_mfu_write_quant(dst_base, index, conf->quant_type,
                                       value, &element_size)) {
            return;
        }
        written += element_size;
    }

    if (written) {
        unsigned int head_size = MIN(written, sizeof(head));
        uint8_t head_buf[sizeof(head)] = {};

        if (k230_gnne_dma_read_bytes(dst_base, head_buf, head_size)) {
            head = ldq_le_p(head_buf);
        }
    }

    trace_k230_kpu_mfu_act1(k230_kpu_name(s), pc, src_logical, dst_logical,
                            arg_logical, count, deq->quant_type,
                            conf->quant_type, written, head);
    fe->mfu_act1s++;
    fe->output_bytes += written;
}

static bool k230_gnne_offset4(K230GnneStride *stride, uint32_t n,
                              uint32_t c, uint32_t h, uint32_t w,
                              uint64_t *offset)
{
    uint64_t result = 0;
    uint64_t term;

    if (umul64_overflow((uint64_t)n, stride->n, &term)) {
        return false;
    }
    result += term;
    if (umul64_overflow((uint64_t)c, stride->c, &term) ||
        UINT64_MAX - result < term) {
        return false;
    }
    result += term;
    if (umul64_overflow((uint64_t)h, stride->h, &term) ||
        UINT64_MAX - result < term ||
        UINT64_MAX - result < w) {
        return false;
    }

    *offset = result + term + w;
    return true;
}

static bool k230_gnne_packed_offset4(K230GnneStride *stride, uint32_t n,
                                     uint32_t c, uint32_t h, uint32_t w,
                                     uint64_t *offset)
{
    uint64_t result;
    uint64_t term;

    if (umul64_overflow((uint64_t)n, stride->n, &result) ||
        UINT64_MAX - result < c) {
        return false;
    }
    result += c;
    if (umul64_overflow(result, stride->c, &term) ||
        UINT64_MAX - term < h) {
        return false;
    }
    result = term + h;
    if (umul64_overflow(result, stride->h, &term) ||
        UINT64_MAX - term < w) {
        return false;
    }

    *offset = term + w;
    return true;
}

static bool k230_gnne_packed_stride_footprint(K230GnneShape *shape,
                                              K230GnneStride *stride,
                                              uint64_t *span)
{
    uint64_t last;

    if (!shape->n || !shape->c || !shape->h || !shape->w) {
        *span = 0;
        return true;
    }

    if (!k230_gnne_packed_offset4(stride, shape->n - 1, shape->c - 1,
                                  shape->h - 1, shape->w - 1, &last) ||
        last == UINT64_MAX) {
        return false;
    }

    *span = last + 1;
    return true;
}

static bool k230_gnne_mfu_pdp1_source_offset(K230GnneStride *stride,
                                             uint32_t n, uint32_t c,
                                             uint32_t h, uint32_t w,
                                             uint64_t *offset)
{
    return k230_gnne_packed_offset4(stride, n, c, h, w, offset);
}

static bool k230_gnne_mfu_pdp1_quant(K230GnneFrontend *fe,
                                     K230GnneMfuPdp1Conf *conf,
                                     double *value)
{
    uint32_t scale_raw;
    uint32_t bias;
    uint32_t shift;
    double scale;
    bool valid;

    scale_raw = k230_gnne_gp(fe, conf->quant_rscale, &valid);
    if (!valid) {
        return false;
    }
    bias = k230_gnne_gp(fe, conf->quant_rbias, &valid);
    if (!valid) {
        return false;
    }
    shift = k230_gnne_gp(fe, conf->quant_rshift_bits, &valid);
    if (!valid) {
        return false;
    }

    scale = k230_gnne_fp16_to_double(scale_raw);
    if (scale != 0.0) {
        *value /= scale;
    }
    *value += bias;
    *value *= k230_gnne_pow2(shift);
    return true;
}

static void k230_gnne_mfu_pdp1(K230KpuState *s, K230GnneFrontend *fe,
                               uint32_t word, uint64_t pc)
{
    K230GnneMfuPdp1Conf *conf = &fe->mfu_pdp1;
    K230GnneMfuAct1Deq *deq = &conf->deq;
    K230GnneShape *input_shape;
    K230GnneStride input_stride;
    K230GnneStride output_stride;
    unsigned int raddr_d = extract32(word, 7, 5);
    unsigned int raddr_s = extract32(word, 12, 5);
    unsigned int rshape = extract32(word, 17, 3);
    unsigned int write_size;
    uint32_t dst_encoded;
    uint32_t src_encoded;
    uint32_t scale_raw;
    uint32_t bias;
    uint32_t deq_shift;
    uint32_t window_h;
    uint32_t window_w;
    uint64_t dst_base;
    uint64_t src_base;
    uint64_t dst_logical;
    uint64_t src_logical;
    uint64_t output_count;
    uint64_t samples;
    uint64_t written = 0;
    uint64_t head = 0;
    double scale;
    bool valid;

    if (rshape >= K230_GNNE_SHAPE_COUNT ||
        !fe->shape[rshape].valid ||
        !conf->conf1_valid || !deq->valid || !conf->quant_valid ||
        conf->funct2 > 3 ||
        !k230_gnne_stride_value(fe, conf->rstride_s, &input_stride) ||
        !k230_gnne_stride_value(fe, conf->rstride_d, &output_stride) ||
        !k230_gnne_quant_type_size(conf->quant_type, &write_size)) {
        return;
    }

    input_shape = &fe->shape[rshape];
    if (!input_shape->n || !input_shape->c ||
        !input_shape->h || !input_shape->w ||
        umul64_overflow((uint64_t)input_shape->n, input_shape->c,
                        &output_count) ||
        output_count > K230_GNNE_MAX_OUTPUT_SIZE / write_size) {
        return;
    }

    window_h = conf->conf4_valid && conf->rwindow_h ?
               MIN(conf->rwindow_h, input_shape->h) : input_shape->h;
    window_w = conf->conf4_valid && conf->rwindow_w ?
               MIN(conf->rwindow_w, input_shape->w) : input_shape->w;
    if (!window_h || !window_w ||
        umul64_overflow((uint64_t)window_h, window_w, &samples) ||
        !samples) {
        return;
    }

    dst_encoded = k230_gnne_gp(fe, raddr_d, &valid);
    if (!valid) {
        return;
    }
    src_encoded = k230_gnne_gp(fe, raddr_s, &valid);
    if (!valid ||
        !k230_gnne_translate(fe, dst_encoded, &dst_base, &dst_logical) ||
        !k230_gnne_translate(fe, src_encoded, &src_base, &src_logical)) {
        return;
    }

    scale_raw = k230_gnne_gp(fe, deq->rscale, &valid);
    if (!valid) {
        return;
    }
    bias = k230_gnne_gp(fe, deq->rbias, &valid);
    if (!valid) {
        return;
    }
    deq_shift = k230_gnne_gp(fe, deq->rshift_bits, &valid);
    if (!valid) {
        return;
    }
    scale = k230_gnne_fp16_to_double(scale_raw);

    for (uint32_t n = 0; n < input_shape->n; n++) {
        for (uint32_t c = 0; c < input_shape->c; c++) {
            unsigned int element_size;
            uint64_t output_index;
            double result = 0.0;
            bool have_sample = false;

            for (uint32_t h = 0; h < window_h; h++) {
                for (uint32_t w = 0; w < window_w; w++) {
                    uint64_t input_index;
                    double value;

                    if (!k230_gnne_mfu_pdp1_source_offset(&input_stride,
                                                          n, c, h, w,
                                                          &input_index) ||
                        !k230_gnne_mfu_dequant(src_base, input_index,
                                               deq->quant_type, scale, bias,
                                               deq_shift, &value)) {
                        return;
                    }
                    if (!have_sample) {
                        result = value;
                        have_sample = true;
                    } else if (conf->funct2 == 0 && value > result) {
                        result = value;
                    } else if (conf->funct2 == 1 && value < result) {
                        result = value;
                    } else if (conf->funct2 >= 2) {
                        result += value;
                    }
                }
            }

            if (!have_sample) {
                return;
            }
            if (!k230_gnne_offset4(&output_stride, n, c, 0, 0,
                                   &output_index)) {
                return;
            }
            if (conf->funct2 == 2) {
                result /= samples;
            }
            if (!k230_gnne_mfu_pdp1_quant(fe, conf, &result) ||
                !k230_gnne_mfu_write_quant(dst_base, output_index,
                                           conf->quant_type, result,
                                           &element_size)) {
                return;
            }
            written += element_size;
        }
    }

    if (written) {
        unsigned int head_size = MIN(written, sizeof(head));
        uint8_t head_buf[sizeof(head)] = {};

        if (k230_gnne_dma_read_bytes(dst_base, head_buf, head_size)) {
            head = ldq_le_p(head_buf);
        }
    }

    trace_k230_kpu_mfu_pdp1(k230_kpu_name(s), pc, src_logical, dst_logical,
                            output_count, conf->funct2, deq->quant_type,
                            conf->quant_type, written, head);
    fe->mfu_pdp1s++;
    fe->output_bytes += written;
}

static const uint8_t *k230_gnne_permute_axes(uint32_t permute)
{
    static const uint8_t axes[24][4] = {
        { 0, 1, 2, 3 }, /* NCHW */
        { 0, 1, 3, 2 }, /* NCWH */
        { 0, 2, 1, 3 }, /* NHCW */
        { 0, 2, 3, 1 }, /* NHWC */
        { 0, 3, 1, 2 }, /* NWCH */
        { 0, 3, 2, 1 }, /* NWHC */
        { 1, 0, 2, 3 }, /* CNHW */
        { 1, 0, 3, 2 }, /* CNWH */
        { 1, 2, 0, 3 }, /* CHNW */
        { 1, 2, 3, 0 }, /* CHWN */
        { 1, 3, 0, 2 }, /* CWNH */
        { 1, 3, 2, 0 }, /* CWHN */
        { 2, 0, 1, 3 }, /* HNCW */
        { 2, 0, 3, 1 }, /* HNWC */
        { 2, 1, 0, 3 }, /* HCNW */
        { 2, 1, 3, 0 }, /* HCWN */
        { 2, 3, 0, 1 }, /* HWNC */
        { 2, 3, 1, 0 }, /* HWCN */
        { 3, 0, 1, 2 }, /* WNCH */
        { 3, 0, 2, 1 }, /* WNHC */
        { 3, 1, 0, 2 }, /* WCNH */
        { 3, 1, 2, 0 }, /* WCHN */
        { 3, 2, 0, 1 }, /* WHNC */
        { 3, 2, 1, 0 }, /* WHCN */
    };

    return permute < ARRAY_SIZE(axes) ? axes[permute] : NULL;
}

static bool k230_gnne_linear_offset4(K230GnneShape *shape, uint32_t n,
                                     uint32_t c, uint32_t h, uint32_t w,
                                     uint64_t *offset)
{
    uint64_t result;

    if (umul64_overflow((uint64_t)n, shape->c, &result) ||
        UINT64_MAX - result < c) {
        return false;
    }
    result += c;
    if (umul64_overflow(result, shape->h, &result) ||
        UINT64_MAX - result < h) {
        return false;
    }
    result += h;
    if (umul64_overflow(result, shape->w, &result) ||
        UINT64_MAX - result < w) {
        return false;
    }

    *offset = result + w;
    return true;
}

static bool k230_gnne_permute_shape(K230GnneShape *input_shape,
                                    uint32_t permute,
                                    K230GnneShape *output_shape)
{
    const uint8_t *axes = k230_gnne_permute_axes(permute);
    uint32_t input_dims[4] = {
        input_shape->n, input_shape->c, input_shape->h, input_shape->w,
    };

    if (!axes) {
        return false;
    }

    output_shape->n = input_dims[axes[0]];
    output_shape->c = input_dims[axes[1]];
    output_shape->h = input_dims[axes[2]];
    output_shape->w = input_dims[axes[3]];
    output_shape->valid = true;
    return true;
}

static bool k230_gnne_permute_coords(uint32_t n, uint32_t c, uint32_t h,
                                     uint32_t w, uint32_t permute,
                                     uint32_t output_coords[4])
{
    const uint8_t *axes = k230_gnne_permute_axes(permute);
    uint32_t input_coords[4] = { n, c, h, w };

    if (!axes) {
        return false;
    }

    for (unsigned int i = 0; i < 4; i++) {
        output_coords[i] = input_coords[axes[i]];
    }
    return true;
}

static void k230_gnne_mfu_transpose(K230KpuState *s, K230GnneFrontend *fe,
                                    uint32_t word, uint64_t pc)
{
    K230GnneMfuTransposeConf *conf = &fe->mfu_transpose;
    K230GnneShape *input_shape;
    K230GnneShape output_shape;
    K230GnneStride input_stride;
    K230GnneStride output_stride;
    unsigned int raddr_d = extract32(word, 7, 5);
    unsigned int raddr_s = extract32(word, 12, 5);
    unsigned int rshape = extract32(word, 17, 3);
    unsigned int element_size;
    uint32_t dst_encoded;
    uint32_t src_encoded;
    uint64_t dst_base;
    uint64_t src_base;
    uint64_t dst_logical;
    uint64_t src_logical;
    uint64_t input_count;
    uint64_t input_span;
    uint64_t output_span;
    uint64_t written = 0;
    uint64_t head = 0;
    g_autofree uint8_t *tmp = NULL;
    bool valid;

    if (!conf->valid ||
        rshape >= K230_GNNE_SHAPE_COUNT ||
        !fe->shape[rshape].valid ||
        !k230_gnne_l2_datatype_size(conf->l2_datatype, &element_size) ||
        !k230_gnne_stride_value(fe, conf->rstride_s, &input_stride) ||
        !k230_gnne_stride_value(fe, conf->rstride_d, &output_stride) ||
        !k230_gnne_shape_product(fe, rshape, &input_count)) {
        return;
    }

    input_shape = &fe->shape[rshape];
    if (!k230_gnne_permute_shape(input_shape, conf->permute, &output_shape) ||
        !k230_gnne_packed_stride_footprint(input_shape, &input_stride,
                                           &input_span) ||
        !k230_gnne_packed_stride_footprint(&output_shape, &output_stride,
                                           &output_span) ||
        input_count > K230_GNNE_MAX_OUTPUT_SIZE / element_size ||
        input_span > K230_GNNE_MAX_OUTPUT_SIZE / element_size ||
        output_span > K230_GNNE_MAX_OUTPUT_SIZE / element_size) {
        return;
    }

    dst_encoded = k230_gnne_gp(fe, raddr_d, &valid);
    if (!valid) {
        return;
    }
    src_encoded = k230_gnne_gp(fe, raddr_s, &valid);
    if (!valid ||
        !k230_gnne_translate(fe, dst_encoded, &dst_base, &dst_logical) ||
        !k230_gnne_translate(fe, src_encoded, &src_base, &src_logical)) {
        return;
    }

    tmp = g_malloc(input_count * element_size);
    for (uint32_t n = 0; n < input_shape->n; n++) {
        for (uint32_t c = 0; c < input_shape->c; c++) {
            for (uint32_t h = 0; h < input_shape->h; h++) {
                for (uint32_t w = 0; w < input_shape->w; w++) {
                    uint64_t input_index;
                    uint64_t linear_index;
                    uint64_t input_offset;
                    uint64_t linear_offset;

                    if (!k230_gnne_packed_offset4(&input_stride, n, c, h, w,
                                                  &input_index) ||
                        !k230_gnne_linear_offset4(input_shape, n, c, h, w,
                                                  &linear_index) ||
                        umul64_overflow(input_index, element_size,
                                        &input_offset) ||
                        umul64_overflow(linear_index, element_size,
                                        &linear_offset) ||
                        UINT64_MAX - src_base < input_offset ||
                        !k230_gnne_dma_read_bytes(src_base + input_offset,
                                                  tmp + linear_offset,
                                                  element_size)) {
                        return;
                    }
                }
            }
        }
    }

    for (uint32_t n = 0; n < input_shape->n; n++) {
        for (uint32_t c = 0; c < input_shape->c; c++) {
            for (uint32_t h = 0; h < input_shape->h; h++) {
                for (uint32_t w = 0; w < input_shape->w; w++) {
                    uint32_t output_coords[4];
                    uint64_t linear_index;
                    uint64_t output_index;
                    uint64_t linear_offset;
                    uint64_t output_offset;

                    if (!k230_gnne_permute_coords(n, c, h, w, conf->permute,
                                                  output_coords) ||
                        !k230_gnne_linear_offset4(input_shape, n, c, h, w,
                                                  &linear_index) ||
                        !k230_gnne_packed_offset4(&output_stride,
                                                  output_coords[0],
                                                  output_coords[1],
                                                  output_coords[2],
                                                  output_coords[3],
                                                  &output_index) ||
                        umul64_overflow(linear_index, element_size,
                                        &linear_offset) ||
                        umul64_overflow(output_index, element_size,
                                        &output_offset) ||
                        UINT64_MAX - dst_base < output_offset ||
                        !k230_gnne_dma_write_bytes(dst_base + output_offset,
                                                   tmp + linear_offset,
                                                   element_size)) {
                        return;
                    }
                    written += element_size;
                }
            }
        }
    }

    if (written) {
        unsigned int head_size = MIN(written, sizeof(head));
        uint8_t head_buf[sizeof(head)] = {};

        if (k230_gnne_dma_read_bytes(dst_base, head_buf, head_size)) {
            head = ldq_le_p(head_buf);
        }
    }

    trace_k230_kpu_mfu_transpose(k230_kpu_name(s), pc, src_logical,
                                 dst_logical, input_count, conf->permute,
                                 conf->l2_datatype, written, head);
    fe->mfu_transposes++;
    fe->output_bytes += written;
}

static bool k230_gnne_act0_write(uint64_t dst, uint64_t index,
                                 uint32_t datatype, double value,
                                 unsigned int *written)
{
    uint64_t offset;
    int64_t rounded;
    uint8_t byte;
    uint8_t word[2];

    switch (datatype) {
    case 0:
        rounded = k230_gnne_clamp_i64(k230_gnne_round_nearest(value), 0,
                                      255);
        byte = rounded;
        if (UINT64_MAX - dst < index ||
            !k230_gnne_dma_write_bytes(dst + index, &byte, sizeof(byte))) {
            return false;
        }
        *written = 1;
        return true;
    case 1:
        rounded = k230_gnne_clamp_i64(k230_gnne_round_nearest(value), -127,
                                      127);
        byte = rounded;
        if (UINT64_MAX - dst < index ||
            !k230_gnne_dma_write_bytes(dst + index, &byte, sizeof(byte))) {
            return false;
        }
        *written = 1;
        return true;
    case 2:
        if (umul64_overflow(index, 2, &offset) ||
            UINT64_MAX - dst < offset ||
            !k230_gnne_write_fp16(dst + offset, value)) {
            return false;
        }
        *written = 2;
        return true;
    case 3:
        rounded = k230_gnne_clamp_i64(k230_gnne_round_nearest(value), -32767,
                                      32767);
        if (umul64_overflow(index, 2, &offset) ||
            UINT64_MAX - dst < offset) {
            return false;
        }
        stw_le_p(word, rounded);
        if (!k230_gnne_dma_write_bytes(dst + offset, word, sizeof(word))) {
            return false;
        }
        *written = 2;
        return true;
    default:
        return false;
    }
}

static bool k230_gnne_act0_value(uint64_t params, uint32_t channel,
                                 int32_t psum, uint32_t shift,
                                 double *result)
{
    uint64_t base;
    double input;
    double threshold;
    double slope;
    double bias;
    double lower;
    double upper;

    if (umul64_overflow((uint64_t)channel, 14, &base) ||
        UINT64_MAX - params < base) {
        return false;
    }
    base += params;

    input = k230_gnne_fp16_to_double(k230_gnne_double_to_fp16(
        (double)psum * k230_gnne_pow2(-(int64_t)shift)));

    if (!k230_gnne_read_fp16(base + 12, &threshold)) {
        return false;
    }
    if (input < threshold) {
        if (!k230_gnne_read_fp16(base, &slope) ||
            !k230_gnne_read_fp16(base + 4, &bias)) {
            return false;
        }
    } else {
        if (!k230_gnne_read_fp16(base + 2, &slope) ||
            !k230_gnne_read_fp16(base + 6, &bias)) {
            return false;
        }
    }
    if (!k230_gnne_read_fp16(base + 8, &lower) ||
        !k230_gnne_read_fp16(base + 10, &upper)) {
        return false;
    }

    input = k230_gnne_fp16_to_double(k230_gnne_double_to_fp16(input * slope));
    input = k230_gnne_fp16_to_double(k230_gnne_double_to_fp16(input + bias));
    if (input < lower) {
        input = lower;
    } else if (input > upper) {
        input = upper;
    }

    *result = input;
    return true;
}

static int32_t k230_gnne_pu_shift_psum(int32_t value, uint32_t mode)
{
    int64_t shifted;

    switch (mode) {
    case 0:
        return value;
    case 1:
        shifted = (int64_t)value << 4;
        return k230_gnne_clamp_i64(shifted, INT32_MIN, INT32_MAX);
    default:
        return value >> 4;
    }
}

static int32_t k230_gnne_add_i32_sat(int32_t a, int32_t b)
{
    int64_t sum = (int64_t)a + b;

    return k230_gnne_clamp_i64(sum, INT32_MIN, INT32_MAX);
}

static bool k230_gnne_prepare_psum(K230GnneFrontend *fe, uint64_t count)
{
    if (count > K230_GNNE_MAX_OUTPUT_SIZE / sizeof(int32_t)) {
        return false;
    }
    if (fe->pu_psum_valid && fe->pu_psum_count >= count) {
        return true;
    }

    g_free(fe->pu_psum);
    fe->pu_psum = g_new0(int32_t, count);
    fe->pu_psum_count = count;
    fe->pu_psum_valid = true;
    return true;
}

static bool k230_gnne_read_u8(uint64_t addr, uint8_t *value)
{
    return k230_gnne_dma_read_bytes(addr, value, sizeof(*value));
}

static bool k230_gnne_pu_read_input(uint64_t addr, uint32_t quant_type,
                                    int32_t zero_point, int32_t *value)
{
    uint8_t raw;

    if (!k230_gnne_read_u8(addr, &raw)) {
        return false;
    }

    switch (quant_type) {
    case 1:
        *value = (int32_t)raw - zero_point;
        return true;
    case 2:
        *value = (int8_t)raw;
        return true;
    default:
        return false;
    }
}

static void k230_gnne_pu_compute(K230KpuState *s, K230GnneFrontend *fe,
                                 uint32_t word, uint64_t pc)
{
    K230GnnePuConf *pu = &fe->pu_conf;
    K230GnneShape input_shape;
    K230GnneShape output_shape;
    K230GnneStride input_stride;
    K230GnneStride output_stride;
    unsigned int tcu_id = extract32(word, 7, 3);
    unsigned int of_shift_mode = extract32(word, 10, 2);
    unsigned int dest_size;
    uint32_t input_encoded;
    uint32_t weight_encoded = 0;
    uint32_t weight_zp_encoded = 0;
    uint32_t act0_encoded;
    uint32_t dest_encoded;
    int32_t input_zp;
    uint64_t input_base;
    uint64_t weight_base = 0;
    uint64_t weight_zp_base = 0;
    uint64_t act0_base = 0;
    uint64_t dest_base = 0;
    uint64_t dest_logical = 0;
    uint64_t psum_base_index = 0;
    uint64_t output_count = 0;
    uint64_t output_span = 0;
    uint64_t psum_span;
    uint64_t written = 0;
    uint64_t head = 0;
    bool to_act0;
    bool deconv;
    bool valid;

#define PU_COMPUTE_SKIP(reason, source, dest, detail) \
    do { \
        trace_k230_kpu_pu_compute_skip(k230_kpu_name(s), pc, reason, \
                                       tcu_id, source, dest, detail); \
        return; \
    } while (0)

    if (!fe->dm_load_w_conf.valid || !fe->dm_load_w.valid ||
        !pu->fetch1_valid || !pu->fetch3_valid ||
        !pu->fetch_deq_valid || !pu->w_valid || !pu->of1_valid ||
        !pu->of2_valid || !pu->compute_valid || pu->mode > 1 ||
        pu->dest_target > 1 ||
        (pu->mode == 0 && (pu->quant_type < 1 || pu->quant_type > 2)) ||
        (pu->mode == 1 && (pu->quant_type < 1 || pu->quant_type > 2 ||
                           fe->dm_load_w_conf.kernel_h != 1 ||
                           fe->dm_load_w_conf.kernel_w != 1))) {
        PU_COMPUTE_SKIP(K230_GNNE_SKIP_CONF, 0, 0, pu->mode);
    }
    deconv = pu->mode == 1;
    to_act0 = pu->dest_target == 1;
    if (to_act0) {
        if (!fe->dm_load_act0.valid || !fe->act0_conf.valid ||
            !fe->act0_compute.valid ||
            !k230_gnne_act0_type_size(fe->act0_compute.dest_datatype,
                                      &dest_size)) {
            PU_COMPUTE_SKIP(K230_GNNE_SKIP_ACT0, 0, 0,
                            fe->act0_compute.dest_datatype);
        }
    } else {
        dest_size = sizeof(int32_t);
    }

    if (pu->rshape >= K230_GNNE_SHAPE_COUNT ||
        !fe->shape[pu->rshape].valid ||
        !k230_gnne_stride_value(fe, pu->rstride_s, &input_stride)) {
        PU_COMPUTE_SKIP(K230_GNNE_SKIP_SHAPE, pu->rshape, pu->rshape_d,
                        pu->rstride_s);
    }

    input_shape = fe->shape[pu->rshape];
    if (pu->output_shape_valid) {
        output_shape = pu->output_shape;
    } else if (pu->rshape_d >= K230_GNNE_SHAPE_COUNT ||
               !fe->shape[pu->rshape_d].valid) {
        PU_COMPUTE_SKIP(K230_GNNE_SKIP_SHAPE, pu->rshape, pu->rshape_d,
                        pu->rstride_s);
    } else {
        output_shape = fe->shape[pu->rshape_d];
    }
    if (pu->output_stride_valid) {
        output_stride = pu->output_stride;
    } else if (!k230_gnne_stride_value(fe, pu->rstride_d, &output_stride)) {
        PU_COMPUTE_SKIP(K230_GNNE_SKIP_SHAPE, pu->rshape, pu->rshape_d,
                        pu->rstride_d);
    }

    if (!input_shape.c || !output_shape.c ||
        !fe->dm_load_w_conf.kernel_h || !fe->dm_load_w_conf.kernel_w ||
        !k230_gnne_shape_count(&output_shape, &output_count) ||
        !k230_gnne_packed_stride_footprint(&output_shape, &output_stride,
                                           &output_span) ||
        output_count > K230_GNNE_MAX_OUTPUT_SIZE / dest_size ||
        output_span > K230_GNNE_MAX_OUTPUT_SIZE / dest_size) {
        PU_COMPUTE_SKIP(K230_GNNE_SKIP_COUNT, output_count, output_span,
                        dest_size);
    }

    if (!to_act0) {
        dest_encoded = k230_gnne_gp(fe, pu->raddr_d, &valid);
        if (!valid ||
            !k230_gnne_translate(fe, dest_encoded, NULL, &dest_logical)) {
            PU_COMPUTE_SKIP(K230_GNNE_SKIP_DST_TRANSLATE, 0,
                            dest_encoded, pu->raddr_d);
        }
        psum_base_index = dest_logical;
        fe->pu_psum_base = psum_base_index;
        fe->pu_psum_base_valid = true;
    } else if (pu->load_psum) {
        if (fe->pu_psum_base_valid) {
            psum_base_index = fe->pu_psum_base;
        } else {
            dest_encoded = k230_gnne_gp(fe, pu->raddr_d, &valid);
            if (!valid ||
                !k230_gnne_translate(fe, dest_encoded, NULL,
                                     &dest_logical)) {
                PU_COMPUTE_SKIP(K230_GNNE_SKIP_DST_TRANSLATE, 0,
                                dest_encoded, pu->raddr_d);
            }
            psum_base_index = dest_logical;
        }
    }
    if (UINT64_MAX - psum_base_index < output_span) {
        PU_COMPUTE_SKIP(K230_GNNE_SKIP_OVERFLOW, psum_base_index,
                        output_span, 0);
    }
    psum_span = psum_base_index + output_span;
    if (!k230_gnne_prepare_psum(fe, psum_span)) {
        PU_COMPUTE_SKIP(K230_GNNE_SKIP_PSUM, psum_base_index,
                        output_span, psum_span);
    }
    if (pu->clr_psum && output_span) {
        memset(fe->pu_psum, 0, psum_span * sizeof(*fe->pu_psum));
    }

    if (deconv) {
        input_encoded = k230_gnne_gp(fe, pu->raddr_s, &valid);
    } else {
        input_encoded = k230_gnne_gp(fe, pu->raddr_s, &valid);
        if (!valid && fe->dm_load_l1.valid) {
            input_encoded = k230_gnne_gp(fe, fe->dm_load_l1.raddr_s, &valid);
        }
    }
    if (!valid) {
        PU_COMPUTE_SKIP(K230_GNNE_SKIP_SRC_GP, pu->raddr_s, 0, 0);
    }
    weight_encoded = k230_gnne_gp(fe, fe->dm_load_w.raddr_s, &valid);
    if (!valid) {
        PU_COMPUTE_SKIP(K230_GNNE_SKIP_SRC_GP, fe->dm_load_w.raddr_s, 0, 1);
    }
    weight_zp_encoded = k230_gnne_gp(fe, fe->dm_load_w.raddr_bw, &valid);
    if (!valid) {
        PU_COMPUTE_SKIP(K230_GNNE_SKIP_SRC_GP, fe->dm_load_w.raddr_bw, 0, 2);
    }
    input_zp = k230_gnne_gp(fe, pu->rbx, &valid);
    if (!valid ||
        !k230_gnne_translate(fe, input_encoded, &input_base, NULL) ||
        !k230_gnne_translate(fe, weight_encoded, &weight_base, NULL) ||
        !k230_gnne_translate(fe, weight_zp_encoded, &weight_zp_base, NULL)) {
        PU_COMPUTE_SKIP(K230_GNNE_SKIP_SRC_TRANSLATE, input_encoded,
                        weight_encoded, weight_zp_encoded);
    }
    if (to_act0) {
        act0_encoded = k230_gnne_gp(fe, fe->dm_load_act0.raddr_s, &valid);
        if (!valid) {
            PU_COMPUTE_SKIP(K230_GNNE_SKIP_ACT0,
                            fe->dm_load_act0.raddr_s, 0, 0);
        }
        dest_encoded = k230_gnne_gp(fe, fe->act0_compute.raddr_d, &valid);
        if (!valid) {
            dest_encoded = k230_gnne_gp(fe, pu->raddr_d, &valid);
        }
        if (!k230_gnne_translate(fe, act0_encoded, &act0_base, NULL) ||
            !k230_gnne_translate(fe, dest_encoded, &dest_base,
                                 &dest_logical)) {
            PU_COMPUTE_SKIP(K230_GNNE_SKIP_ACT0, act0_encoded,
                            dest_encoded, 1);
        }
    }

    for (uint32_t n = 0; n < output_shape.n; n++) {
        for (uint32_t oc = 0; oc < output_shape.c; oc++) {
            uint8_t weight_zp;
            int32_t signed_weight_zp;

            if (!k230_gnne_read_u8(weight_zp_base + oc, &weight_zp)) {
                PU_COMPUTE_SKIP(K230_GNNE_SKIP_SOURCE_READ,
                                weight_zp_base + oc, 0, oc);
            }
            signed_weight_zp = weight_zp;
            for (uint32_t oh = 0; oh < output_shape.h; oh++) {
                for (uint32_t ow = 0; ow < output_shape.w; ow++) {
                    int32_t acc = 0;
                    double activated;
                    uint32_t act_channel;
                    unsigned int written_size;
                    uint64_t output_index;
                    uint64_t psum_index;
                    uint64_t output_offset;

                    for (uint32_t ic = 0; ic < input_shape.c; ic++) {
                        for (uint32_t ky = 0;
                             ky < fe->dm_load_w_conf.kernel_h; ky++) {
                            for (uint32_t kx = 0;
                                 kx < fe->dm_load_w_conf.kernel_w; kx++) {
                                uint32_t ih = oh * pu->stride_h + ky;
                                uint32_t iw = ow * pu->stride_w + kx;
                                uint64_t input_index;
                                uint64_t weight_index;
                                int32_t input;
                                uint8_t weight;

                                if (deconv) {
                                    ih = oh;
                                    iw = ow;
                                }
                                if (ih >= input_shape.h ||
                                    iw >= input_shape.w) {
                                    continue;
                                }
                                if (!k230_gnne_packed_offset4(&input_stride,
                                                              n, ic, ih, iw,
                                                              &input_index)) {
                                    PU_COMPUTE_SKIP(K230_GNNE_SKIP_OVERFLOW,
                                                    input_base, ic, ih);
                                }
                                weight_index = ((uint64_t)oc *
                                                fe->dm_load_w_conf.kernel_h *
                                                fe->dm_load_w_conf.kernel_w +
                                                (uint64_t)ky *
                                                fe->dm_load_w_conf.kernel_w +
                                                kx) *
                                               K230_GNNE_L2_LANE_WIDTH + ic;
                                if (!k230_gnne_pu_read_input(input_base +
                                                             input_index,
                                                             pu->quant_type,
                                                             input_zp,
                                                             &input) ||
                                    !k230_gnne_read_u8(weight_base +
                                                       weight_index,
                                                       &weight)) {
                                    PU_COMPUTE_SKIP(
                                        K230_GNNE_SKIP_SOURCE_READ,
                                        input_base + input_index,
                                        weight_base + weight_index,
                                        ic);
                                }
                                acc += input *
                                       ((int32_t)weight - signed_weight_zp);
                            }
                        }
                    }

                    if (!k230_gnne_packed_offset4(&output_stride, n, oc, oh,
                                                  ow, &output_index)) {
                        PU_COMPUTE_SKIP(K230_GNNE_SKIP_OVERFLOW,
                                        dest_base, oc, oh);
                    }
                    psum_index = output_index;
                    if (!to_act0 || pu->load_psum) {
                        if (UINT64_MAX - psum_base_index < output_index) {
                            PU_COMPUTE_SKIP(K230_GNNE_SKIP_OVERFLOW,
                                            psum_base_index, output_index, 1);
                        }
                        psum_index = psum_base_index + output_index;
                    }
                    acc = k230_gnne_pu_shift_psum(acc, of_shift_mode);
                    if (pu->load_psum) {
                        if (psum_index >= fe->pu_psum_count) {
                            PU_COMPUTE_SKIP(K230_GNNE_SKIP_PSUM,
                                            psum_index, fe->pu_psum_count, 0);
                        }
                        acc = k230_gnne_add_i32_sat(acc,
                                                    fe->pu_psum[psum_index]);
                    }
                    if (!to_act0) {
                        if (psum_index >= fe->pu_psum_count) {
                            PU_COMPUTE_SKIP(K230_GNNE_SKIP_PSUM,
                                            psum_index, fe->pu_psum_count, 1);
                        }
                        fe->pu_psum[psum_index] = acc;
                        written += sizeof(int32_t);
                        continue;
                    }
                    act_channel = fe->act0_compute.is_by_channel ? oc : 0;
                    if (!k230_gnne_act0_value(act0_base, act_channel, acc,
                                              fe->act0_conf.rshift_bits,
                                              &activated)) {
                        PU_COMPUTE_SKIP(K230_GNNE_SKIP_ACT0, act0_base,
                                        act_channel, acc);
                    }

                    if (umul64_overflow(output_index, dest_size,
                                        &output_offset) ||
                        UINT64_MAX - dest_base < output_offset ||
                        !k230_gnne_act0_write(dest_base + output_offset, 0,
                                              fe->act0_compute.dest_datatype,
                                              activated, &written_size)) {
                        PU_COMPUTE_SKIP(K230_GNNE_SKIP_DEST_WRITE,
                                        dest_base, output_offset,
                                        output_index);
                    }
                    written += written_size;
                }
            }
        }
    }

    if (to_act0 && written) {
        unsigned int head_size = MIN(written, sizeof(head));
        uint8_t head_buf[sizeof(head)] = {};

        if (k230_gnne_dma_read_bytes(dest_base, head_buf, head_size)) {
            head = ldq_le_p(head_buf);
        }
    }

    trace_k230_kpu_pu_compute(k230_kpu_name(s), pc, tcu_id, dest_logical,
                              written, head);
    fe->pu_computes++;
    fe->output_bytes += written;
#undef PU_COMPUTE_SKIP
}

static void k230_gnne_pdp0_compute(K230KpuState *s, K230GnneFrontend *fe,
                                   uint32_t word, uint64_t pc)
{
    K230GnnePdp0Conf *pdp0 = &fe->pdp0_conf;
    K230GnneShape input_shape;
    K230GnneShape output_shape;
    K230GnneStride output_stride;
    unsigned int tcu_id = extract32(word, 7, 3);
    unsigned int raddr_s = extract32(word, 10, 5);
    unsigned int dest_size;
    uint32_t input_encoded;
    uint32_t weight_encoded;
    uint32_t weight_zp_encoded;
    uint32_t act0_encoded;
    uint32_t dest_encoded;
    int32_t input_zp;
    uint64_t input_base;
    uint64_t weight_base;
    uint64_t weight_zp_base;
    uint64_t act0_base;
    uint64_t dest_base;
    uint64_t dest_logical;
    uint64_t input_count;
    uint64_t output_count;
    uint64_t written = 0;
    uint64_t head = 0;
    bool valid;

    if (!fe->dm_load_act0.valid || !fe->dm_store_of.valid ||
        !fe->act0_conf.valid ||
        !fe->act0_compute.valid || !pdp0->mode_valid ||
        !pdp0->fetch1_valid || !pdp0->fetch3_valid ||
        !pdp0->fetch_deq_valid || !pdp0->w_valid || !pdp0->of_valid ||
        pdp0->mode > 4 || pdp0->quant_type != 1 ||
        (pdp0->mode == 0 && !fe->dm_load_w.valid) ||
        !k230_gnne_act0_type_size(fe->act0_compute.dest_datatype,
                                  &dest_size)) {
        return;
    }

    if (pdp0->input_shape_valid) {
        input_shape = pdp0->input_shape;
    } else if (pdp0->rshape >= K230_GNNE_SHAPE_COUNT ||
               !fe->shape[pdp0->rshape].valid) {
        return;
    } else {
        input_shape = fe->shape[pdp0->rshape];
    }
    if (pdp0->output_shape_valid) {
        output_shape = pdp0->output_shape;
    } else if (pdp0->rshape_d >= K230_GNNE_SHAPE_COUNT ||
               !fe->shape[pdp0->rshape_d].valid) {
        return;
    } else {
        output_shape = fe->shape[pdp0->rshape_d];
    }
    if (pdp0->output_stride_valid) {
        output_stride = pdp0->output_stride;
    } else if (!k230_gnne_stride_value(fe, pdp0->rstride_d,
                                       &output_stride)) {
        return;
    }

    if (!input_shape.c || !input_shape.h || !input_shape.w ||
        !output_shape.c || output_shape.c > input_shape.c ||
        !pdp0->kernel_h || !pdp0->kernel_w ||
        !k230_gnne_shape_count(&input_shape, &input_count) ||
        !k230_gnne_shape_count(&output_shape, &output_count) ||
        input_count > K230_GNNE_MAX_OUTPUT_SIZE ||
        output_count > K230_GNNE_MAX_OUTPUT_SIZE / dest_size) {
        return;
    }

    input_encoded = k230_gnne_gp(fe, raddr_s, &valid);
    if (!valid) {
        return;
    }
    if (pdp0->mode == 0) {
        weight_encoded = k230_gnne_gp(fe, fe->dm_load_w.raddr_s, &valid);
        if (!valid) {
            return;
        }
        weight_zp_encoded = k230_gnne_gp(fe, fe->dm_load_w.raddr_bw, &valid);
        if (!valid) {
            return;
        }
    }
    act0_encoded = k230_gnne_gp(fe, fe->dm_load_act0.raddr_s, &valid);
    if (!valid) {
        return;
    }
    dest_encoded = k230_gnne_gp(fe, fe->dm_store_of.raddr_d, &valid);
    input_zp = k230_gnne_gp(fe, pdp0->rbx, &valid);
    if (!valid ||
        !k230_gnne_translate(fe, input_encoded, &input_base, NULL) ||
        !k230_gnne_translate(fe, act0_encoded, &act0_base, NULL) ||
        !k230_gnne_translate(fe, dest_encoded, &dest_base, &dest_logical)) {
        return;
    }
    if (pdp0->mode == 0 &&
        (!k230_gnne_translate(fe, weight_encoded, &weight_base, NULL) ||
         !k230_gnne_translate(fe, weight_zp_encoded, &weight_zp_base,
                              NULL))) {
        return;
    }

    for (uint32_t n = 0; n < output_shape.n; n++) {
        for (uint32_t c = 0; c < output_shape.c; c++) {
            int32_t signed_weight_zp = 0;

            if (pdp0->mode == 0) {
                uint8_t weight_zp;

                if (!k230_gnne_read_u8(weight_zp_base + c, &weight_zp)) {
                    return;
                }
                signed_weight_zp = weight_zp;
            }
            for (uint32_t oh = 0; oh < output_shape.h; oh++) {
                for (uint32_t ow = 0; ow < output_shape.w; ow++) {
                    int32_t acc = 0;
                    double activated;
                    uint32_t act_channel;
                    unsigned int written_size;
                    uint64_t output_index;
                    uint64_t output_offset;

                    bool have_sample = false;
                    uint64_t samples = 0;

                    for (uint32_t ky = 0; ky < pdp0->kernel_h; ky++) {
                        for (uint32_t kx = 0; kx < pdp0->kernel_w; kx++) {
                            uint32_t ih = oh * pdp0->stride_h + ky;
                            uint32_t iw = ow * pdp0->stride_w + kx;
                            uint64_t input_index;
                            uint64_t weight_index;
                            uint8_t input;
                            int32_t value;

                            if (ih >= input_shape.h || iw >= input_shape.w) {
                                input = pdp0->rpad_value;
                            } else {
                                input_index = (uint64_t)n * input_shape.c *
                                              input_shape.h *
                                              input_shape.w +
                                              (uint64_t)c * input_shape.h *
                                              input_shape.w +
                                              (uint64_t)ih * input_shape.w +
                                              iw;
                                if (!k230_gnne_read_u8(input_base +
                                                       input_index, &input)) {
                                    return;
                                }
                            }
                            value = (int32_t)input - input_zp;
                            samples++;

                            switch (pdp0->mode) {
                            case 0:
                            {
                                uint8_t weight;

                                weight_index = ((uint64_t)c *
                                                pdp0->kernel_h *
                                                pdp0->kernel_w +
                                                (uint64_t)ky *
                                                pdp0->kernel_w + kx);
                                if (!k230_gnne_read_u8(weight_base +
                                                       weight_index,
                                                       &weight)) {
                                    return;
                                }
                                acc += value *
                                       ((int32_t)weight - signed_weight_zp);
                                break;
                            }
                            case 1:
                                if (!have_sample || value < acc) {
                                    acc = value;
                                }
                                break;
                            case 2:
                                if (!have_sample || value > acc) {
                                    acc = value;
                                }
                                break;
                            case 3:
                            case 4:
                                acc = k230_gnne_add_i32_sat(acc, value);
                                break;
                            default:
                                return;
                            }
                            have_sample = true;
                        }
                    }
                    if (!have_sample) {
                        return;
                    }
                    if (pdp0->mode == 4 && samples) {
                        acc = k230_gnne_round_nearest((double)acc / samples);
                    }

                    act_channel = fe->act0_compute.is_by_channel ? c : 0;
                    if (!k230_gnne_act0_value(act0_base, act_channel, acc,
                                              fe->act0_conf.rshift_bits,
                                              &activated)) {
                        return;
                    }

                    output_index = (uint64_t)n * output_stride.n +
                                   (uint64_t)c * output_stride.c +
                                   (uint64_t)oh * output_stride.h + ow;
                    if (umul64_overflow(output_index, dest_size,
                                        &output_offset) ||
                        UINT64_MAX - dest_base < output_offset ||
                        !k230_gnne_act0_write(dest_base + output_offset, 0,
                                              fe->act0_compute.dest_datatype,
                                              activated, &written_size)) {
                        return;
                    }
                    written += written_size;
                }
            }
        }
    }

    if (written) {
        unsigned int head_size = MIN(written, sizeof(head));
        uint8_t head_buf[sizeof(head)] = {};

        if (k230_gnne_dma_read_bytes(dest_base, head_buf, head_size)) {
            head = ldq_le_p(head_buf);
        }
    }

    trace_k230_kpu_pdp0_compute(k230_kpu_name(s), pc, tcu_id, dest_logical,
                                written, head);
    fe->pdp0_computes++;
    fe->output_bytes += written;
}

static void k230_gnne_l2_load_conf(K230GnneFrontend *fe, uint32_t word)
{
    fe->l2_load_conf.rstride_d = extract32(word, 7, 3);
    fe->l2_load_conf.rstride_s = extract32(word, 10, 3);
    fe->l2_load_conf.stride_d_valid =
        k230_gnne_stride_value(fe, fe->l2_load_conf.rstride_d,
                               &fe->l2_load_conf.stride_d);
    fe->l2_load_conf.stride_s_valid =
        k230_gnne_stride_value(fe, fe->l2_load_conf.rstride_s,
                               &fe->l2_load_conf.stride_s);
    fe->l2_load_conf.l2_datatype = extract32(word, 13, 2);
    fe->l2_load_conf.ddr_datatype = extract32(word, 15, 3);
    fe->l2_load_conf.valid = true;
}

static void k230_gnne_l2_load_w_conf(K230GnneFrontend *fe, uint32_t word)
{
    bool valid;

    fe->l2_load_w_conf.rlen_compressed =
        k230_gnne_gp(fe, extract32(word, 7, 5), &valid);
    fe->l2_load_w_conf.valid = valid;
    fe->l2_load_w_conf.rlen_decompressed =
        k230_gnne_gp(fe, extract32(word, 12, 5), &valid);
    fe->l2_load_w_conf.valid &= valid;
    fe->l2_load_w_conf.l2_datatype = extract32(word, 17, 2);
    fe->l2_load_w_conf.ddr_datatype = extract32(word, 19, 3);
    fe->l2_load_w_conf.enable_decompress = extract32(word, 22, 1);
}

static void k230_gnne_l2_store_conf(K230GnneFrontend *fe, uint32_t word)
{
    fe->l2_store_conf.rstride_d = extract32(word, 7, 3);
    fe->l2_store_conf.rstride_s = extract32(word, 10, 3);
    fe->l2_store_conf.stride_d_valid =
        k230_gnne_stride_value(fe, fe->l2_store_conf.rstride_d,
                               &fe->l2_store_conf.stride_d);
    fe->l2_store_conf.stride_s_valid =
        k230_gnne_stride_value(fe, fe->l2_store_conf.rstride_s,
                               &fe->l2_store_conf.stride_s);
    fe->l2_store_conf.l2_datatype = extract32(word, 13, 2);
    fe->l2_store_conf.ddr_datatype = extract32(word, 15, 3);
    fe->l2_store_conf.valid = true;
}

static void k230_gnne_mfu_conf(K230GnneFrontend *fe, uint32_t word)
{
    K230GnneMfuAct1Conf *conf = &fe->mfu_act1;
    K230GnneMfuPdp1Conf *pdp1 = &fe->mfu_pdp1;
    K230GnneMfuTransposeConf *transpose = &fe->mfu_transpose;
    K230GnneMfuAct1Deq *deq;
    unsigned int sid;
    bool valid;

    switch (extract32(word, 7, 5)) {
    case 0x00:
        transpose->rstride_d = extract32(word, 12, 3);
        transpose->rstride_s = extract32(word, 15, 3);
        transpose->l2_datatype = extract32(word, 18, 2);
        transpose->permute = extract32(word, 20, 5);
        transpose->valid = true;
        break;
    case 0x01:
        pdp1->stride_w = extract32(word, 12, 5);
        pdp1->stride_h = extract32(word, 17, 5);
        pdp1->rstride_s = extract32(word, 22, 3);
        pdp1->funct2 = extract32(word, 25, 2);
        pdp1->rstride_d = extract32(word, 27, 3);
        pdp1->conf1_valid = true;
        break;
    case 0x02:
        pdp1->rcount_w = extract32(word, 12, 5);
        pdp1->rcount_h = extract32(word, 17, 5);
        pdp1->rpe_h = extract32(word, 22, 5);
        pdp1->rpe_last_h = extract32(word, 27, 5);
        pdp1->conf2_valid = true;
        break;
    case 0x03:
        pdp1->rpe_channels = extract32(word, 12, 5);
        pdp1->rpe_last_channels = extract32(word, 17, 5);
        pdp1->rpad_value = extract32(word, 22, 5);
        pdp1->sspad = extract32(word, 27, 3);
        pdp1->conf3_valid = true;
        break;
    case 0x04:
        pdp1->rwindow_w = extract32(word, 12, 5);
        pdp1->rwindow_h = extract32(word, 17, 5);
        pdp1->rscale = extract32(word, 22, 5);
        pdp1->enable_h2c = extract32(word, 27, 1);
        pdp1->enable_bw = extract32(word, 28, 1);
        pdp1->conf4_valid = true;
        break;
    case 0x06:
        pdp1->deq.rscale = extract32(word, 12, 5);
        pdp1->deq.rbias = extract32(word, 17, 5);
        pdp1->deq.quant_type = extract32(word, 22, 2);
        pdp1->deq.rshift_bits = extract32(word, 24, 5);
        pdp1->deq.valid = true;
        break;
    case 0x07:
        pdp1->quant_rscale = extract32(word, 12, 5);
        pdp1->quant_rbias = extract32(word, 17, 5);
        pdp1->quant_type = extract32(word, 22, 2);
        pdp1->quant_rshift_bits = extract32(word, 24, 5);
        pdp1->quant_valid = true;
        break;
    case 0x08:
        conf->rstride_s1 = extract32(word, 12, 3);
        conf->rstride_s2 = extract32(word, 15, 3);
        conf->rstride_d1 = extract32(word, 18, 3);
        conf->stride_valid = true;
        break;
    case 0x09:
        sid = extract32(word, 27, 1);
        conf->src1[sid].rslice = extract32(word, 12, 5);
        conf->src1[sid].rright_repeats = extract32(word, 17, 5);
        conf->src1[sid].rslice_repeats = extract32(word, 22, 5);
        conf->src1[sid].slice_loc = extract32(word, 28, 1);
        conf->src1[sid].valid = true;
        break;
    case 0x0a:
        sid = extract32(word, 20, 1);
        conf->src2[sid].rleft_repeats = extract32(word, 12, 5);
        conf->src2[sid].rshape = extract32(word, 17, 3);
        conf->src2[sid].source_type = extract32(word, 21, 1);
        conf->src2[sid].valid = true;
        break;
    case 0x0b:
        conf->dest_rlen = extract32(word, 12, 5);
        conf->dest_len = k230_gnne_gp(fe, conf->dest_rlen, &valid);
        conf->dest_len_valid = valid && conf->dest_len;
        conf->dest_rshape = extract32(word, 17, 3);
        conf->dest_valid = true;
        break;
    case 0x0c:
        sid = extract32(word, 24, 1);
        deq = &conf->deq[sid];
        deq->rscale = extract32(word, 12, 5);
        deq->rbias = extract32(word, 17, 5);
        deq->quant_type = extract32(word, 22, 2);
        deq->rshift_bits = extract32(word, 25, 5);
        deq->valid = true;
        break;
    case 0x0d:
        conf->quant_type = extract32(word, 12, 2);
        conf->quant_rshift_bits = extract32(word, 14, 5);
        conf->quant_valid = true;
        break;
    case 0x0e:
        conf->funct4 = extract32(word, 12, 4);
        conf->is_by_channel = extract32(word, 16, 1);
        conf->is_16_segments = extract32(word, 17, 1);
        conf->act_valid = true;
        break;
    default:
        break;
    }
}

static void k230_gnne_dm_conf(K230GnneFrontend *fe, uint32_t word)
{
    switch (extract32(word, 13, 4)) {
    case 0:
        fe->dm_load_l1_conf.rstride_s = extract32(word, 17, 3);
        fe->dm_load_l1_conf.datatype = extract32(word, 20, 2);
        fe->dm_load_l1_conf.l1_type = extract32(word, 22, 2);
        fe->dm_load_l1_conf.valid = true;
        break;
    case 1:
        fe->dm_load_w_conf.kernel_h = extract32(word, 17, 5);
        fe->dm_load_w_conf.kernel_w = extract32(word, 22, 5);
        fe->dm_load_w_conf.rstride_oc = extract32(word, 27, 5);
        fe->dm_load_w_conf.valid = true;
        break;
    case 2:
        fe->dm_load_w_conf.quant_type = extract32(word, 17, 2);
        fe->dm_load_w_conf.valid = true;
        break;
    case 5:
        fe->dm_load_w_conf.rgroups = extract32(word, 17, 5);
        fe->dm_load_w_conf.rgoc = extract32(word, 22, 5);
        fe->dm_load_w_conf.valid = true;
        break;
    default:
        break;
    }
}

static void k230_gnne_pu_conf(K230GnneFrontend *fe, uint32_t word)
{
    K230GnnePuConf *conf = &fe->pu_conf;

    switch (extract32(word, 13, 4)) {
    case 0:
        conf->stride_w = extract32(word, 17, 5);
        conf->stride_h = extract32(word, 22, 5);
        conf->rstride_s = extract32(word, 27, 3);
        conf->fetch1_valid = true;
        break;
    case 1:
        conf->rgic = extract32(word, 17, 5);
        conf->rgic_last = extract32(word, 22, 5);
        conf->fetch2_valid = true;
        break;
    case 2:
        conf->raddr_s = extract32(word, 17, 5);
        conf->rgroups = extract32(word, 22, 5);
        conf->rshape = extract32(word, 27, 3);
        conf->fetch3_valid = true;
        break;
    case 3:
        conf->rpad_value = extract32(word, 17, 5);
        conf->sspad = extract32(word, 27, 3);
        conf->fetch4_valid = true;
        break;
    case 4:
        conf->ric = extract32(word, 17, 5);
        conf->rbx = extract32(word, 22, 5);
        conf->quant_type = extract32(word, 27, 2);
        conf->fetch_deq_valid = true;
        break;
    case 5:
        conf->kernel_h = extract32(word, 17, 5);
        conf->kernel_w = extract32(word, 22, 5);
        conf->w_valid = true;
        break;
    case 6:
        conf->rgoc = extract32(word, 17, 5);
        conf->rgoc_last = extract32(word, 22, 5);
        conf->rstride_d = extract32(word, 27, 3);
        conf->output_stride_valid =
            k230_gnne_stride_value(fe, conf->rstride_d,
                                   &conf->output_stride);
        conf->of1_valid = true;
        break;
    case 7:
        conf->raddr_d = extract32(word, 17, 5);
        conf->rshape_d = extract32(word, 27, 3);
        conf->output_shape_valid = false;
        if (conf->rshape_d < K230_GNNE_SHAPE_COUNT &&
            fe->shape[conf->rshape_d].valid) {
            conf->output_shape = fe->shape[conf->rshape_d];
            conf->output_shape_valid = true;
        }
        conf->of2_valid = true;
        break;
    case 8:
        conf->load_psum = extract32(word, 17, 1);
        conf->clr_psum = extract32(word, 18, 1);
        conf->dest_target = extract32(word, 19, 1);
        conf->release_if = extract32(word, 20, 1);
        conf->mode = extract32(word, 21, 1);
        conf->compute_valid = true;
        break;
    default:
        break;
    }
}

static void k230_gnne_pdp0_conf(K230GnneFrontend *fe, uint32_t word)
{
    K230GnnePdp0Conf *conf = &fe->pdp0_conf;

    switch (extract32(word, 13, 4)) {
    case 0:
        conf->mode = extract32(word, 17, 3);
        conf->mode_valid = true;
        break;
    case 1:
        conf->stride_w = extract32(word, 17, 5);
        conf->stride_h = extract32(word, 22, 5);
        conf->fetch1_valid = true;
        break;
    case 2:
        conf->rgic = extract32(word, 17, 5);
        conf->rgic_last = extract32(word, 22, 5);
        conf->fetch2_valid = true;
        break;
    case 3:
        conf->rshape = extract32(word, 27, 3);
        conf->input_shape_valid = false;
        if (conf->rshape < K230_GNNE_SHAPE_COUNT &&
            fe->shape[conf->rshape].valid) {
            conf->input_shape = fe->shape[conf->rshape];
            conf->input_shape_valid = true;
        }
        conf->fetch3_valid = true;
        break;
    case 4:
        conf->rpad_value = extract32(word, 17, 5);
        conf->sspad = extract32(word, 22, 3);
        conf->fetch4_valid = true;
        break;
    case 5:
        conf->rbx = extract32(word, 17, 5);
        conf->quant_type = extract32(word, 22, 2);
        conf->fetch_deq_valid = true;
        break;
    case 6:
        conf->kernel_h = extract32(word, 17, 5);
        conf->kernel_w = extract32(word, 22, 5);
        conf->w_valid = true;
        break;
    case 7:
        conf->rstride_d = extract32(word, 17, 3);
        conf->rshape_d = extract32(word, 20, 3);
        conf->output_shape_valid = false;
        if (conf->rshape_d < K230_GNNE_SHAPE_COUNT &&
            fe->shape[conf->rshape_d].valid) {
            conf->output_shape = fe->shape[conf->rshape_d];
            conf->output_shape_valid = true;
        }
        conf->output_stride_valid =
            k230_gnne_stride_value(fe, conf->rstride_d, &conf->output_stride);
        conf->of_valid = true;
        break;
    default:
        break;
    }
}

static void k230_gnne_dm_op(K230GnneFrontend *fe, uint32_t word)
{
    switch (word & 0x7f) {
    case 0x52:
        fe->dm_load_l1.raddr_s = extract32(word, 13, 5);
        fe->dm_load_l1.rshape = extract32(word, 23, 3);
        fe->dm_load_l1.valid = true;
        break;
    case 0x54:
        fe->dm_load_w.raddr_s = extract32(word, 13, 5);
        fe->dm_load_w.raddr_bw = extract32(word, 18, 5);
        fe->dm_load_w.r_iochannels = extract32(word, 23, 3);
        fe->dm_load_w.valid = true;
        break;
    case 0x56:
        fe->dm_load_act0.raddr_s = extract32(word, 13, 5);
        fe->dm_load_act0.rlen = extract32(word, 18, 5);
        fe->dm_load_act0.is_by_channel = extract32(word, 24, 1);
        fe->dm_load_act0.valid = true;
        break;
    case 0x58:
        fe->dm_store_of.raddr_d = extract32(word, 13, 5);
        fe->dm_store_of.rshape = extract32(word, 18, 3);
        fe->dm_store_of.valid = true;
        break;
    default:
        break;
    }
}

static void k230_gnne_act0_conf(K230GnneFrontend *fe, uint32_t word)
{
    fe->act0_conf.rshape = extract32(word, 17, 3);
    fe->act0_conf.rshift_bits = extract32(word, 20, 5);
    fe->act0_conf.valid = true;
}

static void k230_gnne_act0_compute(K230GnneFrontend *fe, uint32_t word)
{
    fe->act0_compute.raddr_d = extract32(word, 12, 5);
    fe->act0_compute.dest_datatype = extract32(word, 23, 2);
    fe->act0_compute.is_by_channel = extract32(word, 25, 1);
    fe->act0_compute.valid = true;
}

static void k230_gnne_step(K230KpuState *s, K230GnneFrontend *fe,
                           uint32_t word, uint64_t pc)
{
    uint32_t opcode = word & 0x7f;
    uint32_t rd;
    uint32_t rs;
    uint32_t rs1;
    uint32_t rs2;
    uint32_t value;
    uint32_t left;
    uint32_t right;
    bool left_valid;
    bool right_valid;
    bool valid;

    fe->instructions++;

    switch (opcode) {
    case 0x02:
        rd = extract32(word, 7, 5);
        k230_gnne_set_gp(fe, rd, extract32(word, 12, 20) << 12, true);
        break;
    case 0x04:
        rd = extract32(word, 7, 5);
        value = pc + (extract32(word, 12, 20) << 12);
        k230_gnne_set_gp(fe, rd, value, true);
        break;
    case 0x06:
        rd = extract32(word, 7, 5);
        rs = extract32(word, 12, 5);
        left = k230_gnne_gp(fe, rs, &left_valid);
        value = left + sextract32(word, 20, 12);
        switch (extract32(word, 17, 3)) {
        case 0:
            valid = left_valid &&
                    k230_gnne_read_scalar(fe, value, 4, false, &value);
            break;
        case 1:
            valid = left_valid &&
                    k230_gnne_read_scalar(fe, value, 2, true, &value);
            break;
        case 2:
            valid = left_valid &&
                    k230_gnne_read_scalar(fe, value, 2, false, &value);
            break;
        case 3:
            valid = left_valid &&
                    k230_gnne_read_scalar(fe, value, 1, true, &value);
            break;
        case 4:
            valid = left_valid &&
                    k230_gnne_read_scalar(fe, value, 1, false, &value);
            break;
        default:
            valid = false;
            break;
        }
        k230_gnne_set_gp(fe, rd, value, valid);
        break;
    case 0x0c:
        rd = extract32(word, 7, 5);
        rs1 = extract32(word, 12, 5);
        rs2 = extract32(word, 22, 5);
        left = k230_gnne_gp(fe, rs1, &left_valid);
        right = k230_gnne_gp(fe, rs2, &right_valid);
        valid = left_valid && right_valid;
        switch (extract32(word, 17, 5)) {
        case 0:
            value = left + right;
            break;
        case 1:
            value = left - right;
            break;
        case 2:
            value = left * right;
            break;
        default:
            valid = false;
            value = 0;
            break;
        }
        k230_gnne_set_gp(fe, rd, value, valid);
        break;
    case 0x0e:
        if (extract32(word, 17, 3) != 0) {
            break;
        }
        rd = extract32(word, 7, 5);
        rs = extract32(word, 12, 5);
        left = k230_gnne_gp(fe, rs, &left_valid);
        value = left + sextract32(word, 20, 12);
        k230_gnne_set_gp(fe, rd, value, left_valid);
        break;
    case 0x40:
        if (extract32(word, 27, 3) >= K230_GNNE_SHAPE_COUNT) {
            break;
        }
        rs = extract32(word, 27, 3);
        fe->shape[rs].n = k230_gnne_gp(fe, extract32(word, 7, 5),
                                       &valid);
        fe->shape[rs].valid = valid;
        fe->shape[rs].c = k230_gnne_gp(fe, extract32(word, 12, 5),
                                       &valid);
        fe->shape[rs].valid &= valid;
        fe->shape[rs].h = k230_gnne_gp(fe, extract32(word, 17, 5),
                                       &valid);
        fe->shape[rs].valid &= valid;
        fe->shape[rs].w = k230_gnne_gp(fe, extract32(word, 22, 5),
                                       &valid);
        fe->shape[rs].valid &= valid;
        break;
    case 0x42:
        if (extract32(word, 27, 3) >= K230_GNNE_SHAPE_COUNT) {
            break;
        }
        rs = extract32(word, 27, 3);
        fe->stride[rs].n = k230_gnne_gp(fe, extract32(word, 7, 5),
                                        &valid);
        fe->stride[rs].valid = valid;
        fe->stride[rs].c = k230_gnne_gp(fe, extract32(word, 12, 5),
                                        &valid);
        fe->stride[rs].valid &= valid;
        fe->stride[rs].h = k230_gnne_gp(fe, extract32(word, 17, 5),
                                        &valid);
        fe->stride[rs].valid &= valid;
        break;
    case 0x44:
        rs = extract32(word, 17, 4);
        if (rs >= K230_GNNE_MMU_COUNT) {
            break;
        }
        fe->mmu[rs].start = k230_gnne_gp(fe, extract32(word, 7, 5),
                                         &valid);
        fe->mmu[rs].valid = valid;
        fe->mmu[rs].depth = k230_gnne_gp(fe, extract32(word, 12, 5),
                                         &valid);
        fe->mmu[rs].valid &= valid;
        break;
    case 0x46:
        k230_gnne_l2_load_conf(fe, word);
        break;
    case 0x48:
        k230_gnne_l2_load_w_conf(fe, word);
        break;
    case 0x4a:
        k230_gnne_l2_store_conf(fe, word);
        break;
    case 0x4c:
        k230_gnne_l2_load(s, fe, word);
        break;
    case 0x4d:
        k230_gnne_pu_compute(s, fe, word, pc);
        break;
    case 0x4e:
        k230_gnne_l2_store(s, fe, word, pc);
        break;
    case 0x4f:
        k230_gnne_pdp0_compute(s, fe, word, pc);
        break;
    case 0x50:
        k230_gnne_dm_conf(fe, word);
        break;
    case 0x51:
        fe->ai2d_computes++;
        trace_k230_kpu_ai2d_compute(k230_kpu_name(s), pc, word & 0xffff);
        break;
    case 0x52:
    case 0x54:
    case 0x56:
    case 0x58:
        k230_gnne_dm_op(fe, word);
        break;
    case 0x57:
        k230_gnne_l2_load_w(s, fe, word, pc);
        break;
    case 0x5a:
        k230_gnne_pu_conf(fe, word);
        break;
    case 0x5e:
        k230_gnne_pdp0_conf(fe, word);
        break;
    case 0x60:
        k230_gnne_act0_conf(fe, word);
        break;
    case 0x62:
        k230_gnne_mfu_conf(fe, word);
        break;
    case 0x68:
        k230_gnne_mfu_transpose(s, fe, word, pc);
        break;
    case 0x6a:
        k230_gnne_mfu_pdp1(s, fe, word, pc);
        break;
    case 0x72:
        k230_gnne_mfu_act1(s, fe, word, pc);
        break;
    case 0x74:
        k230_gnne_act0_compute(fe, word);
        break;
    default:
        if (!k230_gnne_opcode_known(word)) {
            fe->unknown++;
        }
        break;
    }
}

static void k230_kpu_execute_gnne(K230KpuState *s)
{
    uint64_t command_start = k230_kpu_command_addr(s,
                                                   K230_KPU_COMMAND_START);
    uint64_t command_end = k230_kpu_command_addr(s, K230_KPU_COMMAND_END);
    uint64_t size;
    K230GnneFrontend fe;
    g_autofree uint8_t *buf = NULL;

    if (!command_start || !command_end || command_end <= command_start) {
        return;
    }

    size = command_end - command_start;
    if (size > K230_GNNE_MAX_COMMAND_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "k230 KPU command stream too large: 0x%" PRIx64
                      " bytes\n", size);
        return;
    }

    buf = g_malloc(size);
    if (dma_memory_read(&address_space_memory, command_start, buf, size,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "k230 KPU failed to read command stream 0x%" PRIx64
                      "..0x%" PRIx64 "\n", command_start, command_end);
        return;
    }

    k230_gnne_frontend_init(s, &fe, command_start);
    k230_gnne_active_fe = &fe;
    for (uint64_t pc = 0; pc < size; ) {
        uint32_t opcode;
        uint32_t word = 0;
        unsigned int length;

        if (pc + sizeof(uint16_t) > size) {
            break;
        }
        opcode = buf[pc] & 0x7f;
        length = k230_gnne_is_short(opcode) ? sizeof(uint16_t) :
                 sizeof(uint32_t);
        if (pc + length > size) {
            break;
        }
        if (length == sizeof(uint16_t)) {
            word = lduw_le_p(buf + pc);
        } else {
            word = ldl_le_p(buf + pc);
        }
        k230_gnne_step(s, &fe, word, pc);
        pc += length;
    }

    trace_k230_kpu_gnne_summary(k230_kpu_name(s), fe.instructions,
                                fe.l2_loads, fe.l2_load_ws, fe.l2_stores,
                                fe.input_bytes + fe.output_bytes,
                                fe.unknown);
    trace_k230_kpu_gnne_compute_summary(k230_kpu_name(s), fe.mfu_act1s,
                                        fe.mfu_pdp1s, fe.mfu_transposes,
                                        fe.ai2d_computes, fe.pu_computes,
                                        fe.pdp0_computes);
    k230_gnne_active_fe = NULL;
    k230_gnne_frontend_destroy(&fe);
}

static void k230_kpu_completion_zero(K230KpuState *s)
{
    uint64_t command_start = k230_kpu_command_addr(s,
                                                   K230_KPU_COMMAND_START);
    uint64_t command_end = k230_kpu_command_addr(s, K230_KPU_COMMAND_END);
    uint64_t zero_end = K230_KPU_FAKE_OUTPUT_BASE +
                        K230_KPU_FAKE_OUTPUT_SIZE;
    uint64_t pages = K230_KPU_FAKE_OUTPUT_SIZE / K230_KPU_PAGE_SIZE;
    long page_count = pages;
    g_autofree unsigned long *seen_pages = NULL;

    if (!command_start || !command_end || command_end <= command_start ||
        zero_end < K230_KPU_FAKE_OUTPUT_BASE || !pages) {
        return;
    }

    seen_pages = bitmap_new(page_count);
    for (uint64_t addr = command_start; addr + sizeof(uint32_t) <= command_end;
         addr += sizeof(uint32_t)) {
        uint32_t raw;
        uint32_t value;
        uint64_t page;
        uint64_t page_index;

        if (dma_memory_read(&address_space_memory, addr, &raw, sizeof(raw),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            break;
        }

        value = ldl_le_p(&raw);
        page = value & ~(K230_KPU_PAGE_SIZE - 1);
        if (page < K230_KPU_FAKE_OUTPUT_BASE ||
            page + K230_KPU_PAGE_SIZE > zero_end) {
            continue;
        }

        page_index = (page - K230_KPU_FAKE_OUTPUT_BASE) / K230_KPU_PAGE_SIZE;
        if (page_index >= pages ||
            test_and_set_bit((long)page_index, seen_pages)) {
            continue;
        }

        trace_k230_kpu_completion_zero_page(k230_kpu_name(s), page, value);
        dma_memory_set(&address_space_memory, page, 0, K230_KPU_PAGE_SIZE,
                       MEMTXATTRS_UNSPECIFIED);
    }
}

static void k230_kpu_set_irq(K230KpuState *s, bool level)
{
    s->irq_level = level;
    trace_k230_kpu_irq(k230_kpu_name(s), level);
    qemu_set_irq(s->irq, level);
}

static void k230_kpu_complete(K230KpuState *s)
{
    s->busy = false;
    k230_kpu_execute_gnne(s);
    k230_kpu_completion_zero(s);
    k230_kpu_writeq_regs(s, K230_KPU_STATUS, K230_KPU_DONE);
    k230_kpu_set_irq(s, true);
}

static void k230_kpu_complete_timer(void *opaque)
{
    K230KpuState *s = K230_KPU(opaque);

    if (s->busy) {
        k230_kpu_complete(s);
    }
}

static void k230_kpu_start(K230KpuState *s)
{
    uint64_t command_start = k230_kpu_command_addr(s,
                                                   K230_KPU_COMMAND_START);
    uint64_t command_end = k230_kpu_command_addr(s, K230_KPU_COMMAND_END);

    if (s->busy) {
        return;
    }

    k230_kpu_writeq_regs(s, K230_KPU_STATUS, 0);
    s->busy = true;
    trace_k230_kpu_start(k230_kpu_name(s), command_start, command_end,
                         k230_kpu_readl_regs(s, K230_KPU_COMMAND_HI));
    timer_mod(&s->complete_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              K230_KPU_COMPLETE_DELAY_NS);
}

static uint64_t k230_kpu_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230KpuState *s = K230_KPU(opaque);
    uint64_t val;

    if (!k230_kpu_range_ok(addr, size)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "k230 KPU bad read offset 0x%" HWADDR_PRIx
                      " size %u\n", addr, size);
        return 0;
    }

    val = k230_kpu_read_bytes(s->regs, addr, size);
    trace_k230_kpu_read(k230_kpu_name(s), addr, val, size);
    return val;
}

static void k230_kpu_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size)
{
    K230KpuState *s = K230_KPU(opaque);

    if (!k230_kpu_range_ok(addr, size)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "k230 KPU bad write offset 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 " size %u\n", addr, val, size);
        return;
    }

    k230_kpu_write_bytes(s->regs, addr, val, size);
    trace_k230_kpu_write(k230_kpu_name(s), addr, val, size);

    if (k230_kpu_access_hits(addr, size, K230_KPU_CONTROL)) {
        if (k230_kpu_control_has(val, K230_KPU_CONTROL_CLEAR)) {
            timer_del(&s->complete_timer);
            s->busy = false;
            k230_kpu_writeq_regs(s, K230_KPU_STATUS, 0);
            k230_kpu_set_irq(s, false);
        }

        if (k230_kpu_control_has(val, K230_KPU_CONTROL_START)) {
            k230_kpu_start(s);
        }
    }
}

static const MemoryRegionOps k230_kpu_ops = {
    .read = k230_kpu_read,
    .write = k230_kpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

static void k230_kpu_reset(DeviceState *dev)
{
    K230KpuState *s = K230_KPU(dev);

    memset(s->regs, 0, sizeof(s->regs));
    timer_del(&s->complete_timer);
    s->busy = false;
    k230_kpu_clear_rdata_shadow(s);
    k230_kpu_set_irq(s, false);
}

static const VMStateDescription vmstate_k230_kpu = {
    .name = TYPE_K230_KPU,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(busy, K230KpuState),
        VMSTATE_BOOL(irq_level, K230KpuState),
        VMSTATE_TIMER(complete_timer, K230KpuState),
        VMSTATE_UINT8_ARRAY(regs, K230KpuState, K230_KPU_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_kpu_realize(DeviceState *dev, Error **errp)
{
    K230KpuState *s = K230_KPU(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_kpu_ops, s,
                          TYPE_K230_KPU, K230_KPU_SIZE);
    timer_init_ns(&s->complete_timer, QEMU_CLOCK_VIRTUAL,
                  k230_kpu_complete_timer, s);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void k230_kpu_unrealize(DeviceState *dev)
{
    K230KpuState *s = K230_KPU(dev);

    k230_kpu_clear_rdata_shadow(s);
}

static void k230_kpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_kpu_realize;
    dc->unrealize = k230_kpu_unrealize;
    device_class_set_legacy_reset(dc, k230_kpu_reset);
    dc->vmsd = &vmstate_k230_kpu;
    dc->desc = "K230 KPU/GNNE engine";
}

static const TypeInfo k230_kpu_type_info = {
    .name = TYPE_K230_KPU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230KpuState),
    .class_init = k230_kpu_class_init,
};

static void k230_kpu_register_types(void)
{
    type_register_static(&k230_kpu_type_info);
}

type_init(k230_kpu_register_types)
