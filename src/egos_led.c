/**
 * @file egos_led.c
 * @brief RGB LED status indicator via LEDC PWM (optional)
 *
 * Only compiled when CONFIG_EGOS_STATUS_LED_ENABLED is set in Kconfig.
 */

/* sdkconfig.h must be included BEFORE the guard below, otherwise
 * CONFIG_EGOS_STATUS_LED_ENABLED is undefined when the #ifdef is evaluated and
 * this file compiles to nothing whatever the configuration says - which is
 * exactly what used to happen, leaving egos_led_init/egos_led_set_state
 * undefined at link time. */
#include "sdkconfig.h"

#ifdef CONFIG_EGOS_STATUS_LED_ENABLED

#include "egos_internal.h"
#include "driver/ledc.h"

static const char *TAG = "egos_led";

#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_CH_RED         LEDC_CHANNEL_0
#define LEDC_CH_GREEN       LEDC_CHANNEL_1
#define LEDC_CH_BLUE        LEDC_CHANNEL_2
#define LEDC_DUTY_RES       LEDC_TIMER_8_BIT
#define LEDC_FREQ           5000

/* Timing constants. These match the values the hardware modules use, so a
 * migrated module blinks at the same cadence operators are used to. */
#define BLINK_FAST_MS       200
#define BLINK_MEDIUM_MS     500
#define PULSE_PERIOD_MS     2000
#define DOUBLE_BLINK_MS     200
#define TRIPLE_FLASH_MS     200
#define INPUT_FLASH_MS      100
#define FADE_DURATION_MS    2000

/* Colours of the EGOS status-LED language (see egos_led_state_t) */
#define C_OFF               0,   0,   0
#define C_RED             255,   0,   0
#define C_GREEN             0, 255,   0
#define C_BLUE              0,   0, 255
#define C_YELLOW          255, 255,   0
#define C_CYAN              0, 255, 255
#define C_MAGENTA         255,   0, 255
#define C_WHITE           255, 255, 255
#define C_ORANGE          255, 128,   0   /* stored WiFi credentials */
#define C_DIM_WHITE       128, 128, 128   /* initialising */
#define C_YELLOW_ORANGE   255, 192,   0   /* MQTT errors */
#define C_PURPLE          128,   0, 255   /* network switching */

static volatile egos_led_state_t s_state = EGOS_LED_OFF;
static TaskHandle_t s_task = NULL;
static bool s_initialized = false;

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    ledc_set_duty(LEDC_MODE, LEDC_CH_RED, r);
    ledc_update_duty(LEDC_MODE, LEDC_CH_RED);
    ledc_set_duty(LEDC_MODE, LEDC_CH_GREEN, g);
    ledc_update_duty(LEDC_MODE, LEDC_CH_GREEN);
    ledc_set_duty(LEDC_MODE, LEDC_CH_BLUE, b);
    ledc_update_duty(LEDC_MODE, LEDC_CH_BLUE);
}

static void blink(uint8_t r, uint8_t g, uint8_t b, uint32_t interval_ms)
{
    static bool on = false;
    if (on) {
        set_rgb(r, g, b);
    } else {
        set_rgb(0, 0, 0);
    }
    on = !on;
    vTaskDelay(pdMS_TO_TICKS(interval_ms));
}

/* Two quick blinks then a gap - used to mark a transition away from a layer
 * (e.g. Ethernet gave up and WiFi is about to be tried). */
