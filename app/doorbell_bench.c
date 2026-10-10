#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>

#define BELL_FREQUENCY 433920000UL
#define BELL_CODE_COUNT 531441UL
#define BELL_BATCH_SIZE 16UL
#define BELL_PULSE_COUNT 50U
#define BELL_REPEATS 3U
#define BELL_GAP_MS 2500U

typedef enum {
    BellStatusReady,
    BellStatusRunning,
    BellStatusStopped,
    BellStatusDone,
    BellStatusBlocked,
    BellStatusTxError,
} BellStatus;

typedef struct {
    LevelDuration pulses[BELL_PULSE_COUNT];
    uint32_t cursor;
} BellTx;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;
    Gui* gui;

    uint32_t base;
    uint32_t current;
    uint32_t tested;
    uint32_t target;
    uint8_t timing;
    bool running;
    BellStatus status;

    /*
     * Only the transmitter callback accesses cursor while asynchronous TX
     * is active. The main thread rebuilds this buffer only after stopping TX.
     */
    BellTx tx;
} BellApp;

static const uint16_t bell_timings[] = {250, 350, 450, 550};

static uint32_t bell_batch_length(uint32_t base) {
    uint32_t remaining = BELL_CODE_COUNT - base;
    return remaining < BELL_BATCH_SIZE ? remaining : BELL_BATCH_SIZE;
}

static void bell_code_text(uint32_t code, char text[13]) {
    static const char symbols[] = {'0', '1', 'F'};

    for(uint32_t i = 0; i < 12; i++) {
        text[11 - i] = symbols[code % 3];
        code /= 3;
    }
    text[12] = '\0';
}

static void bell_build_tx(BellTx* tx, uint32_t code, uint16_t pulse) {
    uint8_t trits[12];

    for(uint32_t i = 0; i < 12; i++) {
        trits[11 - i] = code % 3;
        code /= 3;
    }

    uint32_t position = 0;
    for(uint32_t i = 0; i < 12; i++) {
        /*
         * PT2262-style symbols:
         * 0: short/long, short/long
         * 1: long/short, long/short
         * F: short/long, long/short
         */
        bool first_long = trits[i] == 1;
        bool second_long = trits[i] != 0;

        tx->pulses[position++] =
            level_duration_make(true, first_long ? pulse * 3U : pulse);
        tx->pulses[position++] =
            level_duration_make(false, first_long ? pulse : pulse * 3U);
        tx->pulses[position++] =
            level_duration_make(true, second_long ? pulse * 3U : pulse);
        tx->pulses[position++] =
            level_duration_make(false, second_long ? pulse : pulse * 3U);
    }

    tx->pulses[position++] = level_duration_make(true, pulse);
    tx->pulses[position] = level_duration_make(false, pulse * 31U);
    tx->cursor = 0;
}

static LevelDuration bell_tx_callback(void* context) {
    BellTx* tx = context;

    if(tx->cursor >= BELL_PULSE_COUNT * BELL_REPEATS) {
        return level_duration_reset();
    }

    LevelDuration result = tx->pulses[tx->cursor % BELL_PULSE_COUNT];
    tx->cursor++;
    return result;
}

static void bell_draw(Canvas* canvas, void* context) {
    BellApp* app = context;
    FuriString* text = furi_string_alloc();
    char code[13];

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Doorbell Bench");

    canvas_set_font(canvas, FontSecondary);
    furi_string_printf(
        text, "433.92 PT2262  %u us", (unsigned int)bell_timings[app->timing]);
    canvas_draw_str(canvas, 2, 21, furi_string_get_cstr(text));

    bell_code_text(app->current, code);
    furi_string_printf(text, "Code: %s", code);
    canvas_draw_str(canvas, 2, 32, furi_string_get_cstr(text));

    switch(app->status) {
    case BellStatusReady:
        furi_string_set(text, "Own bell only: OK starts");
        break;
    case BellStatusRunning:
        furi_string_printf(
            text,
            "Testing: %lu/%lu sent",
            (unsigned long)app->tested,
            (unsigned long)app->target);
        break;
    case BellStatusStopped:
        furi_string_printf(
            text, "Stopped: %lu sent", (unsigned long)app->tested);
        break;
    case BellStatusDone:
        furi_string_set(text, "Batch done; no RX detection");
        break;
    case BellStatusBlocked:
        furi_string_set(text, "TX blocked by region");
        break;
    case BellStatusTxError:
        furi_string_set(text, "Could not start radio TX");
        break;
    }
    canvas_draw_str(canvas, 2, 43, furi_string_get_cstr(text));

    if(app->running) {
        canvas_draw_str(canvas, 2, 54, "Pause between codes: 2.5s");
    } else {
        canvas_draw_str(canvas, 2, 54, "L/R: block  U/D: timing");
    }

    canvas_draw_str(
        canvas, 2, 63, app->running ? "OK: stop   Back: exit" : "OK: test   Back: exit");

    furi_mutex_release(app->mutex);
    furi_string_free(text);
}

