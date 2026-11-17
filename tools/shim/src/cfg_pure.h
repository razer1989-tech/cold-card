/*
 * cfg_pure.h - Thunderbolt config-space packet core (PURE LOGIC, no
 * EFI/libc dependencies). Shared by the EFI application and the
 * host-side unit test.
 *
 * Faithful port of the Linux thunderbolt driver's config channel,
 * derived from torvalds/linux drivers/thunderbolt/{ctl.c,tb_msgs.h,
 * tb_regs.h} and drivers/thunderbolt/ctl.h (sources read 2026-11-17;
 * in current master the config functions live in ctl.c):
 *
 *   - A config READ request is struct cfg_read_pkg (3 dwords):
 *       dword0/1: struct tb_cfg_header - route_hi:22 (bits 0-21 of
 *                 dword0; the top 10 bits are "unknown", 0 in
 *                 requests, 0x200 in replies), route_lo in dword1.
 *       dword2:   struct tb_cfg_address - offset:13 | length:6 |
 *                 port:6 | space:2 | seq:2 | zero:3.
 *   - On the wire the payload dwords are BIG-ENDIAN with a CRC-32C
 *     trailer (icm_tb_crc), exactly like ICM packets; the frame
 *     travels in an NHI ring descriptor with eof = sof =
 *     TB_CFG_PKG_READ (1) for requests (ctl.c tb_ctl_tx).
 *   - A read response arrives with eof = TB_CFG_PKG_READ as well
 *     (tb_cfg_match: response_type == request_type), payload =
 *     cfg_write_pkg { header, addr echo, data[length] }; the reply
 *     route has bit 63 set (header unknown field == 0x200, checked
 *     by check_header). The echoed addr.port is the sender's upstream
 *     port and is NOT matched (check_config_address); offset, space,
 *     length and seq ARE matched.
 *   - An error response arrives with eof = TB_CFG_PKG_ERROR (3) and
 *     matches ANY pending request (tb_cfg_match); payload =
 *     cfg_error_pkg { header, dword: error:8 | port:6 | ... }.
 *   - Config spaces (enum tb_cfg_space): HOPS=0, PORT=1, SWITCH=2,
 *     COUNTERS=3. Length is in dwords, max 64 per request; a single
 *     frame is at most 256 bytes including the CRC, so responses cap
 *     practical reads well below that (this shim chunks at 32).
 *
 * This code performs no I/O. It only computes bytes.
 */
#ifndef EGPUSHIM_CFG_PURE_H
#define EGPUSHIM_CFG_PURE_H

#include <stdint.h>
#include <stddef.h>

#define CFG_PKG_READ    1u    /* TB_CFG_PKG_READ  */
#define CFG_PKG_WRITE   2u    /* TB_CFG_PKG_WRITE */
#define CFG_PKG_ERROR   3u    /* TB_CFG_PKG_ERROR */

#define CFG_SPACE_HOPS     0u
#define CFG_SPACE_PORT     1u
#define CFG_SPACE_SWITCH   2u
#define CFG_SPACE_COUNTERS 3u

#define CFG_ERR_INVALID_CONFIG_SPACE 2u  /* TB_CFG_ERROR_INVALID_CONFIG_SPACE */

/* Pack a struct tb_cfg_address dword. */
uint32_t cfg_addr_dword(uint32_t port, uint32_t space, uint32_t offset,
                        uint32_t length, uint32_t seq);

/* Build a config READ request payload (host-order dwords) into
 * out[3]: header route_hi, header route_lo, address. Returns 3, or 0
 * if length is 0 or > 64. */
size_t cfg_build_read(uint32_t out[3], uint64_t route, uint32_t port,
                      uint32_t space, uint32_t offset, uint32_t length,
                      uint32_t seq);

/* Frame payload dwords for the wire: big-endian bytes + CRC trailer
 * (icm_tb_crc). out needs 4*n + 4 bytes. Returns total frame bytes. */
size_t cfg_frame_wire(const uint32_t *payload_dwords, size_t n_dwords,
                      uint8_t *out);

/* Verify and unframe a received wire frame: checks the CRC, converts
 * payload dwords back to host order into out_dwords. frame_len is the
 * descriptor length (payload + 4 CRC bytes). Returns the payload
 * dword count, or -1 on a bad CRC / malformed length. */
int cfg_unframe_wire(const uint8_t *wire, size_t frame_len,
                     uint32_t *out_dwords, size_t max_dwords);

/* Parse a config response payload (host-order dwords from
 * cfg_unframe_wire) against a pending READ request.
 *   returns  1: data response - data_out filled with want_len dwords
 *            0: TB error response - *tb_err holds the error code
 *           -1: not a match for this request (stray frame)
 * eof is the descriptor eof of the frame. */
int cfg_parse_read_response(const uint32_t *dw, size_t n_dw, uint32_t eof,
                            uint64_t req_route, uint32_t req_offset,
                            uint32_t req_space, uint32_t req_len,
                            uint32_t req_seq, uint32_t *data_out,
                            uint32_t *tb_err);

#endif /* EGPUSHIM_CFG_PURE_H */
