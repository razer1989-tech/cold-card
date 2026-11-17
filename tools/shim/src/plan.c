/*
 * plan.c - egpushim planner core. See plan.h. Pure computation only.
 */
#include "plan.h"

static int is_bridge(const plan_dev *d) { return (d->hdrtype & 0x7F) == 1; }

static int is_blackmagic(const plan_dev *d, uint16_t did)
{
    uint16_t subvendor = (uint16_t)(d->subsys & 0xFFFF);
    return d->vid == PLAN_VID_AMD && d->did == did &&
           subvendor == PLAN_SUBVENDOR_BMD;
}

int plan_find_gpu(const plan_dev *devs, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        uint16_t subid = (uint16_t)(devs[i].subsys >> 16);
        if (is_blackmagic(&devs[i], PLAN_DID_ELLESMERE) &&
            subid == PLAN_SUBID_EGPU)
            return i;
    }
    return -1;
}

int plan_find_audio(const plan_dev *devs, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (is_blackmagic(&devs[i], PLAN_DID_HDMI_AUDIO)) return i;
    return -1;
}

int plan_build_chain(const plan_dev *devs, int n, int gpu_idx,
                     int *chain, int max_chain)
{
    int len = 0;
    uint8_t cur_bus;
    if (!devs || gpu_idx < 0 || gpu_idx >= n || !chain || max_chain < 1)
        return -1;
    cur_bus = devs[gpu_idx].bus;
    while (len < max_chain) {
        int i, found = -1;
        for (i = 0; i < n; i++) {
            if (is_bridge(&devs[i]) && devs[i].sec == cur_bus &&
                devs[i].pri != cur_bus) { found = i; break; }
        }
        if (found < 0) return -1;              /* gap: chain broken */
        chain[len++] = found;
        if (devs[found].pri == 0) return len;  /* reached root port */
        cur_bus = devs[found].pri;
    }
    return -1;                                  /* too deep: refuse */
}

uint64_t plan_window_end(uint64_t bar2_base)
{
    return bar2_base + PLAN_BAR2_SIZE - 1;
}

void plan_window_regs(uint64_t window_base, uint64_t window_end,
                      uint32_t out[4])
{
    /* Bridge prefetch registers use 1 MiB granularity: the low
     * registers hold address bits [31:20] in bits [15:4], with the
     * low nibble 0x1 marking 64-bit capability. */
    out[0] = (uint32_t)((((window_base >> 20) & 0xFFF) << 4) | 0x1);
    out[1] = (uint32_t)((((window_end  >> 20) & 0xFFF) << 4) | 0x1);
    out[2] = (uint32_t)(window_base >> 32);
    out[3] = (uint32_t)(window_end  >> 32);
}

int plan_build_manifest(const plan_dev *devs, int n, int gpu_idx,
                        const int *chain, int chain_len,
                        uint64_t window_base, uint64_t bar2_base,
                        plan_entry *out, int max_entries)
{
    static const uint8_t win_off[4] = { 0x24, 0x26, 0x28, 0x2C };
    static const uint8_t win_wid[4] = { 2, 2, 4, 4 };
    uint32_t regs[4];
    uint64_t wend = plan_window_end(bar2_base);
    int cnt = 0, i, k;

    if (!devs || gpu_idx < 0 || gpu_idx >= n || !chain || !out)
        return -1;
    if (chain_len < 1 || chain_len * 4 + 4 > max_entries) return -1;

    plan_window_regs(window_base, wend, regs);

    /* Section 3a: bridge windows, chain order (leaf -> root). */
    for (i = 0; i < chain_len; i++) {
        const plan_dev *b = &devs[chain[i]];
        for (k = 0; k < 4; k++) {
            out[cnt].bus = b->bus; out[cnt].dev = b->dev;
            out[cnt].func = b->func;
            out[cnt].offset = win_off[k];
            out[cnt].width = win_wid[k];
            out[cnt].newval = regs[k];
            cnt++;
        }
    }

    /* Section 3b: GPU BARs. Raw BAR value = address bits + the
     * 64-bit prefetchable type field (0xC) in the low nibble. */
    /* (BAR entries are plan-only in v0.3; the write list filters
     * them out - see plan_filter_write_list below.) */
    {
        const plan_dev *g = &devs[gpu_idx];
        static const uint8_t bar_off[4] = { 0x10, 0x14, 0x18, 0x1C };
        uint32_t vals[4];
        vals[0] = (uint32_t)(window_base & 0xFFFFFFFFULL) | 0xCu;
        vals[1] = (uint32_t)(window_base >> 32);
        vals[2] = (uint32_t)(bar2_base & 0xFFFFFFFFULL) | 0xCu;
        vals[3] = (uint32_t)(bar2_base >> 32);
        for (k = 0; k < 4; k++) {
            out[cnt].bus = g->bus; out[cnt].dev = g->dev;
            out[cnt].func = g->func;
            out[cnt].offset = bar_off[k];
            out[cnt].width = 4;
            out[cnt].newval = vals[k];
            cnt++;
        }
    }
    return cnt;
}

