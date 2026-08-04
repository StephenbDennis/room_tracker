/* HLK-LD2450 frame parsing and configuration command building.
 *
 * Pure C: no ESP-IDF dependencies, so this compiles and tests on the host.
 * See docs/protocol.md for the wire format.
 */
#ifndef LD2450_H
#define LD2450_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LD2450_MAX_TARGETS   3
#define LD2450_FRAME_LEN     30
#define LD2450_BAUD          256000

/* A single target as reported by the radar, in the sensor's own frame.
 * +y runs forward along boresight, +x to the sensor's right. */
typedef struct {
    int16_t  x_mm;
    int16_t  y_mm;
    int16_t  speed_cms;      /* + approaching, - receding */
    uint16_t resolution_mm;
} ld2450_target_t;

typedef struct {
    ld2450_target_t targets[LD2450_MAX_TARGETS];
    uint8_t         count;   /* number of populated entries, compacted to the front */
} ld2450_frame_t;

/* Streaming parser. Feed it arbitrary chunks of UART bytes; it resynchronises
 * on the header after garbage or a torn frame. Zero-initialise before use. */
typedef struct {
    uint8_t  buf[LD2450_FRAME_LEN];
    uint8_t  len;
    uint32_t frames_ok;
    uint32_t frames_dropped;   /* bad footer */
    uint32_t bytes_discarded;  /* resync slips */
} ld2450_parser_t;

/* Decode the LD2450's sign-magnitude encoding.
 *
 * This is NOT two's complement. The high bit is a sign flag where SET means
 * positive, and the low 15 bits carry the magnitude. Published sources
 * disagree on the polarity; it is pinned by the datasheet's own example frame,
 * which is asserted in test_ld2450.c. */
static inline int16_t ld2450_decode_signed(uint16_t raw)
{
    int16_t mag = (int16_t)(raw & 0x7FFF);
    return (raw & 0x8000) ? mag : (int16_t)-mag;
}

/* Decode one complete 30-byte frame. Returns false if header or footer is
 * wrong. Absent targets (all-zero blocks) are skipped, so out->count reflects
 * only real detections and out->targets is compacted to the front. */
bool ld2450_parse_frame(const uint8_t *buf, ld2450_frame_t *out);

/* Feed streaming bytes. Invokes nothing; instead returns true and fills *out
 * each time a frame completes. Call repeatedly with the same chunk offset
 * advanced by *consumed until it returns false. */
bool ld2450_parser_feed(ld2450_parser_t *p, const uint8_t *data, size_t len,
                        size_t *consumed, ld2450_frame_t *out);

/* Build a configuration command frame:
 *   FD FC FB FA | len(u16 LE) | word(u16 LE) | value... | 04 03 02 01
 * Returns bytes written, or 0 if the output buffer is too small.
 * Commands must be bracketed by ENABLE_CONFIG / END_CONFIG. */
size_t ld2450_build_command(uint8_t *out, size_t out_cap, uint16_t word,
                            const uint8_t *value, size_t value_len);

#define LD2450_CMD_ENABLE_CONFIG   0x00FF
#define LD2450_CMD_END_CONFIG      0x00FE
#define LD2450_CMD_SINGLE_TARGET   0x0080
#define LD2450_CMD_MULTI_TARGET    0x0090
#define LD2450_CMD_QUERY_MODE      0x0091
#define LD2450_CMD_BLUETOOTH       0x00A4
#define LD2450_CMD_GET_MAC         0x00A5

#endif /* LD2450_H */
