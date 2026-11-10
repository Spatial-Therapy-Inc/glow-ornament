// 摆件 (glow-ornament): ten slow, softly lit ornaments for a Tikpal Voice that stands plugged in on a desk. Swipe for
// the next one; touch it and it answers; tilt or shake it and it moves. It follows the shared screen-off time (set to
// never, it stays lit), and goes to sleep face down or at a short press of the side key. The rules are in orn.c, the
// pictures in orn_draw.c and scenes.c; this file is the template's start-up order (NVS -> yaykit_app -> board -> side
// key -> Yaykit Link -> screen -> console -> yk_app_ready()) and the glue.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "cjk_font.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "orn.h"
#include "scenes.h"
#include "sdkconfig.h"
#include "yaykit_app.h"
#include "yaykit_key.h"
#include "yaykit_link.h"
#include "yaykit_motion.h"
#include "yaykit_ui.h"
#include "yk_app_summary.h"
#include "yk_apps.h"
#include "yk_hint.h"
#include "yk_more.h"
#include "yk_vc_view.h"

#define APP_SLUG "glow-ornament"
static const char *TAG = "glow-ornament";

#define NVS_NAMESPACE "yk_glowornament"
#define NVS_KEY_SHAPE "shape"
#define TICK_MS 33
#define SAVE_DELAY_US (2 * 1000 * 1000)
#define PERF_MS 5000
#define LEVEL_EVERY_MS 100  // the panel's level is sent at most this often: each send waits for the panel's bus
#define TILT_GAIN 1.6f  // the screen's sideways lean (gravity's x, -1..1) to the ornaments' tilt

enum {
    PENDING_SIDE = 1 << 0,
    PENDING_SHAKE = 1 << 1,
    PENDING_DOWN = 1 << 2,
    PENDING_TAP = 1 << 3,  // the rest only from `yk cmd glowornament.input`, in development builds
    PENDING_NEXT = 1 << 4,
    PENDING_PREV = 1 << 5,
};

static orn_t s_orn;  // only the LVGL task changes it
static orn_touch_t s_touch;
static atomic_uint s_pending;
static atomic_bool s_leaving;
static atomic_int s_shape_to_save;  // the shape to be saved (2 s after the last swipe, or when leaving)
static atomic_int s_saved_shape;    // the shape in NVS now
static atomic_int s_shape_cmd = -1;
static esp_timer_handle_t s_save_timer;

static lv_obj_t *s_root, *s_screen;
// The screen's children once it was made (the scene's root, the touch layer, the hint): anything else on it later is
// the system's, put over the app (the pull-down sheet is made there, its top child).
#define OWN_MAX 12
static lv_obj_t *s_own[OWN_MAX];
static uint32_t s_owns;
static int s_built = -1, s_level, s_shown_dx, s_shown_dy;
static int64_t s_last_ms, s_level_ms;
static float s_last_t;

// How fast it runs, for the logs and `glowornament.status`: our frames, the work in them, and LVGL's redraws (from
// the display's own events: how long each took, sending included, and how many pixels were marked to redraw).
static int s_frames, s_refrs;
static int64_t s_perf_at, s_work_us, s_refr_at, s_refr_us, s_inv_px;
static float s_fps, s_work_ms, s_refr_ms, s_inv_kpx;

#ifdef CONFIG_YAY_DEV
static atomic_int s_tilt_milli = INT32_MIN;  // `yk cmd glowornament.tilt`: INT32_MIN, read the sensor
static atomic_int s_night_cmd = -1;          // `yk cmd glowornament.night`: -1, by the clock
#endif

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void pend(unsigned bit) { atomic_fetch_or(&s_pending, bit); }

/** Through yaykit_app (yk_app_on_key). Only a tap is the app's; a hold asks 「回到 Pip 说？」, a double opens the ring. */
static bool on_key(const yk_key_event_t *ev, void *ctx) {
    (void)ctx;
    if (ev->type != YK_KEY_TAP) return false;
    pend(PENDING_SIDE);
    return true;
}

