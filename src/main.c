#include <pebble.h>

// ─── Message keys (must match appinfo.json appKeys) ────────────────────────
#define KEY_REQUEST_ITEMS    0
#define KEY_ITEM_COUNT       1
#define KEY_ITEM_INDEX       2
#define KEY_ITEM_SUMMARY     3
#define KEY_ITEM_STATUS      4
#define KEY_TOGGLE_ITEM      5
#define KEY_DELETE_COMPLETED 6
#define KEY_ERROR_MESSAGE    7
#define KEY_STATUS_MESSAGE   8
#define KEY_LIST_NAME        9

// ─── Layout ────────────────────────────────────────────────────────────────
#define TIME_BAR_H      32
#define MAX_ITEMS       50
#define MAX_SUMMARY_LEN 64
#define ROW_HEIGHT_ITEM 50
#define ROW_HEIGHT_ACT  44
#define HEADER_HEIGHT   32
#define CHECKBOX_SIZE   22
#define CHECKBOX_LEFT    6
#define TEXT_LEFT       34

// ─── HA blue — closest Pebble palette colour to #41BDF5 ────────────────────
#ifdef PBL_COLOR
  #define COLOR_HA_BLUE GColorVividCerulean   // #00AAFF
#else
  #define COLOR_HA_BLUE GColorBlack
#endif

// ─── State ─────────────────────────────────────────────────────────────────
typedef struct {
  char summary[MAX_SUMMARY_LEN];
  bool completed;
} TodoItem;

static Window      *s_window;
static TextLayer   *s_time_layer;
static MenuLayer   *s_menu_layer;
static TextLayer   *s_status_layer;

static TodoItem s_items[MAX_ITEMS];
static int      s_item_count = 0;
static bool     s_loading    = true;
static char     s_status_buf[64];
static char     s_list_name[48] = "To-Do";
static char     s_time_buf[10];

// ─── Time bar ──────────────────────────────────────────────────────────────

static void update_time(struct tm *tick_time) {
  strftime(s_time_buf, sizeof(s_time_buf),
           clock_is_24h_style() ? "%H:%M" : "%I:%M %p", tick_time);
  text_layer_set_text(s_time_layer, s_time_buf);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time(tick_time);
}

// ─── Status strip ──────────────────────────────────────────────────────────

static void show_status(const char *msg) {
  strncpy(s_status_buf, msg, sizeof(s_status_buf) - 1);
  text_layer_set_text(s_status_layer, s_status_buf);
  layer_set_hidden(text_layer_get_layer(s_status_layer), false);
}

static void hide_status(void) {
  layer_set_hidden(text_layer_get_layer(s_status_layer), true);
}

static void request_items(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_int8(iter, KEY_REQUEST_ITEMS, 1);
    app_message_outbox_send();
  }
  show_status("Loading...");
}

// ─── Drawing ───────────────────────────────────────────────────────────────

static void draw_checkbox(GContext *ctx, GRect cell_bounds, bool checked, bool highlighted) {
  int y_center = cell_bounds.origin.y + cell_bounds.size.h / 2;
  GRect box = GRect(
    cell_bounds.origin.x + CHECKBOX_LEFT,
    y_center - CHECKBOX_SIZE / 2,
    CHECKBOX_SIZE,
    CHECKBOX_SIZE
  );

  GColor fg = highlighted ? GColorWhite : GColorBlack;
  graphics_context_set_stroke_color(ctx, fg);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_rect(ctx, box);

  if (checked) {
    GPoint p1 = GPoint(box.origin.x + 3,  box.origin.y + CHECKBOX_SIZE / 2);
    GPoint p2 = GPoint(box.origin.x + 8,  box.origin.y + CHECKBOX_SIZE - 4);
    GPoint p3 = GPoint(box.origin.x + CHECKBOX_SIZE - 3, box.origin.y + 4);
    graphics_context_set_stroke_width(ctx, 3);
    graphics_draw_line(ctx, p1, p2);
    graphics_draw_line(ctx, p2, p3);
  }
}

