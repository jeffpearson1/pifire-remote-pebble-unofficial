#include <pebble.h>

// PiFire remote: shows grill status and sends mode/setpoint commands.
// All HTTP happens in PebbleKit JS (src/pkjs/index.js); the watch only
// speaks AppMessage with the keys declared in package.json.

#define POLL_INTERVAL_MS 10000
#define SETPOINT_STEP 5

// Screens 200px and wider (Pebble Time 2 / emery, Pebble Round 2 / gabbro)
// get larger type.
#define LARGE_SCREEN_MIN_W 200
#define BIG_NUMBER_FONT FONT_KEY_ROBOTO_BOLD_SUBSET_49

static bool is_large(GRect bounds) {
  return bounds.size.w >= LARGE_SCREEN_MIN_W;
}

// Horizontal inset that keeps text inside the circle on round screens.
static int side_inset(GRect bounds) {
  return PBL_IF_ROUND_ELSE(bounds.size.w / 9, 4);
}

// ---------------------------------------------------------------------------
// State

static char s_mode[16] = "PiFire";
static char s_units[2] = "F";
static char s_temp_buf[8] = "--";
static char s_setpoint_buf[32] = "";
static char s_probes_buf[96] = "";
static char s_status_buf[48] = "Waiting for phone...";
static int s_setpoint = 0;
static bool s_splus = false;

// Outbox: one queued command plus a "refresh wanted" flag. Commands win.
static bool s_js_ready = false;
static bool s_sending = false;
static bool s_refresh_wanted = false;
static bool s_has_pending = false;
static char s_pending_cmd[12];
static char s_pending_arg[16];

static AppTimer *s_poll_timer;

static Window *s_main_window;
static TextLayer *s_mode_layer, *s_temp_layer, *s_setpoint_layer, *s_probes_layer, *s_status_layer;
static GFont s_temp_font, s_temp_dash_font;

static Window *s_menu_window;
static MenuLayer *s_menu_layer;

static Window *s_setpoint_window;
static TextLayer *s_sp_title_layer, *s_sp_value_layer, *s_sp_hint_layer;
static int s_sp_value;
static char s_sp_value_buf[8];

static Window *s_confirm_window;
static TextLayer *s_confirm_layer;
static int s_confirm_action;
static char s_confirm_buf[96];

// ---------------------------------------------------------------------------
// Actions

// Shutdown is the normal way to end a cook: PiFire runs its shutdown cycle
// (fan keeps running to burn off pellets). Stop cuts everything immediately
// and is only for emergencies, so it sits last and needs a long press.
typedef enum {
  ACT_STARTUP,
  ACT_SMOKE,
  ACT_HOLD,
  ACT_SPLUS,
  ACT_SHUTDOWN,
  ACT_MONITOR,
  ACT_REIGNITE,
  ACT_STOP,
  ACT_COUNT
} Action;

static const char *s_action_titles[ACT_COUNT] = {
  "Startup", "Smoke", "Hold...", "Smoke Plus", "Shutdown", "Monitor", "Reignite", "Emergency Stop"
};

// PiFire mode name for each action ("" for non-mode actions).
static const char *s_action_modes[ACT_COUNT] = {
  "Startup", "Smoke", "Hold", "", "Shutdown", "Monitor", "Reignite", "Stop"
};

#define STOP_LONG_PRESS_MS 1000

// ---------------------------------------------------------------------------
// Messaging

static void flush_outbox(void) {
  if (!s_js_ready || s_sending || (!s_has_pending && !s_refresh_wanted)) {
    return;
  }
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  if (s_has_pending) {
    dict_write_cstring(iter, MESSAGE_KEY_CMD, s_pending_cmd);
    dict_write_cstring(iter, MESSAGE_KEY_ARG, s_pending_arg);
    s_has_pending = false;
  } else {
    dict_write_cstring(iter, MESSAGE_KEY_CMD, "refresh");
    s_refresh_wanted = false;
  }
  if (app_message_outbox_send() == APP_MSG_OK) {
    s_sending = true;
  }
}

static void update_main_window(void);

static void send_command(const char *cmd, const char *arg) {
  snprintf(s_pending_cmd, sizeof(s_pending_cmd), "%s", cmd);
  snprintf(s_pending_arg, sizeof(s_pending_arg), "%s", arg ? arg : "");
  s_has_pending = true;
  snprintf(s_status_buf, sizeof(s_status_buf), "Sending...");
  update_main_window();
  flush_outbox();
}