plan_gate plan_write_gate(int write_enable, int plan_found)
{
    if (!write_enable) return PLAN_GATE_SKIP_NO_ENABLE;
    if (!plan_found)   return PLAN_GATE_SKIP_NO_PLAN;
    return PLAN_GATE_ARMED;
}

int plan_filter_write_list(const plan_entry *man, int mcount,
                           uint8_t gpu_bus, uint8_t gpu_dev, uint8_t gpu_func,
                           plan_entry *out, int max_out)
{
    int cnt = 0, i;
    if (!man || !out || mcount < 0 || max_out < 0) return -1;
    for (i = 0; i < mcount; i++) {
        const plan_entry *e = &man[i];
        /* Keep only bridge prefetch-window registers... */
        if (!(e->offset == 0x24 || e->offset == 0x26 ||
              e->offset == 0x28 || e->offset == 0x2C))
            continue;
        /* ...and never the GPU itself (its BAR entries use other
         * offsets anyway; this is a second, independent guard). */
        if (e->bus == gpu_bus && e->dev == gpu_dev && e->func == gpu_func)
            continue;
        /* Width sanity: 0x24/0x26 are 16-bit, 0x28/0x2C are 32-bit.
         * A manifest entry that disagrees is corrupt - refuse all. */
        if ((e->offset == 0x24 || e->offset == 0x26) && e->width != 2)
            return -1;
        if ((e->offset == 0x28 || e->offset == 0x2C) && e->width != 4)
            return -1;
        if (cnt >= max_out) return -1;
        out[cnt++] = *e;
    }
    return cnt;
}

int plan_build_write_list_v4(const plan_entry *man, int mcount,
                             uint8_t gpu_bus, uint8_t gpu_dev, uint8_t gpu_func,
                             plan_entry *out, int max_out)
{
    int cnt, i;
    if (!man || !out || mcount < 0 || max_out < 0) return -1;

    /* Windows first: exactly the v0.3 list, same order, same values. */
    cnt = plan_filter_write_list(man, mcount, gpu_bus, gpu_dev, gpu_func,
                                 out, max_out);
    if (cnt < 0) return -1;

    /* Then the GPU's BAR entries (plan 3b), in manifest order. */
    for (i = 0; i < mcount; i++) {
        const plan_entry *e = &man[i];
        if (e->bus != gpu_bus || e->dev != gpu_dev || e->func != gpu_func)
            continue;
        if (!(e->offset == 0x10 || e->offset == 0x14 ||
              e->offset == 0x18 || e->offset == 0x1C))
            continue;
        if (e->width != 4) return -1;   /* corrupt manifest: refuse all */
        if (cnt >= max_out) return -1;
        out[cnt++] = *e;
    }

    /* COMMAND last: enable only after every placement is in. */
    if (cnt >= max_out) return -1;
    out[cnt].bus = gpu_bus; out[cnt].dev = gpu_dev; out[cnt].func = gpu_func;
    out[cnt].offset = PLAN_GPU_COMMAND_OFFSET;
    out[cnt].width = 2;
    out[cnt].newval = PLAN_GPU_COMMAND_VALUE;
    cnt++;
    return cnt;
}
