/*
 * icm_pure.c - see icm_pure.h. Pure logic; no I/O.
 */
#include "icm_pure.h"

/* Reflected CRC-32C (Castagnoli) polynomial. */
#define ICM_CRC32C_POLY 0x82F63B78u

static uint32_t gCrcTable[256];
static int      gCrcReady = 0;

static void icm_crc_table_init(void)
{
    uint32_t i, j, c;
    if (gCrcReady) return;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (ICM_CRC32C_POLY ^ (c >> 1)) : (c >> 1);
        gCrcTable[i] = c;
    }
    gCrcReady = 1;
}

uint32_t icm_crc32c_raw(uint32_t crc, const uint8_t *data, size_t len)
{
    size_t i;
    icm_crc_table_init();
    for (i = 0; i < len; i++)
        crc = gCrcTable[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

uint32_t icm_tb_crc(const uint8_t *data, size_t len)
{
    return ~icm_crc32c_raw(0xFFFFFFFFu, data, len);
}

static void put_be32(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)(v);
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

size_t icm_build_driver_ready(uint8_t *out)
{
    /* struct icm_pkg_driver_ready = header only:
     * code=ICM_DRIVER_READY, flags=0, packet_id=0, total_packets=0.
     * As a host-order dword that is 0x00000003; on the wire it is
     * stored big-endian (ctl.c cpu_to_be32_array). */
    put_be32(out, 0x00000003u);
    put_be32(out + 4, icm_tb_crc(out, 4));
    return 8;
}

int icm_parse_ready_response(const uint8_t *wire, size_t len,
                             uint8_t *code, uint8_t *romver,
                             uint8_t *ramver, uint16_t *info)
{
    uint32_t d0, d1;
    if (len < 8) return -1;
    d0 = get_be32(wire);
    d1 = get_be32(wire + 4);
    /* struct icm_ar_pkg_driver_ready_response (host order):
     * hdr = d0 (code is its low byte), then romver, ramver, info. */
    if (code)   *code   = (uint8_t)(d0 & 0xFFu);
    if (romver) *romver = (uint8_t)(d1 & 0xFFu);
    if (ramver) *ramver = (uint8_t)((d1 >> 8) & 0xFFu);
    if (info)   *info   = (uint16_t)((d1 >> 16) & 0xFFFFu);
    return 0;
}

uint32_t icm_desc_dword(uint32_t length, uint32_t eof, uint32_t sof,
                        uint32_t flags)
{
    return (length & 0xFFFu) | ((eof & 0xFu) << 12) |
           ((sof & 0xFu) << 16) | ((flags & 0xFFFu) << 20);
}

uint32_t icm_desc_length(uint32_t dword) { return dword & 0xFFFu; }
uint32_t icm_desc_eof(uint32_t dword)    { return (dword >> 12) & 0xFu; }
uint32_t icm_desc_flags(uint32_t dword)  { return (dword >> 20) & 0xFFFu; }
