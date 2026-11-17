/*
 * cfg_pure.c - see cfg_pure.h for the protocol derivation.
 */
#include "cfg_pure.h"
#include "icm_pure.h"

uint32_t cfg_addr_dword(uint32_t port, uint32_t space, uint32_t offset,
                        uint32_t length, uint32_t seq)
{
    return (offset & 0x1FFFu)
         | ((length & 0x3Fu) << 13)
         | ((port & 0x3Fu) << 19)
         | ((space & 0x3u) << 25)
         | ((seq & 0x3u) << 27);
}

size_t cfg_build_read(uint32_t out[3], uint64_t route, uint32_t port,
                      uint32_t space, uint32_t offset, uint32_t length,
                      uint32_t seq)
{
    if (length == 0 || length > 64) return 0;
    /* tb_cfg_make_header: route_hi is the low 22 bits of the top
     * dword; the "unknown" field above it stays 0 in requests. */
    out[0] = (uint32_t)((route >> 32) & 0x3FFFFFu);
    out[1] = (uint32_t)(route & 0xFFFFFFFFu);
    out[2] = cfg_addr_dword(port, space, offset, length, seq);
    return 3;
}

size_t cfg_frame_wire(const uint32_t *payload_dwords, size_t n_dwords,
                      uint8_t *out)
{
    size_t i;
    uint32_t crc;
    for (i = 0; i < n_dwords; i++) {
        uint32_t d = payload_dwords[i];
        out[i * 4 + 0] = (uint8_t)(d >> 24);
        out[i * 4 + 1] = (uint8_t)(d >> 16);
        out[i * 4 + 2] = (uint8_t)(d >> 8);
        out[i * 4 + 3] = (uint8_t)(d);
    }
    crc = icm_tb_crc(out, n_dwords * 4);
    out[n_dwords * 4 + 0] = (uint8_t)(crc >> 24);
    out[n_dwords * 4 + 1] = (uint8_t)(crc >> 16);
    out[n_dwords * 4 + 2] = (uint8_t)(crc >> 8);
    out[n_dwords * 4 + 3] = (uint8_t)(crc);
    return n_dwords * 4 + 4;
}

int cfg_unframe_wire(const uint8_t *wire, size_t frame_len,
                     uint32_t *out_dwords, size_t max_dwords)
{
    size_t payload_len, n, i;
    uint32_t want, got;
    if (frame_len < 4 || (frame_len & 3u)) return -1;
    payload_len = frame_len - 4;
    n = payload_len / 4;
    if (n > max_dwords) return -1;
    want = icm_tb_crc(wire, payload_len);
    got = ((uint32_t)wire[payload_len + 0] << 24)
        | ((uint32_t)wire[payload_len + 1] << 16)
        | ((uint32_t)wire[payload_len + 2] << 8)
        | ((uint32_t)wire[payload_len + 3]);
    if (want != got) return -1;
    for (i = 0; i < n; i++) {
        out_dwords[i] = ((uint32_t)wire[i * 4 + 0] << 24)
                      | ((uint32_t)wire[i * 4 + 1] << 16)
                      | ((uint32_t)wire[i * 4 + 2] << 8)
                      | ((uint32_t)wire[i * 4 + 3]);
    }
    return (int)n;
}

int cfg_parse_read_response(const uint32_t *dw, size_t n_dw, uint32_t eof,
                            uint64_t req_route, uint32_t req_offset,
                            uint32_t req_space, uint32_t req_len,
                            uint32_t req_seq, uint32_t *data_out,
                            uint32_t *tb_err)
{
    uint64_t route;
    uint32_t addr;

    if (eof == CFG_PKG_ERROR) {
        /* cfg_error_pkg: header (2 dwords) + error dword. Matches any
         * pending request (tb_cfg_match). */
        if (n_dw < 3) return -1;
        if (tb_err) *tb_err = dw[2] & 0xFFu;
        return 0;
    }
    if (eof != CFG_PKG_READ) return -1;
    if (n_dw != 3u + req_len) return -1;

    /* Reply marker: the header "unknown" field must read 0x200
     * (check_header), i.e. bit 31 of dword0 set. */
    if (((dw[0] >> 22) & 0x3FFu) != 0x200u) return -1;
    route = ((uint64_t)(dw[0] & 0x3FFFFFu) << 32) | (uint64_t)dw[1];
    if (route != req_route) return -1;

    /* check_config_address: offset/space/length/seq must echo; the
     * port field is the sender's upstream port and is not matched;
     * the zero field must be 0. */
    addr = dw[2];
    if ((addr >> 29) != 0) return -1;
    if ((addr & 0x1FFFu) != req_offset) return -1;
    if (((addr >> 13) & 0x3Fu) != req_len) return -1;
    if (((addr >> 25) & 0x3u) != req_space) return -1;
    if (((addr >> 27) & 0x3u) != req_seq) return -1;

    if (data_out) {
        uint32_t i;
        for (i = 0; i < req_len; i++) data_out[i] = dw[3 + i];
    }
    return 1;
}
