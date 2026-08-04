#include "ld2450.h"

#include <string.h>

static const uint8_t FRAME_HEADER[4] = { 0xAA, 0xFF, 0x03, 0x00 };
static const uint8_t FRAME_FOOTER[2] = { 0x55, 0xCC };
static const uint8_t CMD_HEADER[4]   = { 0xFD, 0xFC, 0xFB, 0xFA };
static const uint8_t CMD_FOOTER[4]   = { 0x04, 0x03, 0x02, 0x01 };

static inline uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

bool ld2450_parse_frame(const uint8_t *buf, ld2450_frame_t *out)
{
    if (memcmp(buf, FRAME_HEADER, sizeof FRAME_HEADER) != 0) {
        return false;
    }
    if (memcmp(buf + LD2450_FRAME_LEN - 2, FRAME_FOOTER, sizeof FRAME_FOOTER) != 0) {
        return false;
    }

    memset(out, 0, sizeof *out);

    for (int i = 0; i < LD2450_MAX_TARGETS; i++) {
        const uint8_t *t = buf + 4 + (i * 8);

        /* An all-zero block means this slot holds no target. Checking the raw
         * bytes is the only reliable test: after sign decoding, an absent
         * target and a target at the origin both read as zero. */
        bool empty = true;
        for (int b = 0; b < 8; b++) {
            if (t[b] != 0x00) { empty = false; break; }
        }
        if (empty) {
            continue;
        }

        ld2450_target_t *tgt = &out->targets[out->count++];
        tgt->x_mm          = ld2450_decode_signed(rd_u16le(t + 0));
        tgt->y_mm          = ld2450_decode_signed(rd_u16le(t + 2));
        tgt->speed_cms     = ld2450_decode_signed(rd_u16le(t + 4));
        tgt->resolution_mm = rd_u16le(t + 6);
    }

    return true;
}

bool ld2450_parser_feed(ld2450_parser_t *p, const uint8_t *data, size_t len,
                        size_t *consumed, ld2450_frame_t *out)
{
    size_t i = 0;

    while (i < len) {
        p->buf[p->len++] = data[i++];

        /* Validate the header incrementally so garbage costs at most one byte
         * of resync rather than a whole frame window. */
        if (p->len <= sizeof FRAME_HEADER) {
            if (p->buf[p->len - 1] != FRAME_HEADER[p->len - 1]) {
                /* Slide left by one and retry: the byte that broke the match
                 * may itself begin a valid header. */
                p->bytes_discarded++;
                if (p->len > 1) {
                    memmove(p->buf, p->buf + 1, p->len - 1);
                }
                p->len--;
                /* Re-test the retained prefix against the header. */
                while (p->len > 0 && memcmp(p->buf, FRAME_HEADER, p->len) != 0) {
                    p->bytes_discarded++;
                    if (p->len > 1) {
                        memmove(p->buf, p->buf + 1, p->len - 1);
                    }
                    p->len--;
                }
            }
            continue;
        }

        if (p->len == LD2450_FRAME_LEN) {
            p->len = 0;
            if (ld2450_parse_frame(p->buf, out)) {
                p->frames_ok++;
                *consumed = i;
                return true;
            }
            /* Footer mismatch: the stream is misaligned. Drop and resync. */
            p->frames_dropped++;
        }
    }

    *consumed = i;
    return false;
}

size_t ld2450_build_command(uint8_t *out, size_t out_cap, uint16_t word,
                            const uint8_t *value, size_t value_len)
{
    /* header(4) + len(2) + word(2) + value + footer(4) */
    size_t total = 4 + 2 + 2 + value_len + 4;
    if (out_cap < total) {
        return 0;
    }

    /* The length field covers the command word plus its value, not the
     * header, the length field itself, or the footer. */
    uint16_t payload_len = (uint16_t)(2 + value_len);

    size_t o = 0;
    memcpy(out + o, CMD_HEADER, 4);            o += 4;
    out[o++] = (uint8_t)(payload_len & 0xFF);
    out[o++] = (uint8_t)(payload_len >> 8);
    out[o++] = (uint8_t)(word & 0xFF);
    out[o++] = (uint8_t)(word >> 8);
    if (value_len > 0) {
        memcpy(out + o, value, value_len);     o += value_len;
    }
    memcpy(out + o, CMD_FOOTER, 4);            o += 4;

    return o;
}
