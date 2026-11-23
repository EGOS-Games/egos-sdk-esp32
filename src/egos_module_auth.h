/**
 * @file egos_module_auth.h
 * @brief Module secrets (protocol v1): the decisions, kept free of ESP-IDF
 *
 * A controller gives each module a secret the first time it registers, and
 * from then on the module proves its client id with it (username "module",
 * password = the secret). The contract is "Module secrets (protocol v1)" in
 * egos-controller docs/LOCAL-AUTH.md.
 *
 * Everything here is plain C with no ESP-IDF dependency, so it can be built
 * and tested on a host: see test/host/test_module_auth.c. The NVS storage is
 * in egos_nvs.c and the MQTT side in egos_mqtt.c.
 *
 * NEVER log a secret, or any part of one.
 */

#ifndef EGOS_MODULE_AUTH_H
#define EGOS_MODULE_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** MQTT username of a module that supports secrets, with or without one. */
#define EGOS_MODULE_AUTH_USERNAME       "module"

/** A secret is 32 random bytes, base64url without padding: 43 characters. */
#define EGOS_MODULE_SECRET_LEN          43
#define EGOS_MODULE_SECRET_BUF_SIZE     (EGOS_MODULE_SECRET_LEN + 1)

/** The only protocol version this SDK accepts in a `{id}/module/secret` message. */
#define EGOS_MODULE_SECRET_VERSION      1

/** MQTT 3.1.1 CONNACK return codes the protocol uses. */
#define EGOS_CONNACK_BAD_USERNAME_OR_PASSWORD   4
#define EGOS_CONNACK_NOT_AUTHORIZED             5

/** What to do when the broker refuses a connection. */
typedef enum {
    /** Not a module-secret refusal: leave the normal reconnect alone. */
    EGOS_AUTH_REFUSAL_OTHER,
    /** CONNACK 4 with a stored secret: the controller forgot it. Delete it
     *  and connect without one, so the controller issues a new one. */
    EGOS_AUTH_REFUSAL_DROP_SECRET,
    /** CONNACK 5 without a secret: the id already has a secret this module
     *  lost. Keep retrying; an operator has to press Forget. */
    EGOS_AUTH_REFUSAL_NEEDS_FORGET,
} egos_auth_refusal_action_t;

/**
 * True for exactly 43 characters of [A-Za-z0-9_-] (base64url, no padding).
 * NULL is not valid.
 */
bool egos_module_secret_is_valid(const char *secret);

/**
 * True when a `{id}/module/secret` message's fields are acceptable: a valid
 * secret and version 1.
 *
 * @param secret             The "secret" string, or NULL if it was missing or
 *                           not a string
 * @param version_is_number  Whether "version" was present and a number
 * @param version            Its value (ignored when version_is_number is false)
 */
bool egos_module_secret_message_is_valid(const char *secret,
                                         bool version_is_number,
                                         double version);

/**
 * Decide what a refused connection means.
 *
 * @param connack_code  The broker's CONNACK return code
 * @param used_secret   Whether the refused connection sent a stored secret
 */
egos_auth_refusal_action_t egos_module_auth_refusal_action(int connack_code,
                                                           bool used_secret);

/**
 * Rewrite a broker URI so it carries username "module" and an empty password,
 * e.g. "mqtt://10.0.0.1:1883" -> "mqtt://module:@10.0.0.1:1883". Any userinfo
 * already in the URI is replaced. The result never contains a secret, so it is
 * safe for esp-mqtt to log.
 *
 * @return false if the URI has no "://" or the result does not fit
 */
bool egos_module_auth_anonymous_uri(const char *uri, char *out, size_t out_len);

/** Reconnect backoff after CONNACK 5: 1 s, doubling, capped at 60 s. */
#define EGOS_MODULE_AUTH_BACKOFF_MIN_MS     1000u
#define EGOS_MODULE_AUTH_BACKOFF_MAX_MS     60000u

/**
 * The next reconnect delay after a CONNACK 5 refusal.
 *
 * @param previous_ms  The delay used last time, or 0 for the first refusal
 * @return 1000 for the first, then double the previous, never above 60000
 */
uint32_t egos_module_auth_next_backoff_ms(uint32_t previous_ms);

/**
 * True when `topic` (topic_len bytes, need not be NUL terminated) is exactly
 * "{module_id}/module/secret". Only that topic may carry our secret.
 */
bool egos_module_auth_is_secret_topic(const char *topic, size_t topic_len,
                                      const char *module_id);

/**
 * Throttle for a repeated log line: true when it should be printed now.
 * Updates *last_us when it returns true. A *last_us of 0 means "never logged".
 */
bool egos_module_auth_log_due(int64_t now_us, int64_t *last_us,
                              int64_t interval_us);

#ifdef __cplusplus
}
#endif

#endif /* EGOS_MODULE_AUTH_H */
