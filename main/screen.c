#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "lvgl.h"
#include "lvgl__lvgl/src/misc/cache/instance/lv_image_cache.h"
#include "esp_lvgl_port.h"
#include "global_state.h"
#include "screen.h"
#include "nvs_config.h"
#include "display.h"
#include "esp_timer.h"

typedef enum {
    SCR_SELF_TEST,
    SCR_OVERHEAT,
    SCR_ASIC_STATUS,
    SCR_WELCOME,
    SCR_FIRMWARE,
    SCR_CONNECTION,
    SCR_BITAXE_LOGO,    // oGi
    SCR_BLOCKS_FOUND,   // blocks found this session
    SCR_HASHRATE,       // hashrate + sparkline
    SCR_PICKAXE,        // swinging pickaxe animation
    SCR_CLOCK,          // 24-hour wall clock
    SCR_DATE,           // "25 Oct"
    MAX_SCREENS,
} screen_t;

#define SCREEN_UPDATE_MS 500
#define BUTTON_WAKE_MS 5000

// The carousel runs from here to the last screen and wraps back. Upstream started it at
// SCR_URLS, which left SCR_BITAXE_LOGO below the wrap point and therefore never displayed;
// starting at the logo screen puts the custom name first in every rotation.
#define SCR_CAROUSEL_START SCR_BITAXE_LOGO

// Big custom name shown on the Bitaxe logo screen, in place of the logo and the
// board version, so the text can use the full panel height. Leave empty to keep the
// stock logo screen untouched.
#define BITAXE_DISPLAY_NAME "oGi"

#define BITAXE_LOGO_INVERTED_SIZE (77 * 30 * 2)
#define OSMU_LOGO_INVERTED_SIZE (125 * 27 * 2)

extern const lv_img_dsc_t bitaxe_logo;
extern const lv_img_dsc_t bitaxe_logo_large;
extern const lv_img_dsc_t osmu_logo;
extern const lv_img_dsc_t identify_text;
extern const lv_img_dsc_t bitaxe_background;
extern const lv_font_t ui_font_DigitalNumbers16;
extern const lv_font_t ui_font_DigitalNumbers28;
extern const lv_font_t ui_font_OpenSansBold13;
extern const lv_font_t ui_font_OpenSansBold14;
extern const lv_font_t ui_font_AngelWish40;
extern const lv_font_t ui_font_MontserratBold40;
extern const lv_font_t ui_font_MontserratBold34;
extern const lv_font_t ui_font_MontserratBold24;
extern const lv_font_t ui_font_MontserratBold20;

extern const lv_img_dsc_t pickaxe_frame_1;
extern const lv_img_dsc_t pickaxe_frame_2;
extern const lv_img_dsc_t pickaxe_frame_3;


static lv_img_dsc_t bitaxe_logo_inverted;
static lv_img_dsc_t osmu_logo_inverted;
static uint8_t bitaxe_logo_inverted_map[BITAXE_LOGO_INVERTED_SIZE];
static uint8_t osmu_logo_inverted_map[OSMU_LOGO_INVERTED_SIZE];
static bool inverted_logos_ready;

static lv_obj_t * screens[MAX_SCREENS];
// Dwell per screen, indexed by screen_t. Priority screens (0-5) barely matter; every
// carousel screen shows for the same 5 s so the rotation is even.
static int delays_ms[MAX_SCREENS] = {
    0, 0, 0, 0, 0, 1000,   // self-test .. connection
    5000,                  // oGi
    5000,                  // blocks found
    5000,                  // hashrate graph
    5000,                  // pickaxe
    5000,                  // clock
    5000,                  // date
};

static int current_screen_time_ms;
static int current_screen_delay_ms;

// static int screen_chars;
static int screen_lines;

static GlobalState * GLOBAL_STATE;

static lv_obj_t *self_test_message_label;
static lv_obj_t *self_test_result_label;
static lv_obj_t *self_test_finished_label;

static lv_obj_t *overheat_ip_addr_label;

static lv_obj_t *asic_status_label;

static lv_obj_t *firmware_update_scr_filename_label;
static lv_obj_t *firmware_update_scr_status_label;

static lv_obj_t *connection_wifi_status_label;

static lv_obj_t *blocks_found_count_label;
static lv_obj_t *blocks_found_caption_label;

// --- new dashboard screens ------------------------------------------------
static lv_obj_t *hashrate_value_label;   // "1.05 TH/s"
static lv_obj_t *hashrate_line;          // sparkline

static lv_obj_t *date_value_label;       // "25 Oct"

static lv_obj_t *pickaxe_image;

// Wall clock: four fixed 28px digit cells plus a fixed colon, so the layout never
// reflows when a digit changes width (see create_scr_clock()).
static lv_obj_t *clock_digit_label[4];
static lv_obj_t *clock_colon_label;
static char clock_shown[5];

