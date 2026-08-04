#include "../main/ld2450.h"
#include "test.h"

TEST_STATE_DEFS

/* The example frame printed in the LD2450 serial protocol datasheet.
 * One target present, slots 2 and 3 empty. */
static const uint8_t DATASHEET_FRAME[LD2450_FRAME_LEN] = {
    0xAA, 0xFF, 0x03, 0x00,
    0x0E, 0x03, 0xB1, 0x86, 0x10, 0x00, 0x40, 0x01,   /* target 1 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* target 2: absent */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* target 3: absent */
    0x55, 0xCC,
};

/* This is the test that pins the sign-magnitude convention. Published sources
 * contradict each other on the polarity of the high bit; the inverted reading
 * would place this target 1713 mm BEHIND a sensor that has no rear lobe. */
static void test_datasheet_example_frame(void)
{
    ld2450_frame_t f;
    CHECK(ld2450_parse_frame(DATASHEET_FRAME, &f));
    CHECK_INT(f.count, 1);
    CHECK_INT(f.targets[0].x_mm,          -782);
    CHECK_INT(f.targets[0].y_mm,          1713);
    CHECK_INT(f.targets[0].speed_cms,      -16);
    CHECK_INT(f.targets[0].resolution_mm,  320);
}

static void test_sign_decode_both_branches(void)
{
    /* High bit set => positive, magnitude in the low 15 bits. */
    CHECK_INT(ld2450_decode_signed(0x8000), 0);
    CHECK_INT(ld2450_decode_signed(0x8001), 1);
    CHECK_INT(ld2450_decode_signed(0x86B1), 1713);
    CHECK_INT(ld2450_decode_signed(0xFFFF), 32767);

    /* High bit clear => negative. */
    CHECK_INT(ld2450_decode_signed(0x0000), 0);
    CHECK_INT(ld2450_decode_signed(0x0001), -1);
    CHECK_INT(ld2450_decode_signed(0x030E), -782);
    CHECK_INT(ld2450_decode_signed(0x7FFF), -32767);
}

static void test_rejects_bad_header(void)
{
    uint8_t bad[LD2450_FRAME_LEN];
    memcpy(bad, DATASHEET_FRAME, sizeof bad);
    bad[1] = 0xFE;

    ld2450_frame_t f;
    CHECK(!ld2450_parse_frame(bad, &f));
}

static void test_rejects_bad_footer(void)
{
    uint8_t bad[LD2450_FRAME_LEN];
    memcpy(bad, DATASHEET_FRAME, sizeof bad);
    bad[LD2450_FRAME_LEN - 1] = 0x00;

    ld2450_frame_t f;
    CHECK(!ld2450_parse_frame(bad, &f));
}

static void test_empty_frame_yields_no_targets(void)
{
    uint8_t empty[LD2450_FRAME_LEN] = {
        0xAA, 0xFF, 0x03, 0x00,
    };
    memset(empty + 4, 0x00, 24);
    empty[28] = 0x55;
    empty[29] = 0xCC;

    ld2450_frame_t f;
    CHECK(ld2450_parse_frame(empty, &f));
    CHECK_INT(f.count, 0);
}

/* A target sitting exactly at the sensor origin decodes to all-zero values but
 * must not be confused with an absent slot. The raw encoding distinguishes
 * them: a real target at the origin still carries a resolution value. */
static void test_origin_target_is_not_treated_as_absent(void)
{
    uint8_t frame[LD2450_FRAME_LEN];
    memcpy(frame, DATASHEET_FRAME, sizeof frame);
    /* x=0, y=0, speed=0 encoded as sign-magnitude positive zero, res=320 */
    frame[4] = 0x00; frame[5]  = 0x80;
    frame[6] = 0x00; frame[7]  = 0x80;
    frame[8] = 0x00; frame[9]  = 0x80;
    frame[10] = 0x40; frame[11] = 0x01;

    ld2450_frame_t f;
    CHECK(ld2450_parse_frame(frame, &f));
    CHECK_INT(f.count, 1);
    CHECK_INT(f.targets[0].x_mm, 0);
    CHECK_INT(f.targets[0].y_mm, 0);
    CHECK_INT(f.targets[0].resolution_mm, 320);
}

