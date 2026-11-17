/*
 * plan.h - egpushim planner core (PURE LOGIC, no EFI/libc dependencies)
 *
 * Shared by the EFI application and the host-side unit test.
 * Computes, from a collected PCI device table:
 *   - the Blackmagic eGPU (matched by ID + subsystem, NOT by the
 *     Ellesmere device ID alone - the internal GPU is the same chip)
 *   - the bridge chain from the GPU up to the root port
 *   - the WOULD-WRITE manifest for the placement plan
 *     (egpu-shim-v0.2-placement-plan.md section 3)
 *
 * This code performs no I/O and no PCI access. It only computes.
 */
#ifndef EGPUSHIM_PLAN_H
#define EGPUSHIM_PLAN_H

#include <stdint.h>
#include <stddef.h>

#define PLAN_VID_AMD        0x1002
#define PLAN_DID_ELLESMERE  0x67DF
#define PLAN_DID_HDMI_AUDIO 0xAAF0
#define PLAN_SUBVENDOR_BMD  0xBDBD   /* Blackmagic Design PCI vendor ID */
#define PLAN_SUBID_EGPU     0xA14C   /* Blackmagic eGPU subsystem ID    */

/* Placement defaults = placement plan section 2 (WIDE geometry).
 * Overridable from shim.cfg (WINDOW_BASE / BAR2_BASE). */
#define PLAN_DEFAULT_WINDOW_BASE 0x4000000000ULL
#define PLAN_DEFAULT_BAR2_BASE   0x4200000000ULL
#define PLAN_BAR2_SIZE           0x200000ULL   /* 2 MiB, probe-verified at v0.4 */

typedef struct {
    uint8_t  bus, dev, func;
    uint16_t vid, did;
    uint32_t subsys;      /* raw dword at 0x2C: lo=subvendor, hi=subsystem ID */
    uint8_t  hdrtype;     /* raw header-type byte (0x0E) */
    uint8_t  pri, sec, sub; /* bridges only (header type 1) */
} plan_dev;

typedef struct {
    uint8_t  bus, dev, func;
    uint8_t  offset;      /* config-space offset */
    uint8_t  width;       /* 2 or 4 bytes */
    uint32_t newval;      /* value a write build WOULD write */
} plan_entry;

#define PLAN_MAX_CHAIN   16
#define PLAN_MAX_ENTRIES 96

/* Find the Blackmagic eGPU GPU function. Returns index or -1. */
int plan_find_gpu(const plan_dev *devs, int n);

/* Find the Blackmagic eGPU HDMI-audio function. Returns index or -1. */
int plan_find_audio(const plan_dev *devs, int n);

/* Walk from the GPU up to the root port. Fills chain[] with dev-table
 * indices in LEAF->ROOT order (the bridge directly above the GPU
 * first, the root port last). Returns chain length, or -1 if the
 * chain does not reach a bus-0 bridge. */
int plan_build_chain(const plan_dev *devs, int n, int gpu_idx,
                     int *chain, int max_chain);

/* Window end for the WIDE geometry: BAR2 base + BAR2 size - 1. */
uint64_t plan_window_end(uint64_t bar2_base);

/* Prefetch-window register encodings for a bridge.
 * out: [0]=value for 0x24 (16-bit PrefBaseLow)
 *      [1]=value for 0x26 (16-bit PrefLimitLow)
 *      [2]=value for 0x28 (32-bit PrefBaseUpper32)
 *      [3]=value for 0x2C (32-bit PrefLimitUpper32) */
void plan_window_regs(uint64_t window_base, uint64_t window_end,
                      uint32_t out[4]);

/* Build the full WOULD-WRITE manifest: for each chain bridge (in
 * chain order) the four window registers of plan section 3a, then the
 * GPU's four BAR registers of section 3b. Returns entry count, or -1
 * on bad input / overflow. */
int plan_build_manifest(const plan_dev *devs, int n, int gpu_idx,
                        const int *chain, int chain_len,
                        uint64_t window_base, uint64_t bar2_base,
                        plan_entry *out, int max_entries);

/* ---- v0.3: write-execution planning (still pure logic) ---- */

/* The writer's arming gate. The EFI side may perform config-space
 * writes ONLY when this returns PLAN_GATE_ARMED. The enable flag is
 * checked first: a build whose shim.cfg does not carry WRITE_ENABLE=1
 * can never arm, whatever the plan says. */
typedef enum {
    PLAN_GATE_SKIP_NO_ENABLE = 0,  /* WRITE_ENABLE != 1 in shim.cfg   */
    PLAN_GATE_SKIP_NO_PLAN   = 1,  /* no valid plan (GPU/chain absent) */
    PLAN_GATE_ARMED          = 2   /* writes permitted                */
} plan_gate;

plan_gate plan_write_gate(int write_enable, int plan_found);

/* Build the ORDERED WRITE LIST for v0.3 from a full manifest: keeps
 * ONLY the bridge prefetch-window entries (offsets 0x24/0x26/0x28/0x2C
 * on devices other than the GPU), preserving the manifest's
 * leaf->root order. The GPU's BAR entries are excluded entirely -
 * they remain plan-only in v0.3. Returns entry count, or -1 on bad
 * input / overflow. */
int plan_filter_write_list(const plan_entry *man, int mcount,
                           uint8_t gpu_bus, uint8_t gpu_dev, uint8_t gpu_func,
                           plan_entry *out, int max_out);

/* ---- v0.4: full-placement write list ---- */

/* The GPU's COMMAND register: offset 0x04, written LAST, with the
 * value Apple's firmware leaves in the internal twin at the EFI
 * stage (IO | MEMORY | BUS_MASTER). The eGPU's is 0x0000 there. */
#define PLAN_GPU_COMMAND_OFFSET 0x04
#define PLAN_GPU_COMMAND_VALUE  0x0007

/* Build the v0.4 ORDERED WRITE LIST from a full manifest: the v0.3
 * bridge-window entries first (leaf->root, unchanged), then the
 * GPU's own BAR entries (plan 3b, manifest order), then ONE final
 * entry writing the GPU's COMMAND register - the device is enabled
 * only after every placement is in and verified by the caller.
 * A GPU BAR entry with a width other than 4 refuses the whole
 * list (-1), as does any overflow. */
int plan_build_write_list_v4(const plan_entry *man, int mcount,
                             uint8_t gpu_bus, uint8_t gpu_dev, uint8_t gpu_func,
                             plan_entry *out, int max_out);

#endif /* EGPUSHIM_PLAN_H */