// Sparkline history for the hashrate screen: one sample per SCREEN_UPDATE_MS tick, oldest
// first once wrapped. (The temperature screen has no sparkline any more: the 34px value
// inks down to row 27 and the old lv_line sat on rows 24..31, drawing a black line straight
// through the digits. It never rendered as a chart in the first place -- see section 8 of
// the handoff -- so it was pure damage there.)
#define SPARK_POINTS 64
#define SPARK_HEIGHT 8
static lv_point_precise_t hashrate_points[SPARK_POINTS];
static float hashrate_hist[SPARK_POINTS];
static int spark_head;      // next write index
static int spark_filled;    // number of valid samples (<= SPARK_POINTS)
static uint32_t pickaxe_tick;

static lv_obj_t *notification_label;
static lv_obj_t *identify_image;


#define NOTIFICATION_SHARE_ACCEPTED (1 << 0)
#define NOTIFICATION_SHARE_REJECTED (1 << 1)
#define NOTIFICATION_WORK_RECEIVED  (1 << 2)

static const char *notifications[] = {
    "",     // 0b000: NONE
    "↑",    // 0b001:                   ACCEPTED
    "x",    // 0b010:          REJECTED
    "x↑",   // 0b011:          REJECTED ACCEPTED
    "↓",    // 0b100: RECEIVED
    "↕",    // 0b101: RECEIVED          ACCEPTED
    "x↓",   // 0b110: RECEIVED REJECTED 
    "x↕"    // 0b111: RECEIVED REJECTED ACCEPTED
};

static uint64_t current_shares_accepted;
static uint64_t current_shares_rejected;
static uint64_t current_work_received;

static screen_t get_current_screen() {
    lv_obj_t * active_screen = lv_screen_active();
    for (screen_t scr = 0; scr < MAX_SCREENS; scr++) {
        if (screens[scr] == active_screen) return scr;
    }
    return -1;
}

static bool is_large_display(void) {
    return lv_display_get_vertical_resolution(NULL) > 128;
}

static void apply_screen_padding(lv_obj_t * obj) {
    if (is_large_display()) {
        lv_obj_set_style_pad_all(obj, 8, LV_PART_MAIN);
    }
}

static void make_inverted_logo(const lv_img_dsc_t * src, lv_img_dsc_t * dst, uint8_t * dst_map, size_t dst_map_size) {
    size_t data_size = src->data_size;
    if (data_size > dst_map_size) {
        data_size = dst_map_size;
    }

    *dst = *src;
    dst->data = dst_map;
    dst->data_size = data_size;

    for (size_t i = 0; i + 1 < data_size; i += 2) {
        uint16_t pixel = src->data[i] | (src->data[i + 1] << 8);
        uint16_t inverted_pixel = pixel == 0xffff ? 0x0000 : 0xffff;
        dst_map[i] = inverted_pixel & 0xff;
        dst_map[i + 1] = inverted_pixel >> 8;
    }
}

static void init_inverted_logos(void) {
    if (inverted_logos_ready) {
        return;
    }

    make_inverted_logo(&bitaxe_logo, &bitaxe_logo_inverted, bitaxe_logo_inverted_map, sizeof(bitaxe_logo_inverted_map));
    make_inverted_logo(&osmu_logo, &osmu_logo_inverted, osmu_logo_inverted_map, sizeof(osmu_logo_inverted_map));
    lv_image_cache_drop(&bitaxe_logo);
    lv_image_cache_drop(&osmu_logo);
    lv_image_cache_drop(&bitaxe_logo_inverted);
    lv_image_cache_drop(&osmu_logo_inverted);
    inverted_logos_ready = true;
}

static const lv_img_dsc_t * get_bitaxe_logo_src(void) {
    if (is_large_display()) {
        init_inverted_logos();
        return &bitaxe_logo_inverted;
    }

    return &bitaxe_logo;
}

static lv_obj_t * create_flex_screen(int expected_lines) {
    lv_obj_t * scr = lv_obj_create(NULL);
    apply_screen_padding(scr);

    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // Give text a bit more space on larger displays
    if (screen_lines > expected_lines) lv_obj_set_style_pad_row(scr, 1, LV_PART_MAIN);

    return scr;
}

static lv_obj_t * create_scr_self_test() {
    lv_obj_t * scr = create_flex_screen(4);

    lv_obj_t *label1 = lv_label_create(scr);
    lv_label_set_text(label1, "BITAXE SELF-TEST");

    self_test_message_label = lv_label_create(scr);
    self_test_result_label = lv_label_create(scr);
    self_test_finished_label = lv_label_create(scr);

    return scr;
}

