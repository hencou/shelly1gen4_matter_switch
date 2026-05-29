/*
 * Button driver voor alle 3 inputs van de Shelly 1 Gen4:
 *   INPUT_DRUKKER     (GPIO10) System 55 impulsdrukker
 *   INPUT_TOUCH       (TTP223 op Add-on digital in)
 *   INPUT_DEVICE_BTN  (GPIO4)  onboard pair-knop
 *
 * Alle 3 inputs hebben hetzelfde gedrag (zie on_button_event in app_main.cpp):
 *   SHORT_PRESS       -> Matter Toggle + relais tikken
 *   LONG_PRESS_START  -> dim start
 *   LONG_PRESS_STOP   -> dim stop
 *   6x klik           -> MODE_TOGGLE (universeel: Matter <-> OTA mode)
 *
 * Polariteit per input:
 *   - INPUT_DRUKKER:    active-high (productie 230V optocoupler);
 *                       active-low in BENCH_MODE (interne pull-up)
 *   - INPUT_TOUCH:      altijd active-high (TTP223 momentary)
 *   - INPUT_DEVICE_BTN: altijd active-low (interne pull-up, knop naar GND)
 *
 * Driver gebruikt ISR-to-queue met een eigen FreeRTOS-task zodat
 * Matter-stack-calls in task-context gebeuren.
 *
 * NOOT over System 55 drukker (optocoupler, active-high):
 *   De drukker stuurt een puls: pin gaat HOOG bij indrukken, LAAG bij loslaten.
 *   ANYEDGE vangt beide flanken. De opgaande flank (indrukken) start de
 *   long-press timer; de neergaande flank (loslaten) beslist short vs long.
 *   Zo werkt dimmen correct: lang ingedrukt houden -> LONG_PRESS_START tijdens
 *   de puls, loslaten -> LONG_PRESS_STOP.
 */

#include "button.h"
#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "button";

typedef struct {
    input_id_t id;
    int64_t    t_us;
    int        level;       /* raw GPIO level uit ISR */
} btn_isr_msg_t;

#define CLICK_HISTORY 8     /* groot genoeg voor 6x MODE_TOGGLE + marge */

typedef struct {
    int     gpio;
    bool    enabled;           /* niet alle inputs zijn bedraad (TTP223 optioneel) */
    bool    active_low;        /* press = low edge (true) of high edge (false) */
    bool    pressed;
    int64_t press_start_us;
    bool    long_fired;
    /* ring-buffer met de laatste short-press timestamps in microseconden */
    int64_t click_hist[CLICK_HISTORY];
    uint8_t click_idx;
} btn_state_t;

static QueueHandle_t s_evt_q;
static button_cb_t   s_cb;
static btn_state_t   s_state[INPUT_COUNT];

/* Helper: zet logische "pressed" status om vanuit raw GPIO level. */
static inline bool level_is_pressed(const btn_state_t *s, int level)
{
    return s->active_low ? (level == 0) : (level == 1);
}

static void IRAM_ATTR btn_isr(void *arg)
{
    input_id_t id = (input_id_t)(uintptr_t)arg;
    btn_isr_msg_t msg = {
        .id    = id,
        .t_us  = esp_timer_get_time(),
        .level = gpio_get_level(s_state[id].gpio),
    };
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_evt_q, &msg, &hpw);
    if (hpw) {
        portYIELD_FROM_ISR();
    }
}

static void handle_edge(btn_isr_msg_t *m)
{
    btn_state_t *s = &s_state[m->id];
    if (!s->enabled) return;

    bool pressed_now = level_is_pressed(s, m->level);

    if (pressed_now && !s->pressed) {
        /* --- opgaande flank: knop ingedrukt --- */
        s->pressed        = true;
        s->press_start_us = m->t_us;
        s->long_fired     = false;

    } else if (!pressed_now && s->pressed) {
        /* --- neergaande flank: knop losgelaten --- */
        int64_t dur_ms = (m->t_us - s->press_start_us) / 1000;
        s->pressed = false;

        if (s->long_fired) {
            /* lang ingedrukt gehouden -> stop dimmen */
            if (s_cb) s_cb(m->id, BTN_EVT_LONG_PRESS_STOP);

        } else if (dur_ms >= LONG_PRESS_MS) {
            /* Rand-geval: long_fired nog net niet gezet door check_long_press()
             * maar duur is wel >= drempel. Fire START + STOP samen. */
            s->long_fired = true;
            if (s_cb) s_cb(m->id, BTN_EVT_LONG_PRESS_START);
            if (s_cb) s_cb(m->id, BTN_EVT_LONG_PRESS_STOP);

        } else if (dur_ms > 20 /* debounce */) {
            /* kort ingedrukt -> short press
             * Log timestamp in ring buffer en kijk hoeveel clicks er
             * binnen het MODE_TOGGLE-window vallen. */
            s->click_hist[s->click_idx] = m->t_us;
            s->click_idx = (s->click_idx + 1) % CLICK_HISTORY;

            uint8_t cnt = 0;
            for (int i = 0; i < CLICK_HISTORY; i++) {
                int64_t t = s->click_hist[i];
                if (t == 0) continue;
                int64_t age_ms = (m->t_us - t) / 1000;
                if (age_ms <= MODE_TOGGLE_WINDOW_MS) cnt++;
            }

            if (cnt >= MODE_TOGGLE_CLICKS) {
                /* 6x gehaald: wis history (anders fired 7e klik direct weer) */
                memset(s->click_hist, 0, sizeof(s->click_hist));
                if (s_cb) s_cb(m->id, BTN_EVT_MODE_TOGGLE);
            } else {
                if (s_cb) s_cb(m->id, BTN_EVT_SHORT_PRESS);
            }
        }
        /* dur_ms <= 20: debounce ruis, stil negeren */
    }
}

