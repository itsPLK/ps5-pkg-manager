#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "smb_client.h"
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/smb2-errors.h>
#include "libsmb2-private.h"

extern int (*mock_smb_connect_hook)(struct smb2_context *);
static int calls, anonymous_only, fail_both;
static uint32_t failure;
static const char *expected_user = "Guest", *expected_password = "";

static int connect_hook(struct smb2_context *ctx) {
    calls++;
    if (calls == 1) {
        assert(strcmp(ctx->user, expected_user) == 0);
        assert(ctx->password && strcmp(ctx->password, expected_password) == 0);
        if (!anonymous_only && !failure) return 0;
    } else {
        assert(calls == 2);
        assert(strcmp(ctx->user, "") == 0);
        assert(ctx->password == NULL);
        assert(strcmp(ctx->domain, "") == 0);
        if (!fail_both) return 0;
    }
    ctx->nterror = failure ? failure : SMB2_STATUS_ACCESS_DENIED;
    return -1;
}

int main(void) {
    mkdir("/tmp/mock_smb", 0700);
    smb_share_config_t cfg = {0};
    strcpy(cfg.server, "test-server"); strcpy(cfg.share, "shared");
    char error[512];
    mock_smb_connect_hook = connect_hook;
    assert(smb_client_test_connection(&cfg, error, sizeof(error)) == 0);
    assert(calls == 1);

    calls = 0; anonymous_only = 1;
    assert(smb_client_test_connection(&cfg, error, sizeof(error)) == 0);
    assert(calls == 2); /* Preserve anonymous-only shares. */

    calls = 0; failure = SMB2_STATUS_ACCOUNT_DISABLED; fail_both = 1;
    assert(smb_client_test_connection(&cfg, error, sizeof(error)) < 0);
    assert(calls == 2 && strstr(error, "disabled"));

    calls = 0; failure = SMB2_STATUS_BAD_NETWORK_NAME;
    assert(smb_client_test_connection(&cfg, error, sizeof(error)) < 0);
    assert(calls == 1); /* A missing share is not an authentication failure. */

    calls = 0; failure = SMB2_STATUS_LOGON_FAILURE;
    strcpy(cfg.username, "user"); strcpy(cfg.password, "wrong");
    expected_user = "user"; expected_password = "wrong";
    assert(smb_client_test_connection(&cfg, error, sizeof(error)) < 0);
    assert(calls == 1); /* No guest downgrade for explicit credentials. */

    calls = 0; cfg.password[0] = 0; expected_password = "";
    failure = 0; anonymous_only = 0;
    assert(smb_client_test_connection(&cfg, error, sizeof(error)) == 0);
    assert(calls == 1); /* A named user can have an empty password too. */
    puts("SMB authentication regressions passed");
    return 0;
}