static lv_obj_t * create_scr_overheat() {
    lv_obj_t * scr = create_flex_screen(4);

    lv_obj_t *label1 = lv_label_create(scr);
    lv_label_set_text(label1, "DEVICE OVERHEAT!");

    lv_obj_t *label2 = lv_label_create(scr);
    lv_obj_set_width(label2, LV_HOR_RES);
    lv_label_set_long_mode(label2, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(label2, "Power, frequency and fan configurations have been reset. Go to AxeOS to reconfigure device.");

    lv_obj_t *label3 = lv_label_create(scr);
    lv_label_set_text(label3, "IP Address:");

    overheat_ip_addr_label = lv_label_create(scr);

    return scr;
}

static lv_obj_t * create_scr_asic_status() {
    lv_obj_t * scr = create_flex_screen(2);

    lv_obj_t *label1 = lv_label_create(scr);
    lv_label_set_text(label1, "ASIC STATUS:");

    asic_status_label = lv_label_create(scr);
    lv_label_set_long_mode(asic_status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);

    return scr;
}

static lv_obj_t * create_screen_with_qr(const char * ap_ssid, int expected_lines, lv_obj_t ** out_text_cont) {
    lv_obj_t * scr = lv_obj_create(NULL);
    apply_screen_padding(scr);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(scr, is_large_display() ? 8 : 2, LV_PART_MAIN);

    lv_obj_t * text_cont = lv_obj_create(scr);
    lv_obj_set_style_bg_opa(text_cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_opa(text_cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(text_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_flex_grow(text_cont, 1);
    lv_obj_set_height(text_cont, LV_VER_RES);

    // Give text a bit more space on larger displays
    if (screen_lines > expected_lines) lv_obj_set_style_pad_row(text_cont, 1, LV_PART_MAIN);

    lv_obj_t * qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(qr, is_large_display() ? 72 : 32);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());

    char data[64];
    snprintf(data, sizeof(data), "WIFI:S:%s;;", ap_ssid);
    lv_qrcode_update(qr, data, strlen(data));

    *out_text_cont = text_cont;
    return scr;
}

static lv_obj_t * create_scr_welcome(const char * ap_ssid) {
    lv_obj_t * text_cont;
    lv_obj_t * scr = create_screen_with_qr(ap_ssid, 3, &text_cont);

    lv_obj_t *label1 = lv_label_create(text_cont);
    lv_obj_set_width(label1, lv_pct(100));
    lv_obj_set_style_anim_duration(label1, 15000, LV_PART_MAIN);
    lv_label_set_long_mode(label1, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(label1, "Welcome to your new Bitaxe! Connect to the configuration Wi-Fi and connect the Bitaxe to your network.");

    // add a bit of padding, it looks nicer this way
    lv_obj_set_style_pad_bottom(label1, 4, LV_PART_MAIN);

    lv_obj_t *label2 = lv_label_create(text_cont);
    lv_label_set_text(label2, "Setup Wi-Fi:");

    lv_obj_t *label3 = lv_label_create(text_cont);
    lv_label_set_text(label3, ap_ssid);

    return scr;
}

static lv_obj_t * create_scr_firmware() {
    lv_obj_t * scr = create_flex_screen(3);

    lv_obj_t *label1 = lv_label_create(scr);
    lv_obj_set_width(label1, LV_HOR_RES);
    lv_label_set_text(label1, "Firmware update");

    firmware_update_scr_filename_label = lv_label_create(scr);

    firmware_update_scr_status_label = lv_label_create(scr);

    return scr;
}

static lv_obj_t * create_scr_connection(const char * ssid, const char * ap_ssid) {
    lv_obj_t * text_cont;
    lv_obj_t * scr = create_screen_with_qr(ap_ssid, 4, &text_cont);

    lv_obj_t *label1 = lv_label_create(text_cont);
    lv_obj_set_width(label1, lv_pct(100));
    lv_label_set_long_mode(label1, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text_fmt(label1, "Wi-Fi: %s", ssid);

    connection_wifi_status_label = lv_label_create(text_cont);
    lv_obj_set_width(connection_wifi_status_label, lv_pct(100));
    lv_label_set_long_mode(connection_wifi_status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);

    lv_obj_t *label3 = lv_label_create(text_cont);
    lv_label_set_text(label3, "Setup Wi-Fi:");

    lv_obj_t *label4 = lv_label_create(text_cont);
    lv_label_set_text(label4, ap_ssid);

    return scr;
}

// Renders BITAXE_DISPLAY_NAME as large as the panel allows, replacing the logo screen.
static lv_obj_t * create_scr_custom_name(const char * name) {
    lv_obj_t * scr = lv_obj_create(NULL);
    apply_screen_padding(scr);

    // Force the content area to equal the whole panel: any theme padding or border would
    // offset the text area and push a 42px-tall line box off a 32px screen (the ink would
    // be laid out outside the visible area).
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(scr);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_font(label, &ui_font_MontserratBold40, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);

    // ui_font_MontserratBold40 is generated so that its tallest glyph ("i" plus its
    // dot) is exactly 32px tall, i.e. the ink fills a 128x32 panel top to bottom.
    // LVGL draws a glyph at (line_height - base_line) - box_h - ofs_y below the top
    // of the label, and this line box (42px) is taller than that panel, so centring
    // the box would push the ink off the top edge. Anchor on the baseline instead.
    // Panels taller than the line box are centred as usual.
    const int32_t panel_height = lv_display_get_vertical_resolution(NULL);
    if (panel_height <= ui_font_MontserratBold40.line_height) {
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0,
                     panel_height - (ui_font_MontserratBold40.line_height - ui_font_MontserratBold40.base_line));
    } else {
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    }

    return scr;
}

static lv_obj_t * create_scr_bitaxe_logo(const char * name, const char * board_version) {
    if (BITAXE_DISPLAY_NAME[0] != '\0') {
        return create_scr_custom_name(BITAXE_DISPLAY_NAME);
    }

    lv_obj_t * scr = lv_obj_create(NULL);
    apply_screen_padding(scr);

    if (is_large_display()) {
        lv_obj_t *img = lv_img_create(scr);
        lv_img_set_src(img, &bitaxe_logo_large);
        lv_obj_align(img, LV_ALIGN_CENTER, -42, -8);
    } else {
        lv_obj_t *img = lv_img_create(scr);
        lv_img_set_src(img, get_bitaxe_logo_src());
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 1);
    }

    lv_obj_t *label1 = lv_label_create(scr);
    lv_label_set_text(label1, name);
    lv_obj_align(label1, LV_ALIGN_RIGHT_MID, -8, is_large_display() ? -18 : -12);

    lv_obj_t *label2 = lv_label_create(scr);
    lv_label_set_text(label2, board_version);
    lv_obj_align(label2, LV_ALIGN_RIGHT_MID, -8, is_large_display() ? 2 : -4);

    return scr;
}

static lv_obj_t * create_scr_blocks_found() {
    lv_obj_t * scr = lv_obj_create(NULL);
    apply_screen_padding(scr);

    // Same reasoning as the name screen: a 42px line box on a 32px panel is placed by
    // baseline maths, so the content area must equal the panel exactly.
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t panel_height = lv_display_get_vertical_resolution(NULL);

    blocks_found_count_label = lv_label_create(scr);
    lv_label_set_text(blocks_found_count_label, "0");
    lv_obj_set_style_text_font(blocks_found_count_label, &ui_font_MontserratBold40, LV_PART_MAIN);
    lv_obj_set_style_pad_all(blocks_found_count_label, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blocks_found_count_label, LV_OPA_TRANSP, LV_PART_MAIN);

    blocks_found_caption_label = lv_label_create(scr);
    lv_label_set_text(blocks_found_caption_label, "found");
    lv_obj_set_style_text_font(blocks_found_caption_label, &ui_font_MontserratBold20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(blocks_found_caption_label, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blocks_found_caption_label, LV_OPA_TRANSP, LV_PART_MAIN);

    // Digit and caption are laid out as one centred group: the digit's ink is 24px wide and
    // the 20px caption is 63px, so 13 + 27 + 12 + 63 = 115 puts the pair on rows 4..27 with
    // ~13px air on both sides, and leaves a visible gap between the two (it used to be 2px).
    const int32_t digit_x = 13;
    const int32_t caption_x = 52;

    if (panel_height <= ui_font_MontserratBold40.line_height) {
        // Baseline-anchored so the digit fills the panel height, exactly like the name screen.
        lv_obj_align(blocks_found_count_label, LV_ALIGN_TOP_LEFT, digit_x,
                     panel_height - (ui_font_MontserratBold40.line_height - ui_font_MontserratBold40.base_line));
        lv_obj_align(blocks_found_caption_label, LV_ALIGN_LEFT_MID, caption_x, 0);
    } else {
        lv_obj_align(blocks_found_count_label, LV_ALIGN_LEFT_MID, digit_x, 0);
        lv_obj_align(blocks_found_caption_label, LV_ALIGN_LEFT_MID, caption_x, 0);
    }

    return scr;
}

// ---------------------------------------------------------------------------
// Dashboard screens. Each one fills the whole 128x32 panel: a value in the largest
// font that can spell it, with a chart/bars/picture below or beside it.
// ---------------------------------------------------------------------------

static lv_obj_t * create_blank_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    apply_screen_padding(scr);
    if (!is_large_display()) {
        // A 42px line box / 14px rows are placed by exact pixel maths on a 32px panel, so
        // the content area must equal the panel: no theme padding, no border.
        lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    }
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

// Value in the largest face that still fits a whole "1.05 TH/s" (24px, ~110px wide). Baseline
// anchored: the 24px face puts its baseline 21px below the label top, so this parks the ink on
// rows 0..23 and leaves 24..31 for the chart.
#define VALUE_BASELINE_ROW 23
static lv_obj_t * make_value_label(lv_obj_t * parent) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, "--");
    lv_obj_set_style_text_font(label, &ui_font_MontserratBold24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
    int32_t label_y = VALUE_BASELINE_ROW - (ui_font_MontserratBold24.line_height - ui_font_MontserratBold24.base_line);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, label_y);
    return label;
}

// Temperature and date have no width problem, so they get the 34px face: the widest date
// ("25 May"/"25 Aug") is 124px and "58.8 C" is 108px, both inside 128px. There is no spare
// room in the 24px face's layout for this, so it is a separate helper rather than a knob on
// make_value_label(): the hashrate value genuinely needs 24px to fit "1.05 TH/s" (108px).
// At 34px a digit's ink is 24px tall, so label_y = -2 puts it on rows 4..27 of 32.
#define BIG_VALUE_LABEL_Y (-2)
static lv_obj_t * make_big_value_label(lv_obj_t * parent) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, "--");
    lv_obj_set_style_text_font(label, &ui_font_MontserratBold34, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, BIG_VALUE_LABEL_Y);
    return label;
}

