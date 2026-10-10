#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>

#define CHANNEL_COUNT 4U
#define DWELL_COUNT 4U

static const uint32_t frequencies[CHANNEL_COUNT] = {
    315000000,
    433920000,
    868350000,
    915000000,
};

static const char* const frequency_labels[CHANNEL_COUNT] = {
    "315.00",
    "433.92",
    "868.35",
    "915.00",
};

static const uint32_t dwell_times[DWELL_COUNT] = {
    200,
    400,
    800,
    1600,
};

typedef struct {
    uint8_t selected;
    uint8_t current;
    uint8_t dwell_index;
    bool hold;
    bool measured[CHANNEL_COUNT];
    int32_t peak_rssi[CHANNEL_COUNT];
    uint32_t pulse_rate[CHANNEL_COUNT];
} MonitorState;

/*
 * Only the asynchronous RX callback writes this field during reception.
 * The main thread reads/resets it only after stop_async_rx() has returned.
 * No mutex operations are performed from the interrupt callback.
 */
typedef struct {
    volatile uint32_t pulses;
} RxCapture;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* input_queue;
    MonitorState state;
    RxCapture capture;
} MonitorApp;

static void monitor_rx_callback(bool level, uint32_t duration, void* context) {
    UNUSED(level);
    RxCapture* capture = context;

    /*
     * Count useful-length high/low intervals, rather than counting every
     * tiny noise transition. This is activity detection, not frame decoding.
     */
    if(duration >= 80U && duration <= 20000U) {
        capture->pulses++;
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
    canvas_draw_str(canvas, 0, 9, "Multi RX");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 88, 9, state.hold ? "HOLD" : "SCAN");
    canvas_draw_line(canvas, 0, 11, 127, 11);

    canvas_draw_str(canvas, 9, 19, "MHz");
    canvas_draw_str(canvas, 51, 19, "dBm");
    canvas_draw_str(canvas, 86, 19, "edges/s");

    char text[24];

    for(uint8_t i = 0; i < CHANNEL_COUNT; i++) {
        const uint8_t y = 28U + i * 9U;

        if(i == state.current) {
            canvas_draw_str(canvas, 0, y, ">");
        }

        if(i == state.selected) {
            canvas_draw_frame(canvas, 7, y - 7, 41, 9);
        }

        canvas_draw_str(canvas, 9, y, frequency_labels[i]);

        if(state.measured[i]) {
            snprintf(text, sizeof(text), "%ld", (long)state.peak_rssi[i]);
            canvas_draw_str(canvas, 51, y, text);

            snprintf(text, sizeof(text), "%lu", (unsigned long)state.pulse_rate[i]);
            canvas_draw_str(canvas, 87, y, text);
        } else {
            canvas_draw_str(canvas, 51, y, "---");
            canvas_draw_str(canvas, 87, y, "---");
        }
    }

    snprintf(
        text,
        sizeof(text),
        "OK %s <>%lums",
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
    /* Called with the asynchronous receiver stopped. */
    app->capture.pulses = 0;

    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency_and_path(frequencies[channel]);
    furi_hal_subghz_flush_rx();
    furi_hal_subghz_start_async_rx(monitor_rx_callback, &app->capture);
}

int32_t multi_band_rx_app(void* p) {
    UNUSED(p);

    MonitorApp* app = malloc(sizeof(MonitorApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(16, sizeof(InputEvent));
    app->capture.pulses = 0;

    MonitorState state = {0};
    state.dwell_index = 1;
    app->state = state;

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, monitor_draw_callback, app);
    view_port_input_callback_set(view_port, monitor_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);

    /*
     * The internal CC1101 is a single receiver: only the current channel
     * is monitored. OOK650 offers RAW-style edge activity, not universal
     * demodulation of every modulation or protocol.
     */
    furi_hal_subghz_reset();
    furi_hal_subghz_idle();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async, NULL);

    monitor_start_window(app, state.current);
    uint32_t window_start = furi_get_tick();
    int32_t peak_rssi = -127;

    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    bool running = true;

    while(running) {
        InputEvent event;
        bool retune = false;
        uint8_t target = state.current;

        const FuriStatus status = furi_message_queue_get(
            app->input_queue, &event, furi_ms_to_ticks(25));

        if(status == FuriStatusOk) {
            if(event.key == InputKeyBack &&
               (event.type == InputTypeShort || event.type == InputTypeLong)) {
                running = false;
            } else if(event.type == InputTypeShort || event.type == InputTypeRepeat) {
                switch(event.key) {
                case InputKeyUp:
                    state.selected =
                        (state.selected + CHANNEL_COUNT - 1U) % CHANNEL_COUNT;
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

        const int32_t rssi = (int32_t)furi_hal_subghz_get_rssi();
        if(rssi > peak_rssi) {
            peak_rssi = rssi;
        }

        const uint32_t now = furi_get_tick();
        const uint32_t elapsed = now - window_start;

        if(retune || elapsed >= furi_ms_to_ticks(dwell_times[state.dwell_index])) {
            furi_hal_subghz_stop_async_rx();

            const uint32_t measured_ticks = furi_get_tick() - window_start;
            state.measured[state.current] = true;
            state.peak_rssi[state.current] = peak_rssi;
            state.pulse_rate[state.current] =
                measured_ticks ?
                    (app->capture.pulses * furi_ms_to_ticks(1000)) / measured_ticks :
                    0;

            if(retune) {
                state.current = target;
            } else if(state.hold) {
                state.current = state.selected;
            } else {
                state.current = (state.current + 1U) % CHANNEL_COUNT;
            }

            monitor_start_window(app, state.current);
            window_start = furi_get_tick();
            peak_rssi = -127;
        }

        monitor_publish(app, view_port, &state);
    }

    furi_hal_subghz_stop_async_rx();
    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();

    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);

    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}