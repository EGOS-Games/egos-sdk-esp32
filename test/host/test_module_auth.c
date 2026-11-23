/**
 * @file test_module_auth.c
 * @brief Host unit tests for src/egos_module_auth.c (no ESP-IDF needed)
 *
 * Build and run from the repository root with any C99 compiler:
 *
 *   gcc -std=c99 -Wall -Wextra -Isrc -o test_module_auth \
 *       test/host/test_module_auth.c src/egos_module_auth.c && ./test_module_auth
 */

#include "egos_module_auth.h"
#include <stdio.h>
#include <string.h>

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(cond) do {                                            \
        s_checks++;                                                 \
        if (!(cond)) {                                              \
            s_failures++;                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                           \
    } while (0)

/* 43 characters, every class of the base64url alphabet. */
static const char *GOOD = "AZaz09-_AZaz09-_AZaz09-_AZaz09-_AZaz09-_abc";

static void test_secret_is_valid(void)
{
    CHECK(strlen(GOOD) == EGOS_MODULE_SECRET_LEN);
    CHECK(egos_module_secret_is_valid(GOOD));

    CHECK(!egos_module_secret_is_valid(NULL));
    CHECK(!egos_module_secret_is_valid(""));
    /* 42 and 44 characters */
    CHECK(!egos_module_secret_is_valid("AZaz09-_AZaz09-_AZaz09-_AZaz09-_AZaz09-_ab"));
    CHECK(!egos_module_secret_is_valid("AZaz09-_AZaz09-_AZaz09-_AZaz09-_AZaz09-_abcd"));
    /* base64 (not url) characters and padding */
    CHECK(!egos_module_secret_is_valid("AZaz09+/AZaz09-_AZaz09-_AZaz09-_AZaz09-_abc"));
    CHECK(!egos_module_secret_is_valid("AZaz09-_AZaz09-_AZaz09-_AZaz09-_AZaz09-_ab="));
    CHECK(!egos_module_secret_is_valid("AZaz09-_AZaz09-_AZaz09-_AZaz09-_AZaz09-_ab "));
}

static void test_message_is_valid(void)
{
    CHECK(egos_module_secret_message_is_valid(GOOD, true, 1));
    CHECK(!egos_module_secret_message_is_valid(GOOD, true, 2));
    CHECK(!egos_module_secret_message_is_valid(GOOD, true, 1.5));
    CHECK(!egos_module_secret_message_is_valid(GOOD, false, 1));
    CHECK(!egos_module_secret_message_is_valid(NULL, true, 1));
    CHECK(!egos_module_secret_message_is_valid("short", true, 1));
}

static void test_refusal_action(void)
{
    /* CONNACK 4 with a secret: drop it */
    CHECK(egos_module_auth_refusal_action(4, true) == EGOS_AUTH_REFUSAL_DROP_SECRET);
    /* CONNACK 4 without one: nothing to drop */
    CHECK(egos_module_auth_refusal_action(4, false) == EGOS_AUTH_REFUSAL_OTHER);
    /* CONNACK 5 without a secret: the id is bound, operator must Forget */
    CHECK(egos_module_auth_refusal_action(5, false) == EGOS_AUTH_REFUSAL_NEEDS_FORGET);
    /* CONNACK 5 with a secret is not the protocol's case: keep the secret */
    CHECK(egos_module_auth_refusal_action(5, true) == EGOS_AUTH_REFUSAL_OTHER);
    /* Other codes never touch the secret */
    for (int code = 0; code <= 3; code++) {
        CHECK(egos_module_auth_refusal_action(code, true) == EGOS_AUTH_REFUSAL_OTHER);
        CHECK(egos_module_auth_refusal_action(code, false) == EGOS_AUTH_REFUSAL_OTHER);
    }
}

static void test_anonymous_uri(void)
{
    char out[128];

    CHECK(egos_module_auth_anonymous_uri("mqtt://192.168.4.1:1883", out, sizeof(out)));
    CHECK(strcmp(out, "mqtt://module:@192.168.4.1:1883") == 0);

    CHECK(egos_module_auth_anonymous_uri("mqtt://egos.local:1883", out, sizeof(out)));
    CHECK(strcmp(out, "mqtt://module:@egos.local:1883") == 0);

    /* Existing userinfo (e.g. a secret) is replaced, never carried over */
    CHECK(egos_module_auth_anonymous_uri("mqtt://module:" "SECRET@10.0.0.2:1883/x", out, sizeof(out)));
    CHECK(strcmp(out, "mqtt://module:@10.0.0.2:1883/x") == 0);
    CHECK(strstr(out, "SECRET") == NULL);

    /* An '@' in the path is not userinfo */
    CHECK(egos_module_auth_anonymous_uri("mqtt://10.0.0.2:1883/a@b", out, sizeof(out)));
    CHECK(strcmp(out, "mqtt://module:@10.0.0.2:1883/a@b") == 0);

    CHECK(!egos_module_auth_anonymous_uri("10.0.0.2:1883", out, sizeof(out)));
    CHECK(!egos_module_auth_anonymous_uri(NULL, out, sizeof(out)));

    /* Too small: refuse rather than truncate */
    char tiny[12];
    CHECK(!egos_module_auth_anonymous_uri("mqtt://192.168.4.1:1883", tiny, sizeof(tiny)));
}

static void test_log_due(void)
{
    int64_t last = 0;
    const int64_t minute = 60LL * 1000 * 1000;

    CHECK(egos_module_auth_log_due(5000000, &last, minute));     /* first: yes */
    CHECK(last == 5000000);
    CHECK(!egos_module_auth_log_due(6000000, &last, minute));    /* 1s later: no */
    CHECK(!egos_module_auth_log_due(5000000 + minute - 1, &last, minute));
    CHECK(egos_module_auth_log_due(5000000 + minute, &last, minute));
    CHECK(last == 5000000 + minute);
}

int main(void)
{
    test_secret_is_valid();
    test_message_is_valid();
    test_refusal_action();
    test_anonymous_uri();
    test_log_due();

    printf("%d checks, %d failed\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