static lv_obj_t * make_sparkline(lv_obj_t * parent) {
    lv_obj_t * line = lv_line_create(parent);
    lv_obj_set_pos(line, 0, LV_VER_RES - SPARK_HEIGHT);
    lv_obj_set_size(line, LV_HOR_RES, SPARK_HEIGHT);
    lv_obj_set_style_line_width(line, 2, LV_PART_MAIN);
    lv_obj_set_style_line_color(line, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
    lv_obj_set_style_pad_all(line, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, LV_PART_MAIN);
    return line;
}


// Redraw a sparkline from the ring buffer: auto-scaled to the window peak, oldest on the left.
static void update_sparkline(lv_obj_t * line, const float * hist, lv_point_precise_t * pts) {
    if (!line || spark_filled < 2) {
        return;
    }
    float peak = 0.0f;
    for (int i = 0; i < spark_filled; i++) {
        if (hist[i] > peak) {
            peak = hist[i];
        }
    }
    if (peak <= 0.0f) {
        return;
    }
    const int w = LV_HOR_RES - 1;
    const int h = SPARK_HEIGHT - 1;
    for (int i = 0; i < spark_filled; i++) {
        int idx = (spark_head - spark_filled + i + SPARK_POINTS * 2) % SPARK_POINTS;
        float v = hist[idx];
        if (v < 0.0f) {
            v = 0.0f;
        }
        int y = h - (int)((v / peak) * (float)h + 0.5f);
        pts[i].x = (i * w) / (spark_filled - 1);
        pts[i].y = y;
    }
    lv_line_set_points(line, pts, spark_filled);
}

static lv_obj_t * create_scr_hashrate() {
    lv_obj_t * scr = create_blank_screen();
    hashrate_value_label = make_value_label(scr);
    hashrate_line = make_sparkline(scr);
    return scr;
}

// Date in the same shape as the other dashboard values: one label, no chart. It borrows the
// 34px face rather than the 40px one because "25 May" is 134px at 40px and would not fit.
static lv_obj_t * create_scr_date() {
    lv_obj_t * scr = create_blank_screen();
    date_value_label = make_big_value_label(scr);
    return scr;
}

static lv_obj_t * create_scr_pickaxe() {
    lv_obj_t * scr = create_blank_screen();

    // A bitmap, deliberately: on this panel's 1-bit path LVGL draws labels and images but
    // skips lv_obj fills and lv_line entirely, so shapes would be invisible.
    pickaxe_image = lv_img_create(scr);
    lv_img_set_src(pickaxe_image, &pickaxe_frame_1);
    lv_obj_align(pickaxe_image, LV_ALIGN_CENTER, 0, 0);

    return scr;
}

// ---------------------------------------------------------------------------
// Wall clock, 24-hour. In the 40px face a digit's ink is 28px tall and the whole
// "HH:MM" is 101..119px wide, so it fills the panel. Montserrat's figures are
// proportional, though: centring one label would swing the text ~9px sideways every
// time a 1 turned into a 0. So each digit gets a fixed 28px cell and is centred
// inside that cell -- the layout is pinned and only the glyphs change.
// ---------------------------------------------------------------------------
#define CLOCK_CELL_W   28
#define CLOCK_COLON_W  11
#define CLOCK_ORIGIN_X 2
// A digit's ink top sits at label_y + (line_height - base_line) - box_h - ofs_y,
// i.e. label_y + (42 - 8) - 28 = label_y + 6, so -4 centres the ink on rows 2..29.
#define CLOCK_LABEL_Y  (-4)
// newlib carries no tz database: this POSIX TZ string defines the offset and the EU
// DST rules itself (+1h from the last Sunday of March 03:00 to the last Sunday of
// October 04:00). Change this one line to move the clock to another zone.
#define CLOCK_TZ "EET-2EEST,M3.5.0/3,M10.5.0/4"

static lv_obj_t * make_clock_cell(lv_obj_t * scr, const char * text, int x, int w) {
    lv_obj_t * label = lv_label_create(scr);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &ui_font_MontserratBold40, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(label, w);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, x, CLOCK_LABEL_Y);
    return label;
}

