#include "../src/esim.c"

#include <assert.h>

int main(void)
{
    char name[NAME_BYTES], copied[NAME_BYTES];
    memset(name, 'a', NAME_CHARS);
    name[NAME_CHARS] = 0;
    copy_text(copied, sizeof copied, name);
    assert(!strcmp(copied, name));

    for (int i = 0; i < NAME_CHARS; i++)
        memcpy(name + i * 4, "\xf0\x9f\x98\x80", 4);
    name[NAME_CHARS * 4] = 0;
    copy_text(copied, sizeof copied, name);
    assert(!strcmp(copied, name));

    char small[8];
    copy_text(small, sizeof small, name);
    assert(!strcmp(small, "\xf0\x9f\x98\x80"));
    copy_text(small, 4, name);
    assert(!*small);
    cJSON *notifications = cJSON_CreateArray();
    for (int i = 0; i < 64; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "seqNumber", i);
        cJSON_AddItemToArray(notifications, item);
    }
    save_json(&state.slots[1].notifs, notifications);
    assert(cJSON_GetArraySize(state.slots[1].notifs) == 64);
    assert(notification_seen(notifications, 63) && !notification_seen(notifications, 64));
    cJSON_Delete(notifications);
    cJSON_Delete(state.slots[1].notifs);
    state.slots[1].notifs = NULL;
    puts("eSIM: 64-character names and UTF-8 truncation passed");
}
