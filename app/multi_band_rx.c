#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#define CHANNEL_COUNT 4U
#define DWELL_COUNT 4U
#define CAPTURE_CAPACITY 8192U
#define DETECT_RSSI_DBM (-70)
#define DETECT_MIN_EDGES 24U
#define SAVE_DIRECTORY EXT_PATH("subghz")

/*
 * Candidate frequencies based on general knowledge, not a live web lookup.
 *
 * 433.42 MHz: used by some blind/shutter systems, including Somfy RTS.
 * 433.92 MHz: common among bells, remotes and sensors; some use
 *            PT2262/EV1527-style OOK encodings.
 * 868.30/868.35 MHz: examples of European SRD channels, also used by
 *                  devices with modulation this OOK-only app cannot decode.
 *
 * These are NOT device/protocol identifications or a complete UK channel list.
 * No protocol decoding, rolling-code processing, or transmission is provided.
 * Check current Ofcom requirements before transmitting with another app.
 *
 * The internal radio receives ONE frequency at a time. Hopping misses traffic
 * on other frequencies. Saved files contain bounded, unfiltered OOK samples:
 * they may include noise, partial frames or repeated frames. Saving a frequency
 * alone would not provide enough information to emulate a remote.
 */

static const uint32_t frequencies[CHANNEL_COUNT] = {
    433420000,
    433920000,
    868300000,
    868350000,
};

static const char* const frequency_labels[CHANNEL_COUNT] = {
    "433.42",
    "433.92",
    "868.30",
    "868.35",
};

static const char* const frequency_hints[CHANNEL_COUNT] = {
    "Candidate: blinds/remotes",
    "Candidate: bells/remotes",
    "Candidate: SRD devices",
    "Candidate: SRD devices",
};

static const uint32_t dwell_times[DWELL_COUNT] = {
    200,
    400,
    800,
    1600,
};

/*
 * Asynchronous OOK, approximately 650 kHz receive bandwidth.
 * Register/value pairs, terminator, then eight PA-table bytes.
 * This exact preset is embedded in saved RAW files as a custom preset.
 * The app never transmits.
 */
static const uint8_t monitor_ook_preset[] = {
    0x02, 0x0D,
    0x03, 0x47,
    0x08, 0x32,
    0x0B, 0x06,
    0x10, 0x17,
    0x11, 0x32,
    0x12, 0x30,
    0x13, 0x00,
    0x14, 0x00,
    0x15, 0x00,
    0x18, 0x18,
    0x19, 0x18,
    0x1B, 0x07,
    0x1C, 0x00,
    0x1D, 0x91,
    0x20, 0xFB,
    0x21, 0xB6,
    0x22, 0x11,
    0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const NotificationSequence activity_beep = {
    &message_note_c5,
    &message_delay_100,
    &message_sound_off,
    NULL,
};

typedef enum {
    SaveStatusReady,
    SaveStatusSaved,
    SaveStatusError,
} SaveStatus;

typedef struct {
    uint8_t selected;
    uint8_t current;
    uint8_t dwell_index;
    bool hold;
    bool found;
    bool measured[CHANNEL_COUNT];
    int32_t peak_rssi[CHANNEL_COUNT];
    uint32_t pulse_rate[CHANNEL_COUNT];
    uint32_t sample_count;
    bool clipped;
    SaveStatus save_status;
    char saved_name[24];
} MonitorState;

/*
 * Only the ISR callback touches this buffer while RX is running.
 * Main reads/resets it only after stop_async_rx() has returned.
 * Draw never accesses it. No mutex, storage or notification calls in ISR.
 */
typedef struct {
    volatile uint32_t count;
    volatile uint32_t edges;
    volatile bool clipped;
    int32_t samples[CAPTURE_CAPACITY];
} RxCapture;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* input_queue;
    MonitorState state;
    RxCapture capture;
} MonitorApp;

static void monitor_rx_callback(bool level, uint32_t duration, void* context) {
    RxCapture* capture = context;

    if(duration == 0U || duration > 0x7FFFFFFFU) {
        return;
    }

    if(duration >= 80U && duration <= 20000U) {
        capture->edges++;
    }

    const uint32_t index = capture->count;
    if(index < CAPTURE_CAPACITY) {
        capture->samples[index] = level ? (int32_t)duration : -(int32_t)duration;
        capture->count = index + 1U;
    } else {
        capture->clipped = true;
    }
}