static void check_long_press(int64_t now_us)
{
    for (int i = 0; i < INPUT_COUNT; i++) {
        btn_state_t *s = &s_state[i];
        if (!s->enabled || !s->pressed || s->long_fired) continue;

        int64_t dur_ms = (now_us - s->press_start_us) / 1000;
        if (dur_ms >= LONG_PRESS_MS) {
            s->long_fired = true;
            if (s_cb) s_cb((input_id_t)i, BTN_EVT_LONG_PRESS_START);
        }
    }
}

static void btn_task(void *arg)
{
    btn_isr_msg_t msg;
    for (;;) {
        /* Wake up elke 50 ms voor long-press timing */
        if (xQueueReceive(s_evt_q, &msg, pdMS_TO_TICKS(50)) == pdTRUE) {
            handle_edge(&msg);
        }
        check_long_press(esp_timer_get_time());
    }
}

void button_driver_init(button_cb_t cb)
{
    s_cb = cb;

    /* ---------- per-input configuratie ---------- */

    s_state[INPUT_DRUKKER].gpio       = PIN_SWITCH_INPUT;
    s_state[INPUT_DRUKKER].enabled    = true;
    s_state[INPUT_DRUKKER].active_low = (BENCH_MODE != 0);

    s_state[INPUT_TOUCH].gpio       = PIN_TOUCH_INPUT;
    s_state[INPUT_TOUCH].enabled    = true;
    s_state[INPUT_TOUCH].active_low = false;

    s_state[INPUT_DEVICE_BTN].gpio       = 4;
    s_state[INPUT_DEVICE_BTN].enabled    = true;
    s_state[INPUT_DEVICE_BTN].active_low = true;

    /* ---------- GPIO-config ---------- */

    ESP_LOGI(TAG, "BD-STEP-1: gpio_config drukker GPIO%d bench=%d (active_low=%d)",
             PIN_SWITCH_INPUT, BENCH_MODE, s_state[INPUT_DRUKKER].active_low);
    gpio_config_t drukker_cfg = {
        .pin_bit_mask = (1ULL << PIN_SWITCH_INPUT),
        .mode         = GPIO_MODE_INPUT,
#if BENCH_MODE
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#endif
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&drukker_cfg);
    ESP_LOGI(TAG, "BD-STEP-2: gpio_config drukker done");

    gpio_config_t touch_cfg = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_INPUT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&touch_cfg);

    gpio_config_t devbtn_cfg = {
        .pin_bit_mask = (1ULL << s_state[INPUT_DEVICE_BTN].gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&devbtn_cfg);

    /* ---------- queue + ISR-service ---------- */

    s_evt_q = xQueueCreate(16, sizeof(btn_isr_msg_t));
    ESP_LOGI(TAG, "BD-STEP-3: xQueueCreate done q=%p", s_evt_q);

    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    ESP_LOGI(TAG, "BD-STEP-4: gpio_install_isr_service -> %d (0=OK)", isr_svc_err);

    gpio_isr_handler_add(PIN_SWITCH_INPUT, btn_isr,
                         (void *)(uintptr_t)INPUT_DRUKKER);
    ESP_LOGI(TAG, "BD-STEP-5a: isr_handler_add drukker (GPIO%d) done",
             PIN_SWITCH_INPUT);
    gpio_isr_handler_add(PIN_TOUCH_INPUT, btn_isr,
                         (void *)(uintptr_t)INPUT_TOUCH);
    ESP_LOGI(TAG, "BD-STEP-5b: isr_handler_add touch (GPIO%d) done",
             PIN_TOUCH_INPUT);
    gpio_isr_handler_add(s_state[INPUT_DEVICE_BTN].gpio,
                         btn_isr, (void *)(uintptr_t)INPUT_DEVICE_BTN);
    ESP_LOGI(TAG, "BD-STEP-5c: isr_handler_add device_btn (GPIO%d) done",
             s_state[INPUT_DEVICE_BTN].gpio);

    BaseType_t btn_r = xTaskCreate(btn_task, "btn_task", 3072, NULL, 10, NULL);
    ESP_LOGI(TAG, "BD-STEP-6: xTaskCreate btn_task -> %s",
             btn_r == pdPASS ? "OK" : "FAIL");

    ESP_LOGI(TAG, "button driver init (drukker=GPIO%d touch=GPIO%d device_btn=GPIO%d)",
             PIN_SWITCH_INPUT, PIN_TOUCH_INPUT,
             s_state[INPUT_DEVICE_BTN].gpio);
}
