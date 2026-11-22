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
 * Utilization (bus, flags and sequence zero): serial, CPU cores 0/1, shared
 * SPI, and CAN buses 0/1, each as u16 per mille over the last sample interval.
 * TX feedback uses the TX result layout. A zero sequence announces support;
 * a nonzero sequence reports the outcome of one individual transmit attempt.
 * An acknowledged TX frame uses the Frame layout and the TX request sequence.
 * Failure feedback may append FIFO status, bus diagnostics and error counters
 * as three u32 controller register snapshots.
 * Flags: bit0 extended, bit1 RTR, bit2 FD, bit3 BRS. TX request bit4 appends
 * a three-byte DBC counter descriptor (start_bit:u16, size:u7 + little-endian
 * bit) and asks the adapter to transmit the last received counter plus one.
 * TX request bit5 appends the checksum byte index and asks the adapter to
 * recalculate the Tesla byte-sum checksum immediately before transmission.
 * USB transmit requests use the Frame layout with the adapter's current boot ID
 * and a nonzero request sequence. TX results carry one status byte at offset
 * 20 and echo the request bus and sequence. Sequence zero announces TX support.
 * Timed requests append one payload-length byte mask after the frame data;
 * masked bits are held at the requested values while unmasked bits follow
 * received frames. An optional second mask matches selector bits against
 * the original payload without overriding them. Timed requests use
 * frequency_hz:u32 and duration_ms:u32 in place of
 * the timestamp (automatic mode requires frequency zero). Repeat controls use the request bus and sequence,
 * an all-zero timestamp, and one opcode byte at offset 20. Repeat results use
 * the TX result layout. Bus 1 in a zero-sequence TX ready record advertises
 * the complete transmit firmware feature set: manual send, controller
 * feedback, fixed-rate repeat, automatic reassert, and counter incrementing.
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
#define DK_UTILIZATION 9
#define DK_UTILIZATION_SIZE 32
#define DK_TX_FEEDBACK 10
#define DK_TX_FRAME 11
#define DK_TX_INCREMENT_COUNTER 16
#define DK_TX_RECALCULATE_CHECKSUM 32
#define DK_TX_ACKNOWLEDGED 1
#define DK_TX_UNCONFIRMED 2
#define DK_TX_OUTCOME_UNKNOWN 3
#define DK_TX_FEEDBACK_DIAGNOSTICS_SIZE 33
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
#define DK_TX_CAPABILITY 1
#define DK_TX_LEGACY_MAX_HZ 10
#define DK_TX_MAX_HZ 1000000  // One microsecond per interval.
#define DK_TX_MIN_DURATION_MS 100
#define DK_TX_MAX_DURATION_MS 600000
#define DK_MAX_RAW 223
#define DK_MAX_WIRE 225

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

static inline bool dk_matches_mask(const uint8_t *wanted, const uint8_t *actual,
                                   const uint8_t *mask, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if ((wanted[i] ^ actual[i]) & mask[i]) return false;
    }
    return true;
}

static inline uint16_t dk_counter_bit(uint16_t start_bit, uint8_t index, bool little_endian)
{
    return little_endian ? start_bit + index :
           (uint16_t)(((start_bit ^ 7) + index) ^ 7);
}

static inline void dk_increment_counter(uint8_t *data, uint16_t start_bit,
                                        uint8_t size, bool little_endian)
{
    uint64_t value = 0;
    for (uint8_t i = 0; i < size; i++) {
        const uint16_t bit = dk_counter_bit(start_bit, i, little_endian);
        const uint8_t shift = little_endian ? i : size - 1 - i;
        value |= (uint64_t)((data[bit / 8] >> (bit % 8)) & 1) << shift;
    }
    value++;
    if (size < 64) value &= (UINT64_C(1) << size) - 1;
    for (uint8_t i = 0; i < size; i++) {
        const uint16_t bit = dk_counter_bit(start_bit, i, little_endian);
        const uint8_t mask = (uint8_t)(1u << (bit % 8));
        const uint8_t shift = little_endian ? i : size - 1 - i;
        data[bit / 8] = (data[bit / 8] & ~mask) | ((value >> shift & 1) ? mask : 0);
    }
}

