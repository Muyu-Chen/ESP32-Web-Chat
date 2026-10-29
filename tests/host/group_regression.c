#include "support.h"

int main(void)
{
    const char *invalid[] = {
        "{\"groupId\":{},\"groupName\":\"Group\"}",
        "{\"groupId\":\"group-a\"}",
        "{\"groupName\":\"Group\"}",
        "{\"groupId\":\"\",\"groupName\":\"Group\"}"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        reset();
        join(10);
        cJSON *root = cJSON_Parse(invalid[i]);
        cJSON_AddStringToObject(root, "type", "text");
        cJSON_AddStringToObject(root, "from", "alice");
        cJSON_AddStringToObject(root, "name", "Alice");
        cJSON_AddStringToObject(root, "data", "hello");
        cJSON *to = cJSON_AddObjectToObject(root, "to");
        cJSON_AddBoolToObject(to, "all", true);
        int before = broadcasts;
        assert(chat_protocol_handle_json(&ctx, 10, root) == ESP_OK);
        assert(errors == 1 && broadcasts == before && ctx.message_id_counter == 0);
        cJSON_Delete(root);
    }

    reset();
    join(10);
    assert(dispatch(10, "{\"type\":\"text\",\"from\":\"alice\",\"name\":\"Alice\","
                        "\"data\":\"group hello\",\"to\":{\"all\":false,\"users\":[\"alice\",\"bob\"]},"
                        "\"groupId\":\"group-a\",\"groupName\":\"Group\"}") == ESP_OK);
    assert(errors == 0 && ctx.message_id_counter == 1);
    cJSON *stored = cJSON_Parse(ctx.message_buffer[0].payload);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(stored, "groupId")), "group-a") == 0);
    cJSON_Delete(stored);
    reset();
    puts("Group regressions passed: optional metadata validation and stored group text");
}