static void test_three_targets_are_all_decoded(void)
{
    uint8_t frame[LD2450_FRAME_LEN] = { 0xAA, 0xFF, 0x03, 0x00 };
    for (int i = 0; i < 3; i++) {
        uint8_t *t = frame + 4 + (i * 8);
        uint16_t x = 0x8000 | (uint16_t)(100 * (i + 1));   /* +100, +200, +300 */
        uint16_t y = 0x8000 | (uint16_t)(1000 * (i + 1));
        t[0] = x & 0xFF; t[1] = x >> 8;
        t[2] = y & 0xFF; t[3] = y >> 8;
        t[4] = 0x00;     t[5] = 0x80;   /* speed 0 */
        t[6] = 0x40;     t[7] = 0x01;   /* res 320 */
    }
    frame[28] = 0x55; frame[29] = 0xCC;

    ld2450_frame_t f;
    CHECK(ld2450_parse_frame(frame, &f));
    CHECK_INT(f.count, 3);
    CHECK_INT(f.targets[0].x_mm, 100);
    CHECK_INT(f.targets[1].x_mm, 200);
    CHECK_INT(f.targets[2].x_mm, 300);
    CHECK_INT(f.targets[2].y_mm, 3000);
}

/* Present targets must be compacted to the front, so a gap in slot 2 does not
 * leave a hole that downstream code would read as a phantom detection. */
static void test_absent_middle_slot_is_compacted(void)
{
    uint8_t frame[LD2450_FRAME_LEN];
    memcpy(frame, DATASHEET_FRAME, sizeof frame);
    /* Populate slot 3 only, leaving slot 2 empty. */
    uint8_t *t = frame + 4 + 16;
    t[0] = 0x00; t[1] = 0x84;   /* x = +1024 */
    t[2] = 0x00; t[3] = 0x88;   /* y = +2048 */
    t[4] = 0x00; t[5] = 0x80;
    t[6] = 0x40; t[7] = 0x01;

    ld2450_frame_t f;
    CHECK(ld2450_parse_frame(frame, &f));
    CHECK_INT(f.count, 2);
    CHECK_INT(f.targets[0].x_mm, -782);   /* datasheet target */
    CHECK_INT(f.targets[1].x_mm, 1024);   /* moved down from slot 3 */
    CHECK_INT(f.targets[1].y_mm, 2048);
}

static void test_streaming_parser_finds_frame(void)
{
    ld2450_parser_t p = {0};
    ld2450_frame_t f;
    size_t consumed = 0;

    CHECK(ld2450_parser_feed(&p, DATASHEET_FRAME, LD2450_FRAME_LEN, &consumed, &f));
    CHECK_INT(consumed, LD2450_FRAME_LEN);
    CHECK_INT(f.count, 1);
    CHECK_INT(f.targets[0].y_mm, 1713);
    CHECK_INT(p.frames_ok, 1);
}

static void test_streaming_parser_resyncs_after_garbage(void)
{
    uint8_t stream[8 + LD2450_FRAME_LEN];
    /* Leading noise, including a near-miss header prefix (AA FF 03 01). */
    const uint8_t noise[8] = { 0x11, 0x22, 0xAA, 0xFF, 0x03, 0x01, 0x99, 0xAA };
    memcpy(stream, noise, sizeof noise);
    memcpy(stream + sizeof noise, DATASHEET_FRAME, LD2450_FRAME_LEN);

    ld2450_parser_t p = {0};
    ld2450_frame_t f;
    size_t consumed = 0;

    CHECK(ld2450_parser_feed(&p, stream, sizeof stream, &consumed, &f));
    CHECK_INT(f.count, 1);
    CHECK_INT(f.targets[0].y_mm, 1713);
    CHECK_INT(p.frames_ok, 1);
}

/* UART reads arrive in arbitrary chunks; a frame split across reads must still
 * assemble. */
static void test_streaming_parser_handles_split_frame(void)
{
    ld2450_parser_t p = {0};
    ld2450_frame_t f;
    size_t consumed = 0;

    CHECK(!ld2450_parser_feed(&p, DATASHEET_FRAME, 7, &consumed, &f));
    CHECK(!ld2450_parser_feed(&p, DATASHEET_FRAME + 7, 11, &consumed, &f));
    CHECK(ld2450_parser_feed(&p, DATASHEET_FRAME + 18,
                             LD2450_FRAME_LEN - 18, &consumed, &f));
    CHECK_INT(f.count, 1);
    CHECK_INT(f.targets[0].y_mm, 1713);
}

