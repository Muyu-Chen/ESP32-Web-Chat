#include "support.h"

int main(void)
{
    const char *text = "{\"type\":\"text\",\"from\":\"alice\",\"name\":\"Alice\","
                       "\"data\":\"hello\",\"to\":{\"all\":true,\"users\":[]}}";
    reset();
    assert(chat_sessions_ensure_slot(&ctx, 10));
    assert(dispatch(10, text) != ESP_OK);
    assert(errors == 1 && closes == 1 && ctx.message_id_counter == 0 && broadcasts == 0);

    const char *unjoined_controls[] = {
        "{\"type\":\"getOnlineUser\",\"from\":\"alice\"}",
        "{\"type\":\"pong\",\"from\":\"alice\"}",
        "{\"type\":\"historyRequest\",\"from\":\"alice\",\"name\":\"Alice\","
        "\"requestId\":\"request-1\",\"restore_before_id\":100}",
        "{\"type\":\"historyResponse\",\"from\":\"alice\",\"name\":\"Alice\"}"
    };
    for (size_t i = 0; i < sizeof(unjoined_controls) / sizeof(unjoined_controls[0]); i++) {
        reset();
        assert(chat_sessions_ensure_slot(&ctx, 10));
        assert(dispatch(10, unjoined_controls[i]) != ESP_OK);
        assert(errors == 1 && closes == 1 && sends == 0 && broadcasts == 0);
    }

    reset();
    join(10);
    int before = broadcasts;
    assert(dispatch(10, "{\"type\":\"text\",\"from\":\"mallory\",\"name\":\"M\","
                        "\"data\":\"spoof\",\"to\":{\"all\":true,\"users\":[]}}") != ESP_OK);
    assert(errors == 1 && ctx.message_id_counter == 0 && broadcasts == before);

    reset();
    error_result = ESP_FAIL;
    assert(dispatch(10, text) == ESP_FAIL);

    reset();
    join(10);
    for (int i = 0; i < 5; i++) { assert(dispatch(10, text) == ESP_OK); }
    sends = 0;
    assert(chat_history_send_to_client(&ctx, 10, 3) == ESP_OK);
    assert(sends == 2 && sent_ids[0] == 4 && sent_ids[1] == 5);
    sends = 0;
    assert(chat_history_send_to_client(&ctx, 10, 100) == ESP_OK && sends == 0);
    assert(chat_history_send_to_client(&ctx, 10, 0) == ESP_OK);
    assert(sends == 3 && sent_ids[0] == 3 && sent_ids[2] == 5);

    const char *invalid_cursors[] = {
        "{\"since_id\":-1}", "{\"since_id\":0.5}", "{\"since_id\":9007199254740992}",
        "{\"since_id\":\"1\"}", "{\"since_id\":null}"
    };
    for (size_t i = 0; i < sizeof(invalid_cursors) / sizeof(invalid_cursors[0]); i++) {
        cJSON *root = cJSON_Parse(invalid_cursors[i]);
        uint64_t since_id = 0;
        assert(!chat_history_parse_since_id(root, &since_id));
        cJSON_Delete(root);
    }

    reset();
    puts("Protocol regressions passed: identity rejection and since_id replay");
}
