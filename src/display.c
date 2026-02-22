#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/driver_backends.h"
#include "lvgl/lvgl.h"
#include "lvgl/simulator_settings.h"
#include "lvgl/simulator_util.h"
#include "lvgl/src/libs/freetype/lv_freetype.h"

/* Global simulator settings */
extern simulator_settings_t settings;

static lv_obj_t *date_label;
static lv_obj_t *time_label;

static lv_font_t *date_font;
static lv_font_t *time_font;

void font_init(void) {
  lv_freetype_init(64);

  date_font = lv_freetype_font_create("./RobotoMono-VariableFont_wght.ttf",
                                      LV_FREETYPE_FONT_STYLE_NORMAL, 120, 0);

  if (date_font == NULL) {
    printf("Font create failed\n");
  }

  time_font = lv_freetype_font_create("./RobotoMono-VariableFont_wght.ttf",
                                      LV_FREETYPE_FONT_STYLE_NORMAL, 200, 0);

  if (time_font == NULL) {
    printf("Font create failed\n");
  }
}

static void update_time_cb(lv_timer_t *timer) {
  LV_UNUSED(timer);

  time_t now;
  struct tm *info;
  char date_buf[64];
  char time_buf[64];

  time(&now);
  info = localtime(&now);

  strftime(date_buf, sizeof(date_buf), "%A, %Y-%m-%d", info);

  strftime(time_buf, sizeof(time_buf), "%H:%M:%S", info);

  lv_label_set_text(date_label, date_buf);
  lv_label_set_text(time_label, time_buf);
}

int app_display_main(void *args) {
  settings.window_width = 1280;
  settings.window_height = 720;
  settings.fullscreen = false;
  settings.maximize = false;

  driver_backends_register();

  lv_init();

  if (driver_backends_init_backend(NULL) == -1) {
    die("Failed to initialize display backend");
  }

#if LV_USE_EVDEV
  if (driver_backends_init_backend("EVDEV") == -1) {
    die("Failed to initialize evdev");
  }
#endif
  font_init();

  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);

  lv_obj_t *container = lv_obj_create(lv_scr_act());
  lv_obj_set_size(container, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(container, 0, 0);

  lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  date_label = lv_label_create(container);
  lv_obj_set_style_text_color(date_label, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(date_label, date_font, 0);
  lv_label_set_text(date_label, "Loading date...");

  time_label = lv_label_create(container);
  lv_obj_set_style_text_color(time_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(time_label, time_font, 0);
  lv_label_set_text(time_label, "Loading time...");

  update_time_cb(NULL);

  lv_timer_create(update_time_cb, 1000, NULL);

  driver_backends_run_loop();

  return 0;
}
