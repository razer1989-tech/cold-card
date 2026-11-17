/*
 * egpushim.c - EFI Shim v0.6: FULL PLACEMENT + ICM CLAIM + TB SNAPSHOT
 * (superset of the v0.4 writer / v0.4.2 tbprobe)
 *
 * v0.5.0-icmclaim adds ONE new phase between the planner and the
 * writer: an ICM CLAIM against the Thunderbolt host controller's
 * firmware connection manager. Rationale (proven 2026-11-16 night):
 * the EFI placement lands and holds inside EFI, and disconnecting
 * every EFI Thunderbolt driver changes nothing - but the kernel
 * journal shows the eGPU ABSENT from the initial PCI scan, appearing
 * only ~29.5 s in via Thunderbolt hotplug in virgin state, because
 * the PCIe tunnel drops at the handover and the enclosure
 * power-cycles the card. The one signal that tells the ICM firmware
 * "an OS driver is present, keep my tunnels" is the ICM
 * DRIVER_READY control packet (Linux icm.c icm_ar_driver_ready).
 * This build sends exactly that packet from EFI, through the NHI
 * ring 0 pair, BEFORE the writer places the card - so if the ICM
 * honours the claim across ExitBootServices, the kernel's first
 * scan finds the card warm at its EFI address with the placed BARs.
 * The claim is gated by ICM_CLAIM=1 in shim.cfg; without it the ICM
 * phase is read-only recon. Every wait is bounded and every failure
 * path falls through to the normal writer + chainload: a failed
 * claim can never block the boot. The v0.4.2 TB probe is retained
 * but now runs only when TB_PROBE=1 (its question is answered).
 *
 * v0.6.0-snapshot adds ONE new phase after the ICM phase: a
 * READ-ONLY capture of the Thunderbolt routers' config spaces
 * (SWITCH, PORT and HOPS spaces of the host router and of the
 * enclosure router behind it), taken over ring 0 with the kernel's
 * config-packet protocol (ctl.c tb_cfg_read_raw; wire core in
 * cfg_pure.c). Rationale (proven 2026-11-17 morning): the v0.5.0
 * recon found NO ICM running on this machine - Apple's EFI stack
 * is a native connection manager whose ring-0 state and router
 * programming vanish at the handover. Before building any
 * freeze/replay machinery (an ExitBootServices hook that would
 * re-apply this state), the state itself must be captured and
 * judged complete at the desk. The capture reuses the ICM phase's
 * ring save/restore discipline, raises TPL during the exchanges so
 * Apple's own TB driver callbacks cannot interleave, buffers all
 * output in memory, and only logs (and writes LOGS\SNAPSHOT.BIN)
 * after the channel has been restored. Gated by SNAPSHOT=1 in
 * shim.cfg. It sends config READ packets only - it never writes a
 * single config-space dword on any router.
 *
 * v0.6.1 fixes the first hardware run's failure (2026-11-17): the
 * NHI never consumed the TX descriptor (tx_completed=0) because
 * the ring doorbell register (+8) still carried Apple's tail index
 * (0x00020002) - the hardware tail does NOT reset when the
 * descriptor base changes, so a fresh prod=1 looked like "nothing
 * new". The kernel's tb_ring_stop zeroes +8 for exactly this
 * reason; both ring-programming sites now zero the doorbell right
 * after the descriptor-base write, and the snapshot phase logs a
 * full ring readback after programming.
 *
 * v0.6.2 fixes the second hardware run's failure (2026-11-17):
 * with the tail cleared the doorbell prod advanced 0 -> 2 across
 * the attempts, yet the tail stayed 0 and the descriptor was
 * never read - the NHI was still working from Apple's descriptor
 * array, because a new descriptor base written into a LIVE
 * (enabled) ring is ignored. The kernel's flow always starts
 * from a stopped ring. Both programming sites now write the
 * options flags to 0 (disable) and stall briefly BEFORE touching
 * the descriptor base, and the snapshot readback covers the
 * counts and options registers as well as the bases/doorbells.
 *
 * v0.7.0 changes the channel, not the questions: three own-ring
 * builds (0.6.0/0.6.1/0.6.2) failed identically - registers
 * programmed exactly per the kernel, readbacks perfect, doorbell
 * prod advancing, and the NHI never fetching a single descriptor
 * (an IOMMU domain that only maps Apple's buffers is the leading
 * unprovable-from-here suspect; a T2 Mac plausibly does exactly
 * that). The snapshot phase therefore no longer reprograms ring 0
 * at all: it BORROWS Apple's live ring - posts each config READ
 * into the idle TX ring's next slot (reusing Apple's own frame
 * buffer for that slot, so every DMA address is one Apple's
 * driver itself set up) and reads replies out of Apple's RX ring
 * by watching for new completions. No ring register is written
 * except the TX doorbell (which is the act of posting itself);
 * the RX doorbell and Apple's consumer accounting are never
 * touched, and nothing needs restoring afterwards.
 *
 * v0.7.1: the borrowed channel WORKS (first hardware exchange
 * completed end-to-end) - but the switch answered the shim's
 * 32-dword switch-space read with INVALID_CONFIG_SPACE. The
 * kernel never reads that wide: the switch header is a 5-dword
 * read, a port header 8, a hop entry 2. snap_read_space now
 * chunks at kernel sizes (8 for SWITCH/PORT, 2 for HOPS), so a
 * space's end is discovered the same way the kernel discovers
 * register boundaries - one modest read at a time.
 *
 * v0.7.2: v0.7.1 captured the host router's switch space in full
 * (24 dwords; 8086:15EA, enabled) and then the channel stalled
 * for good - Apple's RX ring has only 8 slots and the shim was
 * inspecting replies WITHOUT consuming them, so the ring filled
 * and stayed full. The exchange now behaves like a real consumer:
 * it inspects frames starting at Apple's consumer index (the two
 * frames Apple had pending are counted as strays) and advances
 * the RX doorbell's consumer half after every inspected frame.
 * A global sequence counter replaces per-chunk attempt numbers,
 * and the host router walk reuses the first read's header instead
 * of re-reading the switch space. (Advancing cons consumes
 * Apple's pending frames for good - acceptable here because the
 * boot chainloads onward and Apple's EFI driver never resumes.)
 *
 * v0.7.3: v0.7.2 deadlocked its own reply scan - it started
 * inspecting at Apple's consumer index, whose slot was an old
 * already-reposted buffer that will never complete again, while
 * the actual replies landed at the hardware fill index (the RX
 * doorbell's high half) and were never looked at (the log showed
 * tx_completed=1 with zero frames inspected). The scan again
 * starts at the fill index, as in v0.7.0/v0.7.1; the consumer
 * advance after each inspected frame now jumps the doorbell
 * straight past the inspected frame, freeing every slot up to it
 * (Apple's leftovers included) - the drain the v0.7.1 stall
 * needed, without the FIFO trap.
 *
 * v0.7.4: v0.7.3's transport ran clean (21 exchanges, zero
 * timeouts) and the semantics surfaced: the host switch space
 * reads in full, port 0 answers (port 0 is the switch itself -
 * the kernel skips its header read entirely), and every real
 * port 1..13 refuses its PORT space with
 * TB_CFG_ERROR_INVALID_CONFIG_SPACE, which the kernel's
 * tb_cfg_get_error() documents as "port not implemented". So in
 * the EFI environment the host's port config spaces are dark.
 * The one thing never tested: whether the config channel still
 * FORWARDS over the live link - the probe phase now tries every
 * host port 1..max_port as a route to a downstream router,
 * regardless of the dark headers, and walks any router that
 * answers (the enclosure would give up its switch space, its
 * ports and their hop tables - the downstream half of the path).
 *
 * Spec: blackmagic-egpu-efi-shim-spec.md section 6 (goal local-wednesday-home-lab)
 * Placement plan: egpu-shim-v0.2-placement-plan.md sections 3a and 5.
 *
 * What this program does, in order:
 *   1. Opens its own boot volume and creates LOGS\egpushim-<timestamp>.log
 *   2. Logs firmware info + the EFI memory map
 *   3. Enumerates the PCI tree via EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL using
 *      CONFIG-SPACE READS ONLY (raw BARs, bridge windows incl. upper-32
 *      prefetch registers, capability walk - no BAR probing, no writes)
 *   4. PLANNER: computes the placement plan's would-write table from
 *      live values (plan.c), exactly as v0.2 did.
 *   5. WRITER (v0.4; v0.3 wrote bridge windows only): IF AND ONLY IF
 *      the gate in plan.c returns ARMED (WRITE_ENABLE=1 in shim.cfg
 *      AND a valid plan was computed), it writes the FULL placement:
 *      the 20 prefetch-window registers of the 5 chain bridges (plan
 *      section 3a), then the GPU's four BAR registers (plan 3b), then
 *      - LAST - the GPU's COMMAND register <- 0x0007, the value
 *      Apple's firmware leaves in the internal twin at this stage
 *      (the eGPU's COMMAND is 0x0000 here: enumerated, never
 *      enabled). Protocol (plan section 5, binding): save every
 *      original value to the log and FLUSH before the first write;
 *      read back after every write; first mismatch restores ALL
 *      originals, logs WRITER_RESULT: ABORT_RESTORED, and chainloads
 *      anyway. On full success it logs WRITER_RESULT: APPLIED
 *      entries=25.
 *   6. Copies the ACPI tables (raw) into LOGS\ for offline parsing
 *   7. Chainloads the GRUB binary named in shim.cfg (or defaults)
 *
 * HARD INVARIANTS - auditable in this source:
 *   - PCI config-space writes exist ONLY inside do_write(), which is
 *     reachable ONLY when plan_write_gate() returns ARMED. With the
 *     default config (WRITE_ENABLE absent or 0) this binary performs
 *     ZERO config-space writes, exactly like v0.2.
 *   - The writer NEVER touches: the audio function, BAR5, the ROM
 *     BAR, or any device outside the 5-bridge chain and the GPU
 *     function itself. The write list comes from
 *     plan_build_write_list_v4(): bridge windows first (offsets
 *     0x24/0x26/0x28/0x2C on the chain bridges only), then the GPU's
 *     BARs (0x10/0x14/0x18/0x1C), then GPU COMMAND (0x04) last.
 *   - No RuntimeServices SetVariable / no NVRAM writes of any kind.
 *   - No BootOrder manipulation. This app runs only when a human
 *     boots it from the firmware's boot menu.
 *   - File writes go only to this app's own volume (its log + ACPI dumps).
 *   - All config-space writes are volatile: a power cycle restores
 *     firmware's own programming. Nothing here persists.
 *   - v0.5 MMIO exception, separately gated: the ICM phase performs
 *     MMIO reads/writes on the NHI (Thunderbolt host controller)
 *     BAR0 register space ONLY - ring-0 descriptor/options registers,
 *     the ICM mailbox registers, and read-only status registers. It
 *     writes NO PCI config space (the invariant above is intact),
 *     saves the NHI ring-0 registers before touching them and
 *     restores them afterwards, and its DMA buffers are its own
 *     boot-services pages, freed before chainload. With ICM_CLAIM
 *     absent or 0 the phase is strictly read-only.
 *   - v0.6 extends the same MMIO exception to the SNAPSHOT phase:
 *     same NHI BAR0 ring-0 registers, same save/restore discipline.
 *     The phase transmits config READ requests only; every response
 *     is data read FROM the routers. It writes no config space, on
 *     any device, by construction (cfg_pure builds read packets
 *     only; there is no write-packet builder in this build).
 */

#include <efi.h>
#include <efilib.h>
#include "plan.h"
#include "icm_pure.h"
#include "cfg_pure.h"

#define SHIM_VERSION "0.7.4-probeall"

static EFI_HANDLE        gImageHandle;
static EFI_FILE_HANDLE   gRootDir;      /* root of our own volume */
static EFI_FILE_HANDLE   gLogDir;       /* LOGS directory */
static EFI_FILE_HANDLE   gLogFile;      /* current log file, NULL if none */
static BOOLEAN           gPause = TRUE; /* from shim.cfg, default TRUE */
static CHAR16            gChainloadPath[256];
static UINT64            gWindowBase = PLAN_DEFAULT_WINDOW_BASE; /* shim.cfg */
static UINT64            gBar2Base   = PLAN_DEFAULT_BAR2_BASE;   /* shim.cfg */
static BOOLEAN           gWriteEnable = FALSE; /* shim.cfg WRITE_ENABLE, default 0 */
static BOOLEAN           gIcmClaim = FALSE;    /* shim.cfg ICM_CLAIM, default 0 (recon only) */
static BOOLEAN           gTbProbe = FALSE;     /* shim.cfg TB_PROBE, default 0 (answered question) */
static BOOLEAN           gSnapshot = FALSE;    /* shim.cfg SNAPSHOT, default 0 */

/* Planner results, kept for the writer (do_write runs after do_plan). */
static plan_entry        gManifest[PLAN_MAX_ENTRIES];
static int               gManifestCount = 0;
static int               gGpuIdx = -1;         /* index into gDevs, -1 = none */
static BOOLEAN           gPlanFound = FALSE;   /* full plan computed OK */

/* Devices collected during the PCI scan, for the planner. The parallel
 * arrays hold the CURRENT (old) register values the plan diffs against. */
#define MAX_DEVS 512
static plan_dev          gDevs[MAX_DEVS];
static int               gDevCount = 0;
static UINT32            gDevWin24[MAX_DEVS]; /* bridges: dword at 0x24 */
static UINT32            gDevWin28[MAX_DEVS]; /* bridges: dword at 0x28 */
static UINT32            gDevWin2C[MAX_DEVS]; /* bridges: dword at 0x2C */
static UINT32            gDevBars[MAX_DEVS][6]; /* raw BAR dwords 0x10.. */
static int               gSsdtSeq = 0;        /* SSDT dump numbering */

/* ------------------------------------------------------------------ */
/* Small ASCII string helpers (log file is plain ASCII)                */
/* ------------------------------------------------------------------ */

static UINTN a_len(const char *s) { UINTN n = 0; while (s[n]) n++; return n; }

static void a_cat(char *dst, UINTN cap, const char *src)
{
    UINTN d = a_len(dst), i = 0;
    while (src[i] && d + i + 1 < cap) { dst[d + i] = src[i]; i++; }
    dst[d + i] = 0;
}

static void a_cat_u64hex(char *dst, UINTN cap, UINT64 v, int digits)
{
    char tmp[32]; int i;
    const char *h = "0123456789ABCDEF";
    for (i = 0; i < digits; i++) { tmp[digits - 1 - i] = h[v & 0xF]; v >>= 4; }
    tmp[digits] = 0;
    a_cat(dst, cap, tmp);
}

static void a_cat_u64dec(char *dst, UINTN cap, UINT64 v)
{
    char tmp[32]; int i = 0;
    if (v == 0) { a_cat(dst, cap, "0"); return; }
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) { char c[2]; c[0] = tmp[--i]; c[1] = 0; a_cat(dst, cap, c); }
}

/* ------------------------------------------------------------------ */
/* Console echo (ASCII -> CHAR16)                                      */
/* ------------------------------------------------------------------ */

static void con_line(const char *s)
{
    CHAR16 buf[512]; UINTN i = 0;
    while (s[i] && i < 500) { buf[i] = (CHAR16)(UINT8)s[i]; i++; }
    buf[i++] = (CHAR16)'\r'; buf[i++] = (CHAR16)'\n'; buf[i] = 0;
    uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, buf);
}

/* ------------------------------------------------------------------ */
/* Logging: append one line to the log file (flushed by caller policy) */
/* ------------------------------------------------------------------ */

static void log_raw(const char *s)
{
    if (gLogFile) {
        UINTN n = a_len(s);
        uefi_call_wrapper(gLogFile->Write, 3, gLogFile, &n, (VOID *)s);
    }
}

static void log_flush(void)
{
    if (gLogFile) uefi_call_wrapper(gLogFile->Flush, 1, gLogFile);
}

static void log_line(const char *s)
{
    log_raw(s);
    log_raw("\r\n");
}

/* Section header: goes to file (flushed immediately) and console. */
static void log_section(const char *title)
{
    char line[256];
    line[0] = 0;
    a_cat(line, sizeof(line), "=== ");
    a_cat(line, sizeof(line), title);
    a_cat(line, sizeof(line), " ===");
    log_line("");
    log_line(line);
    log_flush();
    con_line(line);
}

/* ------------------------------------------------------------------ */
/* shim.cfg parsing (plain text, key=value, ASCII)                     */
/* ------------------------------------------------------------------ */

