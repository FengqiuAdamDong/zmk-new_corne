/*
 *
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Status screen for the Eyelash Corne's 128x64 SSD1306, which is mounted
 * rotated 90 degrees.
 *
 * Everything is authored in a 64 (wide) x 128 (tall) portrait area and rotated
 * on to the panel, so text reads the right way up. lv_canvas_transform rotates
 * about the canvas centre, which means the canvas itself has to be square:
 * portrait (x, y) lands at screen (CANVAS - y, x).
 *
 * The battery and profile indicators are drawn from rectangles rather than
 * bitmaps so they scale to this panel instead of being fixed-size artwork, and
 * the text uses unscii, a bitmap font, because an anti-aliased font thresholded
 * on to a 1-bit display looks muddy.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <lvgl.h>

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/usb.h>

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/events/usb_conn_state_changed.h>
#endif

#define IS_CENTRAL (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#if IS_CENTRAL
#include <zmk/endpoints.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#else
#include "bongo_cat.h"
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#endif

#define CANVAS 128
#define PORTRAIT_W 64
#define PORTRAIT_H 128

/* How many letters of the layer name to show. */
#define LAYER_NAME_CHARS 3

/* On an OLED an unlit pixel is black, so the natural drawing order is light on
 * dark. Swap these two if the panel ever ends up inverted. */
#define COLOR_BG lv_color_black()
#define COLOR_FG lv_color_white()

/* Vertical layout, in portrait coordinates. */
#define Y_BATTERY_ICON 10
#define Y_BATTERY_TEXT 44
#define Y_ENDPOINT 72
#define Y_LAYER 100
#define Y_PERIPHERAL_LINK 66
#define Y_CAT (PORTRAIT_H - EYELASH_BONGO_CAT_H - 1)

/* Milliseconds per bongo cat frame on the peripheral. The source gif runs at
 * 100ms, but each tick here redraws and re-rotates the whole canvas, so this
 * is slowed down to keep the cost off the peripheral's battery. */
#define CAT_FRAME_MS 200

struct status_state {
    uint8_t battery;
    bool charging;
#if IS_CENTRAL
    struct zmk_endpoint_instance endpoint;
    uint8_t layer_index;
    const char *layer_label;
#else
    bool connected;
#endif
};

struct zmk_widget_screen {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_color_t cbuf[CANVAS * CANVAS];
    struct status_state state;
};

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);
static struct zmk_widget_screen status_widget;

#if !IS_CENTRAL
static uint8_t cat_frame;
#endif

static void init_label(lv_draw_label_dsc_t *dsc, const lv_font_t *font) {
    lv_draw_label_dsc_init(dsc);
    dsc->color = COLOR_FG;
    dsc->font = font;
    dsc->align = LV_TEXT_ALIGN_CENTER;
}

static void init_rect(lv_draw_rect_dsc_t *dsc, lv_color_t color) {
    lv_draw_rect_dsc_init(dsc);
    dsc->bg_color = color;
}

static void draw_text(lv_obj_t *canvas, int y, const char *text) {
    lv_draw_label_dsc_t dsc;
    init_label(&dsc, &lv_font_unscii_16);
    lv_canvas_draw_text(canvas, 0, y, PORTRAIT_W, &dsc, text);
}

/* 59 x 22 battery, drawn as an outline with a proportional fill bar. */
static void draw_battery(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_rect_dsc_t fg, bg;
    init_rect(&fg, COLOR_FG);
    init_rect(&bg, COLOR_BG);

    lv_canvas_draw_rect(canvas, 2, Y_BATTERY_ICON, 52, 22, &fg);
    lv_canvas_draw_rect(canvas, 4, Y_BATTERY_ICON + 2, 48, 18, &bg);

    int fill = (state->battery * 44) / 100;
    if (fill > 0) {
        lv_canvas_draw_rect(canvas, 6, Y_BATTERY_ICON + 4, fill, 14, &fg);
    }

    /* terminal nub */
    lv_canvas_draw_rect(canvas, 54, Y_BATTERY_ICON + 7, 5, 8, &fg);
}

static void rotate_canvas(lv_obj_t *canvas, lv_color_t cbuf[]) {
    static lv_color_t tmp[CANVAS * CANVAS];
    lv_img_dsc_t img;

    memcpy(tmp, cbuf, sizeof(tmp));

    memset(&img, 0, sizeof(img));
    img.data = (void *)tmp;
    img.header.cf = LV_IMG_CF_TRUE_COLOR;
    img.header.w = CANVAS;
    img.header.h = CANVAS;

    lv_canvas_fill_bg(canvas, COLOR_BG, LV_OPA_COVER);
    lv_canvas_transform(canvas, &img, 900, LV_IMG_ZOOM_NONE, -1, 0, CANVAS / 2, CANVAS / 2, false);
}

