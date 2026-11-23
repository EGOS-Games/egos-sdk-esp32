/**
 * @file egos_module_auth.c
 * @brief Module secrets (protocol v1): pure decisions, no ESP-IDF
 */

#include "egos_module_auth.h"
#include <stdio.h>
#include <string.h>

static bool is_base64url_char(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_';
}

bool egos_module_secret_is_valid(const char *secret)
{
    if (secret == NULL) {
        return false;
    }
    for (size_t i = 0; i < EGOS_MODULE_SECRET_LEN; i++) {
        /* A NUL before 43 characters fails here too: '\0' is not base64url. */
        if (!is_base64url_char(secret[i])) {
            return false;
        }
    }
    return secret[EGOS_MODULE_SECRET_LEN] == '\0';
}

bool egos_module_secret_message_is_valid(const char *secret,
                                         bool version_is_number,
                                         double version)
{
    if (!version_is_number || version != (double)EGOS_MODULE_SECRET_VERSION) {
        return false;
    }
    return egos_module_secret_is_valid(secret);
}

egos_auth_refusal_action_t egos_module_auth_refusal_action(int connack_code,
                                                           bool used_secret)
{
    if (connack_code == EGOS_CONNACK_BAD_USERNAME_OR_PASSWORD && used_secret) {
        return EGOS_AUTH_REFUSAL_DROP_SECRET;
    }
    if (connack_code == EGOS_CONNACK_NOT_AUTHORIZED && !used_secret) {
        return EGOS_AUTH_REFUSAL_NEEDS_FORGET;
    }
    return EGOS_AUTH_REFUSAL_OTHER;
}

bool egos_module_auth_anonymous_uri(const char *uri, char *out, size_t out_len)
{
    if (uri == NULL || out == NULL || out_len == 0) {
        return false;
    }

    const char *scheme_end = strstr(uri, "://");
    if (scheme_end == NULL) {
        return false;
    }

    /* Skip any userinfo already present: it ends at the last '@' before the
     * path starts. */
    const char *authority = scheme_end + 3;
    const char *path = strchr(authority, '/');
    const char *host = authority;
    for (const char *p = authority; *p != '\0' && (path == NULL || p < path); p++) {
        if (*p == '@') {
            host = p + 1;
        }
    }

    int n = snprintf(out, out_len, "%.*s://%s:@%s",
                     (int)(scheme_end - uri), uri,
                     EGOS_MODULE_AUTH_USERNAME, host);
    return n > 0 && (size_t)n < out_len;
}

bool egos_module_auth_log_due(int64_t now_us, int64_t *last_us,
                              int64_t interval_us)
{
    if (last_us == NULL) {
        return true;
    }
    if (*last_us == 0 || now_us - *last_us >= interval_us) {
        *last_us = now_us;
        return true;
    }
    return false;
}

uint32_t egos_module_auth_next_backoff_ms(uint32_t previous_ms)
{
    if (previous_ms < EGOS_MODULE_AUTH_BACKOFF_MIN_MS) {
        return EGOS_MODULE_AUTH_BACKOFF_MIN_MS;
    }
    if (previous_ms >= EGOS_MODULE_AUTH_BACKOFF_MAX_MS / 2) {
        return EGOS_MODULE_AUTH_BACKOFF_MAX_MS;
    }
    return previous_ms * 2;
}

bool egos_module_auth_is_secret_topic(const char *topic, size_t topic_len,
                                      const char *module_id)
{
    static const char suffix[] = "/module/secret";
    const size_t suffix_len = sizeof(suffix) - 1;

    if (topic == NULL || module_id == NULL) {
        return false;
    }
    size_t id_len = strlen(module_id);
    if (id_len == 0 || topic_len != id_len + suffix_len) {
        return false;
    }
    return memcmp(topic, module_id, id_len) == 0 &&
           memcmp(topic + id_len, suffix, suffix_len) == 0;
}