static UINT64 parse_hex(const char *s)
{
    UINT64 v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++;
        UINT32 dgt;
        if (c >= '0' && c <= '9') dgt = (UINT32)(c - '0');
        else if (c >= 'a' && c <= 'f') dgt = (UINT32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') dgt = (UINT32)(c - 'A' + 10);
        else break;
        v = (v << 4) | dgt;
    }
    return v;
}

static void load_config(void)
{
    EFI_FILE_HANDLE cfg = NULL;
    EFI_STATUS st;
    /* defaults */
    gPause = TRUE;
    gWriteEnable = FALSE;
    gIcmClaim = FALSE;
    gTbProbe = FALSE;
    gSnapshot = FALSE;
    CopyMem(gChainloadPath, L"\\EFI\\ubuntu\\grubx64.efi", sizeof(L"\\EFI\\ubuntu\\grubx64.efi"));

    if (!gRootDir) return;
    st = uefi_call_wrapper(gRootDir->Open, 5, gRootDir, &cfg, L"shim.cfg",
                           EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !cfg) { log_line("CONFIG: no shim.cfg found, using defaults"); return; }

    {
        char buf[2048]; UINTN sz = sizeof(buf) - 1;
        SetMem(buf, sizeof(buf), 0);
        st = uefi_call_wrapper(cfg->Read, 3, cfg, &sz, buf);
        uefi_call_wrapper(cfg->Close, 1, cfg);
        if (EFI_ERROR(st)) { log_line("CONFIG: shim.cfg unreadable, using defaults"); return; }
        buf[sz] = 0;

        /* very small line parser */
        UINTN i = 0;
        while (buf[i]) {
            char line[512]; UINTN li = 0;
            while (buf[i] && buf[i] != '\n' && li < 500) {
                if (buf[i] != '\r') line[li++] = buf[i];
                i++;
            }
            if (buf[i] == '\n') i++;
            line[li] = 0;
            if (li > 6 && CompareMem(line, "PAUSE=", 6) == 0) {
                gPause = (line[6] != '0');
            } else if (li > 10 && CompareMem(line, "CHAINLOAD=", 10) == 0) {
                UINTN k = 0;
                while (line[10 + k] && k < 250) { gChainloadPath[k] = (CHAR16)(UINT8)line[10 + k]; k++; }
                gChainloadPath[k] = 0;
            } else if (li > 12 && CompareMem(line, "WINDOW_BASE=", 12) == 0) {
                gWindowBase = parse_hex(&line[12]);
            } else if (li > 10 && CompareMem(line, "BAR2_BASE=", 10) == 0) {
                gBar2Base = parse_hex(&line[10]);
            } else if (li > 13 && CompareMem(line, "WRITE_ENABLE=", 13) == 0) {
                gWriteEnable = (line[13] != '0');
            } else if (li > 10 && CompareMem(line, "ICM_CLAIM=", 10) == 0) {
                gIcmClaim = (line[10] != '0');
            } else if (li > 9 && CompareMem(line, "TB_PROBE=", 9) == 0) {
                gTbProbe = (line[9] != '0');
            } else if (li > 9 && CompareMem(line, "SNAPSHOT=", 9) == 0) {
                gSnapshot = (line[9] != '0');
            }
        }
    }
    log_line("CONFIG: shim.cfg loaded");
}

/* ------------------------------------------------------------------ */
/* PCI config-space access. Reads are used everywhere. The single     */
/* write primitive (pci_write) is called ONLY from do_write(), which  */
/* is itself gated by plan_write_gate() - see the header invariants.  */
/* ------------------------------------------------------------------ */

static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *gRB; /* current root bridge */

static EFI_STATUS pci_read(UINT8 bus, UINT8 dev, UINT8 func, UINT32 offset,
                           EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width, VOID *buf)
{
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI_ADDRESS addr;
    SetMem(&addr, sizeof(addr), 0);
    addr.Register = (UINT8)(offset & 0xFF);
    addr.ExtendedRegister = offset >> 8;
    addr.Function = func;
    addr.Device = dev;
    addr.Bus = bus;
    return uefi_call_wrapper(gRB->Pci.Read, 5, gRB, width, *(UINT64 *)&addr, 1, buf);
}

/* THE config-space write primitive. Its only caller is do_write(). */
static EFI_STATUS pci_write(UINT8 bus, UINT8 dev, UINT8 func, UINT32 offset,
                            EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width, VOID *buf)
{
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI_ADDRESS addr;
    SetMem(&addr, sizeof(addr), 0);
    addr.Register = (UINT8)(offset & 0xFF);
    addr.ExtendedRegister = offset >> 8;
    addr.Function = func;
    addr.Device = dev;
    addr.Bus = bus;
    return uefi_call_wrapper(gRB->Pci.Write, 5, gRB, width, *(UINT64 *)&addr, 1, buf);
}

/* Width-aware entry accessors used by the writer: a plan_entry's
 * width field (2 or 4 bytes) selects the access size. */
static UINT32 pci_read_entry(const plan_entry *e)
{
    if (e->width == 2) {
        UINT16 v = 0xFFFF;
        pci_read(e->bus, e->dev, e->func, e->offset, EfiPciIoWidthUint16, &v);
        return v;
    }
    {
        UINT32 v = 0xFFFFFFFF;
        pci_read(e->bus, e->dev, e->func, e->offset, EfiPciIoWidthUint32, &v);
        return v;
    }
}

static EFI_STATUS pci_write_entry(const plan_entry *e, UINT32 val)
{
    if (e->width == 2) {
        UINT16 v = (UINT16)val;
        return pci_write(e->bus, e->dev, e->func, e->offset, EfiPciIoWidthUint16, &v);
    }
    {
        UINT32 v = val;
        return pci_write(e->bus, e->dev, e->func, e->offset, EfiPciIoWidthUint32, &v);
    }
}

static UINT32 pci32(UINT8 b, UINT8 d, UINT8 f, UINT32 off)
{
    UINT32 v = 0xFFFFFFFF;
    pci_read(b, d, f, off, EfiPciIoWidthUint32, &v);
    return v;
}

static UINT16 pci16(UINT8 b, UINT8 d, UINT8 f, UINT32 off)
{
    UINT16 v = 0xFFFF;
    pci_read(b, d, f, off, EfiPciIoWidthUint16, &v);
    return v;
}

static UINT8 pci8(UINT8 b, UINT8 d, UINT8 f, UINT32 off)
{
    UINT8 v = 0xFF;
    pci_read(b, d, f, off, EfiPciIoWidthUint8, &v);
    return v;
}

static BOOLEAN gVisited[256];
static BOOLEAN gEgpuSeen = FALSE;
static BOOLEAN gEgpuAudioSeen = FALSE;

static void fmt_bdf(char *dst, UINTN cap, UINT8 b, UINT8 d, UINT8 f)
{
    dst[0] = 0;
    a_cat_u64hex(dst, cap, b, 2); a_cat(dst, cap, ":");
    a_cat_u64hex(dst, cap, d, 2); a_cat(dst, cap, ".");
    a_cat_u64dec(dst, cap, f);
}

static void log_device(UINT8 bus, UINT8 dev, UINT8 func)
{
    char line[1024];
    UINT32 id = pci32(bus, dev, func, 0x00);
    UINT16 vid = (UINT16)(id & 0xFFFF), did = (UINT16)(id >> 16);
    UINT16 cmd = pci16(bus, dev, func, 0x04);
    UINT16 sts = pci16(bus, dev, func, 0x06);
    UINT32 cls = pci32(bus, dev, func, 0x08);
    UINT8  hdr = pci8(bus, dev, func, 0x0E);
    UINT32 subsys = pci32(bus, dev, func, 0x2C);

    line[0] = 0;
    a_cat(line, sizeof(line), "DEV ");
    { char bdf[16]; fmt_bdf(bdf, sizeof(bdf), bus, dev, func); a_cat(line, sizeof(line), bdf); }
    a_cat(line, sizeof(line), " id=");
    a_cat_u64hex(line, sizeof(line), vid, 4); a_cat(line, sizeof(line), ":");
    a_cat_u64hex(line, sizeof(line), did, 4);
    a_cat(line, sizeof(line), " class=");
    a_cat_u64hex(line, sizeof(line), (cls >> 8) & 0xFFFFFF, 6);
    a_cat(line, sizeof(line), " hdrtype=");
    a_cat_u64hex(line, sizeof(line), hdr, 2);
    a_cat(line, sizeof(line), " cmd=");
    a_cat_u64hex(line, sizeof(line), cmd, 4);
    a_cat(line, sizeof(line), " sts=");
    a_cat_u64hex(line, sizeof(line), sts, 4);
    a_cat(line, sizeof(line), " subsys=");
    a_cat_u64hex(line, sizeof(line), (subsys >> 16) & 0xFFFF, 4);
    a_cat(line, sizeof(line), ":");
    a_cat_u64hex(line, sizeof(line), subsys & 0xFFFF, 4);
    log_line(line);

    /* BARs, raw, as firmware left them - NO probing (probing writes). */
    {
        int maxbar = ((hdr & 0x7F) == 1) ? 2 : 6, i;
        line[0] = 0; a_cat(line, sizeof(line), "  BAR");
        for (i = 0; i < maxbar; i++) {
            UINT32 bar = pci32(bus, dev, func, 0x10 + 4 * i);
            a_cat(line, sizeof(line), " [");
            a_cat_u64dec(line, sizeof(line), i);
            a_cat(line, sizeof(line), "]=");
            a_cat_u64hex(line, sizeof(line), bar, 8);
            if (bar & 0x4) { /* 64-bit BAR: log high half too */
                UINT32 hi = pci32(bus, dev, func, 0x10 + 4 * (i + 1));
                a_cat(line, sizeof(line), "/");
                a_cat_u64hex(line, sizeof(line), hi, 8);
                i++;
            }
        }
        log_line(line);
    }

    /* Bridge windows (header type 1) */
    if ((hdr & 0x7F) == 1) {
        UINT32 busreg = pci32(bus, dev, func, 0x18);
        UINT8 iob = pci8(bus, dev, func, 0x1C), iol = pci8(bus, dev, func, 0x1D);
        UINT16 memb = pci16(bus, dev, func, 0x20), meml = pci16(bus, dev, func, 0x22);
        UINT16 preb = pci16(bus, dev, func, 0x24), prel = pci16(bus, dev, func, 0x26);
        UINT32 prebu = pci32(bus, dev, func, 0x28), prelu = pci32(bus, dev, func, 0x2C);
        UINT32 rom = pci32(bus, dev, func, 0x38);
        line[0] = 0;
        a_cat(line, sizeof(line), "  BRIDGE pri=");
        a_cat_u64dec(line, sizeof(line), busreg & 0xFF);
        a_cat(line, sizeof(line), " sec=");
        a_cat_u64dec(line, sizeof(line), (busreg >> 8) & 0xFF);
        a_cat(line, sizeof(line), " sub=");
        a_cat_u64dec(line, sizeof(line), (busreg >> 16) & 0xFF);
        log_line(line);
        line[0] = 0;
        a_cat(line, sizeof(line), "  WIN io=");
        a_cat_u64hex(line, sizeof(line), iob, 2); a_cat(line, sizeof(line), "-");
        a_cat_u64hex(line, sizeof(line), iol, 2);
        a_cat(line, sizeof(line), " mem=");
        a_cat_u64hex(line, sizeof(line), memb, 4); a_cat(line, sizeof(line), "-");
        a_cat_u64hex(line, sizeof(line), meml, 4);
        a_cat(line, sizeof(line), " pref=");
        a_cat_u64hex(line, sizeof(line), preb, 4); a_cat(line, sizeof(line), "-");
        a_cat_u64hex(line, sizeof(line), prel, 4);
        a_cat(line, sizeof(line), " prefUpper base=");
        a_cat_u64hex(line, sizeof(line), prebu, 8);
        a_cat(line, sizeof(line), " limit=");
        a_cat_u64hex(line, sizeof(line), prelu, 8);
        a_cat(line, sizeof(line), " romBAR=");
        a_cat_u64hex(line, sizeof(line), rom, 8);
        log_line(line);
    } else {
        UINT32 rom = pci32(bus, dev, func, 0x30);
        line[0] = 0;
        a_cat(line, sizeof(line), "  romBAR=");
        a_cat_u64hex(line, sizeof(line), rom, 8);
        log_line(line);
    }

    /* Standard capability walk: look for PCIe cap (link status) */
    if (sts & 0x0010) {
        UINT8 ptr = pci8(bus, dev, func, 0x34) & 0xFC;
        int guard = 0;
        while (ptr >= 0x40 && guard++ < 32) {
            UINT8 capid = pci8(bus, dev, func, ptr);
            UINT8 next = pci8(bus, dev, func, ptr + 1) & 0xFC;
            if (capid == 0x10) { /* PCI Express */
                UINT16 link = pci16(bus, dev, func, ptr + 0x12);
                line[0] = 0;
                a_cat(line, sizeof(line), "  PCIE link speed=");
                a_cat_u64dec(line, sizeof(line), link & 0xF);
                a_cat(line, sizeof(line), " width=x");
                a_cat_u64dec(line, sizeof(line), (link >> 4) & 0x3F);
                log_line(line);
            }
            if (next == ptr) break;
            ptr = next;
        }
        /* Extended capabilities (from 0x100): look for Resizable BAR (0x0015) */
        {
            UINT32 off = 0x100; int g2 = 0;
            while (off != 0 && g2++ < 32) {
                UINT32 hdrv = pci32(bus, dev, func, off);
                UINT32 cid;
                if (hdrv == 0 || hdrv == 0xFFFFFFFF) break;
                cid = hdrv & 0xFFFF;
                if (cid == 0x0015) {
                    UINT32 capreg = pci32(bus, dev, func, off + 4);
                    UINT32 ctrl0 = pci32(bus, dev, func, off + 8);
                    line[0] = 0;
                    a_cat(line, sizeof(line), "  REBAR present capReg=");
                    a_cat_u64hex(line, sizeof(line), capreg, 8);
                    a_cat(line, sizeof(line), " ctrl[bar0]=");
                    a_cat_u64hex(line, sizeof(line), ctrl0, 8);
                    log_line(line);
                    {
                        /* capReg bits [19:4]: supported size indices for
                         * BAR0 (size = 2^(20+idx) bytes). ctrl bits
                         * [12:8]: currently selected size index. */
                        UINT32 mask = (capreg >> 4) & 0xFFFF;
                        UINT32 idx = (ctrl0 >> 8) & 0x1F;
                        line[0] = 0;
                        a_cat(line, sizeof(line), "  REBAR bar0 supportedSizeIdxMask=");
                        a_cat_u64hex(line, sizeof(line), mask, 4);
                        a_cat(line, sizeof(line), " currentSizeIdx=");
                        a_cat_u64dec(line, sizeof(line), idx);
                        a_cat(line, sizeof(line), " (= ");
                        a_cat_u64dec(line, sizeof(line), 1ULL << idx);
                        a_cat(line, sizeof(line), " MiB)");
                        log_line(line);
                    }
                }
                off = (hdrv >> 20) & 0xFFF;
            }
        }
    }

    /* The flag lines the whole project waits on. v0.2 fix: the
     * Ellesmere device ID alone also matches the INTERNAL GPU (same
     * chip, Apple subsystem vendor 106B). Only the Blackmagic device
     * (subsystem vendor BDBD, subsystem ID A14C) is the eGPU. */
    {
        UINT16 subvendor = (UINT16)(subsys & 0xFFFF);
        UINT16 subid = (UINT16)(subsys >> 16);
        if (vid == 0x1002 && did == 0x67DF && subvendor == 0xBDBD && subid == 0xA14C) {
            gEgpuSeen = TRUE;
            log_line("  *** BLACKMAGIC eGPU GPU FUNCTION FOUND ***");
        } else if (vid == 0x1002 && did == 0x67DF) {
            log_line("  (internal Ellesmere twin - control sample, not the eGPU)");
        }
        if (vid == 0x1002 && did == 0xAAF0 && subvendor == 0xBDBD) {
            gEgpuAudioSeen = TRUE;
            log_line("  *** BLACKMAGIC eGPU HDMI-AUDIO FUNCTION FOUND ***");
        }
    }

    /* Collect the device (+ current register values) for the planner */
    if (gDevCount < MAX_DEVS) {
        int idx = gDevCount;
        plan_dev *pd = &gDevs[idx];
        int bi;
        pd->bus = bus; pd->dev = dev; pd->func = func;
        pd->vid = vid; pd->did = did; pd->subsys = subsys; pd->hdrtype = hdr;
        pd->pri = pd->sec = pd->sub = 0;
        gDevWin24[idx] = gDevWin28[idx] = gDevWin2C[idx] = 0;
        if ((hdr & 0x7F) == 1) {
            UINT32 br = pci32(bus, dev, func, 0x18);
            pd->pri = (UINT8)(br & 0xFF);
            pd->sec = (UINT8)((br >> 8) & 0xFF);
            pd->sub = (UINT8)((br >> 16) & 0xFF);
            gDevWin24[idx] = pci32(bus, dev, func, 0x24);
            gDevWin28[idx] = pci32(bus, dev, func, 0x28);
            gDevWin2C[idx] = pci32(bus, dev, func, 0x2C);
        }
        for (bi = 0; bi < 6; bi++)
            gDevBars[idx][bi] = pci32(bus, dev, func, 0x10 + 4 * bi);
        gDevCount++;
    }
}