static void on_motion(const yk_motion_event_t *ev, void *ctx) {
    (void)ctx;
    if (ev->kind == YK_MOTION_SHAKE_START) pend(PENDING_SHAKE);
    else if (ev->kind == YK_MOTION_FACE_DOWN) pend(PENDING_DOWN);
}

/** In the LVGL task. The line says which input really arrived, for testing by hand. */
static void input(orn_input_t in) {
    static const char *const names[] = {"tap", "next", "prev", "shake", "down", "side"};
    printf("ORN_INPUT %s\n", names[in]);
    orn_input(&s_orn, in);
}

/** The whole screen is one touch area: a tap, or a swipe to the next or the previous ornament. In the LVGL task. */
static void on_touch(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        yk_app_input();
        orn_touch_down(&s_touch, p.x, p.y, now_ms());
        break;
    case LV_EVENT_PRESSING:
        orn_touch_move(&s_touch, p.x, p.y);
        break;
    case LV_EVENT_RELEASED:
        switch (orn_touch_up(&s_touch, p.x, p.y)) {
        case ORN_GESTURE_TAP: input(ORN_IN_TAP); break;
        case ORN_GESTURE_LEFT: input(ORN_IN_NEXT); break;
        case ORN_GESTURE_RIGHT: input(ORN_IN_PREV); break;
        case ORN_GESTURE_NONE: break;
        }
        break;
    case LV_EVENT_PRESS_LOST:
        orn_touch_cancel(&s_touch);
        break;
    default:
        break;
    }
}

/** A timer's task, whose stack is in internal RAM, as an NVS write needs. A burst of swipes is one write. */
/** Writes the shape to be saved, if NVS does not have it yet. Its task's stack must be in internal RAM. */
static void write_shape(void) {
    const int shape = atomic_load(&s_shape_to_save);
    if (shape == atomic_load(&s_saved_shape)) return;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_SHAPE, (uint8_t)shape);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) atomic_store(&s_saved_shape, shape);
    else ESP_LOGW(TAG, "shape not saved: %s", esp_err_to_name(err));
}

/** The save timer: 2 s after the last swipe, so a run of swipes is one write. */
static void save_shape(void *arg) {
    (void)arg;
    write_shape();
}

static int load_shape(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_SHAPE, &v);
        nvs_close(h);
    }
    return v < ORN_COUNT ? v : 0;
}

static void on_leave(void *ctx) {
    (void)ctx;
    atomic_store(&s_leaving, true);  // the panel stays as it is until the restart
    // A shape swiped to in the last 2 s is still waiting for the save timer: written now, once (yaykit_app's own
    // task for this, its stack in internal RAM), or the next open would show the one before.
    esp_timer_stop(s_save_timer);
    write_shape();
}

/** Which side is down, -1 .. 1: the sideways part of gravity in screen coordinates. */
static float read_tilt(void) {
#ifdef CONFIG_YAY_DEV
    const int milli = atomic_load(&s_tilt_milli);
    if (milli != INT32_MIN) return milli / 1000.0f;
#endif
    if (!yk_motion_running()) return 0;
    yk_motion_state_t st;
    yk_motion_state(&st);
    return st.now.gravity[0] * TILT_GAIN;
}

static bool read_night(void) {
#ifdef CONFIG_YAY_DEV
    const int cmd = atomic_load(&s_night_cmd);
    if (cmd >= 0) return cmd;
#endif
    const time_t now = time(NULL);
    struct tm tm;
    if (now < YK_APP_TIME_VALID_S || !localtime_r(&now, &tm)) return false;  // no clock: never night
    return orn_is_night(tm.tm_hour);
}

static orn_frame_t frame_now(float dt) {
    return (orn_frame_t){.t = s_orn.t, .dt = dt, .tilt = s_orn.tilt, .poke = orn_since(&s_orn, s_orn.poke_at),
                         .shake = orn_since(&s_orn, s_orn.shake_at)};
}