static void double_blink(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 2; i++) {
        set_rgb(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(DOUBLE_BLINK_MS));
        set_rgb(C_OFF);
        vTaskDelay(pdMS_TO_TICKS(DOUBLE_BLINK_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(DOUBLE_BLINK_MS * 3));
}

/* Three quick flashes, then hold off. Marks a one-shot event rather than a
 * steady state, so it settles dark instead of repeating forever. */
static void triple_flash(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 3; i++) {
        set_rgb(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(TRIPLE_FLASH_MS));
        set_rgb(C_OFF);
        vTaskDelay(pdMS_TO_TICKS(TRIPLE_FLASH_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}

static void pulse(uint8_t r, uint8_t g, uint8_t b, uint32_t *counter)
{
    *counter += 50;
    if (*counter >= PULSE_PERIOD_MS) *counter = 0;

    float factor;
    if (*counter <= PULSE_PERIOD_MS / 2) {
        factor = 0.3f + 0.7f * ((float)*counter / (PULSE_PERIOD_MS / 2));
    } else {
        factor = 1.0f - 0.7f * ((float)(*counter - PULSE_PERIOD_MS / 2) / (PULSE_PERIOD_MS / 2));
    }

    set_rgb((uint8_t)(r * factor), (uint8_t)(g * factor), (uint8_t)(b * factor));
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void led_task(void *pvParameters)
{
    uint32_t pulse_counter = 0;

    while (1) {
        switch (s_state) {
            case EGOS_LED_OFF:
                set_rgb(0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;

            case EGOS_LED_INIT:
                /* Dim white, brief */
                set_rgb(C_DIM_WHITE);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;

            /* ---- Ethernet layer: cyan ---- */
            case EGOS_LED_ETHERNET_CONNECTING:
                blink(C_CYAN, BLINK_MEDIUM_MS);
                break;

            case EGOS_LED_ETHERNET_CONNECTED:
                set_rgb(C_CYAN);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;

            case EGOS_LED_ETHERNET_FAILED:
                double_blink(C_CYAN);
                break;

            /* ---- WiFi layer: yellow on the default network, orange on stored ---- */
            case EGOS_LED_WIFI_CONNECTING_DEFAULT:
                blink(C_YELLOW, BLINK_MEDIUM_MS);
                break;

            case EGOS_LED_WIFI_CONNECTING_STORED:
                blink(C_ORANGE, BLINK_MEDIUM_MS);
                break;

            case EGOS_LED_WIFI_CONNECTED:
                set_rgb(C_YELLOW);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;

            case EGOS_LED_WIFI_FAILED:
                blink(C_RED, BLINK_FAST_MS);
                break;

            /* ---- MQTT / application layer: green ---- */
            case EGOS_LED_MQTT_CONNECTING:
                blink(C_GREEN, BLINK_MEDIUM_MS);
                break;

            case EGOS_LED_MQTT_FAILED:
                blink(C_YELLOW_ORANGE, BLINK_MEDIUM_MS);
                break;

            case EGOS_LED_CONNECTED_DEFAULT:
                set_rgb(C_GREEN);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;

            case EGOS_LED_CONNECTED_STORED:
                /* Green breathing, so "which network am I on" is readable at a glance */
                pulse(C_GREEN, &pulse_counter);
                break;

            /* ---- Errors ---- */
            case EGOS_LED_AUTH_ERROR:
                blink(C_MAGENTA, BLINK_FAST_MS);
                break;

            case EGOS_LED_ERROR:
                blink(C_RED, BLINK_FAST_MS);
                break;

            /* ---- One-shot events ---- */
            case EGOS_LED_INPUT_FLASH:
                set_rgb(C_BLUE);
                vTaskDelay(pdMS_TO_TICKS(INPUT_FLASH_MS));
                set_rgb(C_OFF);
                vTaskDelay(pdMS_TO_TICKS(INPUT_FLASH_MS));
                break;

            case EGOS_LED_CRED_PROVISIONING:
                triple_flash(C_WHITE);
                break;

            case EGOS_LED_NETWORK_SWITCHING:
                pulse(C_PURPLE, &pulse_counter);
                break;

            case EGOS_LED_REBOOTING:
                /* White fade out */
                for (int i = 255; i >= 0; i -= 5) {
                    set_rgb(i, i, i);
                    vTaskDelay(pdMS_TO_TICKS(FADE_DURATION_MS / 51));
                }
                set_rgb(0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;

            default:
                set_rgb(0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
        }
    }
}

esp_err_t egos_led_init(void)
{
    const egos_led_config_t *cfg = &egos_g_config.status_led;

    ESP_LOGI(TAG, "Initializing RGB LED (R=%d, G=%d, B=%d)",
             cfg->red_pin, cfg->green_pin, cfg->blue_pin);

    /* Configure LEDC timer */
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    /* Configure channels */
    ledc_channel_config_t ch_cfg = {
        .speed_mode = LEDC_MODE,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .duty = 0,
        .hpoint = 0,
    };

    ch_cfg.channel = LEDC_CH_RED;
    ch_cfg.gpio_num = cfg->red_pin;
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    ch_cfg.channel = LEDC_CH_GREEN;
    ch_cfg.gpio_num = cfg->green_pin;
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    ch_cfg.channel = LEDC_CH_BLUE;
    ch_cfg.gpio_num = cfg->blue_pin;
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    /* Create LED task */
    BaseType_t ret = xTaskCreate(led_task, "egos_led", 2048, NULL, 3, &s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LED task");
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "RGB LED initialized");
    return ESP_OK;
}

void egos_led_set_state(egos_led_state_t state)
{
    if (!s_initialized) return;
    s_state = state;
}

void egos_led_cleanup(void)
{
    if (!s_initialized) return;

    set_rgb(0, 0, 0);
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    s_initialized = false;
    ESP_LOGI(TAG, "RGB LED cleaned up");
}

#endif /* CONFIG_EGOS_STATUS_LED_ENABLED */