static void scan_bus(UINT8 bus, int depth)
{
    UINT8 dev;
    if (bus > 255 || gVisited[bus] || depth > 16) return;
    gVisited[bus] = TRUE;
    for (dev = 0; dev < 32; dev++) {
        UINT16 vid0 = (UINT16)(pci32(bus, dev, 0, 0) & 0xFFFF);
        UINT8 hdr0, maxfunc = 1, func;
        if (vid0 == 0xFFFF) continue;
        hdr0 = pci8(bus, dev, 0, 0x0E);
        if (hdr0 & 0x80) maxfunc = 8;
        for (func = 0; func < maxfunc; func++) {
            UINT32 id = pci32(bus, dev, func, 0);
            UINT8 hdr;
            if ((id & 0xFFFF) == 0xFFFF) continue;
            log_device(bus, dev, func);
            hdr = pci8(bus, dev, func, 0x0E);
            if ((hdr & 0x7F) == 1) {
                UINT8 sec = pci8(bus, dev, func, 0x19);
                if (sec != 0 && sec != bus) scan_bus(sec, depth + 1);
            }
        }
    }
}

static UINT64 rd_u64le(const UINT8 *p)
{
    UINT64 v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void log_root_bridge_resources(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *rb)
{
    /* ACPI address-space descriptors returned by Configuration().
     * Parsed by BYTE OFFSET, never by struct cast: the descriptor is
     * packed (UINT64 fields begin at offset 6, unaligned) - a natural-
     * alignment struct reads garbage. Caught in QEMU testing, v0.1. */
    VOID *res = NULL;
    EFI_STATUS st = uefi_call_wrapper(rb->Configuration, 2, rb, &res);
    char line[512];
    if (EFI_ERROR(st) || !res) { log_line("  ROOTBRIDGE resources: unavailable"); return; }
    {
        UINT8 *p = (UINT8 *)res;
        int guard = 0;
        while (guard++ < 64) {
            UINT8 desc = p[0];
            UINT16 len;
            if (desc == 0x79) break; /* end tag */
            len = (UINT16)(p[1] | (p[2] << 8));
            if (desc == 0x8A && len >= 43) {
                /* offsets: type@3 genflag@4 specflag@5 gran@6 min@14
                 * max@22 translation@30 addrlength@38 */
                line[0] = 0;
                a_cat(line, sizeof(line), "  ROOTBRIDGE res type=");
                a_cat_u64dec(line, sizeof(line), p[3]); /* 0=mem 1=io 2=bus */
                a_cat(line, sizeof(line), " specFlag=");
                a_cat_u64hex(line, sizeof(line), p[5], 2);
                a_cat(line, sizeof(line), " min=");
                a_cat_u64hex(line, sizeof(line), rd_u64le(p + 14), 16);
                a_cat(line, sizeof(line), " max=");
                a_cat_u64hex(line, sizeof(line), rd_u64le(p + 22), 16);
                a_cat(line, sizeof(line), " len=");
                a_cat_u64hex(line, sizeof(line), rd_u64le(p + 38), 16);
                log_line(line);
            } else if (desc == 0x8A || desc == 0x8B) {
                /* known descriptor family, just advance */
            } else {
                log_line("  ROOTBRIDGE res: unknown descriptor, stopping");
                break;
            }
            if (len == 0) break;
            p += 3 + len;
        }
    }
}

static void do_pci_scan(void)
{
    EFI_HANDLE *handles = NULL;
    UINTN n = 0, i;
    EFI_STATUS st;
    EFI_GUID guid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;

    log_section("PCI TREE (config-space reads only)");
    st = uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol, &guid, NULL, &n, &handles);
    if (EFI_ERROR(st) || n == 0) { log_line("PCI: no root bridge protocol found"); return; }
    for (i = 0; i < n; i++) {
        EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *rb = NULL;
        char line[256];
        st = uefi_call_wrapper(BS->HandleProtocol, 3, handles[i], &guid, (VOID **)&rb);
        if (EFI_ERROR(st) || !rb) continue;
        gRB = rb;
        line[0] = 0;
        a_cat(line, sizeof(line), "ROOTBRIDGE #");
        a_cat_u64dec(line, sizeof(line), i);
        a_cat(line, sizeof(line), " segment=");
        a_cat_u64dec(line, sizeof(line), rb->SegmentNumber);
        log_line(line);
        log_root_bridge_resources(rb);
        SetMem(gVisited, sizeof(gVisited), 0);
        scan_bus(0, 0);
    }
    log_line("");
    if (gEgpuSeen) {
        log_line("EGPU_AT_EFI_STAGE: YES");
        con_line("EGPU_AT_EFI_STAGE: YES");
    } else {
        log_line("EGPU_AT_EFI_STAGE: NO");
        con_line("EGPU_AT_EFI_STAGE: NO");
    }
    log_line(gEgpuAudioSeen ? "EGPU_AUDIO_AT_EFI_STAGE: YES" : "EGPU_AUDIO_AT_EFI_STAGE: NO");
    log_flush();
}

/* ------------------------------------------------------------------ */
/* EFI memory map                                                      */
/* ------------------------------------------------------------------ */

static const char *mem_type_name(UINT32 t)
{
    static const char *names[] = {
        "Reserved", "LoaderCode", "LoaderData", "BootServicesCode",
        "BootServicesData", "RuntimeServicesCode", "RuntimeServicesData",
        "Conventional", "Unusable", "ACPIReclaim", "ACPINVS",
        "MMIO", "MMIOPort", "PalCode", "Persistent"
    };
    if (t < 15) return names[t];
    return "Other";
}

static void do_memory_map(void)
{
    UINTN mapSize = 0, mapKey, descSize, i, count;
    UINT32 descVer;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    EFI_STATUS st;
    char line[512];
    UINT64 totals[16];

    log_section("EFI MEMORY MAP");
    st = uefi_call_wrapper(BS->GetMemoryMap, 5, &mapSize, map, &mapKey, &descSize, &descVer);
    if (st != EFI_BUFFER_TOO_SMALL) { log_line("MEMMAP: GetMemoryMap sizing failed"); return; }
    mapSize += 8 * descSize;
    map = AllocatePool(mapSize);
    if (!map) { log_line("MEMMAP: out of memory"); return; }
    st = uefi_call_wrapper(BS->GetMemoryMap, 5, &mapSize, map, &mapKey, &descSize, &descVer);
    if (EFI_ERROR(st)) { log_line("MEMMAP: GetMemoryMap failed"); FreePool(map); return; }

    SetMem(totals, sizeof(totals), 0);
    count = mapSize / descSize;
    for (i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * descSize);
        if (d->Type < 16) totals[d->Type] += d->NumberOfPages * 4096ULL;
        line[0] = 0;
        a_cat(line, sizeof(line), "MEM ");
        a_cat(line, sizeof(line), mem_type_name(d->Type));
        a_cat(line, sizeof(line), " phys=");
        a_cat_u64hex(line, sizeof(line), d->PhysicalStart, 16);
        a_cat(line, sizeof(line), " pages=");
        a_cat_u64dec(line, sizeof(line), d->NumberOfPages);
        log_line(line);
    }
    log_line("-- totals by type (bytes) --");
    for (i = 0; i < 16; i++) {
        if (!totals[i]) continue;
        line[0] = 0;
        a_cat(line, sizeof(line), "TOTAL ");
        a_cat(line, sizeof(line), mem_type_name((UINT32)i));
        a_cat(line, sizeof(line), " = ");
        a_cat_u64hex(line, sizeof(line), totals[i], 16);
        log_line(line);
    }
    FreePool(map);
    log_flush();
}

/* ------------------------------------------------------------------ */
/* ACPI table capture (raw copies for offline parsing)                 */
/* ------------------------------------------------------------------ */

static void dump_table_file(const char *sig4, const VOID *addr, UINT32 len, int seq)
{
    CHAR16 name[64];
    EFI_FILE_HANDLE f = NULL;
    EFI_STATUS st;
    UINTN i, w;
    char line[256];

    if (!gLogDir || !addr || len == 0 || len > (64U << 20)) return;
    for (i = 0; i < 5; i++) name[i] = 0;
    {
        static const char pre[] = "acpi-";
        UINTN k = 0;
        for (i = 0; pre[i]; i++) name[k++] = (CHAR16)pre[i];
        for (i = 0; i < 4 && sig4[i]; i++) name[k++] = (CHAR16)(UINT8)sig4[i];
        if (seq > 0) { /* v0.2: numbered SSDT dumps (v0.1 overwrote one file) */
            name[k++] = (CHAR16)'-';
            name[k++] = (CHAR16)('0' + (seq / 10) % 10);
            name[k++] = (CHAR16)('0' + seq % 10);
        }
        name[k++] = (CHAR16)'.'; name[k++] = (CHAR16)'a'; name[k++] = (CHAR16)'m';
        name[k++] = (CHAR16)'l'; name[k] = 0;
    }
    st = uefi_call_wrapper(gLogDir->Open, 5, gLogDir, &f, name,
                           EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(st) || !f) {
        line[0] = 0; a_cat(line, sizeof(line), "ACPI: could not create dump file for ");
        a_cat(line, sizeof(line), sig4);
        log_line(line);
        return;
    }
    w = len;
    uefi_call_wrapper(f->Write, 3, f, &w, (VOID *)addr);
    uefi_call_wrapper(f->Close, 1, f);
    line[0] = 0;
    a_cat(line, sizeof(line), "ACPI: dumped ");
    a_cat(line, sizeof(line), sig4);
    a_cat(line, sizeof(line), " (");
    a_cat_u64dec(line, sizeof(line), len);
    a_cat(line, sizeof(line), " bytes)");
    log_line(line);
}

static void handle_acpi_table(const VOID *addr)
{
    const UINT8 *t = (const UINT8 *)addr;
    char sig[5];
    UINT32 len;
    char line[256];
    int i;
    for (i = 0; i < 4; i++) sig[i] = (char)t[i];
    sig[4] = 0;
    len = *(const UINT32 *)(t + 4);
    line[0] = 0;
    a_cat(line, sizeof(line), "ACPI table ");
    a_cat(line, sizeof(line), sig);
    a_cat(line, sizeof(line), " at ");
    a_cat_u64hex(line, sizeof(line), (UINT64)(UINTN)addr, 16);
    a_cat(line, sizeof(line), " len=");
    a_cat_u64dec(line, sizeof(line), len);
    log_line(line);
    {
        int seq = 0;
        if (CompareMem(sig, "SSDT", 4) == 0) seq = ++gSsdtSeq;
        dump_table_file(sig, addr, len, seq);
    }

    /* FADT (FACP): follow its DSDT pointer and dump the DSDT too */
    if (CompareMem(sig, "FACP", 4) == 0 && len >= 148) {
        UINT64 xdsdt = *(const UINT64 *)(t + 140);
        UINT32 dsdt = *(const UINT32 *)(t + 40);
        const UINT8 *d = (const UINT8 *)(UINTN)(xdsdt ? xdsdt : dsdt);
        if (d && CompareMem((VOID *)d, "DSDT", 4) == 0) {
            UINT32 dlen = *(const UINT32 *)(d + 4);
            log_line("ACPI table DSDT (via FACP pointer)");
            dump_table_file("DSDT", d, dlen, 0);
        }
    }
}

static void do_acpi(void)
{
    EFI_GUID acpi20 = ACPI_20_TABLE_GUID;
    EFI_GUID acpi10 = ACPI_TABLE_GUID;
    VOID *rsdp = NULL;
    UINTN i;
    const UINT8 *r;
    UINT8 rev;
    char line[256];

    log_section("ACPI TABLES");
    for (i = 0; i < ST->NumberOfTableEntries; i++) {
        if (CompareMem(&ST->ConfigurationTable[i].VendorGuid, &acpi20, sizeof(EFI_GUID)) == 0 ||
            CompareMem(&ST->ConfigurationTable[i].VendorGuid, &acpi10, sizeof(EFI_GUID)) == 0) {
            rsdp = ST->ConfigurationTable[i].VendorTable;
            break;
        }
    }
    if (!rsdp) { log_line("ACPI: no RSDP in EFI configuration table"); log_flush(); return; }
    r = (const UINT8 *)rsdp;
    if (CompareMem((VOID *)r, "RSD PTR ", 8) != 0) { log_line("ACPI: RSDP signature mismatch"); log_flush(); return; }
    rev = r[15];
    line[0] = 0;
    a_cat(line, sizeof(line), "ACPI RSDP revision ");
    a_cat_u64dec(line, sizeof(line), rev);
    log_line(line);

    /* Prefer XSDT whenever the RSDP carries one (some firmware reports
     * revision 0 in the RSDP byte yet still publishes an XSDT - seen in
     * QEMU/OVMF testing; the address's presence is the reliable test). */
    {
        UINT32 rsdpLen = (rev >= 1) ? *(const UINT32 *)(r + 20) : 20;
        UINT64 xsdtAddr = (rsdpLen >= 36) ? *(const UINT64 *)(r + 24) : 0;
        if (xsdtAddr) {
        const UINT8 *xsdt = (const UINT8 *)(UINTN)xsdtAddr;
        if (xsdt && CompareMem((VOID *)xsdt, "XSDT", 4) == 0) {
            UINT32 len = *(const UINT32 *)(xsdt + 4);
            UINTN entries = (len - 36) / 8, k;
            for (k = 0; k < entries; k++) {
                UINT64 ea = *(const UINT64 *)(xsdt + 36 + k * 8);
                if (ea) handle_acpi_table((const VOID *)(UINTN)ea);
            }
            log_flush();
            return;
        }
        }
    }
    {
        UINT32 rsdtAddr = *(const UINT32 *)(r + 16);
        const UINT8 *rsdt = (const UINT8 *)(UINTN)rsdtAddr;
        if (rsdt && CompareMem((VOID *)rsdt, "RSDT", 4) == 0) {
            UINT32 len = *(const UINT32 *)(rsdt + 4);
            UINTN entries = (len - 36) / 4, k;
            for (k = 0; k < entries; k++) {
                UINT32 ea = *(const UINT32 *)(rsdt + 36 + k * 4);
                if (ea) handle_acpi_table((const VOID *)(UINTN)ea);
            }
        } else {
            log_line("ACPI: RSDT/XSDT not found");
        }
    }
    log_flush();
}

/* ------------------------------------------------------------------ */
/* Planner: compute the would-write table (plan.c) - NO writes         */
/* ------------------------------------------------------------------ */

static int find_dev_idx(UINT8 b, UINT8 d, UINT8 f)
{
    int i;
    for (i = 0; i < gDevCount; i++)
        if (gDevs[i].bus == b && gDevs[i].dev == d && gDevs[i].func == f)
            return i;
    return -1;
}

static UINT32 old_value_for(const plan_entry *e, int idx)
{
    if (idx < 0) return 0;
    switch (e->offset) {
    case 0x24: return gDevWin24[idx] & 0xFFFF;
    case 0x26: return (gDevWin24[idx] >> 16) & 0xFFFF;
    case 0x28: return gDevWin28[idx];
    case 0x2C: return gDevWin2C[idx];
    default:   return gDevBars[idx][(e->offset - 0x10) / 4];
    }
}