// ─── Sort ──────────────────────────────────────────────────────────────────
// Stable bubble sort: incomplete items first, completed at the bottom.

static void sort_items(void) {
  // Bubble sort: incomplete before completed, alphabetical within each group
  for (int i = 0; i < s_item_count - 1; i++) {
    for (int j = 0; j < s_item_count - 1 - i; j++) {
      TodoItem *a = &s_items[j];
      TodoItem *b = &s_items[j + 1];
      bool swap = false;
      if (a->completed != b->completed) {
        swap = a->completed && !b->completed;  // completed sinks to bottom
      } else {
        swap = strcmp(a->summary, b->summary) > 0;  // alphabetical within group
      }
      if (swap) {
        TodoItem tmp = *a;
        *a = *b;
        *b = tmp;
      }
    }
  }
}

// ─── MenuLayer callbacks ───────────────────────────────────────────────────

static uint16_t cb_num_sections(MenuLayer *ml, void *ctx) { return 2; }

static uint16_t cb_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  if (section == 0) return (s_item_count > 0) ? (uint16_t)s_item_count : 1;
  return 1;
}

static int16_t cb_cell_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return (idx->section == 0) ? ROW_HEIGHT_ITEM : ROW_HEIGHT_ACT;
}

static int16_t cb_header_height(MenuLayer *ml, uint16_t section, void *ctx) {
  return HEADER_HEIGHT;
}

static void cb_draw_header(GContext *ctx, const Layer *cell_layer, uint16_t section, void *ctx2) {
  GRect bounds = layer_get_bounds(cell_layer);

  if (section == 0) {
    // Draw list name with HA blue background
    graphics_context_set_fill_color(ctx, COLOR_HA_BLUE);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorBlack);
    GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
    GRect text_rect = GRect(4, -2, bounds.size.w - 8, bounds.size.h + 2);
    graphics_draw_text(ctx, s_list_name, font, text_rect,
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  } else {
    menu_cell_basic_header_draw(ctx, cell_layer, "ACTIONS");
  }
}

static void cb_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *idx, void *ctx2) {
  GRect bounds = layer_get_bounds(cell_layer);
  bool highlighted = menu_cell_layer_is_highlighted(cell_layer);

  if (idx->section == 1) {
    GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    GRect text_rect = GRect(10, 0, bounds.size.w - 12, bounds.size.h);
    graphics_draw_text(ctx, "Delete Completed", font, text_rect,
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    return;
  }

  if (s_item_count == 0) {
    GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_24);
    GRect text_rect = GRect(6, 0, bounds.size.w - 8, bounds.size.h);
    graphics_draw_text(ctx, s_loading ? "Loading..." : "No items found",
                       font, text_rect,
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    return;
  }

  TodoItem *item = &s_items[idx->row];
  draw_checkbox(ctx, bounds, item->completed, highlighted);

  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  GRect text_rect = GRect(TEXT_LEFT, 2, bounds.size.w - TEXT_LEFT - 4, bounds.size.h - 4);
  graphics_draw_text(ctx, item->summary, font, text_rect,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void cb_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (idx->section == 1) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
      dict_write_int8(iter, KEY_DELETE_COMPLETED, 1);
      app_message_outbox_send();
    }
    show_status("Deleting...");
    return;
  }

  if (s_item_count == 0) return;

  int row = idx->row;
  s_items[row].completed = !s_items[row].completed;
  bool new_status = s_items[row].completed;

  // Tell JS before re-sorting so the index still matches JS's array
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_int32(iter, KEY_TOGGLE_ITEM, row);
    dict_write_int8(iter,  KEY_ITEM_STATUS, new_status ? 1 : 0);
    app_message_outbox_send();
  }

  // Re-sort locally for instant visual feedback
  sort_items();
  menu_layer_reload_data(s_menu_layer);
}

