#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Custom USB Serial v1: COBS records terminated by zero, CRC16-CCITT (init FFFF).
 * All integers are little endian. Common header: version, type, bus, flags,
 * boot_id:u32, sequence:u32, timestamp_us:u64. Frame: id:u32, length:u8,
 * data[length], crc:u16. Status: queue_drops:u32, overflow_events:u32,
 * usb_aborts:u32, crc:u16. Sequence counts received frames per bus, before
 * queueing. Overflow events indicate at least one lost frame, not an exact count.
 * Flags: bit0 extended, bit1 RTR, bit2 FD, bit3 BRS.
 */
#define DK_VERSION 1
#define DK_FRAME 0
#define DK_STATUS 1
#define DK_MAX_RAW 91
#define DK_MAX_WIRE 93

static inline void dk_put(uint8_t *p, uint64_t value, size_t n)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(value >> (8 * i));
}

static inline uint64_t dk_get(const uint8_t *p, size_t n)
{
    uint64_t value = 0;
    for (size_t i = 0; i < n; i++) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static inline uint16_t dk_crc(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int bit = 0; bit < 8; bit++)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

/* Appends CRC and COBS-encodes a payload of at most DK_MAX_RAW-2 bytes. */
static inline size_t dk_encode(uint8_t *raw, size_t n, uint8_t *wire)
{
    if (n > DK_MAX_RAW - 2) return 0;
    dk_put(raw + n, dk_crc(raw, n), 2);
    n += 2;
    size_t code_at = 0, out = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < n; i++) {
        if (raw[i] == 0) {
            wire[code_at] = code;
            code_at = out++;
            code = 1;
        } else {
            wire[out++] = raw[i];
            code++;
        }
    }
    wire[code_at] = code;
    wire[out++] = 0;
    return out;
}

/* Input excludes delimiter. Returns zero for corrupt/unsupported records. */
static inline size_t dk_decode(const uint8_t *wire, size_t n, uint8_t *raw)
{
    if (!n || n >= DK_MAX_WIRE) return 0;
    size_t in = 0, out = 0;
    while (in < n) {
        uint8_t code = wire[in++];
        if (!code || in + code - 1 > n) return 0;
        for (unsigned i = 1; i < code; i++) {
            if (out >= DK_MAX_RAW || !wire[in]) return 0;
            raw[out++] = wire[in++];
        }
        if (in < n && code != 255) {
            if (out >= DK_MAX_RAW) return 0;
            raw[out++] = 0;
        }
    }
    if (out < 22 || raw[0] != DK_VERSION || raw[2] > 1 ||
        dk_get(raw + out - 2, 2) != dk_crc(raw, out - 2)) return 0;
    if (raw[1] == DK_STATUS) return out == 34 && raw[3] == 0 ? out : 0;
    if (raw[1] != DK_FRAME || out < 27 || raw[24] > 64 ||
        out != (size_t)(27 + raw[24]) || (raw[3] & ~15)) return 0;
    if (!(raw[3] & 4) && raw[24] > 8) return 0;
    if ((raw[3] & 4) && ((raw[3] & 2) ||
        (raw[24] > 8 && raw[24] != 12 && raw[24] != 16 && raw[24] != 20 &&
         raw[24] != 24 && raw[24] != 32 && raw[24] != 48 && raw[24] != 64))) return 0;
    if ((raw[3] & 8) && !(raw[3] & 4)) return 0;
    if (dk_get(raw + 20, 4) > ((raw[3] & 1) ? 0x1fffffffU : 0x7ffU)) return 0;
    return out;
}