static void do_plan(void)
{
    static int chain[PLAN_MAX_CHAIN];
    plan_entry *man = gManifest;
    int gpu, clen, mcount, i;
    char line[768];
    char bdf[16];

    gPlanFound = FALSE;
    gManifestCount = 0;
    gGpuIdx = -1;

    log_section("PLANNER - WOULD-WRITE TABLE (computation only, NO writes performed)");
    if (gDevCount == 0) { log_line("PLAN: no devices were collected"); log_flush(); return; }

    gpu = plan_find_gpu(gDevs, gDevCount);
    if (gpu < 0) {
        log_line("PLAN: Blackmagic eGPU (1002:67DF, subsystem A14C/BDBD) NOT FOUND - no plan computed");
        log_flush();
        return;
    }
    gGpuIdx = gpu;
    line[0] = 0;
    a_cat(line, sizeof(line), "PLAN GPU: ");
    fmt_bdf(bdf, sizeof(bdf), gDevs[gpu].bus, gDevs[gpu].dev, gDevs[gpu].func);
    a_cat(line, sizeof(line), bdf);
    a_cat(line, sizeof(line), " (audio function ");
    a_cat(line, sizeof(line), plan_find_audio(gDevs, gDevCount) >= 0 ? "present" : "absent");
    a_cat(line, sizeof(line), ")");
    log_line(line);

    clen = plan_build_chain(gDevs, gDevCount, gpu, chain, PLAN_MAX_CHAIN);
    if (clen < 0) {
        log_line("PLAN: bridge chain from GPU does not reach a bus-0 root port - no plan computed");
        log_flush();
        return;
    }
    line[0] = 0;
    a_cat(line, sizeof(line), "PLAN chain leaf->root:");
    for (i = 0; i < clen; i++) {
        a_cat(line, sizeof(line), " ");
        fmt_bdf(bdf, sizeof(bdf), gDevs[chain[i]].bus, gDevs[chain[i]].dev, gDevs[chain[i]].func);
        a_cat(line, sizeof(line), bdf);
    }
    log_line(line);

    mcount = plan_build_manifest(gDevs, gDevCount, gpu, chain, clen,
                                 gWindowBase, gBar2Base, man, PLAN_MAX_ENTRIES);
    if (mcount < 0) { log_line("PLAN: manifest computation failed"); log_flush(); return; }
    gManifestCount = mcount;
    gPlanFound = TRUE;

    line[0] = 0;
    a_cat(line, sizeof(line), "PLAN windowBase=");
    a_cat_u64hex(line, sizeof(line), gWindowBase, 16);
    a_cat(line, sizeof(line), " windowEnd=");
    a_cat_u64hex(line, sizeof(line), plan_window_end(gBar2Base), 16);
    a_cat(line, sizeof(line), " bar2Base=");
    a_cat_u64hex(line, sizeof(line), gBar2Base, 16);
    log_line(line);

    for (i = 0; i < mcount; i++) {
        const plan_entry *e = &man[i];
        int idx = find_dev_idx(e->bus, e->dev, e->func);
        UINT32 old = old_value_for(e, idx);
        line[0] = 0;
        a_cat(line, sizeof(line), "PLAN ");
        fmt_bdf(bdf, sizeof(bdf), e->bus, e->dev, e->func);
        a_cat(line, sizeof(line), bdf);
        a_cat(line, sizeof(line), " off=0x");
        a_cat_u64hex(line, sizeof(line), e->offset, 2);
        a_cat(line, sizeof(line), " w=");
        a_cat_u64dec(line, sizeof(line), e->width);
        a_cat(line, sizeof(line), " old=");
        a_cat_u64hex(line, sizeof(line), old, e->width * 2);
        a_cat(line, sizeof(line), " new=");
        a_cat_u64hex(line, sizeof(line), e->newval, e->width * 2);
        log_line(line);
    }
    line[0] = 0;
    a_cat(line, sizeof(line), "PLAN_ENTRIES: ");
    a_cat_u64dec(line, sizeof(line), mcount);
    log_line(line);
    log_line("PLAN: end of would-write table - the WRITER section below records the gate decision");
    log_flush();
}

/* ------------------------------------------------------------------ */
/* Writer (v0.4): apply the full placement plan - the ONLY             */
/* code path in this program that performs PCI config-space writes.   */
/*                                                                    */
/* Reachable only when plan_write_gate() returns ARMED:               */
/*   WRITE_ENABLE=1 in shim.cfg AND a valid plan was computed.        */
/* Protocol (placement plan section 5, binding):                      */
/*   SAVE:    re-read every target register LIVE, log WSAVE lines,    */
/*            FLUSH the log - all before the first write.             */
/*   WRITE:   write + immediate read-back per entry, log WWRITE.      */
/*            First mismatch or write error stops the phase.          */
/*   RESTORE: on any failure, write back ALL saved originals in       */
/*            reverse (root->leaf) order, log WRESTORE lines and      */
/*            WRITER_RESULT: ABORT_RESTORED; chainload regardless.    */
/*   SUCCESS: log WRITER_RESULT: APPLIED entries=20.                  */
/* The write list comes from plan_build_write_list_v4(): bridge       */
/* windows, then the GPU's BARs, then GPU COMMAND last.               */
/* ------------------------------------------------------------------ */

#define WRITER_EXPECTED_ENTRIES 25  /* 20 window regs (3a) + 4 GPU BARs (3b) + 1 GPU COMMAND */

/* ------------------------------------------------------------------ */
/* ICM CLAIM (v0.5.0): port of the Linux thunderbolt driver's ICM     */
/* claim sequence (drivers/thunderbolt/icm.c + ctl.c + nhi.c,        */
/* torvalds/linux, read 2026-11-17; wire core in icm_pure.c).        */
/*                                                                   */
/* The NHI (07:00.0, 8086:15EB) register space is BAR0, memory-mapped */
/* and already enabled by Apple's firmware (CMD=0006 at EFI stage).  */
/* The claim is a single control-channel exchange over ring 0:       */
/*   - rings: TX/RX hop 0, descriptor arrays in our own pages,       */
/*     options flags = RING_FLAG_ENABLE|RING_FLAG_RAW (0xC0000000),  */
/*     RX sof/eof mask 0xFFFFFFFF, frame size 256 (tb_ring_start).   */
/*   - request: ICM DRIVER_READY packet (header only, code 0x3),     */
/*     dwords big-endian on the wire + CRC-32C trailer (tb_ctl_tx),  */
/*     descriptor eof=sof=11 (TB_CFG_PKG_ICM_CMD).                   */
/*   - response: descriptor eof=12 (TB_CFG_PKG_ICM_RESP) with the    */
/*     same packet code (icm_match); payload is the AR driver-ready  */
/*     response { hdr, romver, ramver, info }.                       */
/* Before the exchange we mirror icm_firmware_init's mode handling:  */
/* read the firmware mode from the OUTMAIL mailbox register; in CM   */
/* mode send NHI_MAILBOX_ALLOW_ALL_DEVS (0x23) first, exactly as     */
/* the kernel does. If the ICM firmware is not running (FW_STS       */
/* ICM_EN clear) or is in safe mode, we skip - starting the firmware */
/* is out of scope. The EFI ring-0 registers are saved first and     */
/* restored afterwards (after a tb_ring_stop-style teardown), and    */
/* the DMA pages are freed, so the NHI is left as we found it.       */
/* Every wait is bounded (mailbox 500 ms, response 3 x 5000 ms,      */
/* mirroring ICM_TIMEOUT/ICM_RETRIES); any failure logs and falls    */
/* through to the writer. MMIO only - no config-space writes here.   */
/* ------------------------------------------------------------------ */

#define NHI_TX_DESC_BASE    0x00000u
#define NHI_RX_DESC_BASE    0x08000u
#define NHI_TX_OPTS_BASE    0x19800u
#define NHI_RX_OPTS_BASE    0x29800u
#define NHI_REG_CAPS        0x39640u
#define NHI_REG_INMAIL_DATA 0x39900u
#define NHI_REG_INMAIL_CMD  0x39904u
#define NHI_REG_OUTMAIL_CMD 0x3990Cu
#define NHI_REG_FW_STS      0x39944u

#define NHI_FW_STS_ICM_EN        (1u << 0)
#define NHI_FW_STS_NVM_AUTH_DONE (1u << 31)
#define NHI_INMAIL_OP_REQUEST    (1u << 31)
#define NHI_INMAIL_ERROR         (1u << 30)
#define NHI_MAILBOX_ALLOW_ALL_DEVS 0x23u

#define ICM_RING_SIZE   16
#define ICM_RX_POSTED   9
#define ICM_TX_ATTEMPTS 3
#define ICM_RESP_TIMEOUT_MS 5000

static UINT32 mmio_rd32(UINT64 base, UINT32 off)
{
    return *(volatile UINT32 *)(UINTN)(base + off);
}

static void mmio_wr32(UINT64 base, UINT32 off, UINT32 v)
{
    *(volatile UINT32 *)(UINTN)(base + off) = v;
}

/* nhi_mailbox_cmd(): write data, set OP_REQUEST|cmd, poll for the
 * firmware to clear OP_REQUEST (500 ms bound, as in nhi.c). */
static int icm_mailbox(UINT64 bar, UINT32 cmd, UINT32 data, int *errbit)
{
    UINT32 v;
    int i;
    mmio_wr32(bar, NHI_REG_INMAIL_DATA, data);
    v = mmio_rd32(bar, NHI_REG_INMAIL_CMD);
    v &= ~(0xFFu | NHI_INMAIL_ERROR);
    v |= NHI_INMAIL_OP_REQUEST | (cmd & 0xFFu);
    mmio_wr32(bar, NHI_REG_INMAIL_CMD, v);
    for (i = 0; i < 500; i++) {
        v = mmio_rd32(bar, NHI_REG_INMAIL_CMD);
        if (!(v & NHI_INMAIL_OP_REQUEST)) {
            if (errbit) *errbit = (v & NHI_INMAIL_ERROR) ? 1 : 0;
            return 0;
        }
        uefi_call_wrapper(BS->Stall, 1, 1000);
    }
    return -1;
}

static const char *icm_mode_name(UINT32 mode)
{
    switch (mode) {
    case 0: return "SAFE";
    case 1: return "AUTH";
    case 2: return "EP";
    case 3: return "CM";
    default: return "?";
    }
}

