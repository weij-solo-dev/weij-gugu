// test_store.c — the diagnostic export: every section is there and no secret leaves the device, even when a
// careless line logged one.
#include "minitest.h"
#include "store/store.h"
#include "foundation/logcap.h"
#include "foundation/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_DATA GUGU_REPO_ROOT "/host/build/test_data"

TEST(test_store_export_masks_secrets)
{
    store_init(TEST_DATA, GUGU_REPO_ROOT "/content");
    char old[256] = ""; bool had = store_secret_get("wifi_pass", old, sizeof old) == ESP_OK;
    store_secret_set("wifi_pass", "hunter2-secret-pass");
    store_event("a careless line with hunter2-secret-pass in it");
    const char *leak = "console: key sk-cp-ABCDEFGHIJ0123456789 leaked\n";
    logcap_write(leak, strlen(leak));                            // the ring was set up by test_foundation
    size_t n = 0; char *s = store_export_logs(&n);
    CHECK(s != NULL);
    if (s) {
        CHECK(strstr(s, "hunter2-secret-pass") == NULL);
        CHECK(strstr(s, "sk-cp-ABCDEFGHIJ") == NULL);
        CHECK(strstr(s, "a careless line with *** in it") != NULL);
        CHECK(strstr(s, "key sk-*** leaked") != NULL);
        CHECK(strstr(s, "==== event log") != NULL && strstr(s, "==== console log") != NULL && strstr(s, "==== secrets") != NULL);
        CHECK(strstr(s, "wifi_pass: set (***, 19 chars)") != NULL);
        CHECK_EQ_INT(strlen(s), n);
        printf("  export: %u bytes\n", (unsigned)n);
        mem_release(s);
    }
    char t[] = "a sk-cp-abcdefghijklmnop b sk-x c";
    size_t m = store_mask_secrets(t, strlen(t));
    CHECK_EQ_STR(t, "a sk-*** b sk-x c"); CHECK_EQ_INT(m, strlen(t));
    store_secret_set("wifi_pass", had ? old : "");
}

void test_store(void)
{
    RUN(test_store_export_masks_secrets);
}
