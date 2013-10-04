#include "pebble_os.h"
#include "pebble_app.h"
#include "pebble_fonts.h"

#include "globals.h"

#define MY_UUID { 0x91, 0x41, 0xB6, 0x28, 0xBC, 0x89, 0x49, 0x8E, 0xB1, 0x47, 0x04, 0x9F, 0x49, 0xC0, 0x99, 0xAD }

PBL_APP_INFO(MY_UUID,
             "SmartFace", "John Flanagan",
             1, 0, /* App version */
             RESOURCE_ID_APP_ICON,
             APP_INFO_WATCH_FACE);

#define STRING_LENGTH 255
#define NUM_WEATHER_IMAGES  8

AppMessageResult sm_message_out_get(DictionaryIterator **iter_out);
void reset_sequence_number();
char* int_to_str(int num, char *outbuf);
void sendCommand(int key);
void sendCommandInt(int key, int param);
void rcv(DictionaryIterator *received, void *context);
void failed(DictionaryIterator *failed, AppMessageResult reason, void *context);
void dropped(void *context, AppMessageResult reason);
void battery_layer_update_callback(Layer *me, GContext* ctx);
void handle_init(AppContextRef ctx);
void handle_minute_tick(AppContextRef ctx, PebbleTickEvent *t);
void handle_deinit(AppContextRef ctx);
void reset();

AppContextRef g_app_context;

static Window window;

static Layer date_time_layer;
static Layer weather_layer;
static Layer calendar_layer;
static Layer battery_layer;
static Layer line_layer;

static TextLayer text_date_layer, text_time_layer;

static TextLayer text_weather_cond_layer, text_weather_temp_layer, text_battery_layer;
static TextLayer calendar_date_layer, calendar_text_layer;

static BitmapLayer weather_image, battery_image_layer;

static char string_buffer[STRING_LENGTH];
static char weather_cond_str[STRING_LENGTH], weather_temp_str[5];
static int weather_img, batteryPercent;

static char calendar_date_str[STRING_LENGTH], calendar_text_str[STRING_LENGTH];

HeapBitmap battery_image;
HeapBitmap weather_status_imgs[NUM_WEATHER_IMAGES];

static AppTimerHandle timerUpdateCalendar = 0;
static AppTimerHandle timerUpdateWeather = 0;
static AppTimerHandle timerRetry = 0;

const int WEATHER_IMG_IDS[] = {
  RESOURCE_ID_IMAGE_SUN,
  RESOURCE_ID_IMAGE_RAIN,
  RESOURCE_ID_IMAGE_CLOUD,
  RESOURCE_ID_IMAGE_SUN_CLOUD,
  RESOURCE_ID_IMAGE_FOG,
  RESOURCE_ID_IMAGE_WIND,
  RESOURCE_ID_IMAGE_SNOW,
  RESOURCE_ID_IMAGE_THUNDER
};

static uint32_t s_sequence_number = 0xFFFFFFFE;

AppMessageResult sm_message_out_get(DictionaryIterator **iter_out) {
    AppMessageResult result = app_message_out_get(iter_out);
    if(result != APP_MSG_OK) return result;

    dict_write_int32(*iter_out, SM_SEQUENCE_NUMBER_KEY, ++s_sequence_number);

    if(s_sequence_number == 0xFFFFFFFF) {
        s_sequence_number = 1;
    }

    return APP_MSG_OK;
}

void reset_sequence_number() {
    DictionaryIterator *iter = NULL;
    app_message_out_get(&iter);
    if(!iter) return;

    dict_write_int32(iter, SM_SEQUENCE_NUMBER_KEY, 0xFFFFFFFF);

    app_message_out_send();
    app_message_out_release();
}

char* int_to_str(int num, char *outbuf) {
    int digit, i=0, j=0;
    char buf[STRING_LENGTH];
    bool negative=false;

    if (num < 0) {
        negative = true;
        num = -1 * num;
    }

    for (i = 0; i < STRING_LENGTH; i++) {
        digit = num % 10;
        if (num == 0 && i > 0) {
            break;
        } else {
            buf[i] = '0' + digit;
        }

        num /= 10;
    }

    if (negative) {
        buf[i++] = '-';
    }

    buf[i--] = '\0';

    while (i >= 0) {
        outbuf[j++] = buf[i--];
    }

    outbuf[j++] = '%';
    outbuf[j] = '\0';

    return outbuf;
}