static lv_obj_t * create_scr_clock() {
    lv_obj_t * scr = create_blank_screen();

    for (int i = 0; i < 4; i++) {
        int x = CLOCK_ORIGIN_X + i * CLOCK_CELL_W + (i >= 2 ? CLOCK_COLON_W : 0);
        clock_digit_label[i] = make_clock_cell(scr, "-", x, CLOCK_CELL_W);
    }
    clock_colon_label = make_clock_cell(scr, ":", CLOCK_ORIGIN_X + 2 * CLOCK_CELL_W, CLOCK_COLON_W);

    return scr;
}

// Refreshes HH:MM, or "--:--" until NTP has set the clock. The cells never move; only
// the glyph inside each one changes, and only when the minute does.
static void update_clock(void) {
    if (!clock_digit_label[0]) {
        return;
    }

    time_t now = time(NULL);
    struct tm local;
    char hhmm[5] = "----";
    bool valid = now >= 1600000000 && localtime_r(&now, &local) != NULL;
    if (valid) {
        snprintf(hhmm, sizeof(hhmm), "%02d%02d", local.tm_hour, local.tm_min);
    }

    if (strcmp(hhmm, clock_shown) == 0) {
        return;
    }
    memcpy(clock_shown, hhmm, sizeof(clock_shown));

    for (int i = 0; i < 4; i++) {
        char digit[2] = { hhmm[i], '\0' };
        lv_label_set_text(clock_digit_label[i], digit);
    }

    // One line on the first minute tick after boot, so the clock can be checked from the
    // log without eyes on the panel. Deliberately not logged any earlier: before NTP lands
    // the RTC still holds whatever it carried across the reset, and that value looks
    // plausible enough to fool a reader.
    static bool clock_logged;
    if (valid && !clock_logged && esp_timer_get_time() > 30LL * 1000 * 1000) {
        clock_logged = true;
        struct tm utc;
        gmtime_r(&now, &utc);
        ESP_LOGI("screen", "clock: local %02d:%02d:%02d, UTC %02d:%02d:%02d (epoch %lld)",
                 local.tm_hour, local.tm_min, local.tm_sec,
                 utc.tm_hour, utc.tm_min, utc.tm_sec, (long long) now);
    }
}