static void do_icm(void)
{
    char line[768];
    int nhi = -1, i;
    UINT64 bar = 0;
    UINT32 v;
    UINT16 cmdreg;

    log_section("ICM - connection-manager recon + claim");

    for (i = 0; i < gDevCount; i++) {
        if (gDevs[i].vid == 0x8086 && gDevs[i].did == 0x15EB) { nhi = i; break; }
    }
    if (nhi < 0) {
        log_line("ICM: NHI (8086:15EB) not found in scan - phase skipped");
        return;
    }
    {
        UINT32 b0 = gDevBars[nhi][0];
        if ((b0 & 0x6u) == 0x4u)
            bar = (UINT64)(b0 & 0xFFFFFFF0u) | ((UINT64)gDevBars[nhi][1] << 32);
        else
            bar = (UINT64)(b0 & 0xFFFFFFF0u);
    }
    cmdreg = pci16(gDevs[nhi].bus, gDevs[nhi].dev, gDevs[nhi].func, 0x04);
    {
        line[0] = 0;
        a_cat(line, sizeof(line), "ICMRECON NHI at ");
        a_cat_u64dec(line, sizeof(line), gDevs[nhi].bus); a_cat(line, sizeof(line), ":");
        a_cat_u64dec(line, sizeof(line), gDevs[nhi].dev); a_cat(line, sizeof(line), ".");
        a_cat_u64dec(line, sizeof(line), gDevs[nhi].func);
        a_cat(line, sizeof(line), " BAR0=0x");
        a_cat_u64hex(line, sizeof(line), bar, 16);
        a_cat(line, sizeof(line), " COMMAND=0x");
        a_cat_u64hex(line, sizeof(line), cmdreg, 4);
        log_line(line);
    }
    if (bar == 0 || !(cmdreg & 0x2u)) {
        log_line("ICM: BAR0 unmapped or memory-space disabled - recon unsafe, phase skipped");
        return;
    }

    /* ---- read-only recon (always) ---- */
    v = mmio_rd32(bar, NHI_REG_CAPS);
    {
        line[0] = 0;
        a_cat(line, sizeof(line), "ICMRECON CAPS=0x");
        a_cat_u64hex(line, sizeof(line), v, 8);
        a_cat(line, sizeof(line), " hops=");
        a_cat_u64dec(line, sizeof(line), v & 0x7FFu);
        a_cat(line, sizeof(line), " version=0x");
        a_cat_u64hex(line, sizeof(line), (v >> 16) & 0xFFu, 2);
        log_line(line);
    }
    {
        UINT32 fwsts = mmio_rd32(bar, NHI_REG_FW_STS);
        UINT32 outmail = mmio_rd32(bar, NHI_REG_OUTMAIL_CMD);
        UINT32 inmail = mmio_rd32(bar, NHI_REG_INMAIL_CMD);
        line[0] = 0;
        a_cat(line, sizeof(line), "ICMRECON FW_STS=0x");
        a_cat_u64hex(line, sizeof(line), fwsts, 8);
        a_cat(line, sizeof(line), " ICM_EN=");
        a_cat_u64dec(line, sizeof(line), (fwsts & NHI_FW_STS_ICM_EN) ? 1 : 0);
        a_cat(line, sizeof(line), " NVM_AUTH_DONE=");
        a_cat_u64dec(line, sizeof(line), (fwsts & NHI_FW_STS_NVM_AUTH_DONE) ? 1 : 0);
        a_cat(line, sizeof(line), " OUTMAIL_CMD=0x");
        a_cat_u64hex(line, sizeof(line), outmail, 8);
        a_cat(line, sizeof(line), " mode=");
        a_cat_u64dec(line, sizeof(line), (outmail >> 8) & 0xFu);
        a_cat(line, sizeof(line), " (");
        a_cat(line, sizeof(line), icm_mode_name((outmail >> 8) & 0xFu));
        a_cat(line, sizeof(line), ") INMAIL_CMD=0x");
        a_cat_u64hex(line, sizeof(line), inmail, 8);
        log_line(line);
    }
    {
        line[0] = 0;
        a_cat(line, sizeof(line), "ICMRECON ring0 TX descbase=0x");
        a_cat_u64hex(line, sizeof(line),
                     (UINT64)mmio_rd32(bar, NHI_TX_DESC_BASE) |
                     ((UINT64)mmio_rd32(bar, NHI_TX_DESC_BASE + 4) << 32), 16);
        a_cat(line, sizeof(line), " doorbell=0x");
        a_cat_u64hex(line, sizeof(line), mmio_rd32(bar, NHI_TX_DESC_BASE + 8), 8);
        a_cat(line, sizeof(line), " count=0x");
        a_cat_u64hex(line, sizeof(line), mmio_rd32(bar, NHI_TX_DESC_BASE + 12), 8);
        a_cat(line, sizeof(line), " opts=0x");
        a_cat_u64hex(line, sizeof(line), mmio_rd32(bar, NHI_TX_OPTS_BASE), 8);
        a_cat(line, sizeof(line), "/0x");
        a_cat_u64hex(line, sizeof(line), mmio_rd32(bar, NHI_TX_OPTS_BASE + 4), 8);
        log_line(line);
        line[0] = 0;
        a_cat(line, sizeof(line), "ICMRECON ring0 RX descbase=0x");
        a_cat_u64hex(line, sizeof(line),
                     (UINT64)mmio_rd32(bar, NHI_RX_DESC_BASE) |
                     ((UINT64)mmio_rd32(bar, NHI_RX_DESC_BASE + 4) << 32), 16);
        a_cat(line, sizeof(line), " doorbell=0x");
        a_cat_u64hex(line, sizeof(line), mmio_rd32(bar, NHI_RX_DESC_BASE + 8), 8);
        a_cat(line, sizeof(line), " count=0x");
        a_cat_u64hex(line, sizeof(line), mmio_rd32(bar, NHI_RX_DESC_BASE + 12), 8);
        a_cat(line, sizeof(line), " opts=0x");
        a_cat_u64hex(line, sizeof(line), mmio_rd32(bar, NHI_RX_OPTS_BASE), 8);
        a_cat(line, sizeof(line), "/0x");
        a_cat_u64hex(line, sizeof(line), mmio_rd32(bar, NHI_RX_OPTS_BASE + 4), 8);
        log_line(line);
    }
    log_flush();

    if (!gIcmClaim) {
        log_line("ICMCLAIM: recon only (ICM_CLAIM is not 1 in shim.cfg) - no NHI writes performed");
        return;
    }

    /* ---- claim gates (mirror icm_firmware_init) ---- */
    if (!(mmio_rd32(bar, NHI_REG_FW_STS) & NHI_FW_STS_ICM_EN)) {
        log_line("ICMCLAIM RESULT: SKIPPED - ICM firmware not running (FW_STS ICM_EN clear); starting it is out of scope");
        return;
    }
    {
        UINT32 mode = (mmio_rd32(bar, NHI_REG_OUTMAIL_CMD) >> 8) & 0xFu;
        if (mode == 0) {
            log_line("ICMCLAIM RESULT: SKIPPED - ICM firmware is in SAFE mode");
            return;
        }
        if (mode == 3) {
            int errbit = 0;
            int r = icm_mailbox(bar, NHI_MAILBOX_ALLOW_ALL_DEVS, 0, &errbit);
            line[0] = 0;
            a_cat(line, sizeof(line), "ICMCLAIM mailbox ALLOW_ALL_DEVS: ");
            a_cat(line, sizeof(line), r == 0 ? (errbit ? "done, ERROR bit set" : "done") : "TIMEOUT");
            log_line(line);
            log_flush();
        }
    }

    /* ---- DMA pages: [0]=TX desc, [0x100]=RX desc, [0x200]=TX buf,
     *      page 2 = RX buffers (9 x 256) ---- */
    {
        EFI_PHYSICAL_ADDRESS pages = 0;
        EFI_STATUS st;
        volatile UINT32 *txd, *rxd;
        UINT8 *txbuf, *rxbuf;
        UINT32 save_tx[6], save_rx[6];
        int attempt, claimed = 0;
        UINT32 seen_rx = 0;

        st = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages,
                               EfiBootServicesData, 2, &pages);
        if (EFI_ERROR(st) || pages == 0) {
            log_line("ICMCLAIM RESULT: SKIPPED - AllocatePages failed for ring buffers");
            return;
        }
        SetMem((VOID *)(UINTN)pages, 2 * 4096, 0);
        txd = (volatile UINT32 *)(UINTN)(pages + 0x000);
        rxd = (volatile UINT32 *)(UINTN)(pages + 0x100);
        txbuf = (UINT8 *)(UINTN)(pages + 0x200);
        rxbuf = (UINT8 *)(UINTN)(pages + 0x1000);

        icm_build_driver_ready(txbuf);

        /* Prepare descriptors (doorbells rung later). */
        for (i = 0; i < ICM_TX_ATTEMPTS; i++) {
            txd[i * 4 + 0] = (UINT32)(pages + 0x200);
            txd[i * 4 + 1] = (UINT32)((pages + 0x200) >> 32);
            txd[i * 4 + 2] = 0; /* dword posted at send time */
            txd[i * 4 + 3] = 0;
        }
        for (i = 0; i < ICM_RX_POSTED; i++) {
            UINT64 bp = pages + 0x1000 + (UINT64)i * 256;
            rxd[i * 4 + 0] = (UINT32)bp;
            rxd[i * 4 + 1] = (UINT32)(bp >> 32);
            rxd[i * 4 + 2] = icm_desc_dword(256, 0, 0,
                                            ICM_DESC_POSTED | ICM_DESC_INTERRUPT);
            rxd[i * 4 + 3] = 0;
        }

        /* Save EFI's ring-0 state: {base_lo, base_hi, doorbell, count,
         * opts0, opts4} per ring. */
        save_tx[0] = mmio_rd32(bar, NHI_TX_DESC_BASE);
        save_tx[1] = mmio_rd32(bar, NHI_TX_DESC_BASE + 4);
        save_tx[2] = mmio_rd32(bar, NHI_TX_DESC_BASE + 8);
        save_tx[3] = mmio_rd32(bar, NHI_TX_DESC_BASE + 12);
        save_tx[4] = mmio_rd32(bar, NHI_TX_OPTS_BASE);
        save_tx[5] = mmio_rd32(bar, NHI_TX_OPTS_BASE + 4);
        save_rx[0] = mmio_rd32(bar, NHI_RX_DESC_BASE);
        save_rx[1] = mmio_rd32(bar, NHI_RX_DESC_BASE + 4);
        save_rx[2] = mmio_rd32(bar, NHI_RX_DESC_BASE + 8);
        save_rx[3] = mmio_rd32(bar, NHI_RX_DESC_BASE + 12);
        save_rx[4] = mmio_rd32(bar, NHI_RX_OPTS_BASE);
        save_rx[5] = mmio_rd32(bar, NHI_RX_OPTS_BASE + 4);

        con_line("ICM: claiming the Thunderbolt controller (DRIVER_READY)");
        log_line("ICMCLAIM: sending DRIVER_READY on ring 0 (kernel icm_ar_driver_ready port)");
        log_flush();

        /* Program the rings (tb_ring_start order: TX, then RX;
         * options flags written last per ring = the enable).
         * Disable first: a new descriptor base written into a live
         * ring is ignored (v0.6.2). */
        mmio_wr32(bar, NHI_TX_OPTS_BASE, 0);
        mmio_wr32(bar, NHI_RX_OPTS_BASE, 0);
        uefi_call_wrapper(BS->Stall, 1, 2000);
        mmio_wr32(bar, NHI_TX_DESC_BASE, (UINT32)pages);
        mmio_wr32(bar, NHI_TX_DESC_BASE + 4, (UINT32)(pages >> 32));
        mmio_wr32(bar, NHI_TX_DESC_BASE + 8, 0); /* clear the tail index (tb_ring_stop does this) */
        mmio_wr32(bar, NHI_TX_DESC_BASE + 12, ICM_RING_SIZE);
        mmio_wr32(bar, NHI_TX_OPTS_BASE + 4, 0);
        mmio_wr32(bar, NHI_TX_OPTS_BASE, 0xC0000000u);
        mmio_wr32(bar, NHI_RX_DESC_BASE, (UINT32)(pages + 0x100));
        mmio_wr32(bar, NHI_RX_DESC_BASE + 4, (UINT32)((pages + 0x100) >> 32));
        mmio_wr32(bar, NHI_RX_DESC_BASE + 8, 0); /* clear the tail index */
        mmio_wr32(bar, NHI_RX_DESC_BASE + 12, (256u << 16) | ICM_RING_SIZE);
        mmio_wr32(bar, NHI_RX_OPTS_BASE + 4, 0xFFFFFFFFu);
        mmio_wr32(bar, NHI_RX_OPTS_BASE, 0xC0000000u);

        /* Post the RX buffers: doorbell = consumer head. */
        mmio_wr32(bar, NHI_RX_DESC_BASE + 8, ICM_RX_POSTED);

        for (attempt = 0; attempt < ICM_TX_ATTEMPTS && !claimed; attempt++) {
            int waited = 0, txdone_logged = 0;
            /* Post this attempt's TX descriptor, then ring prod. */
            txd[attempt * 4 + 2] = icm_desc_dword(8, ICM_PKG_CMD, ICM_PKG_CMD,
                                                  ICM_DESC_POSTED | ICM_DESC_INTERRUPT);
            mmio_wr32(bar, NHI_TX_DESC_BASE + 8, (UINT32)(attempt + 1) << 16);
            {
                line[0] = 0;
                a_cat(line, sizeof(line), "ICMCLAIM attempt ");
                a_cat_u64dec(line, sizeof(line), attempt + 1);
                a_cat(line, sizeof(line), ": DRIVER_READY posted, waiting up to 5000 ms");
                log_line(line);
                log_flush();
            }
            while (waited < ICM_RESP_TIMEOUT_MS && !claimed) {
                int j;
                if (!txdone_logged &&
                    (icm_desc_flags(txd[attempt * 4 + 2]) & ICM_DESC_COMPLETED)) {
                    txdone_logged = 1;
                    log_line("ICMCLAIM: TX descriptor completed (NHI took the frame)");
                    log_flush();
                }
                for (j = 0; j < ICM_RX_POSTED; j++) {
                    UINT32 dw;
                    if (seen_rx & (1u << j)) continue;
                    dw = rxd[j * 4 + 2];
                    if (!(icm_desc_flags(dw) & ICM_DESC_COMPLETED)) continue;
                    seen_rx |= (1u << j);
                    {
                        UINT32 flen = icm_desc_length(dw);
                        UINT32 feof = icm_desc_eof(dw);
                        UINT8 *fp = rxbuf + j * 256;
                        line[0] = 0;
                        a_cat(line, sizeof(line), "ICMCLAIM RX frame: eof=");
                        a_cat_u64dec(line, sizeof(line), feof);
                        a_cat(line, sizeof(line), " len=");
                        a_cat_u64dec(line, sizeof(line), flen);
                        a_cat(line, sizeof(line), " bytes0=0x");
                        a_cat_u64hex(line, sizeof(line),
                                     (UINT32)fp[0] | ((UINT32)fp[1] << 8) |
                                     ((UINT32)fp[2] << 16) | ((UINT32)fp[3] << 24), 8);
                        a_cat(line, sizeof(line), " bytes1=0x");
                        a_cat_u64hex(line, sizeof(line),
                                     (UINT32)fp[4] | ((UINT32)fp[5] << 8) |
                                     ((UINT32)fp[6] << 16) | ((UINT32)fp[7] << 24), 8);
                        log_line(line);
                        log_flush();
                        if (feof == ICM_PKG_RESP && flen >= 8) {
                            UINT8 code = 0, romver = 0, ramver = 0;
                            UINT16 info = 0;
                            icm_parse_ready_response(fp, flen, &code,
                                                     &romver, &ramver, &info);
                            if (code == ICM_CODE_DRIVER_READY) {
                                claimed = 1;
                                line[0] = 0;
                                a_cat(line, sizeof(line), "ICMCLAIM RESULT: CLAIMED - DRIVER_READY response: romver=0x");
                                a_cat_u64hex(line, sizeof(line), romver, 2);
                                a_cat(line, sizeof(line), " ramver=0x");
                                a_cat_u64hex(line, sizeof(line), ramver, 2);
                                a_cat(line, sizeof(line), " info=0x");
                                a_cat_u64hex(line, sizeof(line), info, 4);
                                a_cat(line, sizeof(line), " security_level=");
                                a_cat_u64dec(line, sizeof(line), info & 0xFu);
                                log_line(line);
                                log_flush();
                            }
                        }
                    }
                }
                if (claimed) break;
                uefi_call_wrapper(BS->Stall, 1, 5000);
                waited += 5;
            }
            if (!claimed) {
                line[0] = 0;
                a_cat(line, sizeof(line), "ICMCLAIM attempt ");
                a_cat_u64dec(line, sizeof(line), attempt + 1);
                a_cat(line, sizeof(line), ": no DRIVER_READY response within 5000 ms");
                log_line(line);
                log_flush();
            }
        }
        if (!claimed)
            log_line("ICMCLAIM RESULT: NO_RESPONSE - 3 attempts, ICM did not answer DRIVER_READY");

        /* Teardown (tb_ring_stop), then restore EFI's ring-0 state. */
        mmio_wr32(bar, NHI_TX_OPTS_BASE, 0);
        mmio_wr32(bar, NHI_TX_DESC_BASE, 0);
        mmio_wr32(bar, NHI_TX_DESC_BASE + 4, 0);
        mmio_wr32(bar, NHI_TX_DESC_BASE + 8, 0);
        mmio_wr32(bar, NHI_TX_DESC_BASE + 12, 0);
        mmio_wr32(bar, NHI_RX_OPTS_BASE, 0);
        mmio_wr32(bar, NHI_RX_DESC_BASE, 0);
        mmio_wr32(bar, NHI_RX_DESC_BASE + 4, 0);
        mmio_wr32(bar, NHI_RX_DESC_BASE + 8, 0);
        mmio_wr32(bar, NHI_RX_DESC_BASE + 12, 0);
        mmio_wr32(bar, NHI_TX_DESC_BASE, save_tx[0]);
        mmio_wr32(bar, NHI_TX_DESC_BASE + 4, save_tx[1]);
        mmio_wr32(bar, NHI_TX_DESC_BASE + 12, save_tx[3]);
        mmio_wr32(bar, NHI_TX_DESC_BASE + 8, save_tx[2]);
        mmio_wr32(bar, NHI_TX_OPTS_BASE + 4, save_tx[5]);
        mmio_wr32(bar, NHI_TX_OPTS_BASE, save_tx[4]);
        mmio_wr32(bar, NHI_RX_DESC_BASE, save_rx[0]);
        mmio_wr32(bar, NHI_RX_DESC_BASE + 4, save_rx[1]);
        mmio_wr32(bar, NHI_RX_DESC_BASE + 12, save_rx[3]);
        mmio_wr32(bar, NHI_RX_DESC_BASE + 8, save_rx[2]);
        mmio_wr32(bar, NHI_RX_OPTS_BASE + 4, save_rx[5]);
        mmio_wr32(bar, NHI_RX_OPTS_BASE, save_rx[4]);
        log_line("ICMCLAIM: ring 0 torn down and EFI state restored");
        log_flush();

        uefi_call_wrapper(BS->FreePages, 2, pages, 2);
    }
}

/* ------------------------------------------------------------------ */
/* SNAPSHOT (v0.6.0): read-only capture of the Thunderbolt routers'   */
/* config spaces, ported from the kernel's tb_cfg_read_raw (ctl.c).  */
/*                                                                    */
/* The v0.5.0 recon proved Apple's EFI Thunderbolt stack is a native  */
/* connection manager: ring 0 carries Apple's own channel and the    */
/* tunnel programming lives in the host and enclosure routers'       */
/* config spaces. This phase snapshots those spaces exactly as EFI   */
/* leaves them, so the freeze/replay question can be settled at the  */
/* desk against the captured bytes: SWITCH + PORT + HOPS spaces of   */
/* the host router (route 0) and of any router found behind its      */
/* TB_TYPE_PORT ports (the enclosure).                               */
/*                                                                    */
/* Discipline (mirrors the ICM phase): ring-0 registers are saved     */
/* first and restored after a tb_ring_stop-style teardown; the        */
/* exchanges run at TPL_NOTIFY so Apple's own TB driver callbacks     */
/* cannot interleave with the takeover; ALL output is buffered in     */
/* memory during the window and only written to the log (plus         */
/* LOGS\SNAPSHOT.BIN, a binary copy of the same blocks) after the     */
/* channel is restored and TPL lowered. The phase transmits config    */
/* READ packets only - there is no config-write path in this build.   */
/* Every wait is bounded; any failure logs and falls through to the   */
/* writer.                                                            */
/*                                                                    */
/* SNAPSHOT.BIN format: header { char magic[8]="EGPUSNAP",           */
/* u32 version=1, u32 nblocks, u32 total_dwords, u32 reserved },      */
/* then per block { u64 route, u32 port, u32 space, u32 offset=0,     */
/* u32 count, u32 status, u32 data[count] }. Status: 0 = read to the  */
/* requested length, 1 = ended early (INVALID_CONFIG_SPACE: the      */
/* space's implemented end), 2 = other TB error, 3 = timeout.         */
/* ------------------------------------------------------------------ */

#define SNAP_RING_SIZE    16
#define SNAP_CHUNK        32
#define SNAP_MAX_SPACE_DW 256
#define SNAP_MAX_HOPS_DW  512
#define SNAP_TIMEOUT_MS   2000
#define SNAP_ATTEMPTS     2
#define SNAP_TEXT_PAGES   40    /* 160 KB buffered log text */
#define SNAP_BIN_PAGES    32    /* 128 KB binary stream */

static UINT64  sBar;
/* Borrowed-ring state (v0.7.0): Apple's ring 0, used as found. */
static UINT64  sTxDescBase, sRxDescBase; /* Apple's descriptor arrays */
static UINT32  sTxCount, sRxCount;       /* descriptor counts */
static UINT32  sTxProd;        /* running mirror of the TX prod index */
static UINT32  sRxNext;        /* next RX slot index to inspect (absolute) */
static UINT32  sSeqCtr;        /* global config-packet sequence counter */
static UINT32  sExch, sStray, sBadCrc, sLastTxDone;

static char   *gSnapText;
static UINTN   gSnapTextLen, gSnapTextCap;
static int     gSnapTextTrunc;
static UINT8  *gSnapBin;
static UINTN   gSnapBinLen, gSnapBinCap;
static UINT32  gSnapBlocks, gSnapDwords;
static UINT32  sScratch[SNAP_MAX_HOPS_DW];

static void snap_cat(const char *s)
{
    UINTN i = 0;
    if (!gSnapText) return;
    while (s[i]) {
        if (gSnapTextLen + 1 >= gSnapTextCap) { gSnapTextTrunc = 1; return; }
        gSnapText[gSnapTextLen++] = s[i++];
    }
    gSnapText[gSnapTextLen] = 0;
}

static void snap_line(const char *s)
{
    snap_cat(s);
    snap_cat("\n");
}

static void snap_bin_block(UINT64 route, UINT32 port, UINT32 space,
                           UINT32 count, UINT32 status, const UINT32 *data)
{
    /* Block layout: route(8) port(4) space(4) offset(4) count(4)
     * status(4) = 28 bytes, then count data dwords. */
    UINTN need = 28 + (UINTN)count * 4, i;
    UINT8 *p;
    if (!gSnapBin || gSnapBinLen + need > gSnapBinCap) return;
    p = gSnapBin + gSnapBinLen;
    SetMem(p, need, 0);
    CopyMem(p, &route, 8);
    *(UINT32 *)(p + 8) = port;
    *(UINT32 *)(p + 12) = space;
    *(UINT32 *)(p + 16) = 0; /* offset: blocks always start at 0 */
    *(UINT32 *)(p + 20) = count;
    *(UINT32 *)(p + 24) = status;
    for (i = 0; i < count; i++) *(UINT32 *)(p + 28 + i * 4) = data[i];
    gSnapBinLen += 28 + (UINTN)count * 4;
    gSnapBlocks++;
    gSnapDwords += count;
    *(UINT32 *)(gSnapBin + 8) = 1;            /* version */
    *(UINT32 *)(gSnapBin + 12) = gSnapBlocks;
    *(UINT32 *)(gSnapBin + 16) = gSnapDwords;
}