static void request_refresh(void) {
  s_refresh_wanted = true;
  flush_outbox();
}

static void poll_timer_callback(void *data) {
  request_refresh();
  s_poll_timer = app_timer_register(POLL_INTERVAL_MS, poll_timer_callback, NULL);
}

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *t;
  if (dict_find(iter, MESSAGE_KEY_READY)) {
    s_js_ready = true;
    s_refresh_wanted = true;
    snprintf(s_status_buf, sizeof(s_status_buf), "Connecting...");
  }
  if (dict_find(iter, MESSAGE_KEY_PIFIRE_URL)) {
    // Settings were just saved on the phone.
    s_refresh_wanted = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_MODE))) {
    snprintf(s_mode, sizeof(s_mode), "%s", t->value->cstring);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_GRILL_TEMP))) {
    snprintf(s_temp_buf, sizeof(s_temp_buf), "%s", t->value->cstring);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_SETPOINT))) {
    s_setpoint = (int)t->value->int32;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_UNITS))) {
    snprintf(s_units, sizeof(s_units), "%s", t->value->cstring);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_PROBES))) {
    snprintf(s_probes_buf, sizeof(s_probes_buf), "%s", t->value->cstring);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_SPLUS))) {
    s_splus = t->value->int32 != 0;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_STATUS))) {
    snprintf(s_status_buf, sizeof(s_status_buf), "%s", t->value->cstring);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ERROR))) {
    snprintf(s_status_buf, sizeof(s_status_buf), "%s", t->value->cstring);
    vibes_short_pulse();
  }

  update_main_window();
  if (s_menu_layer) {
    menu_layer_reload_data(s_menu_layer);
  }
  flush_outbox();
}

static void outbox_sent_handler(DictionaryIterator *iter, void *context) {
  s_sending = false;
  flush_outbox();
}

static void outbox_failed_handler(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  s_sending = false;
  snprintf(s_status_buf, sizeof(s_status_buf), "Phone unreachable");
  update_main_window();
}

// ---------------------------------------------------------------------------
// Navigation helpers

static void pop_to_main(void) {
  while (window_stack_get_top_window() != s_main_window) {
    window_stack_pop(false);
  }
}

static void send_mode(const char *mode) {
  // Optimistically show the new mode until the next refresh confirms it.
  snprintf(s_mode, sizeof(s_mode), "%s", mode);
  send_command("mode", mode);
}

// ---------------------------------------------------------------------------
// Setpoint window (Hold)

static void sp_update_text(void) {
  snprintf(s_sp_value_buf, sizeof(s_sp_value_buf), "%d", s_sp_value);
  text_layer_set_text(s_sp_value_layer, s_sp_value_buf);
}

static void sp_adjust(int delta) {
  bool celsius = s_units[0] == 'C';
  int lo = celsius ? 65 : 150;
  int hi = celsius ? 315 : 600;
  s_sp_value += delta;
  if (s_sp_value < lo) s_sp_value = lo;
  if (s_sp_value > hi) s_sp_value = hi;
  sp_update_text();
}

static void sp_up_handler(ClickRecognizerRef rec, void *ctx) { sp_adjust(SETPOINT_STEP); }
static void sp_down_handler(ClickRecognizerRef rec, void *ctx) { sp_adjust(-SETPOINT_STEP); }

static void sp_select_handler(ClickRecognizerRef rec, void *ctx) {
  char arg[8];
  snprintf(arg, sizeof(arg), "%d", s_sp_value);
  s_setpoint = s_sp_value;
  snprintf(s_mode, sizeof(s_mode), "Hold");
  send_command("hold", arg);
  pop_to_main();
}

static void sp_click_config(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, sp_up_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, sp_down_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, sp_select_handler);
}