static void draw_screen(struct zmk_widget_screen *widget) {
    lv_obj_t *canvas = lv_obj_get_child(widget->obj, 0);
    const struct status_state *state = &widget->state;
    lv_draw_rect_dsc_t bg;
    char text[16];

    init_rect(&bg, COLOR_BG);
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS, CANVAS, &bg);

    draw_battery(canvas, state);
    snprintf(text, sizeof(text), state->charging ? "%d%%+" : "%d%%", state->battery);
    draw_text(canvas, Y_BATTERY_TEXT, text);

#if IS_CENTRAL
    draw_text(canvas, Y_ENDPOINT,
              state->endpoint.transport == ZMK_TRANSPORT_USB ? "USB" : "BT");

    if (state->layer_label != NULL && state->layer_label[0] != '\0') {
        snprintf(text, sizeof(text), "%.*s", LAYER_NAME_CHARS, state->layer_label);
    } else {
        snprintf(text, sizeof(text), "L%d", state->layer_index);
    }
    draw_text(canvas, Y_LAYER, text);
#else
    draw_text(canvas, Y_PERIPHERAL_LINK, state->connected ? "LINK" : "----");

    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);
    lv_canvas_draw_img(canvas, (PORTRAIT_W - EYELASH_BONGO_CAT_W) / 2, Y_CAT,
                       eyelash_bongo_cat_frames[cat_frame], &img_dsc);
#endif

    rotate_canvas(canvas, widget->cbuf);
}

#if !IS_CENTRAL
static void cat_timer_cb(lv_timer_t *timer) {
    struct zmk_widget_screen *widget;

    ARG_UNUSED(timer);

    cat_frame = (cat_frame + 1) % EYELASH_BONGO_CAT_FRAMES;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { draw_screen(widget); }
}
#endif

/* Battery ------------------------------------------------------------------ */

struct battery_status_state {
    uint8_t level;
    bool usb_present;
};

static void set_battery_status(struct zmk_widget_screen *widget,
                               struct battery_status_state state) {
    widget->state.battery = state.level;
    widget->state.charging = state.usb_present;
    draw_screen(widget);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#else
        .usb_present = false,
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif

#if IS_CENTRAL

/* Endpoint ----------------------------------------------------------------- */

struct output_status_state {
    struct zmk_endpoint_instance endpoint;
};

static void set_output_status(struct zmk_widget_screen *widget,
                              const struct output_status_state *state) {
    widget->state.endpoint = state->endpoint;
    draw_screen(widget);
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){.endpoint = zmk_endpoints_selected()};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif

/* Layer -------------------------------------------------------------------- */

struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

static void set_layer_status(struct zmk_widget_screen *widget, struct layer_status_state state) {
    widget->state.layer_index = state.index;
    widget->state.layer_label = state.label;
    draw_screen(widget);
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_status(widget, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();

    return (struct layer_status_state){
        .index = index, .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index))};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)
ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

#else

/* Split link --------------------------------------------------------------- */

struct peripheral_status_state {
    bool connected;
};

static void set_peripheral_status(struct zmk_widget_screen *widget,
                                  struct peripheral_status_state state) {
    widget->state.connected = state.connected;
    draw_screen(widget);
}

static void peripheral_status_update_cb(struct peripheral_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_peripheral_status(widget, state); }
}

static struct peripheral_status_state peripheral_status_get_state(const zmk_event_t *_eh) {
    return (struct peripheral_status_state){.connected = zmk_split_bt_peripheral_is_connected()};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_status, struct peripheral_status_state,
                            peripheral_status_update_cb, peripheral_status_get_state)
ZMK_SUBSCRIPTION(widget_peripheral_status, zmk_split_peripheral_status_changed);

#endif /* IS_CENTRAL */

static int zmk_widget_screen_init(struct zmk_widget_screen *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, PORTRAIT_H, PORTRAIT_W);
    lv_obj_set_style_pad_all(widget->obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(widget->obj, COLOR_BG, LV_PART_MAIN);

    lv_obj_t *canvas = lv_canvas_create(widget->obj);
    lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(canvas, widget->cbuf, CANVAS, CANVAS, LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);

    widget_battery_status_init();
#if IS_CENTRAL
    widget_output_status_init();
    widget_layer_status_init();
#else
    widget_peripheral_status_init();
    lv_timer_create(cat_timer_cb, CAT_FRAME_MS, NULL);
#endif

    return 0;
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, COLOR_BG, LV_PART_MAIN);

    zmk_widget_screen_init(&status_widget, screen);
    lv_obj_align(status_widget.obj, LV_ALIGN_TOP_LEFT, 0, 0);

    return screen;
}