static void bell_input(InputEvent* event, void* context) {
    BellApp* app = context;
    furi_message_queue_put(app->queue, event, 0);
}

static void bell_radio_stop(bool* radio_active, bool* transmitting) {
    if(*transmitting) {
        furi_hal_subghz_stop_async_tx();
        *transmitting = false;
    }

    if(*radio_active) {
        furi_hal_subghz_sleep();
        *radio_active = false;
    }
}

int32_t doorbell_bench_app(void* p) {
    UNUSED(p);

    BellApp* app = malloc(sizeof(BellApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(16, sizeof(InputEvent));
    app->view_port = view_port_alloc();
    app->gui = furi_record_open(RECORD_GUI);
    app->base = 0;
    app->current = 0;
    app->tested = 0;
    app->target = BELL_BATCH_SIZE;
    app->timing = 1;
    app->running = false;
    app->status = BellStatusReady;
    app->tx.cursor = 0;

    view_port_draw_callback_set(app->view_port, bell_draw, app);
    view_port_input_callback_set(app->view_port, bell_input, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    bool exit_requested = false;
    bool radio_active = false;
    bool transmitting = false;
    uint32_t next_send = 0;

    while(!exit_requested) {
        InputEvent event;
        bool start_requested = false;
        bool stop_requested = false;
        bool redraw = false;

        if(furi_message_queue_get(app->queue, &event, furi_ms_to_ticks(20)) ==
           FuriStatusOk) {
            if(event.key == InputKeyBack &&
               (event.type == InputTypeShort || event.type == InputTypeLong)) {
                exit_requested = true;
                continue;
            }

            if(event.type == InputTypeShort) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);

                if(event.key == InputKeyOk) {
                    if(app->running) {
                        stop_requested = true;
                    } else {
                        start_requested = true;
                    }
                } else if(!app->running) {
                    bool changed = true;

                    switch(event.key) {
                    case InputKeyLeft:
                        app->base = app->base >= BELL_BATCH_SIZE ?
                                        app->base - BELL_BATCH_SIZE :
                                        ((BELL_CODE_COUNT - 1) / BELL_BATCH_SIZE) *
                                            BELL_BATCH_SIZE;
                        break;
                    case InputKeyRight:
                        app->base += BELL_BATCH_SIZE;
                        if(app->base >= BELL_CODE_COUNT) app->base = 0;
                        break;
                    case InputKeyUp:
                        app->timing = (app->timing + 1) % 4;
                        break;
                    case InputKeyDown:
                        app->timing = (app->timing + 3) % 4;
                        break;
                    default:
                        changed = false;
                        break;
                    }

                    if(changed) {
                        app->current = app->base;
                        app->tested = 0;
                        app->target = bell_batch_length(app->base);
                        app->status = BellStatusReady;
                        redraw = true;
                    }
                }

                furi_mutex_release(app->mutex);
            }
        }

        if(stop_requested) {
            bell_radio_stop(&radio_active, &transmitting);

            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->running = false;
            app->status = BellStatusStopped;
            furi_mutex_release(app->mutex);
            redraw = true;
        }

        if(start_requested) {
            bool allowed = furi_hal_subghz_is_tx_allowed(BELL_FREQUENCY);

            if(allowed) {
                furi_hal_subghz_reset();
                furi_hal_subghz_idle();
                furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async);
                furi_hal_subghz_set_frequency_and_path(BELL_FREQUENCY);
                radio_active = true;
                next_send = furi_get_tick();
            }

            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->tested = 0;
            app->current = app->base;
            app->target = bell_batch_length(app->base);
            app->running = allowed;
            app->status = allowed ? BellStatusRunning : BellStatusBlocked;
            furi_mutex_release(app->mutex);
            redraw = true;
        }

        if(radio_active && transmitting && furi_hal_subghz_is_async_tx_complete()) {
            furi_hal_subghz_stop_async_tx();
            transmitting = false;

            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->tested++;
            bool finished = app->tested >= app->target;
            if(finished) {
                app->running = false;
                app->status = BellStatusDone;
            } else {
                app->current = app->base + app->tested;
            }
            furi_mutex_release(app->mutex);

            if(finished) {
                bell_radio_stop(&radio_active, &transmitting);
            } else {
                next_send = furi_get_tick() + furi_ms_to_ticks(BELL_GAP_MS);
            }
            redraw = true;
        }

        if(radio_active && !transmitting &&
           (int32_t)(furi_get_tick() - next_send) >= 0) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            uint32_t code = app->current;
            uint16_t pulse = bell_timings[app->timing];
            furi_mutex_release(app->mutex);

            bell_build_tx(&app->tx, code, pulse);
            transmitting = furi_hal_subghz_start_async_tx(bell_tx_callback, &app->tx);

            if(!transmitting) {
                bell_radio_stop(&radio_active, &transmitting);

                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->running = false;
                app->status = BellStatusTxError;
                furi_mutex_release(app->mutex);
                redraw = true;
            }
        }

        if(redraw) view_port_update(app->view_port);
    }

    bell_radio_stop(&radio_active, &transmitting);

    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}