static void sp_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  int mid = b.size.h / 2;
  bool large = is_large(b);

  s_sp_title_layer = text_layer_create(large ? GRect(0, mid - 80, b.size.w, 34) : GRect(0, mid - 60, b.size.w, 28));
  text_layer_set_font(s_sp_title_layer,
                      fonts_get_system_font(large ? FONT_KEY_GOTHIC_28_BOLD : FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_sp_title_layer, GTextAlignmentCenter);
  text_layer_set_text(s_sp_title_layer, s_units[0] == 'C' ? "Hold at °C" : "Hold at °F");

  s_sp_value_layer = text_layer_create(large ? GRect(0, mid - 42, b.size.w, 70) : GRect(0, mid - 30, b.size.w, 50));
  text_layer_set_font(s_sp_value_layer, fonts_get_system_font(large ? BIG_NUMBER_FONT : FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_sp_value_layer, GTextAlignmentCenter);

  s_sp_hint_layer = text_layer_create(large ? GRect(0, mid + 36, b.size.w, 24) : GRect(0, mid + 24, b.size.w, 20));
  text_layer_set_font(s_sp_hint_layer, fonts_get_system_font(large ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_sp_hint_layer, GTextAlignmentCenter);
  text_layer_set_text(s_sp_hint_layer, "SELECT to set");

  layer_add_child(root, text_layer_get_layer(s_sp_title_layer));
  layer_add_child(root, text_layer_get_layer(s_sp_value_layer));
  layer_add_child(root, text_layer_get_layer(s_sp_hint_layer));

  if (s_setpoint > 0) {
    s_sp_value = s_setpoint;
  } else {
    s_sp_value = s_units[0] == 'C' ? 110 : 225;
  }
  sp_update_text();
}

static void sp_window_unload(Window *window) {
  text_layer_destroy(s_sp_title_layer);
  text_layer_destroy(s_sp_value_layer);
  text_layer_destroy(s_sp_hint_layer);
}

// ---------------------------------------------------------------------------
// Confirm window (Shutdown: press SELECT; Emergency Stop: hold SELECT)

static void confirm_select_handler(ClickRecognizerRef rec, void *ctx) {
  if (s_confirm_action == ACT_STOP) {
    vibes_long_pulse();
  }
  send_mode(s_action_modes[s_confirm_action]);
  pop_to_main();
}

static void confirm_click_config(void *ctx) {
  if (s_confirm_action == ACT_STOP) {
    window_long_click_subscribe(BUTTON_ID_SELECT, STOP_LONG_PRESS_MS, confirm_select_handler, NULL);
  } else {
    window_single_click_subscribe(BUTTON_ID_SELECT, confirm_select_handler);
  }
}

static void confirm_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  bool stop = s_confirm_action == ACT_STOP;

  window_set_background_color(window, stop ? PBL_IF_COLOR_ELSE(GColorRed, GColorBlack) : GColorWhite);
  int inset = side_inset(b) + 2;
  s_confirm_layer = text_layer_create(grect_inset(b, GEdgeInsets(b.size.h / 6, inset, 0, inset)));
  text_layer_set_background_color(s_confirm_layer, GColorClear);
  text_layer_set_text_color(s_confirm_layer, stop ? GColorWhite : GColorBlack);
  text_layer_set_font(s_confirm_layer,
                      fonts_get_system_font(is_large(b) ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_confirm_layer, GTextAlignmentCenter);
  if (stop) {
    snprintf(s_confirm_buf, sizeof(s_confirm_buf),
             "EMERGENCY STOP\n\nSkips the shutdown cycle.\n\nHold SELECT to stop");
  } else {
    snprintf(s_confirm_buf, sizeof(s_confirm_buf),
             "Shut down grill?\n\nRuns the shutdown cycle to burn off pellets.\n\nSELECT to confirm");
  }
  text_layer_set_text(s_confirm_layer, s_confirm_buf);
  layer_add_child(root, text_layer_get_layer(s_confirm_layer));
}

static void confirm_window_unload(Window *window) {
  text_layer_destroy(s_confirm_layer);
}

// ---------------------------------------------------------------------------
// Action menu

static uint16_t menu_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return ACT_COUNT;
}

static void menu_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  static char subtitle_buf[24];
  const char *subtitle = NULL;
  int row = idx->row;

  if (row == ACT_SPLUS) {
    subtitle = s_splus ? "On" : "Off";
  } else if (strcmp(s_action_modes[row], s_mode) == 0) {
    subtitle = "Current mode";
  } else if (row == ACT_HOLD && s_setpoint > 0) {
    snprintf(subtitle_buf, sizeof(subtitle_buf), "Last: %d°%s", s_setpoint, s_units);
    subtitle = subtitle_buf;
  } else if (row == ACT_SHUTDOWN) {
    subtitle = "End cook";
  } else if (row == ACT_STOP) {
    subtitle = "Skips shutdown";
  }
  menu_cell_basic_draw(ctx, cell, s_action_titles[row], subtitle, NULL);
}