void sendCommand(int key) {
    DictionaryIterator* iterout;
    sm_message_out_get(&iterout);
    if(!iterout) return;

    dict_write_int8(iterout, key, -1);
    app_message_out_send();
    app_message_out_release();
}

void sendCommandInt(int key, int param) {
    DictionaryIterator* iterout;
    sm_message_out_get(&iterout);
    if(!iterout) return;

    dict_write_int8(iterout, key, param);
    app_message_out_send();
    app_message_out_release();
}

void rcv(DictionaryIterator *received, void *context) {
    // Got a message callback
    Tuple *t;

    t=dict_find(received, SM_WEATHER_COND_KEY);
    if (t!=NULL) {
        memcpy(weather_cond_str, t->value->cstring, strlen(t->value->cstring));
        weather_cond_str[strlen(t->value->cstring)] = '\0';
        text_layer_set_text(&text_weather_cond_layer, weather_cond_str);
    }

    t=dict_find(received, SM_WEATHER_TEMP_KEY);
    if (t!=NULL) {
        memcpy(weather_temp_str, t->value->cstring, strlen(t->value->cstring));
        weather_temp_str[strlen(t->value->cstring)] = '\0';
        text_layer_set_text(&text_weather_temp_layer, weather_temp_str);

        layer_set_hidden(&text_weather_cond_layer.layer, true);
        layer_set_hidden(&text_weather_temp_layer.layer, false);
    }

    t=dict_find(received, SM_WEATHER_ICON_KEY);
    if (t!=NULL) {
        bitmap_layer_set_bitmap(&weather_image, &weather_status_imgs[t->value->uint8].bmp);
    }

    t=dict_find(received, SM_COUNT_BATTERY_KEY);
    if (t!=NULL) {
        batteryPercent = t->value->uint8;
        layer_mark_dirty(&battery_layer);
        text_layer_set_text(&text_battery_layer, int_to_str(batteryPercent, string_buffer) );
    }

    t=dict_find(received, SM_STATUS_CAL_TIME_KEY);
    if (t!=NULL) {
        memcpy(calendar_date_str, t->value->cstring, strlen(t->value->cstring));
        calendar_date_str[strlen(t->value->cstring)] = '\0';
        text_layer_set_text(&calendar_date_layer, calendar_date_str);
    }

    t=dict_find(received, SM_STATUS_CAL_TEXT_KEY);
    if (t!=NULL) {
        memcpy(calendar_text_str, t->value->cstring, strlen(t->value->cstring));
        calendar_text_str[strlen(t->value->cstring)] = '\0';
        text_layer_set_text(&calendar_text_layer, calendar_text_str);
    }

    t=dict_find(received, SM_STATUS_UPD_WEATHER_KEY);
    if (t!=NULL) {
        int interval = t->value->int32 * 1000;

        app_timer_cancel_event(g_app_context, timerUpdateWeather);
        timerUpdateWeather = app_timer_send_event(g_app_context, interval /* milliseconds */, 1);
    }

    t=dict_find(received, SM_STATUS_UPD_CAL_KEY);
    if (t!=NULL) {
        int interval = t->value->int32 * 1000;

        app_timer_cancel_event(g_app_context, timerUpdateCalendar);
        timerUpdateCalendar = app_timer_send_event(g_app_context, interval /* milliseconds */, 2);
    }
}

void failed(DictionaryIterator *failed, AppMessageResult reason, void *context) {
    static char time_text[14];
    char *time_format;

    PblTm time;
    get_time(&time);

    if (clock_is_24h_style()) {
      time_format = "%m/%d %H:%M";
    } else {
      time_format = "%m/%d %I:%M%p";
    }

    string_format_time(time_text, sizeof(time_text), time_format, &time);

    if (!clock_is_24h_style() && (time_text[6] == '0')) {
      memmove(time_text, &time_text[7], sizeof(time_text) - 1);
    }

    text_layer_set_text(&calendar_date_layer, time_text);
    text_layer_set_text(&calendar_text_layer, "Failed to connect");

    app_timer_cancel_event(g_app_context, timerRetry);
    timerUpdateWeather = app_timer_send_event(g_app_context, 600000 /* milliseconds */, 3);
}

void dropped(void *context, AppMessageResult reason){
     // DO SOMETHING WITH THE DROPPED REASON / DISPLAY AN ERROR / RESEND
}