static void build(int shape) {
    const int64_t at = esp_timer_get_time();
    orn_scene_build(shape, s_root);
    s_built = shape;
    // One frame at once: nothing shows where the objects were made (the top left) or what a buffer held before.
    const orn_frame_t f = frame_now(0);
    orn_scene_frame(&f);
    printf("ORN_SCENE %s build_ms=%d\n", orn_scene_key(shape), (int)((esp_timer_get_time() - at) / 1000));
}

static void on_display(lv_event_t *e) {
    switch (lv_event_get_code(e)) {
    case LV_EVENT_INVALIDATE_AREA: {
        const lv_area_t *a = lv_event_get_invalidated_area(e);
        if (a) s_inv_px += lv_area_get_size(a);
        break;
    }
    case LV_EVENT_REFR_START: s_refr_at = esp_timer_get_time(); break;
    case LV_EVENT_REFR_READY:
        if (s_refr_at) {
            const int64_t us = esp_timer_get_time() - s_refr_at;
            if (us > 1000) {  // something was drawn
                s_refr_us += us;
                s_refrs++;
            }
            s_refr_at = 0;
        }
        break;
    default: break;
    }
}

static void count_frame(int64_t now, int64_t work_us) {
    s_frames++;
    s_work_us += work_us;
    if (now - s_perf_at < PERF_MS) return;
    s_fps = s_frames * 1000.0f / (now - s_perf_at);
    s_work_ms = s_work_us / 1000.0f / (s_frames ? s_frames : 1);
    s_refr_ms = s_refr_us / 1000.0f / (s_refrs ? s_refrs : 1);
    s_inv_kpx = s_inv_px / 1000.0f / (s_frames ? s_frames : 1);
#ifdef CONFIG_YAY_DEV
    printf("ORN_PERF scene=%s fps=%.1f work_ms=%.1f refr_ms=%.1f inv_kpx=%.0f asleep=%d\n", orn_scene_key(s_built), s_fps,
           s_work_ms, s_refr_ms, s_inv_kpx, s_orn.asleep);
#endif
    s_frames = 0;
    s_refrs = 0;
    s_work_us = 0;
    s_refr_us = 0;
    s_inv_px = 0;
    s_perf_at = now;
}

/** Something of the system's over the app: the app ring, 「回到 Pip 说？」, 「更多」, the 应用码, or anything put on our
 *  screen after we made it (the pull-down sheet, which has no query of its own). In the LVGL task. */
static bool system_screen_up(void) {
    if (yk_apps_is_open() || yk_apps_ask_is_open() || yk_more_is_open() || yk_vc_showing()) return true;
    if (!s_screen) return false;
    const uint32_t n = lv_obj_get_child_count(s_screen);
    for (uint32_t i = 0; i < n; i++) {
        const lv_obj_t *c = lv_obj_get_child(s_screen, (int32_t)i);
        bool own = false;
        for (uint32_t k = 0; k < s_owns && !own; k++) own = s_own[k] == c;
        if (!own) return true;
    }
    return false;
}

