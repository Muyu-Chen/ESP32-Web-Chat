#include "support.h"

static const char *text = "{\"type\":\"text\",\"from\":\"alice\",\"name\":\"Alice\","
                          "\"data\":\"hello\",\"to\":{\"all\":true,\"users\":[]}}";

int main(void)
{
    reset();
    join(10);
    ctx.message_id_counter = 0;
    ctx.message_id_counter_valid = false;
    load_result = ESP_FAIL;
    load_current_id = 42;
    assert(dispatch(10, text) == ESP_OK);
    assert(errors == 1 && ctx.message_id_counter == 0 && !ctx.message_id_counter_valid);
    load_result = ESP_OK;
    assert(dispatch(10, text) == ESP_OK);
    assert(ctx.message_id_counter == 43 && ctx.message_id_counter_valid);

    reset();
    ctx.message_id_counter_valid = false;
    load_result = ESP_FAIL;
    load_current_id = 42;
    assert(chat_sessions_ensure_slot(&ctx, 10));
    assert(dispatch(10, "{\"type\":\"join\",\"from\":\"alice\",\"name\":\"Alice\"}") != ESP_OK);
    assert(errors == 1 && closes == 1 && sends == 0 && broadcasts == 0);
    assert(!chat_sessions_is_joined(&ctx, 10));
    chat_sessions_remove_by_fd(&ctx, 10);
    load_result = ESP_OK;
    join(10);
    assert(ctx.message_id_counter_valid && ctx.message_id_counter == 42 && sends == 2);
    reset();
    puts("ID state regressions passed: send recovery and join gating before counter availability");
}