static void monitor_input_callback(InputEvent* event, void* context) {
    MonitorApp* app = context;
    furi_message_queue_put(app->input_queue, event, 0);
}

static void monitor_draw_callback(Canvas* canvas, void* context) {
    MonitorApp* app = context;
    MonitorState state;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    state = app->state;
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);

    char text[40];

    if(state.found) {
        canvas_draw_str(canvas, 0, 10, "OOK activity - not ID");
        canvas_set_font(canvas, FontSecondary);

        snprintf(
            text,
            sizeof(text),
            "%s MHz  %ld dBm",
            frequency_labels[state.current],
            (long)state.peak_rssi[state.current]);
        canvas_draw_str(canvas, 0, 21, text);
        canvas_draw_str(canvas, 0, 31, frequency_hints[state.current]);

        snprintf(
            text,
            sizeof(text),
            "%lu samples%s",
            (unsigned long)state.sample_count,
            state.clipped ? " (partial)" : "");
        canvas_draw_str(canvas, 0, 41, text);

        if(state.save_status == SaveStatusSaved) {
            canvas_draw_str(canvas, 0, 51, state.saved_name);
            canvas_draw_str(canvas, 0, 63, "Saved  >resume  Back exit");
        } else {
            canvas_draw_str(
                canvas,
                0,
                51,
                state.save_status == SaveStatusError ?
                    "Save failed: check SD card" :
                    "RAW only; replay uncertain");
            canvas_draw_str(canvas, 0, 63, "OK save RAW   >resume");
        }
        return;
    }

    canvas_draw_str(canvas, 0, 9, "UK OOK RX");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 96, 9, state.hold ? "HOLD" : "SCAN");
    canvas_draw_line(canvas, 0, 11, 127, 11);

    canvas_draw_str(canvas, 9, 19, "MHz");
    canvas_draw_str(canvas, 53, 19, "dBm");
    canvas_draw_str(canvas, 87, 19, "edges/s");

    for(uint8_t i = 0; i < CHANNEL_COUNT; i++) {
        const uint8_t y = 28U + i * 9U;

        if(i == state.current) {
            canvas_draw_str(canvas, 0, y, ">");
        }
        if(i == state.selected) {
            canvas_draw_frame(canvas, 7, y - 7, 43, 9);
        }

        canvas_draw_str(canvas, 9, y, frequency_labels[i]);

        if(state.measured[i]) {
            snprintf(text, sizeof(text), "%ld", (long)state.peak_rssi[i]);
            canvas_draw_str(canvas, 53, y, text);

            snprintf(text, sizeof(text), "%lu", (unsigned long)state.pulse_rate[i]);
            canvas_draw_str(canvas, 87, y, text);
        } else {
            canvas_draw_str(canvas, 53, y, "---");
            canvas_draw_str(canvas, 87, y, "---");
        }
    }

    snprintf(
        text,
        sizeof(text),
        "OK %s  <>%lums",
        state.hold ? "scan" : "hold",
        (unsigned long)dwell_times[state.dwell_index]);
    canvas_draw_str(canvas, 0, 64, text);
}

static void monitor_publish(
    MonitorApp* app,
    ViewPort* view_port,
    const MonitorState* state) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->state = *state;
    furi_mutex_release(app->mutex);
    view_port_update(view_port);
}

static void monitor_start_window(MonitorApp* app, uint8_t channel) {
    app->capture.count = 0;
    app->capture.edges = 0;
    app->capture.clipped = false;

    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency_and_path(frequencies[channel]);
    furi_hal_subghz_flush_rx();
    furi_hal_subghz_start_async_rx(monitor_rx_callback, &app->capture);
}

static bool monitor_write_string(File* file, const FuriString* text) {
    const size_t length = furi_string_size(text);
    return storage_file_write(file, furi_string_get_cstr(text), length) == length;
}