static void menu_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  int row = idx->row;
  switch (row) {
    case ACT_HOLD:
      window_stack_push(s_setpoint_window, true);
      break;
    case ACT_SPLUS:
      s_splus = !s_splus;
      send_command("splus", s_splus ? "true" : "false");
      pop_to_main();
      break;
    case ACT_SHUTDOWN:
    case ACT_STOP:
      s_confirm_action = row;
      window_stack_push(s_confirm_window, true);
      break;
    default:
      send_mode(s_action_modes[row]);
      pop_to_main();
      break;
  }
}

static void menu_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_menu_layer = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = menu_get_num_rows,
    .draw_row = menu_draw_row,
    .select_click = menu_select,
  });
  menu_layer_set_highlight_colors(s_menu_layer, PBL_IF_COLOR_ELSE(GColorOrange, GColorBlack), GColorWhite);
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));
}

static void menu_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  s_menu_layer = NULL;
}

// ---------------------------------------------------------------------------
// Main window

static GColor mode_color(void) {
#ifdef PBL_COLOR
  if (strcmp(s_mode, "Startup") == 0 || strcmp(s_mode, "Reignite") == 0) return GColorOrange;
  if (strcmp(s_mode, "Smoke") == 0) return GColorDarkGray;
  if (strcmp(s_mode, "Hold") == 0) return GColorRed;
  if (strcmp(s_mode, "Shutdown") == 0) return GColorCobaltBlue;
  if (strcmp(s_mode, "Monitor") == 0) return GColorIslamicGreen;
  if (strcmp(s_mode, "Error") == 0) return GColorFolly;
#endif
  return GColorBlack;
}

static void update_main_window(void) {
  if (!s_mode_layer) {
    return;
  }
  text_layer_set_background_color(s_mode_layer, mode_color());
  text_layer_set_text(s_mode_layer, s_mode);
  text_layer_set_font(s_temp_layer, s_temp_buf[0] == '-' ? s_temp_dash_font : s_temp_font);
  text_layer_set_text(s_temp_layer, s_temp_buf);

  bool hold = strcmp(s_mode, "Hold") == 0;
  bool show_splus = s_splus && (hold || strcmp(s_mode, "Smoke") == 0);
  if (hold && s_setpoint > 0) {
    snprintf(s_setpoint_buf, sizeof(s_setpoint_buf), "Set %d°%s%s", s_setpoint, s_units,
             show_splus ? " | Smoke+" : "");
  } else {
    snprintf(s_setpoint_buf, sizeof(s_setpoint_buf), "Grill °%s%s", s_units,
             show_splus ? " | Smoke+" : "");
  }
  text_layer_set_text(s_setpoint_layer, s_setpoint_buf);
  text_layer_set_text(s_probes_layer, s_probes_buf);
  text_layer_set_text(s_status_layer, s_status_buf);
}

static void main_select_handler(ClickRecognizerRef rec, void *ctx) {
  window_stack_push(s_menu_window, true);
}

static void main_up_handler(ClickRecognizerRef rec, void *ctx) {
  snprintf(s_status_buf, sizeof(s_status_buf), "Refreshing...");
  update_main_window();
  request_refresh();
}

static void main_down_handler(ClickRecognizerRef rec, void *ctx) {
  window_stack_push(s_setpoint_window, true);
}

static void main_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, main_select_handler);
  window_single_click_subscribe(BUTTON_ID_UP, main_up_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, main_down_handler);
}

static TextLayer *make_text_layer(Layer *root, GRect frame, const char *font_key, GTextAlignment align) {
  TextLayer *layer = text_layer_create(frame);
  text_layer_set_background_color(layer, GColorClear);
  text_layer_set_text_color(layer, GColorBlack);
  text_layer_set_font(layer, fonts_get_system_font(font_key));
  text_layer_set_text_alignment(layer, align);
  text_layer_set_overflow_mode(layer, GTextOverflowModeTrailingEllipsis);
  layer_add_child(root, text_layer_get_layer(layer));
  return layer;
}