// Date for the date screen, e.g. "25 Oct". Same validity gate as the clock, and it only
// touches the label when the string actually changes (once a day).
static void update_date(void) {
    if (!date_value_label) {
        return;
    }

    time_t now = time(NULL);
    struct tm local;
    char buf[16] = "--";
    if (now >= 1600000000 && localtime_r(&now, &local) != NULL) {
        // %b is the abbreviated month from the C locale, which newlib ships as "Jan".."Dec".
        strftime(buf, sizeof(buf), "%d %b", &local);
    }

    if (strcmp(buf, lv_label_get_text(date_value_label)) != 0) {
        lv_label_set_text(date_value_label, buf);
    }
}



static void scr_create_overlay()
{
    identify_image = lv_img_create(lv_layer_top());
    lv_img_set_src(identify_image, &identify_text);
    lv_obj_align(identify_image, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(identify_image, LV_OBJ_FLAG_HIDDEN);

    notification_label = lv_label_create(lv_layer_top());
    lv_label_set_text(notification_label, "");
    lv_obj_set_style_text_color(notification_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(notification_label, LV_ALIGN_TOP_RIGHT, 0, 0);
}

// ---------------------------------------------------------------------------
// Carousel order. Explicit rather than "enum order" because the clock is meant to come
// round far more often: it is interleaved after every dashboard screen, so it is on the
// panel for 3 s out of every 6 s instead of once per full rotation (18 s before).
// Move an entry here to reorder the rotation; the dwell lives in delays_ms[].
// ---------------------------------------------------------------------------
static const screen_t carousel[] = {
    SCR_BITAXE_LOGO,  SCR_CLOCK,
    SCR_DATE,         SCR_CLOCK,
    SCR_BLOCKS_FOUND, SCR_CLOCK,
    SCR_HASHRATE,     SCR_CLOCK,
    SCR_PICKAXE,      SCR_CLOCK,
};
#define CAROUSEL_LEN (sizeof(carousel) / sizeof(carousel[0]))

// Index in carousel[] of the screen on show. Starts one slot before the first entry so the
// first screen_next() lands on carousel[0] (the oGi logo screen).
static size_t carousel_pos = CAROUSEL_LEN - 1;

// Keep carousel_pos on the screen that is actually up. A screen can be shown without going
// through screen_next() -- a found block does exactly that -- and the clock appears more
// than once in the table, so search forward from the current position to tell the
// duplicates apart.
static void carousel_note_shown(screen_t screen) {
    for (size_t i = 0; i < CAROUSEL_LEN; i++) {
        size_t pos = (carousel_pos + i) % CAROUSEL_LEN;
        if (carousel[pos] == screen) {
            carousel_pos = pos;
            return;
        }
    }
}

static bool screen_show(screen_t screen)
{
    if (!lvgl_port_lock(0)) {
        return false;
    }

    if (SCR_CAROUSEL_START > screen) {
        lv_display_trigger_activity(NULL);
    }

    bool is_valid = true;
    screen_t current_screen = get_current_screen();
    if (current_screen != screen) {
        lv_obj_t * scr = screens[screen];

        is_valid = lv_obj_is_valid(scr);
        if (is_valid) {
            // Must stay false: LVGL deletes the outgoing screen when auto_del is true, and
            // the logo screens are now part of the carousel. Once deleted, lv_obj_is_valid()
            // fails and screen_show() returns false, so the carousel would silently skip the
            // custom name screen for the rest of the session.
            bool auto_del = false;
            lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, LV_DEF_REFR_PERIOD * 128 / 8, 0, auto_del);
            carousel_note_shown(screen);
        }

        ESP_LOGI("screen", "screen_show: %d -> %d (valid=%d)", (int)current_screen, (int)screen, (int)is_valid);

        current_screen_time_ms = 0;
        current_screen_delay_ms = delays_ms[screen];
    }

    lvgl_port_unlock();

    return is_valid;
}

void screen_next()
{
    // Bounded walk: with an explicit table there is no "++enum" wrap any more, and skipping
    // an invalid screen must not spin forever.
    for (size_t step = 1; step <= CAROUSEL_LEN; step++) {
        size_t pos = (carousel_pos + step) % CAROUSEL_LEN;
        if (screen_show(carousel[pos])) {
            carousel_pos = pos;
            return;
        }
    }
}

static void screen_update_cb(lv_timer_t * timer)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    // Keep the blocks-found counter current. Cheap to do unconditionally, and it means the
    // number is already correct the instant that screen rotates in.
    if (blocks_found_count_label) {
        char found_buf[12];
        snprintf(found_buf, sizeof(found_buf), "%d", module->block_found);
        if (strcmp(lv_label_get_text(blocks_found_count_label), found_buf) != 0) {
            lv_label_set_text(blocks_found_count_label, found_buf);
        }
    }

    int32_t display_timeout_config = nvs_config_get_i32(NVS_CONFIG_DISPLAY_TIMEOUT);

    uint32_t inactive_time = lv_display_get_inactive_time(NULL);
    screen_t current_screen = get_current_screen();

    if (module->identify_mode_time_ms > 0) {
        module->identify_mode_time_ms -= SCREEN_UPDATE_MS;
    }
    bool is_identify_mode = module->identify_mode_time_ms > 0;

    bool enable_display = false;
    if (display_timeout_config < 0) {
        // display always on
        enable_display = true;
    } else if (display_timeout_config == 0) {
        // display off, except pre-carousel screens or button press
        if (current_screen < SCR_CAROUSEL_START || inactive_time < BUTTON_WAKE_MS) {
            enable_display = true;
        }
    } else {
        // display timeout
        const uint32_t display_timeout = display_timeout_config * 60 * 1000;

        if (inactive_time < display_timeout || current_screen < SCR_CAROUSEL_START || is_identify_mode) {
            enable_display = true;
        }
    }

    display_on(enable_display);

    if (GLOBAL_STATE->SELF_TEST_MODULE.is_active) {
        SelfTestModule * self_test = &GLOBAL_STATE->SELF_TEST_MODULE;
        
        lv_label_set_text(self_test_message_label, self_test->message);
        
        if (self_test->is_finished) {
            lv_label_set_text(self_test_result_label, self_test->result);
            lv_label_set_text(self_test_finished_label, self_test->finished);
        }
        
        screen_show(SCR_SELF_TEST);
        return;
    }

    if (is_identify_mode == lv_obj_has_flag(identify_image, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_flag(identify_image, LV_OBJ_FLAG_HIDDEN, !is_identify_mode);
        lv_obj_set_style_bg_opa(lv_layer_top(), is_identify_mode ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_color(lv_layer_top(), is_identify_mode ? lv_color_black() : lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(notification_label, is_identify_mode ? lv_color_white() : lv_color_black(), LV_PART_MAIN);
    }

    if (module->is_firmware_update) {
        if (strcmp(module->firmware_update_filename, lv_label_get_text(firmware_update_scr_filename_label)) != 0) {
            lv_label_set_text(firmware_update_scr_filename_label, module->firmware_update_filename);
        }
        if (strcmp(module->firmware_update_status, lv_label_get_text(firmware_update_scr_status_label)) != 0) {
            lv_label_set_text(firmware_update_scr_status_label, module->firmware_update_status);
        }
        screen_show(SCR_FIRMWARE);
        return;
    }

    if (module->asic_status) {
        lv_label_set_text(asic_status_label, module->asic_status);

        screen_show(SCR_ASIC_STATUS);
        return;
    }

    if (module->overheat_mode) {
        if (strcmp(module->ip_addr_str, lv_label_get_text(overheat_ip_addr_label)) != 0) {
            lv_label_set_text(overheat_ip_addr_label, module->ip_addr_str);
        }

        screen_show(SCR_OVERHEAT);
        return;
    }

    if (module->ssid[0] == '\0') {
        screen_show(SCR_WELCOME);
        return;
    }

    bool is_wifi_status_changed = strcmp(module->wifi_status, lv_label_get_text(connection_wifi_status_label)) != 0;
    if (is_wifi_status_changed) {
        lv_label_set_text(connection_wifi_status_label, module->wifi_status);
        screen_show(SCR_CONNECTION);
        return;
    }

    if (module->ap_enabled || !module->is_connected) {
        screen_show(SCR_CONNECTION);
        return;
    }

    // Connected display updates
    current_screen_time_ms += SCREEN_UPDATE_MS;

    char value_buf[24];

    float hashrate = module->current_hashrate;
    if (hashrate >= 1000.0f) {
        snprintf(value_buf, sizeof(value_buf), "%.2f TH/s", hashrate / 1000.0f);
    } else {
        snprintf(value_buf, sizeof(value_buf), "%.0f GH/s", hashrate);
    }
    if (hashrate_value_label && strcmp(lv_label_get_text(hashrate_value_label), value_buf) != 0) {
        lv_label_set_text(hashrate_value_label, value_buf);
    }

    // one sample per tick feeds the hashrate sparkline
    hashrate_hist[spark_head] = hashrate > 0.0f ? hashrate : 0.0f;
    spark_head = (spark_head + 1) % SPARK_POINTS;
    if (spark_filled < SPARK_POINTS) {
        spark_filled++;
    }
    update_sparkline(hashrate_line, hashrate_hist, hashrate_points);

    update_clock();
    update_date();

    if (pickaxe_image) {
        static int pickaxe_frame_idx = -1;
        pickaxe_tick++;
        int want = (int)(pickaxe_tick % 3);
        if (want != pickaxe_frame_idx) {
            pickaxe_frame_idx = want;
            lv_img_set_src(pickaxe_image,
                           want == 0 ? &pickaxe_frame_1 : (want == 1 ? &pickaxe_frame_2 : &pickaxe_frame_3));
        }
    }

    uint32_t shares_accepted = module->shares_accepted;
    uint32_t shares_rejected = module->shares_rejected;
    uint32_t work_received = module->work_received;

    if (current_shares_accepted != shares_accepted
        || current_shares_rejected != shares_rejected
        || current_work_received != work_received) {

        uint8_t state = 0;
        if (shares_accepted > current_shares_accepted) state |= NOTIFICATION_SHARE_ACCEPTED;
        if (shares_rejected > current_shares_rejected) state |= NOTIFICATION_SHARE_REJECTED;
        if (work_received > current_work_received) state |= NOTIFICATION_WORK_RECEIVED;

        lv_label_set_text(notification_label, notifications[state]);

        current_shares_accepted = shares_accepted;
        current_shares_rejected = shares_rejected;
        current_work_received = work_received;
    } else {
        lv_label_set_text(notification_label, module->mining_paused ? "||" : "");
    }

    // The clock spans the full panel, so a share arrow drawn over it reads as a glitch.
    if (get_current_screen() == SCR_CLOCK) {
        lv_label_set_text(notification_label, "");
    }

    if (module->show_new_block) {
        if (get_current_screen() != SCR_BLOCKS_FOUND) {
            screen_show(SCR_BLOCKS_FOUND);
        }

        lv_display_trigger_activity(NULL);
        return;
    }

    if (current_screen_time_ms < current_screen_delay_ms) {
        return;
    }

    screen_next();
}

void screen_button_press() 
{
    if (GLOBAL_STATE->SYSTEM_MODULE.identify_mode_time_ms > 0) {
        GLOBAL_STATE->SYSTEM_MODULE.identify_mode_time_ms = 0;
    } else {
        screen_next();
    }
}

esp_err_t screen_start(GlobalState * global_state)
{
    GLOBAL_STATE = global_state;

    // Local wall time for the clock screen: the POSIX string carries both the offset
    // and the EU DST rules, since newlib has no tz database of its own.
    setenv("TZ", CLOCK_TZ, 1);
    tzset();

    // DST self-check, one line per boot. The same UTC instant in January and July must land
    // on different local hours (+2 vs +3) if newlib is honouring the rule part of CLOCK_TZ
    // rather than just the standard offset. This is the only way to confirm the summer
    // behaviour without waiting for March, so do not delete it.
    {
        time_t winter = 1768478400;   // 2026-01-15 12:00:00 UTC
        time_t summer = 1784116800;   // 2026-07-15 12:00:00 UTC
        struct tm winter_local, summer_local;
        if (localtime_r(&winter, &winter_local) != NULL && localtime_r(&summer, &summer_local) != NULL) {
            ESP_LOGI("screen", "clock: DST check, UTC 12:00 => Jan %02d:00 local, Jul %02d:00 local",
                     winter_local.tm_hour, summer_local.tm_hour);
        }
    }

    if (lvgl_port_lock(0)) {
        screen_lines = lv_display_get_vertical_resolution(NULL) / (is_large_display() ? 16 : 8);

        SystemModule * SYSTEM_MODULE = &GLOBAL_STATE->SYSTEM_MODULE;

        if (SYSTEM_MODULE->is_screen_active) {

            screens[SCR_SELF_TEST] = create_scr_self_test();
            screens[SCR_OVERHEAT] = create_scr_overheat();
            screens[SCR_ASIC_STATUS] = create_scr_asic_status();
            screens[SCR_WELCOME] = create_scr_welcome(SYSTEM_MODULE->ap_ssid);
            screens[SCR_FIRMWARE] = create_scr_firmware();
            screens[SCR_CONNECTION] = create_scr_connection(SYSTEM_MODULE->ssid, SYSTEM_MODULE->ap_ssid);
            screens[SCR_BITAXE_LOGO] = create_scr_bitaxe_logo(GLOBAL_STATE->DEVICE_CONFIG.family.name, GLOBAL_STATE->DEVICE_CONFIG.board_version);
            screens[SCR_BLOCKS_FOUND] = create_scr_blocks_found();
            screens[SCR_HASHRATE] = create_scr_hashrate();
            screens[SCR_PICKAXE] = create_scr_pickaxe();
            screens[SCR_CLOCK] = create_scr_clock();
            screens[SCR_DATE] = create_scr_date();

            scr_create_overlay();

            lv_timer_create(screen_update_cb, SCREEN_UPDATE_MS, NULL);

            // Create uptime update timer (runs every 1 second)
        }
        lvgl_port_unlock();
    }

    return ESP_OK;
}