/* Called only while RX is stopped and the capture buffer is stable. */
static bool monitor_save_capture(
    Storage* storage,
    const RxCapture* capture,
    uint32_t frequency,
    char* saved_name,
    size_t saved_name_size) {
    if(capture->count == 0U) {
        return false;
    }

    const FS_Error mkdir_result = storage_common_mkdir(storage, SAVE_DIRECTORY);
    if(mkdir_result != FSE_OK && mkdir_result != FSE_EXIST) {
        return false;
    }

    FuriString* path = furi_string_alloc();
    FuriString* line = furi_string_alloc();
    File* file = storage_file_alloc(storage);
    bool available = false;
    bool opened = false;
    bool success = false;
    char basename[24] = {0};

    do {
        for(uint32_t number = 1; number <= 9999U; number++) {
            snprintf(basename, sizeof(basename), "UK_RX_%04lu.sub", (unsigned long)number);
            furi_string_printf(path, "%s/%s", SAVE_DIRECTORY, basename);

            const FS_Error result =
                storage_common_stat(storage, furi_string_get_cstr(path), NULL);
            if(result == FSE_NOT_EXIST) {
                available = true;
                break;
            }
            if(result != FSE_OK) {
                break;
            }
        }

        if(!available) {
            break;
        }

        opened = storage_file_open(
            file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_NEW);
        if(!opened) {
            break;
        }

        furi_string_printf(
            line,
            "Filetype: Flipper SubGhz RAW File\n"
            "Version: 1\n"
            "Frequency: %lu\n"
            "Preset: FuriHalSubGhzPresetCustom\n"
            "Custom_preset_module: CC1101\n"
            "Custom_preset_data:",
            (unsigned long)frequency);

        for(size_t i = 0; i < sizeof(monitor_ook_preset); i++) {
            furi_string_cat_printf(line, " %02X", (unsigned int)monitor_ook_preset[i]);
        }
        furi_string_cat_str(line, "\nProtocol: RAW\n");

        if(!monitor_write_string(file, line)) {
            break;
        }

        bool write_ok = true;
        const uint32_t count = capture->count;

        for(uint32_t offset = 0; offset < count && write_ok; offset += 32U) {
            furi_string_set_str(line, "RAW_Data:");

            uint32_t end = offset + 32U;
            if(end > count) {
                end = count;
            }
            for(uint32_t i = offset; i < end; i++) {
                furi_string_cat_printf(line, " %ld", (long)capture->samples[i]);
            }

            furi_string_cat_str(line, "\n");
            write_ok = monitor_write_string(file, line);
        }

        if(write_ok) {
            success = storage_file_sync(file);
        }
    } while(false);

    if(opened) {
        if(!storage_file_close(file)) {
            success = false;
        }
        if(!success) {
            storage_common_remove(storage, furi_string_get_cstr(path));
        }
    }

    if(success) {
        snprintf(saved_name, saved_name_size, "%s", basename);
    }

    storage_file_free(file);
    furi_string_free(line);
    furi_string_free(path);
    return success;
}