void battery_layer_update_callback(Layer *me, GContext* ctx) {
    //draw the remaining battery percentage
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_fill_color(ctx, GColorWhite);

    int x = 18 - batteryPercent * 16 / 100;
    int y = 2;
    int w = batteryPercent * 16 / 100;
    int h = 8;

    graphics_fill_rect(ctx, GRect(x, y, w, h), 0, GCornerNone);
}

void line_layer_update_callback(Layer *me, GContext* ctx) {
  graphics_context_set_stroke_color(ctx, GColorWhite);

  graphics_draw_line(ctx, GPoint(8, 30), GPoint(131, 30));
  graphics_draw_line(ctx, GPoint(8, 31), GPoint(131, 31));
}

void reset() {
    layer_set_hidden(&text_weather_temp_layer.layer, true);
    layer_set_hidden(&text_weather_cond_layer.layer, false);
    text_layer_set_text(&text_weather_cond_layer, "Updating...");

    sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);
}

void handle_init(AppContextRef ctx) {
    g_app_context = ctx;

    window_init(&window, "Window Name");
    window_stack_push(&window, true /* Animated */);
    window_set_background_color(&window, GColorBlack);

    resource_init_current_app(&APP_RESOURCES);

    layer_init(&calendar_layer, GRect(0, 123, 144, 45));
    layer_add_child(&window.layer, &calendar_layer);

    layer_init(&weather_layer, GRect(0, 0, 144, 45));
    layer_add_child(&window.layer, &weather_layer);

    layer_init(&date_time_layer, GRect(0, 43, 144, 78));
    layer_add_child(&window.layer, &date_time_layer);

    //init layers for time and date
    text_layer_init(&text_date_layer, window.layer.frame);
    text_layer_set_text_color(&text_date_layer, GColorWhite);
    text_layer_set_background_color(&text_date_layer, GColorClear);
    layer_set_frame(&text_date_layer.layer, GRect(8, 0, 144-8, 21));
    text_layer_set_font(&text_date_layer, fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_ROBOTO_CONDENSED_21)));
    layer_add_child(&date_time_layer, &text_date_layer.layer);

    text_layer_init(&text_time_layer, window.layer.frame);
    text_layer_set_text_color(&text_time_layer, GColorWhite);
    text_layer_set_background_color(&text_time_layer, GColorClear);
    layer_set_frame(&text_time_layer.layer, GRect(8, 25, 144-8, 49));
    text_layer_set_font(&text_time_layer, fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_ROBOTO_BOLD_SUBSET_49)));
    layer_add_child(&date_time_layer, &text_time_layer.layer);

    layer_init(&line_layer, window.layer.frame);
    line_layer.update_proc = &line_layer_update_callback;
    layer_add_child(&date_time_layer, &line_layer);

    //init weather layer and add weather image, weather condition, temperature, and battery indicator
    heap_bitmap_init(&battery_image, RESOURCE_ID_IMAGE_BATTERY);

    bitmap_layer_init(&battery_image_layer, GRect(107, 8, 23, 14));
    layer_add_child(&weather_layer, &battery_image_layer.layer);
    bitmap_layer_set_bitmap(&battery_image_layer, &battery_image.bmp);

    text_layer_init(&text_battery_layer, GRect(99, 20, 40, 60));
    text_layer_set_text_alignment(&text_battery_layer, GTextAlignmentCenter);
    text_layer_set_text_color(&text_battery_layer, GColorWhite);
    text_layer_set_background_color(&text_battery_layer, GColorClear);
    text_layer_set_font(&text_battery_layer,  fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    layer_add_child(&weather_layer, &text_battery_layer.layer);
    text_layer_set_text(&text_battery_layer, "-");

    layer_init(&battery_layer, GRect(109, 9, 19, 11));
    battery_layer.update_proc = &battery_layer_update_callback;
    layer_add_child(&weather_layer, &battery_layer);

    batteryPercent = 100;
    layer_mark_dirty(&battery_layer);

    text_layer_init(&text_weather_cond_layer, GRect(48, 1, 48, 40)); // GRect(5, 2, 47, 40)
    text_layer_set_text_alignment(&text_weather_cond_layer, GTextAlignmentCenter);
    text_layer_set_text_color(&text_weather_cond_layer, GColorWhite);
    text_layer_set_background_color(&text_weather_cond_layer, GColorClear);
    text_layer_set_font(&text_weather_cond_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
    layer_add_child(&weather_layer, &text_weather_cond_layer.layer);

    layer_set_hidden(&text_weather_cond_layer.layer, false);
    text_layer_set_text(&text_weather_cond_layer, "Updating...");

    //init weather images
    for (int i = 0; i < NUM_WEATHER_IMAGES; i++) {
        heap_bitmap_init(&weather_status_imgs[i], WEATHER_IMG_IDS[i]);
    }

    weather_img = 0;

    bitmap_layer_init(&weather_image, GRect(5, 2, 40, 40)); // GRect(52, 2, 40, 40)
    layer_add_child(&weather_layer, &weather_image.layer);
    bitmap_layer_set_bitmap(&weather_image, &weather_status_imgs[0].bmp);

    text_layer_init(&text_weather_temp_layer, GRect(48, 3, 48, 40)); // GRect(98, 4, 47, 40)
    text_layer_set_text_alignment(&text_weather_temp_layer, GTextAlignmentCenter);
    text_layer_set_text_color(&text_weather_temp_layer, GColorWhite);
    text_layer_set_background_color(&text_weather_temp_layer, GColorClear);
    text_layer_set_font(&text_weather_temp_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28));
    layer_add_child(&weather_layer, &text_weather_temp_layer.layer);
    text_layer_set_text(&text_weather_temp_layer, "-°");

    layer_set_hidden(&text_weather_temp_layer.layer, true);

    //init calendar layer
    text_layer_init(&calendar_date_layer, GRect(6, 0, 132, 21));
    text_layer_set_text_alignment(&calendar_date_layer, GTextAlignmentLeft);
    text_layer_set_text_color(&calendar_date_layer, GColorWhite);
    text_layer_set_background_color(&calendar_date_layer, GColorClear);
    text_layer_set_font(&calendar_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
    layer_add_child(&calendar_layer, &calendar_date_layer.layer);
    text_layer_set_text(&calendar_date_layer, "No Upcoming");

    text_layer_init(&calendar_text_layer, GRect(6, 15, 132, 28));
    text_layer_set_text_alignment(&calendar_text_layer, GTextAlignmentLeft);
    text_layer_set_text_color(&calendar_text_layer, GColorWhite);
    text_layer_set_background_color(&calendar_text_layer, GColorClear);
    text_layer_set_font(&calendar_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    layer_add_child(&calendar_layer, &calendar_text_layer.layer);
    text_layer_set_text(&calendar_text_layer, "Appointment");

    reset();
}

void handle_minute_tick(AppContextRef ctx, PebbleTickEvent *t) {
  /* Display the time */
  static char time_text[] = "00:00";
  static char date_text[] = "Xxxxxxxxx 00";

  char *time_format;

  string_format_time(date_text, sizeof(date_text), "%a %b %e", t->tick_time);
  text_layer_set_text(&text_date_layer, date_text);

  if (clock_is_24h_style()) {
    time_format = "%R";
  } else {
    time_format = "%I:%M";
  }

  string_format_time(time_text, sizeof(time_text), time_format, t->tick_time);

  if (!clock_is_24h_style() && (time_text[0] == '0')) {
    memmove(time_text, &time_text[1], sizeof(time_text) - 1);
  }

  text_layer_set_text(&text_time_layer, time_text);
}

void handle_deinit(AppContextRef ctx) {
    for (int i=0; i<NUM_WEATHER_IMAGES; i++) {
        heap_bitmap_deinit(&weather_status_imgs[i]);
    }
    sendCommandInt(SM_SCREEN_EXIT_KEY, STATUS_SCREEN_APP);
}

void handle_timer(AppContextRef ctx, AppTimerHandle handle, uint32_t cookie) {
    /* Request new data from the phone once the timers expire */
    switch (cookie) {
        case 1:
            sendCommand(SM_STATUS_UPD_WEATHER_KEY);
            break;
        case 2:
            sendCommand(SM_STATUS_UPD_CAL_KEY);
            break;
        case 3:
            app_timer_cancel_event(g_app_context, timerRetry);
            sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);
            break;
    }
}

void pbl_main(void *params) {
  PebbleAppHandlers handlers = {
    .init_handler = &handle_init,
    .deinit_handler = &handle_deinit,
    .messaging_info = {
        .buffer_sizes = {
            .inbound = 124,
            .outbound = 256
        },
        .default_callbacks.callbacks = {
            .in_received = rcv,
            .in_dropped = dropped,
            .out_failed = failed
        }
    },
    .tick_info = {
      .tick_handler = &handle_minute_tick,
      .tick_units = MINUTE_UNIT
    },
    .timer_handler = &handle_timer,
  };
  app_event_loop(params, &handlers);
}