/** LVGL timer: runs with the board lock held, so it may touch the screen. */
static void tick(lv_timer_t *timer) {
    (void)timer;
    const int64_t start = esp_timer_get_time();
    const int64_t now = now_ms();
    const int dt = (int)(now - s_last_ms);
    s_last_ms = now;

    const unsigned pending = atomic_exchange(&s_pending, 0);
    if (pending & PENDING_SIDE) input(ORN_IN_SIDE);
    if (pending & PENDING_SHAKE) input(ORN_IN_SHAKE);
    if (pending & PENDING_DOWN) input(ORN_IN_FACE_DOWN);
    if (pending & PENDING_TAP) input(ORN_IN_TAP);
    if (pending & PENDING_NEXT) input(ORN_IN_NEXT);
    if (pending & PENDING_PREV) input(ORN_IN_PREV);
    orn_touch_poll(&s_touch, now);  // a long press is not used, and is no tap
    const int cmd = atomic_exchange(&s_shape_cmd, -1);
    if (cmd >= 0) orn_set_shape(&s_orn, cmd);

    // The system's screens open in their own time; asking each tick keeps the panel up for them.
    orn_tick(&s_orn, dt, yk_app_screen_off_s(), read_tilt(), read_night(), system_screen_up());
    if (s_orn.shape_changed) {
        s_orn.shape_changed = false;
        atomic_store(&s_shape_to_save, s_orn.shape);
        esp_timer_stop(s_save_timer);
        esp_timer_start_once(s_save_timer, SAVE_DELAY_US);
    }
    if (s_orn.shape != s_built) build(s_orn.shape);

    float light = 0;
    if (!s_orn.asleep) {
        const orn_frame_t f = frame_now(s_orn.t - s_last_t);
        light = orn_scene_frame(&f);
        int dx, dy;
        orn_shift(now / 60000, &dx, &dy);
        if (dx != s_shown_dx || dy != s_shown_dy) {
            s_shown_dx = dx;
            s_shown_dy = dy;
            lv_obj_set_pos(s_root, dx, dy);
        }
    }
    s_last_t = s_orn.t;

    if (!atomic_load(&s_leaving)) {
        // Ramped by the time since the last send, so sending less often does not slow the ramp.
        const int since = (int)(now - s_level_ms > 1000 ? 1000 : now - s_level_ms);  // hours unchanged: no overflow
        const int target = orn_level(&s_orn, yk_app_brightness(), light);
        // A system screen up: its level at once, no ramp and no wait (asleep, the panel was dark under it).
        const bool at_once = s_orn.ui_open;
        const int level = at_once ? target : orn_ramp(s_level, target, since);
        if (level != s_level && (at_once || since >= LEVEL_EVERY_MS)) {
            s_level = level;
            s_level_ms = now;
            board_brightness_raw_set((uint8_t)level);
        }
    }
    count_frame(now, esp_timer_get_time() - start);
}

static esp_err_t on_command(const char *name, const cJSON *args, cJSON *reply, const char **code, void *ctx) {
    (void)ctx;
    (void)args;
    yk_app_input();
    esp_err_t err = yk_app_command(name, args, reply, code);  // app.exit, app.recovery, app.status
    if (err != ESP_ERR_NOT_SUPPORTED) return err;
    if (!strcmp(name, "glowornament.status")) {
        cJSON_AddStringToObject(reply, "scene", orn_scene_key(s_orn.shape));
        cJSON_AddNumberToObject(reply, "index", s_orn.shape);
        cJSON_AddBoolToObject(reply, "asleep", s_orn.asleep);
        cJSON_AddBoolToObject(reply, "fading", s_orn.fading);
        cJSON_AddBoolToObject(reply, "night", s_orn.night);
        cJSON_AddNumberToObject(reply, "tilt", roundf(s_orn.tilt * 100) / 100);
        cJSON_AddNumberToObject(reply, "level", s_level);
        cJSON_AddBoolToObject(reply, "systemScreen", s_orn.ui_open);
        cJSON_AddNumberToObject(reply, "fps", roundf(s_fps * 10) / 10);
        cJSON_AddNumberToObject(reply, "workMs", roundf(s_work_ms * 10) / 10);
        cJSON_AddNumberToObject(reply, "refrMs", roundf(s_refr_ms * 10) / 10);
        cJSON_AddNumberToObject(reply, "invKpx", roundf(s_inv_kpx));
        const time_t now = time(NULL);
        cJSON_AddNumberToObject(reply, "clock", now >= YK_APP_TIME_VALID_S ? (double)now : 0);  // 0: not known
        return ESP_OK;
    }
#ifdef CONFIG_YAY_DEV
    // Testing without a finger or a hand: the same inputs, a chosen ornament, a tilt and the night.
    if (!strcmp(name, "glowornament.input")) {
        const char *in = cJSON_GetStringValue(cJSON_GetObjectItem(args, "in"));
        static const struct { const char *name; unsigned bit; } inputs[] = {
            {"tap", PENDING_TAP}, {"next", PENDING_NEXT}, {"prev", PENDING_PREV},
            {"shake", PENDING_SHAKE}, {"down", PENDING_DOWN}, {"side", PENDING_SIDE},
        };
        for (size_t i = 0; in && i < sizeof(inputs) / sizeof(inputs[0]); i++) {
            if (strcmp(in, inputs[i].name)) continue;
            pend(inputs[i].bit);
            return ESP_OK;
        }
        *code = "BAD_INPUT";
        return ESP_ERR_INVALID_ARG;
    }
    if (!strcmp(name, "glowornament.shape")) {
        const cJSON *i = cJSON_GetObjectItem(args, "i");
        if (!cJSON_IsNumber(i) || i->valueint < 0 || i->valueint >= ORN_COUNT) {
            *code = "BAD_INPUT";
            return ESP_ERR_INVALID_ARG;
        }
        atomic_store(&s_shape_cmd, i->valueint);
        return ESP_OK;
    }
    if (!strcmp(name, "glowornament.tilt")) {
        const cJSON *x = cJSON_GetObjectItem(args, "x");
        atomic_store(&s_tilt_milli, cJSON_IsNumber(x) ? (int)lround(x->valuedouble * 1000) : INT32_MIN);
        return ESP_OK;
    }
    if (!strcmp(name, "glowornament.night")) {
        const cJSON *on = cJSON_GetObjectItem(args, "on");
        atomic_store(&s_night_cmd, cJSON_IsBool(on) ? cJSON_IsTrue(on) : -1);
        return ESP_OK;
    }
#endif
    *code = "UNKNOWN_COMMAND";
    return ESP_ERR_NOT_SUPPORTED;
}