static void test_streaming_parser_reads_back_to_back_frames(void)
{
    uint8_t stream[LD2450_FRAME_LEN * 2];
    memcpy(stream, DATASHEET_FRAME, LD2450_FRAME_LEN);
    memcpy(stream + LD2450_FRAME_LEN, DATASHEET_FRAME, LD2450_FRAME_LEN);

    ld2450_parser_t p = {0};
    ld2450_frame_t f;
    size_t off = 0, consumed = 0;
    int found = 0;

    while (off < sizeof stream) {
        if (ld2450_parser_feed(&p, stream + off, sizeof stream - off, &consumed, &f)) {
            found++;
        }
        off += consumed;
        if (consumed == 0) break;
    }
    CHECK_INT(found, 2);
    CHECK_INT(p.frames_ok, 2);
}

static void test_build_enable_config_command(void)
{
    const uint8_t expect[] = {
        0xFD, 0xFC, 0xFB, 0xFA,   /* header */
        0x04, 0x00,               /* length: word(2) + value(2) */
        0xFF, 0x00,               /* enable configuration */
        0x01, 0x00,               /* value */
        0x04, 0x03, 0x02, 0x01,   /* footer */
    };
    const uint8_t value[2] = { 0x01, 0x00 };

    uint8_t out[32];
    size_t n = ld2450_build_command(out, sizeof out,
                                    LD2450_CMD_ENABLE_CONFIG, value, 2);
    CHECK_INT(n, sizeof expect);
    CHECK_BYTES(out, expect, sizeof expect);
}

static void test_build_end_config_command(void)
{
    const uint8_t expect[] = {
        0xFD, 0xFC, 0xFB, 0xFA,
        0x02, 0x00,               /* length: word only */
        0xFE, 0x00,
        0x04, 0x03, 0x02, 0x01,
    };

    uint8_t out[32];
    size_t n = ld2450_build_command(out, sizeof out,
                                    LD2450_CMD_END_CONFIG, NULL, 0);
    CHECK_INT(n, sizeof expect);
    CHECK_BYTES(out, expect, sizeof expect);
}

static void test_build_bluetooth_off_command(void)
{
    const uint8_t expect[] = {
        0xFD, 0xFC, 0xFB, 0xFA,
        0x04, 0x00,
        0xA4, 0x00,
        0x00, 0x00,               /* off */
        0x04, 0x03, 0x02, 0x01,
    };
    const uint8_t value[2] = { 0x00, 0x00 };

    uint8_t out[32];
    size_t n = ld2450_build_command(out, sizeof out,
                                    LD2450_CMD_BLUETOOTH, value, 2);
    CHECK_INT(n, sizeof expect);
    CHECK_BYTES(out, expect, sizeof expect);
}

static void test_build_command_rejects_small_buffer(void)
{
    uint8_t out[4];
    CHECK_INT(ld2450_build_command(out, sizeof out,
                                   LD2450_CMD_END_CONFIG, NULL, 0), 0);
}

int main(void)
{
    printf("\nld2450\n");
    RUN_TEST(test_datasheet_example_frame);
    RUN_TEST(test_sign_decode_both_branches);
    RUN_TEST(test_rejects_bad_header);
    RUN_TEST(test_rejects_bad_footer);
    RUN_TEST(test_empty_frame_yields_no_targets);
    RUN_TEST(test_origin_target_is_not_treated_as_absent);
    RUN_TEST(test_three_targets_are_all_decoded);
    RUN_TEST(test_absent_middle_slot_is_compacted);
    RUN_TEST(test_streaming_parser_finds_frame);
    RUN_TEST(test_streaming_parser_resyncs_after_garbage);
    RUN_TEST(test_streaming_parser_handles_split_frame);
    RUN_TEST(test_streaming_parser_reads_back_to_back_frames);
    RUN_TEST(test_build_enable_config_command);
    RUN_TEST(test_build_end_config_command);
    RUN_TEST(test_build_bluetooth_off_command);
    RUN_TEST(test_build_command_rejects_small_buffer);
    return TEST_SUMMARY("ld2450");
}
