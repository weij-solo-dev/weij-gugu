// test_parent.c — the pairing page's speaker volume: saved, in use at once (not after a reboot), 0–100 only.
#include "minitest.h"
#include "parent/parent_internal.h"
#include "store/store.h"
#include <stdlib.h>
#include <string.h>

#define TEST_DATA GUGU_REPO_ROOT "/host/build/test_data"

static int s_heard = -1;
static void on_volume(int percent, void *user) { (void)user; s_heard = percent; }

static int post(const char *path, const char *body)
{
    char *out = NULL; size_t n = 0; const char *ct = NULL, *disp = NULL;
    int status = parent_api_handle("POST", path, "", body, body ? strlen(body) : 0, &out, &n, &ct, &disp);
    free(out);
    return status;
}

TEST(test_parent_volume)
{
    store_init(TEST_DATA, GUGU_REPO_ROOT "/content");
    int keep = store_setting_int("volume", -1);
    parent_hooks_t h = { .volume = on_volume };
    parent_set_hooks(&h);
    CHECK_EQ_INT(post("/api/volume", "{\"volume\":40}"), 200);
    CHECK_EQ_INT(s_heard, 40);                                   // the speaker has it now
    CHECK_EQ_INT(store_setting_int("volume", -1), 40);           // and it is kept for the next boot
    const char *bad[] = { "{\"volume\":101}", "{\"volume\":-1}", "{\"volume\":\"loud\"}", "{}", "not json", NULL };
    for (int i = 0; bad[i]; i++) {
        s_heard = -1;
        CHECK_EQ_INT(post("/api/volume", bad[i]), 400);
        CHECK_EQ_INT(s_heard, -1);
    }
    CHECK_EQ_INT(post("/api/volume", NULL), 400);
    CHECK_EQ_INT(store_setting_int("volume", -1), 40);           // nothing bad was kept
    if (keep >= 0) store_setting_set_int("volume", keep);
    parent_set_hooks(NULL);
}

void test_parent(void)
{
    RUN(test_parent_volume);
}