static int heap_cmd(int argc, char **argv) {
    (void)argc, (void)argv;
    printf("HEAP internal=%u internal_min=%u psram=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static void start_console(void) {
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = APP_SLUG ">";
    cfg.task_stack_size = 8192;
    esp_console_dev_usb_serial_jtag_config_t usb = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb, &cfg, &repl));
    esp_console_register_help_command();
    ESP_ERROR_CHECK(yk_link_register_console());
    ESP_ERROR_CHECK(yk_app_register_console());
    yk_ui_register_snapshot();
    const esp_console_cmd_t heap = {.command = "heap", .help = "available memory", .func = heap_cmd};
    ESP_ERROR_CHECK(esp_console_cmd_register(&heap));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

// Setting the console up waits up to half a second for a terminal to answer (ESP-IDF's linenoiseProbe(); the kit's
// tools do not answer, nor does anything when no computer is on the USB), and the main task, at priority 1, waited
// longer than that once the scene was moving: 0.8 s of a 1.8 s open. So it is set up in a task of its own while the
// screen is made, and yk_app_ready() still waits for it.
static SemaphoreHandle_t s_console_up;

static void console_task(void *arg) {
    (void)arg;
    start_console();
    xSemaphoreGive(s_console_up);
    vTaskDelete(NULL);
}

static void make_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_hex(YK_NIGHT), 0);

    s_root = lv_obj_create(screen);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 466, 466);
    lv_obj_set_clickable(s_root, false);
    lv_obj_set_scrollable(s_root, false);

    lv_obj_t *touch = lv_obj_create(screen);
    lv_obj_remove_style_all(touch);
    lv_obj_set_size(touch, 466, 466);
    lv_obj_set_clickable(touch, true);
    lv_obj_set_scrollable(touch, false);
    lv_obj_set_gesture_bubble(touch, false);
    lv_obj_add_event_cb(touch, on_touch, LV_EVENT_ALL, NULL);

    lv_display_add_event_cb(lv_display_get_default(), on_display, LV_EVENT_ALL, NULL);
    build(s_orn.shape);
    lv_screen_load(screen);
    s_last_ms = now_ms();
    s_perf_at = s_last_ms;
    lv_refr_now(NULL);

    // The first time in this app: an arrow at the side key, 「双击：应用和回家」, until the app ring was opened once.
    if (yk_hint_should(YK_HINT_APPS)) yk_hint_show(screen, YK_HINT_APPS, NULL);
    // What is ours on the screen (see system_screen_up).
    s_screen = screen;
    s_owns = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen) && s_owns < OWN_MAX; i++) s_own[s_owns++] = lv_obj_get_child(screen, (int32_t)i);
}

