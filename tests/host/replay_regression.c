#include "support.h"

static const char *text = "{\"type\":\"text\",\"from\":\"alice\",\"name\":\"Alice\","
                          "\"data\":\"hello\",\"to\":{\"all\":true,\"users\":[]}}";

static void add_during_replay(void)
{
    on_send = NULL;
    assert(dispatch(10, text) == ESP_OK);
}

static void seed(void)
{
    reset();
    join(10);
    for (int i = 0; i < 5; i++) { assert(dispatch(10, text) == ESP_OK); }
    sends = 0;
}

int main(void)
{
    seed();
    on_send = add_during_replay;
    assert(chat_history_send_to_client(&ctx, 10, 0) == ESP_OK);
    assert(ctx.message_id_counter == 6);
    assert(sends == 3 && sent_ids[0] == 3 && sent_ids[1] == 4 && sent_ids[2] == 5);

    seed();
    fail_on_send = 2;
    assert(chat_history_send_to_client(&ctx, 10, 0) == ESP_FAIL);
    assert(sends == 2 && closes == 1 && sent_ids[0] == 3 && sent_ids[1] == 4);

    seed();
    assert(chat_history_send_to_client(&ctx, 10, CHAT_MESSAGE_MAX_SAFE_ID) == ESP_OK);
    assert(sends == 0);
    assert(chat_history_send_to_client(NULL, 10, 0) == ESP_ERR_INVALID_ARG);
    reset();
    puts("Replay regressions passed: bounded snapshots, ordered sends, failures and future cursors");
}
