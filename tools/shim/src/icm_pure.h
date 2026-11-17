/*
 * icm_pure.h - ICM (Thunderbolt Internal Connection Manager) wire
 * protocol core (PURE LOGIC, no EFI/libc dependencies).
 *
 * Shared by the EFI application and the host-side unit test.
 * Everything here is a faithful port of the Linux thunderbolt
 * driver's ICM control-channel framing, derived from torvalds/linux
 * drivers/thunderbolt/{icm.c,ctl.c,nhi.c,tb_msgs.h,nhi_regs.h} and
 * include/linux/thunderbolt.h (sources read 2026-11-17):
 *
 *   - An ICM command is a small packet whose first dword is
 *     struct icm_pkg_header { u8 code; u8 flags; u8 packet_id;
 *     u8 total_packets; }. DRIVER_READY (code 0x3) is header-only.
 *   - On the wire, packet dwords are BIG-ENDIAN (ctl.c tb_ctl_tx
 *     runs cpu_to_be32_array over the packet), and a CRC is appended:
 *     tb_crc = cpu_to_be32(~crc32c(~0, wire_bytes, len)) - CRC-32C
 *     (Castagnoli), kernel chaining semantics (no final xor inside
 *     crc32c itself; the ~ is applied by tb_crc).
 *   - The frame travels in an NHI ring descriptor whose eof/sof
 *     fields carry the package type: 11 = TB_CFG_PKG_ICM_CMD for
 *     requests, 12 = TB_CFG_PKG_ICM_RESP for responses.
 *   - A response matches a request when eof == ICM_RESP and the
 *     response packet's code equals the request's code (icm_match).
 *
 * This code performs no I/O. It only computes bytes.
 */
#ifndef EGPUSHIM_ICM_PURE_H
#define EGPUSHIM_ICM_PURE_H

#include <stdint.h>
#include <stddef.h>

#define ICM_CODE_DRIVER_READY   0x3u
#define ICM_PKG_CMD             11u   /* TB_CFG_PKG_ICM_CMD  */
#define ICM_PKG_RESP            12u   /* TB_CFG_PKG_ICM_RESP */
#define ICM_PKG_EVENT           10u   /* TB_CFG_PKG_ICM_EVENT */

/* NHI ring descriptor flag values (12-bit flags field). */
#define ICM_DESC_COMPLETED      0x2u
#define ICM_DESC_POSTED         0x4u
#define ICM_DESC_INTERRUPT      0x8u

/* Kernel-style CRC-32C: seed-chaining, NO final xor (lib/crc32.c
 * crc32c semantics). */
uint32_t icm_crc32c_raw(uint32_t crc, const uint8_t *data, size_t len);

/* ctl.c tb_crc(): ~crc32c(~0, data, len) - the value whose big-endian
 * bytes are appended to a control frame. */
uint32_t icm_tb_crc(const uint8_t *data, size_t len);

/* Build the complete wire frame for a DRIVER_READY request into out[]
 * (needs >= 8 bytes): the 4-byte big-endian packet header followed by
 * the 4-byte big-endian CRC. Returns the frame length (8). */
size_t icm_build_driver_ready(uint8_t *out);

/* Parse a received ICM response payload (already the frame bytes as
 * they lie in the DMA buffer, CRC still attached is fine - only the
 * first 8 bytes are read). Converts the two dwords from wire
 * big-endian and extracts the icm_ar_pkg_driver_ready_response
 * fields. Returns 0 on success, -1 if len < 8. */
int icm_parse_ready_response(const uint8_t *wire, size_t len,
                             uint8_t *code, uint8_t *romver,
                             uint8_t *ramver, uint16_t *info);

/* Pack/unpack the second dword of an NHI ring descriptor
 * (struct ring_desc): length:12 | eof:4 | sof:4 | flags:12. */
uint32_t icm_desc_dword(uint32_t length, uint32_t eof, uint32_t sof,
                        uint32_t flags);
uint32_t icm_desc_length(uint32_t dword);
uint32_t icm_desc_eof(uint32_t dword);
uint32_t icm_desc_flags(uint32_t dword);

#endif /* EGPUSHIM_ICM_PURE_H */