// ─── AppMessage callbacks ──────────────────────────────────────────────────

static void inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *t;

  t = dict_find(iter, KEY_LIST_NAME);
  if (t) {
    strncpy(s_list_name, t->value->cstring, sizeof(s_list_name) - 1);
    menu_layer_reload_data(s_menu_layer);
  }

  t = dict_find(iter, KEY_ITEM_COUNT);
  if (t) {
    s_item_count = (int)t->value->int32;
    if (s_item_count > MAX_ITEMS) s_item_count = MAX_ITEMS;
    memset(s_items, 0, sizeof(s_items));
    s_loading = false;
  }

  t = dict_find(iter, KEY_ITEM_INDEX);
  if (t) {
    int idx = (int)t->value->int32;
    if (idx >= 0 && idx < MAX_ITEMS) {
      Tuple *sum = dict_find(iter, KEY_ITEM_SUMMARY);
      Tuple *sta = dict_find(iter, KEY_ITEM_STATUS);
      if (sum) strncpy(s_items[idx].summary, sum->value->cstring, MAX_SUMMARY_LEN - 1);
      if (sta) s_items[idx].completed = (sta->value->int8 == 1);
    }
    hide_status();
  }

  t = dict_find(iter, KEY_ERROR_MESSAGE);
  if (t) {
    s_loading = false;
    show_status(t->value->cstring);
  }

  t = dict_find(iter, KEY_STATUS_MESSAGE);
  if (t) {
    show_status(t->value->cstring);
    app_timer_register(2000, (AppTimerCallback)request_items, NULL);
  }

  menu_layer_reload_data(s_menu_layer);
}

static void inbox_dropped(AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Inbox dropped: %d", (int)reason);
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox failed: %d", (int)reason);
  show_status("Send failed");
}

// ─── Window lifecycle ──────────────────────────────────────────────────────

static void window_load(Window *window) {
  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);

  // ── Time bar (top strip, HA blue background, black text) ─────────────────
  s_time_layer = text_layer_create(GRect(0, 0, bounds.size.w, TIME_BAR_H));
  text_layer_set_background_color(s_time_layer, COLOR_HA_BLUE);
  text_layer_set_text_color(s_time_layer, GColorBlack);
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  // Seed the time display immediately
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  update_time(t);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  // ── Menu (below time bar) ─────────────────────────────────────────────────
  GRect menu_rect = GRect(0, TIME_BAR_H, bounds.size.w, bounds.size.h - TIME_BAR_H);
  s_menu_layer = menu_layer_create(menu_rect);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_sections  = cb_num_sections,
    .get_num_rows      = cb_num_rows,
    .get_cell_height   = cb_cell_height,
    .get_header_height = cb_header_height,
    .draw_header       = cb_draw_header,
    .draw_row          = cb_draw_row,
    .select_click      = cb_select,
  });
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));

  // ── Floating status strip ─────────────────────────────────────────────────
  int strip_h = 36;
  s_status_layer = text_layer_create(
    GRect(0, bounds.size.h / 2 - strip_h / 2, bounds.size.w, strip_h));
  text_layer_set_background_color(s_status_layer, GColorWhite);
  text_layer_set_text_color(s_status_layer, GColorBlack);
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  layer_set_hidden(text_layer_get_layer(s_status_layer), true);
  layer_add_child(root, text_layer_get_layer(s_status_layer));
}

static void window_unload(Window *window) {
  tick_timer_service_unsubscribe();
  text_layer_destroy(s_time_layer);
  menu_layer_destroy(s_menu_layer);
  text_layer_destroy(s_status_layer);
}

// ─── App lifecycle ─────────────────────────────────────────────────────────

static void init(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(app_message_inbox_size_maximum(),
                   app_message_outbox_size_maximum());

  request_items();
}

static void deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
