#include "support.h"

static void test_json(void)
{
    const char *invalid[] = {
        "{\"x\":1} trailing", "{\"x\":1}{\"y\":2}", "[]", "null",
        "{\"x\":{\"id\":1,\"id\":2}}", "{\"id\":\"alice\\u0000evil\"}",
        "{\"id\\u0000evil\":1}", "{\"x\":\"unterminated}", "{\"x\":\"raw\nline\"}"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(chat_json_parse_object(invalid[i], strlen(invalid[i])) == NULL);
    }
    const char raw_nul[] = "{\"x\":1}\0garbage";
    assert(chat_json_parse_object(raw_nul, sizeof(raw_nul) - 1) == NULL);
    const char *valid = "{\"x\":\"literal \\\\u0000, [ { \\\"\",\"nested\":[{}]} \r\n";
    cJSON *root = chat_json_parse_object(valid, strlen(valid));
    assert(root);
    cJSON_Delete(root);
    const char exact_buffer[] = { '{', '}' };
    root = chat_json_parse_object(exact_buffer, sizeof(exact_buffer));
    assert(root);
    cJSON_Delete(root);

    for (int depth = MAX_JSON_NESTING_DEPTH - 1; depth <= MAX_JSON_NESTING_DEPTH; depth++) {
        char nested[128];
        size_t len = 0;
        memcpy(nested, "{\"x\":", 5);
        len = 5;
        for (int i = 0; i < depth; i++) { nested[len++] = '['; }
        nested[len++] = '0';
        for (int i = 0; i < depth; i++) { nested[len++] = ']'; }
        nested[len++] = '}';
        root = chat_json_parse_object(nested, len);
        assert((root != NULL) == (depth < MAX_JSON_NESTING_DEPTH));
        cJSON_Delete(root);
    }
}

int main(void)
{
    test_json();
    puts("JSON regressions passed: depth, duplicate keys, escaped NUL and complete objects");
}