static const char *snap_space_name(UINT32 space)
{
    switch (space) {
    case CFG_SPACE_HOPS:   return "HOPS";
    case CFG_SPACE_PORT:   return "PORT";
    case CFG_SPACE_SWITCH: return "SWITCH";
    default:               return "?";
    }
}

static const char *snap_port_type_name(UINT32 type)
{
    switch (type) {
    case 0x000000: return "INACTIVE";
    case 0x000001: return "PORT";
    case 0x000002: return "NHI";
    case 0x0E0101: return "DP_IN";
    case 0x0E0102: return "DP_OUT";
    case 0x100101: return "PCIE_DOWN";
    case 0x100102: return "PCIE_UP";
    case 0x200101: return "USB3_DOWN";
    case 0x200102: return "USB3_UP";
    default:       return "?";
    }
}

/* One config READ exchange on Apple's borrowed ring 0 (v0.7.0).
 * The frame is written into the TX slot's own Apple-owned buffer
 * and the descriptor's phys pointer is left exactly as Apple set
 * it; only the length/eof/sof/flags dword is replaced. Replies are
 * spotted by watching Apple's RX ring for new completions at the
 * hardware producer index; Apple's consumer accounting (the RX
 * doorbell) is never touched.
 * Returns 1 = data (data_out filled), 0 = TB error (*tb_err set),
 * -1 = timeout. Non-matching frames are counted as strays. */
static int snap_exchange(UINT64 route, UINT32 port, UINT32 space,
                         UINT32 offset, UINT32 len, UINT32 seq,
                         UINT32 *data_out, UINT32 *tb_err)
{
    UINT32 payload[3];
    UINT8 frame[256];
    size_t wlen;
    UINT32 di;
    volatile UINT32 *txd;
    UINT64 bufphys;
    int waited = 0;

    seq = (sSeqCtr++ & 3u); /* global sequence, like the kernel's ctl */
    cfg_build_read(payload, route, port, space, offset, len, seq);
    wlen = cfg_frame_wire(payload, 3, frame);

    di = sTxProd % sTxCount;
    txd = (volatile UINT32 *)(UINTN)(sTxDescBase + (UINT64)di * 16);
    bufphys = (UINT64)txd[0] | ((UINT64)txd[1] << 32);
    if (bufphys == 0 || bufphys >= 0x100000000ull) return -1;
    CopyMem((VOID *)(UINTN)bufphys, frame, wlen);
    txd[3] = 0;
    txd[2] = icm_desc_dword((UINT32)wlen, CFG_PKG_READ,
                            CFG_PKG_READ,
                            ICM_DESC_POSTED | ICM_DESC_INTERRUPT);
    (VOID)mmio_rd32(sBar, NHI_REG_CAPS); /* flush the writes before the bell */
    sTxProd++;
    sExch++;
    mmio_wr32(sBar, NHI_TX_DESC_BASE + 8, sTxProd << 16);

    while (waited < SNAP_TIMEOUT_MS) {
        for (;;) {
            UINT32 idx = sRxNext % sRxCount;
            volatile UINT32 *rxd =
                (volatile UINT32 *)(UINTN)(sRxDescBase + (UINT64)idx * 16);
            UINT32 dw = rxd[2];
            UINT32 flen, feof;
            UINT64 rphys;
            UINT32 pdw[3 + 64];
            int n, r;
            if (!(icm_desc_flags(dw) & ICM_DESC_COMPLETED)) break;
            flen = icm_desc_length(dw);
            feof = icm_desc_eof(dw);
            rphys = (UINT64)rxd[0] | ((UINT64)rxd[1] << 32);
            sRxNext++;
            /* Consume the frame: advance the RX doorbell's consumer
             * half so the NHI can reuse the slot (v0.7.2). */
            mmio_wr32(sBar, NHI_RX_DESC_BASE + 8, sRxNext);
            if (rphys == 0 || rphys >= 0x100000000ull || flen > 256) {
                sBadCrc++;
                continue;
            }
            n = cfg_unframe_wire((UINT8 *)(UINTN)rphys, flen, pdw, 3 + 64);
            if (n < 0) { sBadCrc++; continue; }
            r = cfg_parse_read_response(pdw, (size_t)n, feof, route,
                                        offset, space, len, seq,
                                        data_out, tb_err);
            if (r == 1 || r == 0) {
                sLastTxDone =
                    (icm_desc_flags(txd[2]) & ICM_DESC_COMPLETED) ? 1 : 0;
                return r;
            }
            sStray++;
        }
        uefi_call_wrapper(BS->Stall, 1, 1000);
        waited += 1;
    }
    sLastTxDone =
        (icm_desc_flags(txd[2]) & ICM_DESC_COMPLETED) ? 1 : 0;
    return -1;
}

/* Capture up to max_dw dwords of one config space. Returns the dword
 * count captured; first8 (if non-NULL) receives the first <=8. */
static UINT32 snap_read_space(UINT64 route, UINT32 port, UINT32 space,
                              UINT32 max_dw, UINT32 *first8)
{
    char line[768];
    UINT32 got = 0, status = 0, err_code = 0;

    line[0] = 0;
    a_cat(line, sizeof(line), "SNAP route=0x");
    a_cat_u64hex(line, sizeof(line), route, 16);
    a_cat(line, sizeof(line), " port=");
    a_cat_u64dec(line, sizeof(line), port);
    a_cat(line, sizeof(line), " space=");
    a_cat(line, sizeof(line), snap_space_name(space));
    a_cat(line, sizeof(line), " max=");
    a_cat_u64dec(line, sizeof(line), max_dw);
    snap_line(line);

    while (got < max_dw) {
        /* Kernel-sized reads (v0.7.1): the responder refuses wide
         * requests with INVALID_CONFIG_SPACE. */
        UINT32 step = (space == CFG_SPACE_HOPS) ? 2 : 8;
        UINT32 chunk = max_dw - got;
        UINT32 data[SNAP_CHUNK];
        UINT32 tb_err = 0;
        int r = -1, attempt;
        if (chunk > step) chunk = step;
        for (attempt = 0; attempt < SNAP_ATTEMPTS; attempt++) {
            r = snap_exchange(route, port, space, got, chunk,
                              (UINT32)attempt, data, &tb_err);
            if (r != -1) break;
        }
        if (r == 1) {
            UINT32 i, base;
            for (i = 0; i < chunk; i++) sScratch[got + i] = data[i];
            for (base = 0; base < chunk; base += 8) {
                line[0] = 0;
                a_cat(line, sizeof(line), "  +0x");
                a_cat_u64hex(line, sizeof(line), got + base, 4);
                a_cat(line, sizeof(line), ":");
                for (i = 0; i < 8 && base + i < chunk; i++) {
                    a_cat(line, sizeof(line), " ");
                    a_cat_u64hex(line, sizeof(line), data[base + i], 8);
                }
                snap_line(line);
            }
            got += chunk;
            continue;
        }
        if (r == 0) {
            err_code = tb_err;
            status = (tb_err == CFG_ERR_INVALID_CONFIG_SPACE) ? 1 : 2;
        } else {
            status = 3;
        }
        break;
    }

    snap_bin_block(route, port, space, got, status, sScratch);
    if (first8) {
        UINT32 i;
        for (i = 0; i < 8; i++) first8[i] = (i < got) ? sScratch[i] : 0;
    }
    line[0] = 0;
    a_cat(line, sizeof(line), "SNAP-END route=0x");
    a_cat_u64hex(line, sizeof(line), route, 16);
    a_cat(line, sizeof(line), " port=");
    a_cat_u64dec(line, sizeof(line), port);
    a_cat(line, sizeof(line), " space=");
    a_cat(line, sizeof(line), snap_space_name(space));
    a_cat(line, sizeof(line), " got=");
    a_cat_u64dec(line, sizeof(line), got);
    a_cat(line, sizeof(line), " status=");
    a_cat(line, sizeof(line), status == 0 ? "OK" :
          status == 1 ? "END_OF_SPACE" :
          status == 2 ? "TB_ERROR" : "TIMEOUT");
    if (status == 2) {
        a_cat(line, sizeof(line), " err=");
        a_cat_u64dec(line, sizeof(line), err_code);
    }
    if (status == 3) {
        a_cat(line, sizeof(line), " tx_completed=");
        a_cat_u64dec(line, sizeof(line), sLastTxDone);
        a_cat(line, sizeof(line), " tx_doorbell=0x");
        a_cat_u64hex(line, sizeof(line), mmio_rd32(sBar, NHI_TX_DESC_BASE + 8), 8);
        a_cat(line, sizeof(line), " rx_doorbell=0x");
        a_cat_u64hex(line, sizeof(line), mmio_rd32(sBar, NHI_RX_DESC_BASE + 8), 8);
    }
    snap_line(line);
    return got;
}

/* Walk one router: switch space, then every port's PORT + HOPS
 * spaces. For the host router (route 0), TB_TYPE_PORT port numbers
 * are collected into cand[] (candidate routes to child routers). */
static void snap_router(UINT64 route, UINT32 *cand, UINT32 *cand_count,
                        UINT32 hdr_n)
{
    char line[768];
    UINT32 hdr[8];
    UINT32 n, p, max_port;

    if (hdr_n >= 5) {
        /* Switch space already read into sScratch by the caller. */
        UINT32 i;
        n = hdr_n;
        for (i = 0; i < 8; i++) hdr[i] = sScratch[i];
    } else {
        n = snap_read_space(route, 0, CFG_SPACE_SWITCH, SNAP_MAX_SPACE_DW, hdr);
    }
    if (n < 5) {
        snap_line("SNAP: no switch header at this route - walk stopped");
        return;
    }
    max_port = (hdr[1] >> 14) & 0x3Fu;
    line[0] = 0;
    a_cat(line, sizeof(line), "SNAP ROUTER route=0x");
    a_cat_u64hex(line, sizeof(line), route, 16);
    a_cat(line, sizeof(line), " id=");
    a_cat_u64hex(line, sizeof(line), hdr[0] & 0xFFFFu, 4);
    a_cat(line, sizeof(line), ":");
    a_cat_u64hex(line, sizeof(line), hdr[0] >> 16, 4);
    a_cat(line, sizeof(line), " rev=");
    a_cat_u64dec(line, sizeof(line), (hdr[1] >> 24) & 0xFFu);
    a_cat(line, sizeof(line), " max_port=");
    a_cat_u64dec(line, sizeof(line), max_port);
    a_cat(line, sizeof(line), " upstream_port=");
    a_cat_u64dec(line, sizeof(line), (hdr[1] >> 8) & 0x3Fu);
    a_cat(line, sizeof(line), " depth=");
    a_cat_u64dec(line, sizeof(line), (hdr[1] >> 20) & 0x7u);
    a_cat(line, sizeof(line), " enabled=");
    a_cat_u64dec(line, sizeof(line), (hdr[3] >> 31) & 1u);
    snap_line(line);
    if ((hdr[0] & 0xFFFFu) != 0x8086u) {
        snap_line("SNAP: vendor is not Intel - not a Thunderbolt router, walk stopped");
        return;
    }

    for (p = 0; p <= max_port; p++) {
        UINT32 ph[8];
        UINT32 pn = snap_read_space(route, p, CFG_SPACE_PORT,
                                    SNAP_MAX_SPACE_DW, ph);
        if (pn >= 6) {
            UINT32 type = ph[2] & 0xFFFFFFu;
            UINT32 max_in = ph[5] & 0x7FFu;
            UINT32 max_out = (ph[5] >> 11) & 0x7FFu;
            UINT32 hops = (max_in + max_out + 2) * 2;
            if (hops > SNAP_MAX_HOPS_DW) hops = SNAP_MAX_HOPS_DW;
            line[0] = 0;
            a_cat(line, sizeof(line), "SNAP PORT route=0x");
            a_cat_u64hex(line, sizeof(line), route, 16);
            a_cat(line, sizeof(line), " port=");
            a_cat_u64dec(line, sizeof(line), p);
            a_cat(line, sizeof(line), " type=0x");
            a_cat_u64hex(line, sizeof(line), type, 6);
            a_cat(line, sizeof(line), " (");
            a_cat(line, sizeof(line), snap_port_type_name(type));
            a_cat(line, sizeof(line), ") max_in_hop=");
            a_cat_u64dec(line, sizeof(line), max_in);
            a_cat(line, sizeof(line), " max_out_hop=");
            a_cat_u64dec(line, sizeof(line), max_out);
            snap_line(line);
            if (hops > 0)
                snap_read_space(route, p, CFG_SPACE_HOPS, hops, NULL);
            if (route == 0 && type == 0x000001u && cand && cand_count &&
                *cand_count < 4)
                cand[(*cand_count)++] = p;
        }
    }
}