// Main screen geometry for one size class. Rows stack top to bottom; the
// probes row takes whatever is left above the status line.
typedef struct {
  const char *mode_font, *temp_font, *setpoint_font, *probes_font, *status_font;
  int mode_h, temp_h, setpoint_h, status_h;
} MainLayout;

static const MainLayout s_small_layout = {
  FONT_KEY_GOTHIC_24_BOLD, FONT_KEY_BITHAM_42_BOLD, FONT_KEY_GOTHIC_18, FONT_KEY_GOTHIC_14_BOLD, FONT_KEY_GOTHIC_14,
  28, 48, 24, 18
};

static const MainLayout s_large_layout = {
  FONT_KEY_GOTHIC_28_BOLD, BIG_NUMBER_FONT, FONT_KEY_GOTHIC_24, FONT_KEY_GOTHIC_18_BOLD, FONT_KEY_GOTHIC_18,
  34, 60, 30, 24
};

static void main_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  int w = b.size.w;
  int h = b.size.h;
  const MainLayout *l = is_large(b) ? &s_large_layout : &s_small_layout;
  int inset = side_inset(b);
  int top = PBL_IF_ROUND_ELSE(h / 24, 0);
  int bottom = PBL_IF_ROUND_ELSE(h / 12, 0);
  GTextAlignment align = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);

  int y = top;
  s_mode_layer = make_text_layer(root, GRect(0, y, w, l->mode_h + 2), l->mode_font, GTextAlignmentCenter);
  text_layer_set_text_color(s_mode_layer, GColorWhite);
  y += l->mode_h;

  // "--" uses Bitham: the Roboto subset only covers digits.
  s_temp_font = fonts_get_system_font(l->temp_font);
  s_temp_dash_font = fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD);
  s_temp_layer = make_text_layer(root, GRect(0, y, w, l->temp_h + 4), l->temp_font, GTextAlignmentCenter);
  y += l->temp_h;

  s_setpoint_layer = make_text_layer(root, GRect(inset, y, w - 2 * inset, l->setpoint_h),
                                     l->setpoint_font, GTextAlignmentCenter);
  y += l->setpoint_h;

  int status_y = h - l->status_h - bottom;
  s_probes_layer = make_text_layer(root, GRect(inset, y, w - 2 * inset, status_y - y), l->probes_font, align);
  text_layer_set_overflow_mode(s_probes_layer, GTextOverflowModeWordWrap);

  s_status_layer = make_text_layer(root, GRect(inset, status_y, w - 2 * inset, l->status_h),
                                   l->status_font, GTextAlignmentCenter);

  update_main_window();
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_mode_layer);
  text_layer_destroy(s_temp_layer);
  text_layer_destroy(s_setpoint_layer);
  text_layer_destroy(s_probes_layer);
  text_layer_destroy(s_status_layer);
  s_mode_layer = NULL;
}

// ---------------------------------------------------------------------------
// App lifecycle

static Window *make_window(WindowHandlers handlers, ClickConfigProvider click_config) {
  Window *window = window_create();
  window_set_window_handlers(window, handlers);
  if (click_config) {
    window_set_click_config_provider(window, click_config);
  }
  return window;
}

static void init(void) {
  s_main_window = make_window((WindowHandlers) {
    .load = main_window_load, .unload = main_window_unload
  }, main_click_config);
  s_menu_window = make_window((WindowHandlers) {
    .load = menu_window_load, .unload = menu_window_unload
  }, NULL);
  s_setpoint_window = make_window((WindowHandlers) {
    .load = sp_window_load, .unload = sp_window_unload
  }, sp_click_config);
  s_confirm_window = make_window((WindowHandlers) {
    .load = confirm_window_load, .unload = confirm_window_unload
  }, confirm_click_config);

  app_message_register_inbox_received(inbox_received_handler);
  app_message_register_outbox_sent(outbox_sent_handler);
  app_message_register_outbox_failed(outbox_failed_handler);
  app_message_open(512, 64);

  window_stack_push(s_main_window, true);
  s_poll_timer = app_timer_register(POLL_INTERVAL_MS, poll_timer_callback, NULL);
}

static void deinit(void) {
  app_timer_cancel(s_poll_timer);
  window_destroy(s_confirm_window);
  window_destroy(s_setpoint_window);
  window_destroy(s_menu_window);
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
