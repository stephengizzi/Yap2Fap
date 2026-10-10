#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <notification/notification_messages.h>

typedef struct {
    FuriMutex* mutex;
    uint8_t result;
    bool rolled;
} DiceRollerState;

static const NotificationSequence dice_roller_vibration = {
    &message_vibro_on,
    &message_delay_50,
    &message_vibro_off,
    NULL,
};

static void dice_roller_draw(Canvas* canvas, void* context) {
    DiceRollerState* state = context;

    furi_mutex_acquire(state->mutex, FuriWaitForever);
    const uint8_t result = state->result;
    const bool rolled = state->rolled;
    furi_mutex_release(state->mutex);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "Dice Roller");
    canvas_draw_line(canvas, 4, 14, 123, 14);

    if(rolled) {
        const char number[] = {(char)('0' + result), '\0'};
        canvas_set_font(canvas, FontBigNumbers);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, number);
    } else {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "Press OK to roll");
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas, 64, 62, AlignCenter, AlignBottom, "OK: Roll    Back: Exit");
}

static void dice_roller_input(InputEvent* event, void* context) {
    FuriMessageQueue* queue = context;
    furi_message_queue_put(queue, event, FuriWaitForever);
}

static uint8_t dice_roller_roll(void) {
    /* Reject the four excess values so all six outcomes are equally likely. */
    uint32_t random;
    do {
        random = furi_hal_random_get();
    } while(random >= 0xFFFFFFFCu);

    return (uint8_t)(random % 6u + 1u);
}

int32_t dice_roller_app(void* p) {
    UNUSED(p);

    DiceRollerState state = {
        .mutex = furi_mutex_alloc(FuriMutexTypeNormal),
        .result = 1,
        .rolled = false,
    };
    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* view_port = view_port_alloc();
    Gui* gui = furi_record_open(RECORD_GUI);
    NotificationApp* notification = furi_record_open(RECORD_NOTIFICATION);

    view_port_draw_callback_set(view_port, dice_roller_draw, &state);
    view_port_input_callback_set(view_port, dice_roller_input, queue);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    bool running = true;
    while(running) {
        InputEvent event;
        if(furi_message_queue_get(queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        if(event.key == InputKeyBack &&
           (event.type == InputTypeShort || event.type == InputTypeLong)) {
            running = false;
        } else if(event.key == InputKeyOk && event.type == InputTypeShort) {
            const uint8_t result = dice_roller_roll();

            furi_mutex_acquire(state.mutex, FuriWaitForever);
            state.result = result;
            state.rolled = true;
            furi_mutex_release(state.mutex);

            view_port_update(view_port);
            notification_message(notification, &dice_roller_vibration);
        }
    }

    notification_message_block(notification, &sequence_reset_vibro);
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(queue);
    furi_mutex_free(state.mutex);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    return 0;
}