static void do_snapshot(void)
{
    char line[768];
    int nhi = -1, i;
    UINT64 bar = 0;
    UINT16 cmdreg;
    EFI_PHYSICAL_ADDRESS textp = 0, binp = 0;
    EFI_STATUS st;
    UINTN old_tpl = 0;
    UINT32 cand[4], cand_count = 0;
    UINT32 host_max_port = 0;
    UINT32 host_hdr_ok = 0;
    int borrow_ok = 0;
    const char *skip_reason = NULL;

    log_section("SNAPSHOT - Thunderbolt router config capture (read-only)");
    if (!gSnapshot) {
        log_line("SNAPSHOT: skipped (SNAPSHOT is not 1 in shim.cfg)");
        return;
    }
    for (i = 0; i < gDevCount; i++) {
        if (gDevs[i].vid == 0x8086 && gDevs[i].did == 0x15EB) { nhi = i; break; }
    }
    if (nhi < 0) {
        log_line("SNAPSHOT RESULT: SKIPPED - NHI (8086:15EB) not found in scan");
        return;
    }
    {
        UINT32 b0 = gDevBars[nhi][0];
        if ((b0 & 0x6u) == 0x4u)
            bar = (UINT64)(b0 & 0xFFFFFFF0u) | ((UINT64)gDevBars[nhi][1] << 32);
        else
            bar = (UINT64)(b0 & 0xFFFFFFF0u);
    }
    cmdreg = pci16(gDevs[nhi].bus, gDevs[nhi].dev, gDevs[nhi].func, 0x04);
    if (bar == 0 || !(cmdreg & 0x2u)) {
        log_line("SNAPSHOT RESULT: SKIPPED - NHI BAR0 unmapped or memory-space disabled");
        return;
    }
    sBar = bar;

    st = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages,
                           EfiBootServicesData, SNAP_TEXT_PAGES, &textp);
    if (EFI_ERROR(st) || textp == 0) {
        log_line("SNAPSHOT RESULT: SKIPPED - AllocatePages failed for text buffer");
        return;
    }
    st = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages,
                           EfiBootServicesData, SNAP_BIN_PAGES, &binp);
    if (EFI_ERROR(st) || binp == 0) {
        log_line("SNAPSHOT RESULT: SKIPPED - AllocatePages failed for binary buffer");
        uefi_call_wrapper(BS->FreePages, 2, textp, SNAP_TEXT_PAGES);
        return;
    }
    SetMem((VOID *)(UINTN)textp, SNAP_TEXT_PAGES * 4096, 0);
    SetMem((VOID *)(UINTN)binp, SNAP_BIN_PAGES * 4096, 0);
    gSnapText = (char *)(UINTN)textp;
    gSnapTextLen = 0;
    gSnapTextCap = SNAP_TEXT_PAGES * 4096;
    gSnapTextTrunc = 0;
    gSnapBin = (UINT8 *)(UINTN)binp;
    gSnapBinLen = 24; /* stream header, patched by snap_bin_block */
    gSnapBinCap = SNAP_BIN_PAGES * 4096;
    gSnapBlocks = 0;
    gSnapDwords = 0;
    CopyMem(gSnapBin, "EGPUSNAP", 8);
    sTxProd = 0;
    sRxNext = 0;
    sExch = 0;
    sStray = 0;
    sBadCrc = 0;
    sLastTxDone = 0;

    snap_line("SNAPSHOT capture: config READ packets only, on Apple's borrowed ring 0 (v0.7.0)");
    line[0] = 0;
    a_cat(line, sizeof(line), "SNAPSHOT NHI BAR0=0x");
    a_cat_u64hex(line, sizeof(line), bar, 16);
    snap_line(line);

    /* Borrow setup (v0.7.0): read Apple's ring-0 state as found.
     * Nothing is reprogrammed; the walk below only posts into the
     * idle TX ring and watches the RX ring. */
    {
        UINT32 txdb, rxdb, txcnt, rxcnt, txprod, txtail, rxprod, rxcons;
        UINT32 k;
        sTxDescBase = (UINT64)mmio_rd32(bar, NHI_TX_DESC_BASE) |
                      ((UINT64)mmio_rd32(bar, NHI_TX_DESC_BASE + 4) << 32);
        sRxDescBase = (UINT64)mmio_rd32(bar, NHI_RX_DESC_BASE) |
                      ((UINT64)mmio_rd32(bar, NHI_RX_DESC_BASE + 4) << 32);
        txdb = mmio_rd32(bar, NHI_TX_DESC_BASE + 8);
        rxdb = mmio_rd32(bar, NHI_RX_DESC_BASE + 8);
        txcnt = mmio_rd32(bar, NHI_TX_DESC_BASE + 12) & 0xFFFFu;
        rxcnt = mmio_rd32(bar, NHI_RX_DESC_BASE + 12) & 0xFFFFu;
        txprod = txdb >> 16; txtail = txdb & 0xFFFFu;
        rxprod = rxdb >> 16; rxcons = rxdb & 0xFFFFu;
        sTxCount = txcnt;
        sRxCount = rxcnt;
        sTxProd = txprod;
        sRxNext = rxprod; /* inspect from the hardware fill index (v0.7.3) */
        sSeqCtr = 0;
        line[0] = 0;
        a_cat(line, sizeof(line), "SNAP ring0 borrowed: TX base=0x");
        a_cat_u64hex(line, sizeof(line), sTxDescBase, 16);
        a_cat(line, sizeof(line), " prod=");
        a_cat_u64dec(line, sizeof(line), txprod);
        a_cat(line, sizeof(line), " tail=");
        a_cat_u64dec(line, sizeof(line), txtail);
        a_cat(line, sizeof(line), " count=");
        a_cat_u64dec(line, sizeof(line), txcnt);
        a_cat(line, sizeof(line), " RX base=0x");
        a_cat_u64hex(line, sizeof(line), sRxDescBase, 16);
        a_cat(line, sizeof(line), " prod=");
        a_cat_u64dec(line, sizeof(line), rxprod);
        a_cat(line, sizeof(line), " cons=");
        a_cat_u64dec(line, sizeof(line), rxcons);
        a_cat(line, sizeof(line), " count=");
        a_cat_u64dec(line, sizeof(line), rxcnt);
        snap_line(line);
        if (sTxDescBase == 0 || sRxDescBase == 0 ||
            sTxDescBase >= 0x100000000ull || sRxDescBase >= 0x100000000ull ||
            txcnt < 1 || txcnt > 64 || rxcnt < 1 || rxcnt > 64) {
            skip_reason = "Apple ring geometry implausible";
        } else if (txprod != txtail) {
            skip_reason = "Apple TX ring not idle";
        } else {
            borrow_ok = 1;
            for (k = 0; k < txcnt && k < 16; k++) {
                volatile UINT32 *d = (volatile UINT32 *)(UINTN)(
                    sTxDescBase + (UINT64)k * 16);
                line[0] = 0;
                a_cat(line, sizeof(line), "SNAP APPLE-TX slot ");
                a_cat_u64dec(line, sizeof(line), k);
                a_cat(line, sizeof(line), " phys=0x");
                a_cat_u64hex(line, sizeof(line),
                             (UINT64)d[0] | ((UINT64)d[1] << 32), 16);
                a_cat(line, sizeof(line), " dw2=0x");
                a_cat_u64hex(line, sizeof(line), d[2], 8);
                snap_line(line);
            }
            for (k = 0; k < rxcnt && k < 16; k++) {
                volatile UINT32 *d = (volatile UINT32 *)(UINTN)(
                    sRxDescBase + (UINT64)k * 16);
                line[0] = 0;
                a_cat(line, sizeof(line), "SNAP APPLE-RX slot ");
                a_cat_u64dec(line, sizeof(line), k);
                a_cat(line, sizeof(line), " phys=0x");
                a_cat_u64hex(line, sizeof(line),
                             (UINT64)d[0] | ((UINT64)d[1] << 32), 16);
                a_cat(line, sizeof(line), " dw2=0x");
                a_cat_u64hex(line, sizeof(line), d[2], 8);
                snap_line(line);
            }
        }
        if (!borrow_ok && skip_reason) {
            line[0] = 0;
            a_cat(line, sizeof(line), "SNAPSHOT: borrow refused - ");
            a_cat(line, sizeof(line), skip_reason);
            snap_line(line);
        }
    }

    con_line("SNAPSHOT: capturing Thunderbolt router config (read-only)");

    if (borrow_ok) {
    /* Walk under raised TPL so Apple's driver callbacks cannot
     * interleave with the borrowed posts. */
    old_tpl = uefi_call_wrapper(BS->RaiseTPL, 1, TPL_NOTIFY);

    {
        UINT32 hdr8[8];
        UINT32 n0 = snap_read_space(0, 0, CFG_SPACE_SWITCH,
                                    SNAP_MAX_SPACE_DW, hdr8);
        if (n0 >= 5 && (hdr8[0] & 0xFFFFu) == 0x8086u) {
            host_hdr_ok = 1;
            host_max_port = (hdr8[1] >> 14) & 0x3Fu;
            /* Walk from the header just read (sScratch holds the
             * switch space) - no duplicate re-read (v0.7.2). */
            snap_router(0, cand, &cand_count, n0);
        } else {
            snap_line("SNAPSHOT: host router switch-space header read failed (see SNAP-END above)");
        }
    }
    /* Probe EVERY host port as a route (v0.7.4): the port headers
     * are dark in EFI, but the config channel may still forward
     * over a live link to the enclosure's router. */
    for (i = 1; (UINT32)i <= host_max_port; i++) {
        UINT32 data[5];
        UINT32 tb_err = 0;
        int r = -1, attempt;
        for (attempt = 0; attempt < SNAP_ATTEMPTS; attempt++) {
            r = snap_exchange((UINT64)i, 0, CFG_SPACE_SWITCH, 0, 5,
                              (UINT32)attempt, data, &tb_err);
            if (r != -1) break;
        }
        line[0] = 0;
        a_cat(line, sizeof(line), "SNAP PROBE host port ");
        a_cat_u64dec(line, sizeof(line), (UINT64)i);
        if (r == 1 && (data[0] & 0xFFFFu) == 0x8086u) {
            a_cat(line, sizeof(line), ": router found (id ");
            a_cat_u64hex(line, sizeof(line), data[0] & 0xFFFFu, 4);
            a_cat(line, sizeof(line), ":");
            a_cat_u64hex(line, sizeof(line), data[0] >> 16, 4);
            a_cat(line, sizeof(line), ") - walking it");
            snap_line(line);
            snap_router((UINT64)i, NULL, NULL, 0);
        } else {
            a_cat(line, sizeof(line), ": no router (result=");
            a_cat_u64dec(line, sizeof(line), (UINT64)(r + 1));
            a_cat(line, sizeof(line), " tb_err=");
            a_cat_u64dec(line, sizeof(line), tb_err);
            a_cat(line, sizeof(line), ")");
            snap_line(line);
        }
    }
    } /* if (borrow_ok) */
    line[0] = 0;
    a_cat(line, sizeof(line), "SNAPSHOT CAPTURE DONE exchanges=");
    a_cat_u64dec(line, sizeof(line), sExch);
    a_cat(line, sizeof(line), " strays=");
    a_cat_u64dec(line, sizeof(line), sStray);
    a_cat(line, sizeof(line), " bad_crc=");
    a_cat_u64dec(line, sizeof(line), sBadCrc);
    a_cat(line, sizeof(line), " blocks=");
    a_cat_u64dec(line, sizeof(line), gSnapBlocks);
    a_cat(line, sizeof(line), " dwords=");
    a_cat_u64dec(line, sizeof(line), gSnapDwords);
    if (gSnapTextTrunc) a_cat(line, sizeof(line), " TEXT_TRUNCATED");
    snap_line(line);

    /* Borrow release: no ring register was ever reprogrammed, so
     * there is nothing to restore - just drop the TPL. */
    if (borrow_ok) uefi_call_wrapper(BS->RestoreTPL, 1, old_tpl);

    /* Emit the buffered capture to the log, then SNAPSHOT.BIN. */
    {
        UINTN pos = 0;
        while (pos < gSnapTextLen) {
            UINTN start = pos;
            while (pos < gSnapTextLen && gSnapText[pos] != '\n') pos++;
            gSnapText[pos] = 0; /* buffer has slack: cap is pages*4096 */
            log_raw(gSnapText + start);
            log_raw("\r\n");
            pos++;
        }
    }
    log_line("SNAPSHOT: borrowed ring released - no ring registers were reprogrammed");
    if (gSnapBlocks > 0 && gLogDir) {
        EFI_FILE_HANDLE f = NULL;
        EFI_STATUS fst = uefi_call_wrapper(gLogDir->Open, 5, gLogDir, &f,
                                           L"SNAPSHOT.BIN",
                                           EFI_FILE_MODE_CREATE |
                                           EFI_FILE_MODE_READ |
                                           EFI_FILE_MODE_WRITE, 0);
        if (!EFI_ERROR(fst) && f) {
            UINTN w = gSnapBinLen;
            uefi_call_wrapper(f->Write, 3, f, &w, (VOID *)gSnapBin);
            uefi_call_wrapper(f->Close, 1, f);
            line[0] = 0;
            a_cat(line, sizeof(line), "SNAPSHOT: SNAPSHOT.BIN written (");
            a_cat_u64dec(line, sizeof(line), gSnapBinLen);
            a_cat(line, sizeof(line), " bytes)");
            log_line(line);
        } else {
            log_line("SNAPSHOT: could not create SNAPSHOT.BIN (log text holds the same data)");
        }
    }
    line[0] = 0;
    a_cat(line, sizeof(line), "SNAPSHOT RESULT: ");
    if (skip_reason) {
        a_cat(line, sizeof(line), "SKIPPED - ");
        a_cat(line, sizeof(line), skip_reason);
    } else {
        a_cat(line, sizeof(line), host_hdr_ok ? "CAPTURED" : "FAILED - host router silent");
    }
    a_cat(line, sizeof(line), " blocks=");
    a_cat_u64dec(line, sizeof(line), gSnapBlocks);
    a_cat(line, sizeof(line), " dwords=");
    a_cat_u64dec(line, sizeof(line), gSnapDwords);
    log_line(line);
    log_flush();

    uefi_call_wrapper(BS->FreePages, 2, textp, SNAP_TEXT_PAGES);
    uefi_call_wrapper(BS->FreePages, 2, binp, SNAP_BIN_PAGES);
    gSnapText = NULL;
    gSnapBin = NULL;
}

/* ------------------------------------------------------------------ */
/* TB-PROBE (v0.4.2): the v0.4.1 run proved the placement is stable   */
/* inside EFI (RECHECK intact at +10 s), and the user heard the       */
/* card's fans stop exactly at GRUB's "loading initial ramdisk" -     */
/* the card is reset when ExitBootServices tears down the EFI driver  */
/* stack, Apple's Thunderbolt driver included. This probe takes that  */
/* reset EARLY, while the shim is in charge: it disconnects every     */
/* EFI driver from the Thunderbolt silicon (Alpine Ridge parts        */
/* 1578/15EA/15EB/15EF), then watches the GPU's identity register     */
/* for 20 s, logging every disappearance and return. The writer then */
/* places whatever state the card settles into, and the real          */
/* handover has nothing left to tear down. NO config-space writes     */
/* happen here - DisconnectController is a driver-model call. All     */
/* config access in this app goes through the root bridge (gRB),      */
/* which no disconnect can touch. Gated exactly like the writer:      */
/* armed config + valid plan only; unarmed boots stay purely          */
/* observational, as in every earlier version.                        */
/* ------------------------------------------------------------------ */
static int tb_is_part(UINT16 vid, UINT16 did)
{
    if (vid != 0x8086) return 0;
    return did == 0x1578 || did == 0x15EA || did == 0x15EB || did == 0x15EF;
}

static void do_tb_probe(void)
{
    char line[768];
    char bdf[16];
    EFI_HANDLE *handles = NULL;
    UINTN nh = 0, i;
    EFI_STATUS st;
    UINT8 gb, gd, gf;
    UINT32 idreg;
    int was_present, present, k;

    if (!gWriteEnable || !gPlanFound || gGpuIdx < 0) return;
    gb = gDevs[gGpuIdx].bus; gd = gDevs[gGpuIdx].dev; gf = gDevs[gGpuIdx].func;

    log_section("TB-PROBE - early Thunderbolt teardown, on our turf");
    con_line("TB-PROBE: disconnecting Thunderbolt drivers - fans may cycle, that is the point");
    log_line("TB-PROBE: disconnecting EFI drivers from the Thunderbolt silicon");
    log_flush();

    st = uefi_call_wrapper(gBS->LocateHandleBuffer, 5, ByProtocol,
                           &gEfiPciIoProtocolGuid, NULL, &nh, &handles);
    if (st != EFI_SUCCESS || handles == NULL) {
        line[0] = 0;
        a_cat(line, sizeof(line), "TB-PROBE: LocateHandleBuffer failed, status=0x");
        a_cat_u64hex(line, sizeof(line), (UINT64)st, 16);
        log_line(line);
        log_flush();
    } else {
        for (i = 0; i < nh; i++) {
            EFI_PCI_IO_PROTOCOL *pio = NULL;
            UINTN seg, bus, dev, fn;
            int j, match;
            if (uefi_call_wrapper(gBS->HandleProtocol, 3, handles[i],
                                  &gEfiPciIoProtocolGuid, (VOID **)&pio) != EFI_SUCCESS || pio == NULL)
                continue;
            if (uefi_call_wrapper(pio->GetLocation, 5, pio, &seg, &bus, &dev, &fn) != EFI_SUCCESS)
                continue;
            match = -1;
            for (j = 0; j < gDevCount; j++) {
                if (gDevs[j].bus == (UINT8)bus && gDevs[j].dev == (UINT8)dev &&
                    gDevs[j].func == (UINT8)fn) { match = j; break; }
            }
            if (match < 0 || !tb_is_part(gDevs[match].vid, gDevs[match].did))
                continue;
            fmt_bdf(bdf, sizeof(bdf), (UINT8)bus, (UINT8)dev, (UINT8)fn);
            st = uefi_call_wrapper(gBS->DisconnectController, 3, handles[i], NULL, NULL);
            line[0] = 0;
            a_cat(line, sizeof(line), "TB-PROBE DISCONNECT ");
            a_cat(line, sizeof(line), bdf);
            a_cat(line, sizeof(line), " did=0x");
            a_cat_u64hex(line, sizeof(line), gDevs[match].did, 4);
            a_cat(line, sizeof(line), " status=0x");
            a_cat_u64hex(line, sizeof(line), (UINT64)st, 16);
            log_line(line);
            con_line(line);
            log_flush();
        }
        FreePool(handles);
    }

    /* Watch the GPU's identity register for 20 s, logging transitions. */
    idreg = pci32(gb, gd, gf, 0x00);
    was_present = (idreg == 0x67DF1002);
    line[0] = 0;
    a_cat(line, sizeof(line), "TB-PROBE WATCH start: GPU id=0x");
    a_cat_u64hex(line, sizeof(line), idreg, 8);
    a_cat(line, sizeof(line), was_present ? " (present)" : " (ABSENT)");
    log_line(line);
    log_flush();
    for (k = 1; k <= 40; k++) {
        uefi_call_wrapper(gBS->Stall, 1, 500000);
        idreg = pci32(gb, gd, gf, 0x00);
        present = (idreg == 0x67DF1002);
        if (present != was_present) {
            line[0] = 0;
            a_cat(line, sizeof(line), "TB-PROBE WATCH t+");
            a_cat_u64dec(line, sizeof(line), (UINT64)(k / 2));
            a_cat(line, sizeof(line), ".");
            a_cat_u64dec(line, sizeof(line), (UINT64)((k % 2) * 5));
            a_cat(line, sizeof(line), present ? "s: GPU RETURNED id=0x" : "s: GPU GONE id=0x");
            a_cat_u64hex(line, sizeof(line), idreg, 8);
            log_line(line);
            con_line(line);
            log_flush();
            was_present = present;
        }
    }
    {
        UINT32 bar0lo = pci32(gb, gd, gf, 0x10), bar0hi = pci32(gb, gd, gf, 0x14);
        UINT32 cmd = 0;
        pci_read(gb, gd, gf, 0x04, EfiPciIoWidthUint16, &cmd);
        fmt_bdf(bdf, sizeof(bdf), gb, gd, gf);
        line[0] = 0;
        a_cat(line, sizeof(line), "TB-PROBE RESULT ");
        a_cat(line, sizeof(line), bdf);
        a_cat(line, sizeof(line), was_present ? " present" : " ABSENT");
        a_cat(line, sizeof(line), " BAR0=0x");
        a_cat_u64hex(line, sizeof(line), ((UINT64)bar0hi << 32) | bar0lo, 16);
        a_cat(line, sizeof(line), " CMD=0x");
        a_cat_u64hex(line, sizeof(line), cmd, 4);
        log_line(line);
        con_line(line);
        log_flush();
    }
}