int32_t multi_band_rx_app(void* p) {
    UNUSED(p);

    MonitorApp* app = malloc(sizeof(MonitorApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(16, sizeof(InputEvent));

    MonitorState state = {0};
    state.dwell_index = 2;
    state.current = 1;
    state.selected = 1;
    app->state = state;

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, monitor_draw_callback, app);
    view_port_input_callback_set(view_port, monitor_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);

    furi_hal_subghz_reset();
    furi_hal_subghz_idle();
    furi_hal_subghz_load_custom_preset(monitor_ook_preset);

    monitor_start_window(app, state.current);
    bool rx_running = true;
    uint32_t window_start = furi_get_tick();
    int32_t peak_rssi = -127;

    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    bool running = true;
    while(running) {
        InputEvent event;
        bool retune = false;
        uint8_t target = state.current;

        const FuriStatus status =
            furi_message_queue_get(app->input_queue, &event, furi_ms_to_ticks(25));

        if(status == FuriStatusOk) {
            if(event.key == InputKeyBack &&
               (event.type == InputTypeShort || event.type == InputTypeLong)) {
                running = false;
            } else if(state.found) {
                if(event.type == InputTypeShort && event.key == InputKeyOk &&
                   state.save_status != SaveStatusSaved) {
                    const bool saved = monitor_save_capture(
                        storage,
                        &app->capture,
                        frequencies[state.current],
                        state.saved_name,
                        sizeof(state.saved_name));
                    state.save_status = saved ? SaveStatusSaved : SaveStatusError;
                } else if(event.type == InputTypeShort && event.key == InputKeyRight) {
                    state.found = false;
                    state.save_status = SaveStatusReady;
                    state.saved_name[0] = '\0';
                    state.current = state.hold ?
                                        state.selected :
                                        (state.current + 1U) % CHANNEL_COUNT;

                    monitor_start_window(app, state.current);
                    rx_running = true;
                    window_start = furi_get_tick();
                    peak_rssi = -127;
                }
            } else if(event.type == InputTypeShort || event.type == InputTypeRepeat) {
                switch(event.key) {
                case InputKeyUp:
                    state.selected = (state.selected + CHANNEL_COUNT - 1U) % CHANNEL_COUNT;
                    if(state.hold) {
                        target = state.selected;
                        retune = true;
                    }
                    break;
                case InputKeyDown:
                    state.selected = (state.selected + 1U) % CHANNEL_COUNT;
                    if(state.hold) {
                        target = state.selected;
                        retune = true;
                    }
                    break;
                case InputKeyLeft:
                    if(state.dwell_index > 0) {
                        state.dwell_index--;
                    }
                    break;
                case InputKeyRight:
                    if(state.dwell_index + 1U < DWELL_COUNT) {
                        state.dwell_index++;
                    }
                    break;
                case InputKeyOk:
                    if(event.type == InputTypeShort) {
                        state.hold = !state.hold;
                        target = state.hold ?
                                     state.selected :
                                     (state.current + 1U) % CHANNEL_COUNT;
                        retune = true;
                    }
                    break;
                default:
                    break;
                }
            }
        }

        if(!running) {
            break;
        }

        if(state.found) {
            monitor_publish(app, view_port, &state);
            continue;
        }

        const uint32_t now = furi_get_tick();
        const uint32_t elapsed = now - window_start;

        /* Ignore the first few milliseconds of tuning/receiver settling. */
        if(elapsed >= furi_ms_to_ticks(10)) {
            const int32_t rssi = (int32_t)furi_hal_subghz_get_rssi();
            if(rssi > peak_rssi) {
                peak_rssi = rssi;
            }
        }

        if(retune || elapsed >= furi_ms_to_ticks(dwell_times[state.dwell_index])) {
            furi_hal_subghz_stop_async_rx();
            rx_running = false;

            const uint32_t measured_ticks = furi_get_tick() - window_start;
            state.measured[state.current] = true;
            state.peak_rssi[state.current] = peak_rssi;
            state.pulse_rate[state.current] =
                measured_ticks ?
                    (uint32_t)(((uint64_t)app->capture.edges * furi_ms_to_ticks(1000)) /
                               measured_ticks) :
                    0;

            /*
             * Activity heuristic only: RSSI plus useful-length intervals.
             * This can false-trigger on interference and does not identify a
             * device or protocol. Manual retuning takes priority over detection.
             */
            const bool detected =
                !retune && peak_rssi >= DETECT_RSSI_DBM &&
                app->capture.edges >= DETECT_MIN_EDGES &&
                app->capture.count >= DETECT_MIN_EDGES;

            if(detected) {
                state.found = true;
                state.sample_count = app->capture.count;
                state.clipped = app->capture.clipped;
                state.save_status = SaveStatusReady;
                state.saved_name[0] = '\0';

                /* Freeze this capture until the user saves or resumes. */
                furi_hal_subghz_idle();
                notification_message(notifications, &activity_beep);
            } else {
                if(retune) {
                    state.current = target;
                } else if(state.hold) {
                    state.current = state.selected;
                } else {
                    state.current = (state.current + 1U) % CHANNEL_COUNT;
                }

                monitor_start_window(app, state.current);
                rx_running = true;
                window_start = furi_get_tick();
                peak_rssi = -127;
            }
        }

        monitor_publish(app, view_port, &state);
    }

    if(rx_running) {
        furi_hal_subghz_stop_async_rx();
    }
    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();

    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);

    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}