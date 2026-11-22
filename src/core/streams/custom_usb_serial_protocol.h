#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Custom USB Serial v1: COBS records terminated by zero, CRC16-CCITT (init FFFF).
 * All integers are little endian. Common header: version, type, bus, flags,
 * boot_id:u32, sequence:u32, timestamp_us:u64. Frame: id:u32, length:u8,
 * data[length], crc:u16. Status: queue_drops:u32, overflow_events:u32,
 * usb_aborts:u32, crc:u16. Sequence counts received frames per bus, before
 * queueing. Overflow events indicate at least one lost frame, not an exact count.
 * Flags: bit0 extended, bit1 RTR, bit2 FD, bit3 BRS.
 * USB transmit requests use the Frame layout with the adapter's current boot ID
 * and a nonzero request sequence. TX results carry one status byte at offset
 * 20 and echo the request bus and sequence. Sequence zero announces TX support.
 * Timed requests append one payload-length byte mask after the frame data;
 * masked bits are held at the requested values while unmasked bits follow
 * received frames. They use frequency_hz:u32 and duration_ms:u32 in place of
 * the timestamp (automatic mode requires frequency zero). Repeat controls use the request bus and sequence,
 * an all-zero timestamp, and one opcode byte at offset 20. Repeat results use
 * the TX result layout. Bus 1 in a zero-sequence TX ready record advertises
 * repeat support. A separate zero-sequence Auto result advertises automatic
 * reassert support without changing the fixed-rate capability announcement.
 */
#define DK_VERSION 1
#define DK_FRAME 0
#define DK_STATUS 1
#define DK_STATUS_SIZE 32
#define DK_TX_REQUEST 2
#define DK_TX_RESULT 3
#define DK_TX_REPEAT 4
#define DK_TX_REPEAT_CONTROL 5
#define DK_TX_REPEAT_RESULT 6
#define DK_TX_AUTO 7
#define DK_TX_AUTO_RESULT 8
#define DK_TX_READY 0
#define DK_TX_QUEUED 1
#define DK_TX_REJECTED 2
#define DK_TX_RATE_LIMIT 3
#define DK_TX_FIFO_FULL 4
#define DK_TX_CONTROLLER_ERROR 5
#define DK_TX_PREVIOUS_FAILED 6
#define DK_TX_REPEAT_STARTED 7
#define DK_TX_REPEAT_FINISHED 8
#define DK_TX_REPEAT_CANCELLED 9
#define DK_TX_REPEAT_BUSY 10
#define DK_TX_REPEAT_LEASE_EXPIRED 11
#define DK_TX_KEEPALIVE 0
#define DK_TX_CANCEL 1
#define DK_TX_REPEAT_CAPABILITY 1
#define DK_TX_MAX_HZ 10
#define DK_TX_MIN_DURATION_MS 100
#define DK_TX_MAX_DURATION_MS 600000
#define DK_MAX_RAW 155
#define DK_MAX_WIRE 157

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

/* Retains masked wanted bits, adopts unmasked actual bits, and reports whether
 * any retained bit differed before the merge. */
static inline bool dk_merge_masked(uint8_t *wanted, const uint8_t *actual,
                                   const uint8_t *mask, size_t n)
{
    bool changed = false;
    for (size_t i = 0; i < n; i++) {
        changed |= ((wanted[i] ^ actual[i]) & mask[i]) != 0;
        wanted[i] = (wanted[i] & mask[i]) | (actual[i] & ~mask[i]);
    }
    return changed;
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
    if ((raw[1] == DK_TX_REPEAT || raw[1] == DK_TX_AUTO) &&
        (n != (size_t)(25 + 2 * raw[24]) || raw[24] > 64)) return 0;
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
    if (raw[1] == DK_TX_REPEAT || raw[1] == DK_TX_AUTO) {
        uint32_t hz = dk_get(raw + 12, 4), duration = dk_get(raw + 16, 4);
        if (!dk_get(raw + 8, 4) ||
            (raw[1] == DK_TX_REPEAT ? (!hz || hz > DK_TX_MAX_HZ) : hz != 0) ||
            duration < DK_TX_MIN_DURATION_MS || duration > DK_TX_MAX_DURATION_MS) return 0;
    }
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
    if (raw[1] == DK_STATUS)
        return out == DK_STATUS_SIZE + 2 && raw[3] == 0 ? out : 0;
    if (raw[1] == DK_TX_RESULT)
        return out == 23 && raw[3] == 0 && raw[20] <= DK_TX_PREVIOUS_FAILED &&
            ((dk_get(raw + 8, 4) == 0) == (raw[20] == DK_TX_READY)) ? out : 0;
    if (raw[1] == DK_TX_REPEAT_RESULT || raw[1] == DK_TX_AUTO_RESULT)
        return out == 23 && raw[3] == 0 &&
            ((raw[1] == DK_TX_AUTO_RESULT && raw[20] == DK_TX_READY && dk_get(raw + 8, 4) == 0) ||
             (raw[20] >= DK_TX_REJECTED && raw[20] <= DK_TX_REPEAT_LEASE_EXPIRED &&
              dk_get(raw + 8, 4) != 0)) ? out : 0;
    if (raw[1] == DK_TX_REPEAT_CONTROL)
        return out == 23 && raw[3] == 0 && dk_get(raw + 8, 4) != 0 &&
            dk_get(raw + 12, 8) == 0 && raw[20] <= DK_TX_CANCEL ? out : 0;
    if ((raw[1] != DK_FRAME && raw[1] != DK_TX_REQUEST && raw[1] != DK_TX_REPEAT &&
         raw[1] != DK_TX_AUTO) ||
        out < 27 || raw[24] > 64 || (raw[3] & ~15)) return 0;
    const bool timed = raw[1] == DK_TX_REPEAT || raw[1] == DK_TX_AUTO;
    if (out != (size_t)(27 + raw[24] * (timed ? 2 : 1))) return 0;
    if (!(raw[3] & 4) && raw[24] > 8) return 0;
    if ((raw[3] & 4) && ((raw[3] & 2) ||
        (raw[24] > 8 && raw[24] != 12 && raw[24] != 16 && raw[24] != 20 &&
         raw[24] != 24 && raw[24] != 32 && raw[24] != 48 && raw[24] != 64))) return 0;
    if ((raw[3] & 8) && !(raw[3] & 4)) return 0;
    if (dk_get(raw + 20, 4) > ((raw[3] & 1) ? 0x1fffffffU : 0x7ffU)) return 0;
    if (raw[1] == DK_TX_REPEAT || raw[1] == DK_TX_AUTO) {
        uint32_t hz = dk_get(raw + 12, 4), duration = dk_get(raw + 16, 4);
        if (!dk_get(raw + 8, 4) ||
            (raw[1] == DK_TX_REPEAT ? (!hz || hz > DK_TX_MAX_HZ) : hz != 0) ||
            duration < DK_TX_MIN_DURATION_MS || duration > DK_TX_MAX_DURATION_MS) return 0;
        bool any_mask = false;
        for (uint8_t i = 0; i < raw[24]; i++) any_mask |= raw[25 + raw[24] + i] != 0;
        if (!any_mask) return 0;
    }
    return out;
}