/* ------------------------------------------------------------------ */
/* RECHECK (v0.4.1, READ-ONLY): 10 s after the writer, re-read the    */
/* GPU's BAR0/BAR2/COMMAND and log them. On 2026-11-16 the v0.4       */
/* writer APPLIED entries=25 with clean readbacks, yet the kernel's   */
/* first enumeration read all-zero BARs and the user heard the card's */
/* fans cycle in the handoff. If the placement still reads back here, */
/* it is stable inside EFI and the loss happens at/after chainload.   */
/* If it already reads zero, the card reset on its own after the      */
/* write - listen for the fans during the wait.                       */
/* ------------------------------------------------------------------ */
static void do_recheck(void)
{
    char line[768];
    char bdf[16];
    UINT32 bar0lo = 0, bar0hi = 0, bar2lo = 0, bar2hi = 0, cmd = 0;
    UINT8 b, d, f;

    if (gGpuIdx < 0) return;
    b = gDevs[gGpuIdx].bus; d = gDevs[gGpuIdx].dev; f = gDevs[gGpuIdx].func;
    fmt_bdf(bdf, sizeof(bdf), b, d, f);

    log_section("RECHECK - placement stability 10 s after the writer");
    con_line("RECHECK: waiting 10 s, then re-reading the GPU (listen for the fans)");
    log_line("RECHECK: waiting 10 s before re-reading");
    log_flush();
    uefi_call_wrapper(gBS->Stall, 1, 10000000);

    pci_read(b, d, f, 0x10, EfiPciIoWidthUint32, &bar0lo);
    pci_read(b, d, f, 0x14, EfiPciIoWidthUint32, &bar0hi);
    pci_read(b, d, f, 0x18, EfiPciIoWidthUint32, &bar2lo);
    pci_read(b, d, f, 0x1C, EfiPciIoWidthUint32, &bar2hi);
    pci_read(b, d, f, 0x04, EfiPciIoWidthUint16, &cmd);

    line[0] = 0;
    a_cat(line, sizeof(line), "RECHECK ");
    a_cat(line, sizeof(line), bdf);
    a_cat(line, sizeof(line), " BAR0=0x");
    a_cat_u64hex(line, sizeof(line), ((UINT64)bar0hi << 32) | bar0lo, 16);
    a_cat(line, sizeof(line), " BAR2=0x");
    a_cat_u64hex(line, sizeof(line), ((UINT64)bar2hi << 32) | bar2lo, 16);
    a_cat(line, sizeof(line), " CMD=0x");
    a_cat_u64hex(line, sizeof(line), cmd, 4);
    log_line(line);
    con_line(line);
    log_flush();
}

static void do_write(void)
{
    static plan_entry wl[PLAN_MAX_ENTRIES];
    static UINT32 saved[PLAN_MAX_ENTRIES];
    plan_gate gate;
    int wn, i, failed;
    char line[768];
    char bdf[16];

    log_section("WRITER - FULL PLACEMENT (plan 3a + 3b + GPU COMMAND)");
    con_line("WRITER: gate check (see log)");

    gate = plan_write_gate(gWriteEnable ? 1 : 0, gPlanFound ? 1 : 0);
    if (gate == PLAN_GATE_SKIP_NO_ENABLE) {
        log_line("WRITER: SKIPPED - WRITE_ENABLE is not 1 in shim.cfg (zero writes performed)");
        log_flush();
        return;
    }
    if (gate == PLAN_GATE_SKIP_NO_PLAN) {
        log_line("WRITER: SKIPPED - no valid plan was computed (zero writes performed)");
        log_flush();
        return;
    }
    /* gate == ARMED from here on */
    if (!gRB) {
        log_line("WRITER: SKIPPED - no root bridge protocol handle captured (zero writes performed)");
        log_flush();
        return;
    }
    wn = plan_build_write_list_v4(gManifest, gManifestCount,
                                gDevs[gGpuIdx].bus, gDevs[gGpuIdx].dev,
                                gDevs[gGpuIdx].func, wl, PLAN_MAX_ENTRIES);
    if (wn != WRITER_EXPECTED_ENTRIES) {
        line[0] = 0;
        a_cat(line, sizeof(line), "WRITER: SKIPPED - write list has ");
        a_cat_u64dec(line, sizeof(line), wn < 0 ? 0 : wn);
        a_cat(line, sizeof(line), " entries, expected exactly ");
        a_cat_u64dec(line, sizeof(line), WRITER_EXPECTED_ENTRIES);
        a_cat(line, sizeof(line), " (chain differs from plan - zero writes performed)");
        log_line(line);
        log_flush();
        return;
    }

    log_line("WRITER: ARMED - WRITE_ENABLE=1 and a valid plan exists");
    con_line("WRITER: ARMED - writing full placement: windows + GPU BARs + COMMAND (log has the record)");

    /* ---- Phase SAVE: live re-read of every original, logged and
     * flushed BEFORE the first write (plan section 5). ---- */
    log_line("WRITER PHASE SAVE: recording live values before any write");
    for (i = 0; i < wn; i++) {
        saved[i] = pci_read_entry(&wl[i]);
        line[0] = 0;
        a_cat(line, sizeof(line), "WSAVE ");
        fmt_bdf(bdf, sizeof(bdf), wl[i].bus, wl[i].dev, wl[i].func);
        a_cat(line, sizeof(line), bdf);
        a_cat(line, sizeof(line), " off=0x");
        a_cat_u64hex(line, sizeof(line), wl[i].offset, 2);
        a_cat(line, sizeof(line), " old=0x");
        a_cat_u64hex(line, sizeof(line), saved[i], wl[i].width * 2);
        log_line(line);
    }
    log_flush();  /* originals are durable on disk before any write */

    /* ---- Phase WRITE+VERIFY ---- */
    failed = 0;
    for (i = 0; i < wn; i++) {
        EFI_STATUS st = pci_write_entry(&wl[i], wl[i].newval);
        UINT32 rb = pci_read_entry(&wl[i]);
        int ok = !EFI_ERROR(st) && rb == wl[i].newval;
        line[0] = 0;
        a_cat(line, sizeof(line), "WWRITE ");
        fmt_bdf(bdf, sizeof(bdf), wl[i].bus, wl[i].dev, wl[i].func);
        a_cat(line, sizeof(line), bdf);
        a_cat(line, sizeof(line), " off=0x");
        a_cat_u64hex(line, sizeof(line), wl[i].offset, 2);
        a_cat(line, sizeof(line), " new=0x");
        a_cat_u64hex(line, sizeof(line), wl[i].newval, wl[i].width * 2);
        a_cat(line, sizeof(line), " readback=0x");
        a_cat_u64hex(line, sizeof(line), rb, wl[i].width * 2);
        a_cat(line, sizeof(line), ok ? " OK" : (EFI_ERROR(st) ? " WRITE_ERROR" : " MISMATCH"));
        log_line(line);
        log_flush();
        if (!ok) { failed = 1; break; }
    }
    if (!failed) {
        line[0] = 0;
        a_cat(line, sizeof(line), "WRITER_RESULT: APPLIED entries=");
        a_cat_u64dec(line, sizeof(line), wn);
        log_line(line);
        log_flush();
        con_line("WRITER_RESULT: APPLIED");
        return;
    }

    /* ---- Phase RESTORE: every saved original, reverse order ---- */
    log_line("WRITER: failure during WRITE phase - restoring ALL saved originals (root->leaf)");
    for (i = wn - 1; i >= 0; i--) {
        pci_write_entry(&wl[i], saved[i]);
        line[0] = 0;
        a_cat(line, sizeof(line), "WRESTORE ");
        fmt_bdf(bdf, sizeof(bdf), wl[i].bus, wl[i].dev, wl[i].func);
        a_cat(line, sizeof(line), bdf);
        a_cat(line, sizeof(line), " off=0x");
        a_cat_u64hex(line, sizeof(line), wl[i].offset, 2);
        a_cat(line, sizeof(line), " val=0x");
        a_cat_u64hex(line, sizeof(line), saved[i], wl[i].width * 2);
        log_line(line);
    }
    log_line("WRITER_RESULT: ABORT_RESTORED");
    log_flush();
    con_line("WRITER_RESULT: ABORT_RESTORED (originals restored, chainloading anyway)");
    /* fall through: main() continues to ACPI dump + chainload, always */
}

/* ------------------------------------------------------------------ */
/* Chainload GRUB (the only "action" the logger takes, after logging)  */
/* ------------------------------------------------------------------ */

static BOOLEAN try_chainload_on_handle(EFI_HANDLE fsHandle)
{
    EFI_DEVICE_PATH *dp = FileDevicePath(fsHandle, gChainloadPath);
    EFI_HANDLE child = NULL;
    EFI_STATUS st;
    if (!dp) return FALSE;
    st = uefi_call_wrapper(BS->LoadImage, 6, FALSE, gImageHandle, dp, NULL, 0, &child);
    if (EFI_ERROR(st)) { FreePool(dp); return FALSE; }
    log_line("CHAINLOAD: image loaded, starting it now");
    log_flush();
    con_line("CHAINLOAD: starting next stage");
    st = uefi_call_wrapper(BS->StartImage, 3, child, NULL, NULL);
    {
        char line[256];
        line[0] = 0;
        a_cat(line, sizeof(line), "CHAINLOAD: started image returned status ");
        a_cat_u64hex(line, sizeof(line), (UINT64)st, 16);
        log_line(line);
        log_flush();
    }
    FreePool(dp);
    return TRUE; /* it loaded and ran (and exited) - do not try other volumes */
}

static void do_chainload(void)
{
    EFI_HANDLE *handles = NULL;
    UINTN n = 0, i;
    EFI_STATUS st;
    EFI_GUID fsg = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

    log_section("CHAINLOAD");
    st = uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol, &fsg, NULL, &n, &handles);
    if (EFI_ERROR(st) || n == 0) { log_line("CHAINLOAD: no filesystems found"); return; }
    for (i = 0; i < n; i++) {
        if (try_chainload_on_handle(handles[i])) return;
    }
    log_line("CHAINLOAD: target not found on any volume");
    log_flush();
    con_line("CHAINLOAD FAILED: target not found (see log)");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void wait_key(void)
{
    UINTN idx;
    con_line("Press any key to return to the firmware boot menu...");
    uefi_call_wrapper(BS->WaitForEvent, 3, 1, &ST->ConIn->WaitForKey, &idx);
    {
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_LOADED_IMAGE *loaded = NULL;
    EFI_GUID loadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_FILE_IO_INTERFACE *fs = NULL;
    EFI_STATUS st;
    EFI_TIME tm;
    CHAR16 fname[64];
    char line[256];

    InitializeLib(ImageHandle, SystemTable);
    gImageHandle = ImageHandle;

    con_line("egpushim v" SHIM_VERSION " - PLACEMENT + ICM CLAIM + SNAPSHOT (gated)");
    con_line("MODE=WRITER+ICM+SNAPSHOT: config writes ONLY if WRITE_ENABLE=1; ICM claim ONLY if ICM_CLAIM=1; snapshot ONLY if SNAPSHOT=1");

    /* Own volume */
    st = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle, &loadedGuid, (VOID **)&loaded);
    if (EFI_ERROR(st) || !loaded) { con_line("FATAL: no LoadedImage protocol"); return EFI_LOAD_ERROR; }
    st = uefi_call_wrapper(BS->HandleProtocol, 3, loaded->DeviceHandle, &fsGuid, (VOID **)&fs);
    if (EFI_ERROR(st) || !fs) { con_line("FATAL: no filesystem on boot device"); return EFI_LOAD_ERROR; }
    st = uefi_call_wrapper(fs->OpenVolume, 2, fs, &gRootDir);
    if (EFI_ERROR(st) || !gRootDir) { con_line("FATAL: cannot open boot volume"); return EFI_LOAD_ERROR; }

    /* LOGS directory */
    gLogDir = NULL;
    st = uefi_call_wrapper(gRootDir->Open, 5, gRootDir, &gLogDir, L"LOGS",
                           EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                           EFI_FILE_DIRECTORY);
    if (EFI_ERROR(st) || !gLogDir) { con_line("FATAL: cannot create LOGS directory"); return EFI_LOAD_ERROR; }

    /* Log file, timestamped if the clock works */
    SetMem(&tm, sizeof(tm), 0);
    st = uefi_call_wrapper(RT->GetTime, 2, &tm, NULL);
    if (!EFI_ERROR(st) && tm.Year >= 2020 && tm.Year < 2100) {
        SPrint(fname, sizeof(fname), L"egpushim-%04d%02d%02d-%02d%02d%02d.log",
               tm.Year, tm.Month, tm.Day, tm.Hour, tm.Minute, tm.Second);
    } else {
        StrCpy(fname, L"egpushim-boot.log");
    }
    gLogFile = NULL;
    st = uefi_call_wrapper(gLogDir->Open, 5, gLogDir, &gLogFile, fname,
                           EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(st) || !gLogFile) { con_line("FATAL: cannot create log file"); return EFI_LOAD_ERROR; }

    /* Header, flushed before anything else happens (spec 5.3) */
    log_line("egpushim " SHIM_VERSION);
    log_line("MODE=WRITER+ICM+SNAPSHOT (full placement, gated; ICM claim and TB snapshot before the writer, gated)");
    log_line("PCI access in this build: config reads everywhere; config writes ONLY inside the gated WRITER path (plan 3a + 3b)");
    log_line("Writer gate: WRITE_ENABLE=1 in shim.cfg AND a valid computed plan - otherwise ZERO config writes");
    log_line("ICM gate: ICM_CLAIM=1 in shim.cfg enables NHI MMIO claim writes; otherwise the ICM phase is read-only recon");
    log_line("Snapshot gate: SNAPSHOT=1 in shim.cfg enables the read-only Thunderbolt router config capture (config READ packets only)");
    log_line("NVRAM writes in this build: NONE");
    {
        line[0] = 0;
        a_cat(line, sizeof(line), "FirmwareVendor chars: ");
        a_cat_u64dec(line, sizeof(line), StrLen(ST->FirmwareVendor));
        a_cat(line, sizeof(line), " FirmwareRevision: ");
        a_cat_u64hex(line, sizeof(line), ST->FirmwareRevision, 8);
        a_cat(line, sizeof(line), " UEFISpec: ");
        a_cat_u64dec(line, sizeof(line), (ST->Hdr.Revision >> 16) & 0xFFFF);
        a_cat(line, sizeof(line), ".");
        a_cat_u64dec(line, sizeof(line), ST->Hdr.Revision & 0xFFFF);
        log_line(line);
    }
    log_flush();

    load_config();
    {
        line[0] = 0;
        a_cat(line, sizeof(line), "CFG: WRITE_ENABLE=");
        a_cat_u64dec(line, sizeof(line), gWriteEnable ? 1 : 0);
        a_cat(line, sizeof(line), " ICM_CLAIM=");
        a_cat_u64dec(line, sizeof(line), gIcmClaim ? 1 : 0);
        a_cat(line, sizeof(line), " TB_PROBE=");
        a_cat_u64dec(line, sizeof(line), gTbProbe ? 1 : 0);
        a_cat(line, sizeof(line), " SNAPSHOT=");
        a_cat_u64dec(line, sizeof(line), gSnapshot ? 1 : 0);
        log_line(line);
    }
    log_flush();

    do_memory_map();
    do_pci_scan();
    do_plan();
    do_icm();
    do_snapshot();
    if (gTbProbe) do_tb_probe();
    do_write();
    do_recheck();
    do_acpi();

    log_section("DONE - logging complete");
    log_flush();

    do_chainload();

    log_line("egpushim exiting to firmware boot manager");
    log_flush();
    if (gLogFile) uefi_call_wrapper(gLogFile->Close, 1, gLogFile);
    if (gPause) wait_key();
    return EFI_SUCCESS;
}