void app_main(void) {
    // 1. NVS: never erase it (it is shared with every app and holds the Wi-Fi credentials).
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS unavailable (%s); running without saved settings", esp_err_to_name(err));
    // 2. yaykit_app first, so that a crash anywhere below counts towards the crash loop guard.
    ESP_ERROR_CHECK(yk_app_start(&(yk_app_config_t){.slug = APP_SLUG, .version = APP_VERSION}));
    yk_app_on_leave(on_leave, NULL);
    const int shape = load_shape();
    atomic_store(&s_shape_to_save, shape);
    atomic_store(&s_saved_shape, shape);
    orn_init(&s_orn, shape);
    const esp_timer_create_args_t save = {.callback = save_shape, .name = "orn_save"};
    ESP_ERROR_CHECK(esp_timer_create(&save, &s_save_timer));
    // 3. Board and screen (dark until the first frame), the clock and its zone (for the night), then the side key.
    if (!board_display_start_opts(&(board_display_opts_t){.start_dark = true})) {
        ESP_LOGE(TAG, "display failed");
        return;
    }
    board_rtc_init();
    board_pref_tz_apply("CST-8");
    // delay_tap: the first press of a double click (the app ring) is not a tap; a tap comes 0.45 s after its release.
    ESP_ERROR_CHECK(yk_key_init(&(yk_key_config_t){.delay_tap = true}));
    ESP_ERROR_CHECK(yk_app_on_key(on_key, NULL));
    // 4. Yaykit Link: `yk info` and `yk cmd` over USB (BLE stays off until asked for).
    const yk_link_config_t link = {.board = "tikpal-r1", .app_slug = APP_SLUG, .app_version = APP_VERSION,
                                   .layout = "yaykit-v1", .idle_seconds = 90, .on_command = on_command};
    ESP_ERROR_CHECK(yk_link_init(&link));
    // The console sets itself up meanwhile (see console_task); the screen is not waiting for it.
    s_console_up = xSemaphoreCreateBinary();
    if (!s_console_up || xTaskCreate(console_task, "orn_console_up", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "console task");
        if (s_console_up) vSemaphoreDelete(s_console_up);
        s_console_up = NULL;
        start_console();
    }
    // 5. Launch parameters: none are declared, the ornament just shows. Read so the record does not linger.
    yk_launch_t launch;
    yk_app_launch(&launch);
    // Gravity for the tilt, and two events: a shake and face down. Without the sensor the rest works.
    const esp_err_t motion = yk_motion_start(&(yk_motion_config_t){.odr_hz = 56, .on_event = on_motion});
    if (motion != ESP_OK) ESP_LOGW(TAG, "no motion: %s", esp_err_to_name(motion));

    // 6. The pictures, the screen; then confirm the image (the bootloader would roll back an app that never gets here).
    const int64_t t_scenes = esp_timer_get_time();
    if (!orn_scenes_init()) ESP_LOGE(TAG, "the ornaments will be missing parts");
    board_lock(-1);
    ESP_ERROR_CHECK(cjk_font_init());
    make_screen();
    const int64_t t_shown = esp_timer_get_time();
    lv_timer_create(tick, TICK_MS, NULL);
    board_unlock();
    // 7. The console up, then yk_app_ready(): its banner tells tools the app is up, so the console must already listen.
    if (s_console_up) xSemaphoreTake(s_console_up, portMAX_DELAY);
    yk_app_ready();
#ifdef CONFIG_YAY_DEV
    // How long opening took (ms since the app's own start; the boot before it is not counted here).
    printf("ORN_OPEN scenes_at=%lld shown_at=%lld ready_at=%lld\n", t_scenes / 1000, t_shown / 1000,
           esp_timer_get_time() / 1000);
#endif
}