static inline void dk_recalculate_tesla_checksum(uint8_t *data, uint8_t length,
                                                 uint32_t address, uint8_t checksum_byte)
{
    uint8_t sum = (uint8_t)((address & 0xff) + ((address >> 8) & 0xff));
    for (uint8_t i = 0; i < length; i++) if (i != checksum_byte) sum += data[i];
    data[checksum_byte] = sum;
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
        (n < 25 || raw[24] > 64 ||
         (n != (size_t)(25 + 2 * raw[24] + ((raw[3] & DK_TX_INCREMENT_COUNTER) ? 3 : 0) +
                         ((raw[3] & DK_TX_RECALCULATE_CHECKSUM) ? 1 : 0)) &&
          n != (size_t)(25 + 3 * raw[24] + ((raw[3] & DK_TX_INCREMENT_COUNTER) ? 3 : 0) +
                         ((raw[3] & DK_TX_RECALCULATE_CHECKSUM) ? 1 : 0))))) return 0;
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
    if (raw[1] == DK_UTILIZATION) {
        if (out != DK_UTILIZATION_SIZE + 2 || raw[2] || raw[3] || dk_get(raw + 8, 4)) return 0;
        for (size_t i = 20; i < DK_UTILIZATION_SIZE; i += 2)
            if (dk_get(raw + i, 2) > 1000) return 0;
        return out;
    }
    if (raw[1] == DK_TX_FEEDBACK)
        return (out == 23 || (out == DK_TX_FEEDBACK_DIAGNOSTICS_SIZE + 2 &&
                             raw[20] >= DK_TX_UNCONFIRMED)) &&
            raw[3] == 0 && raw[20] <= DK_TX_OUTCOME_UNKNOWN &&
            ((dk_get(raw + 8, 4) == 0) == (raw[20] == DK_TX_READY)) ? out : 0;
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
         raw[1] != DK_TX_AUTO && raw[1] != DK_TX_FRAME) ||
        out < 27 || raw[24] > 64 ||
        (raw[3] & ~(15 | DK_TX_INCREMENT_COUNTER | DK_TX_RECALCULATE_CHECKSUM))) return 0;
    if ((raw[3] & (DK_TX_INCREMENT_COUNTER | DK_TX_RECALCULATE_CHECKSUM)) &&
        raw[1] != DK_TX_REQUEST && raw[1] != DK_TX_REPEAT && raw[1] != DK_TX_AUTO) return 0;
    if (raw[1] == DK_TX_FRAME && !dk_get(raw + 8, 4)) return 0;
    const bool timed = raw[1] == DK_TX_REPEAT || raw[1] == DK_TX_AUTO;
    const size_t counter_bytes = (raw[3] & DK_TX_INCREMENT_COUNTER) ? 3 : 0;
    const size_t checksum_bytes = (raw[3] & DK_TX_RECALCULATE_CHECKSUM) ? 1 : 0;
    const bool has_selector = timed && out == (size_t)(27 + 3 * raw[24] + counter_bytes + checksum_bytes);
    if (out != (size_t)(27 + raw[24] * (timed ? 2 : 1) + counter_bytes + checksum_bytes) && !has_selector) return 0;
    if (!(raw[3] & 4) && raw[24] > 8) return 0;
    if ((raw[3] & 4) && ((raw[3] & 2) ||
        (raw[24] > 8 && raw[24] != 12 && raw[24] != 16 && raw[24] != 20 &&
         raw[24] != 24 && raw[24] != 32 && raw[24] != 48 && raw[24] != 64))) return 0;
    if ((raw[3] & 8) && !(raw[3] & 4)) return 0;
    if (dk_get(raw + 20, 4) > ((raw[3] & 1) ? 0x1fffffffU : 0x7ffU)) return 0;
    if (counter_bytes) {
        const size_t at = out - 2 - checksum_bytes - counter_bytes;
        const uint16_t start = (uint16_t)dk_get(raw + at, 2);
        const uint8_t size = raw[at + 2] & 0x7f;
        if (!size || size > 64 || start >= raw[24] * 8) return 0;
        for (uint8_t i = 0; i < size; i++) {
            const uint16_t bit = (raw[at + 2] & 0x80) ? start + i :
                (uint16_t)(((start ^ 7) + i) ^ 7);
            if (bit >= raw[24] * 8) return 0;
        }
    }
    if (checksum_bytes && raw[out - 3] >= raw[24]) return 0;
    if (raw[1] == DK_TX_REPEAT || raw[1] == DK_TX_AUTO) {
        uint32_t hz = dk_get(raw + 12, 4), duration = dk_get(raw + 16, 4);
        if (!dk_get(raw + 8, 4) ||
            (raw[1] == DK_TX_REPEAT ? (!hz || hz > DK_TX_MAX_HZ) : hz != 0) ||
            duration < DK_TX_MIN_DURATION_MS || duration > DK_TX_MAX_DURATION_MS) return 0;
        bool any_mask = false;
        for (uint8_t i = 0; i < raw[24]; i++) {
            uint8_t overwrite = raw[25 + raw[24] + i];
            if (has_selector && (overwrite & raw[25 + 2 * raw[24] + i])) return 0;
            any_mask |= overwrite != 0;
        }
        if (!any_mask) return 0;
    }
    return out;
}
