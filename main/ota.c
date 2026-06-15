/*
 * OTA + WiFi module for the Shelly 1 Gen4 custom Matter firmware.
 *
 * Two paths, both via WiFi:
 *   1) STA mode with saved creds            -> direct esp_https_ota fetch.
 *   2) SoftAP mode + HTTP upload form       -> user uploads .bin directly from browser.
 *
 * During OTA mode the Matter stack is NOT started. This avoids
 * coexistence overhead and keeps the firmware size during OTA minimal.
 *
 * HTTP handlers and the management dashboard live in web_api.c.
 */

#include "ota.h"
#include "web_api.h"
#include "app_config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#include "button.h"
#include "status_led.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "ota";

#define NVS_NS              "ota"
#define NVS_KEY_PENDING     "pending"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASS        "pass"
#define NVS_KEY_URL         "url"
#define NVS_KEY_BENCH       "bench"
#define NVS_KEY_WIFI_PERS   "wifi_pers"
#define NVS_KEY_TBR         "tbr"

/* Runtime bench mode — initialised from NVS in bench_mode_init(). */
int g_bench_mode = BENCH_MODE;

/* Runtime flags — initialised from NVS early in boot. */
static bool s_wifi_persistent = false;
static bool s_tbr_mode = false;

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_evt;
static int                s_retry = 0;

/* ---------- NVS helpers ---------- */

static esp_err_t nvs_set_str_safe(nvs_handle_t h, const char *k, const char *v)
{
    return nvs_set_str(h, k, v ? v : "");
}

static bool nvs_load_str(nvs_handle_t h, const char *k, char *buf, size_t buflen)
{
    size_t l = buflen;
    if (nvs_get_str(h, k, buf, &l) != ESP_OK) return false;
    return strlen(buf) > 0;
}

esp_err_t ota_save_credentials(const char *ssid, const char *pass, const char *url)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str_safe(h, NVS_KEY_SSID, ssid);
    nvs_set_str_safe(h, NVS_KEY_PASS, pass);
    nvs_set_str_safe(h, NVS_KEY_URL,  url);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "credentials saved (ssid=%s, url=%s)",
             ssid ? ssid : "?", url ? url : "?");
    return ESP_OK;
}

bool ota_load_credentials(char *ssid, size_t ssidlen,
                          char *pass, size_t passlen,
                          char *url,  size_t urllen)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_load_str(h, NVS_KEY_SSID, ssid, ssidlen);
    if (ok) {
        nvs_load_str(h, NVS_KEY_PASS, pass, passlen);
        nvs_load_str(h, NVS_KEY_URL,  url,  urllen);
    }
    nvs_close(h);
    return ok;
}

/* ---------- WiFi persistent + TBR mode NVS ---------- */

static void wifi_persistent_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY_WIFI_PERS, &v) == ESP_OK) {
            s_wifi_persistent = (v != 0);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "wifi_persistent = %d", s_wifi_persistent);
}

bool ota_wifi_persistent_get(void)
{
    return s_wifi_persistent;
}

esp_err_t ota_wifi_persistent_set(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NVS_KEY_WIFI_PERS, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    s_wifi_persistent = on;
    ESP_LOGI(TAG, "wifi_persistent saved: %d", on);
    return ESP_OK;
}

static void tbr_mode_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY_TBR, &v) == ESP_OK) {
            s_tbr_mode = (v != 0);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "tbr_mode = %d", s_tbr_mode);
}

bool ota_tbr_mode_get(void)
{
    return s_tbr_mode;
}

esp_err_t ota_tbr_mode_set(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NVS_KEY_TBR, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    s_tbr_mode = on;
    ESP_LOGI(TAG, "tbr_mode saved: %d", on);
    return ESP_OK;
}

/* ---------- Bench mode NVS ---------- */

void bench_mode_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0xff;
        if (nvs_get_u8(h, NVS_KEY_BENCH, &v) == ESP_OK) {
            g_bench_mode = (v != 0) ? 1 : 0;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "bench_mode = %d (compile-time default = %d)",
             g_bench_mode, BENCH_MODE);

    wifi_persistent_init();
    tbr_mode_init();
}

esp_err_t ota_bench_mode_save(int on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NVS_KEY_BENCH, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    g_bench_mode = on;
    ESP_LOGI(TAG, "bench_mode saved: %d", on);
    return err;
}

/* ---------- OTA pending flag ---------- */

static bool ota_pending_read_and_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t v = 0;
    nvs_get_u8(h, NVS_KEY_PENDING, &v);
    nvs_set_u8(h, NVS_KEY_PENDING, 0);
    nvs_commit(h);
    nvs_close(h);
    return v != 0;
}

void ota_request_at_next_boot(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_PENDING, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "OTA requested -> rebooting");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

void ota_request_ota_reboot(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_PENDING, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_restart();
}

/* ---------- WiFi STA ---------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry++ < 5) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_evt, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(const char *ssid, const char *pass)
{
    s_wifi_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t any_id, got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &got_ip));

    wifi_config_t wcfg = { 0 };
    strncpy((char*)wcfg.sta.ssid,     ssid, sizeof(wcfg.sta.ssid));
    strncpy((char*)wcfg.sta.password, pass, sizeof(wcfg.sta.password));
    wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

/* ---------- HTTPS OTA fetch (STA path: fetch URL) ---------- */

static esp_err_t do_ota_from_url(const char *url)
{
    ESP_LOGI(TAG, "starting OTA from %s", url);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .crt_bundle_attach = NULL,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };
    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA OK, rebooting");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    return err;
}

/* ---------- OTA timeout ---------- */

#define OTA_TIMEOUT_MS   (10 * 60 * 1000)
#define OTA_TICK_MS      5000

static void wait_for_upload_or_timeout(void)
{
    int32_t remaining_ms = OTA_TIMEOUT_MS;
    while (remaining_ms > 0) {
        int32_t sleep = (remaining_ms < OTA_TICK_MS)
                        ? remaining_ms : OTA_TICK_MS;
        vTaskDelay(pdMS_TO_TICKS(sleep));
        remaining_ms -= sleep;
        if (remaining_ms > 0) {
            ESP_LOGW(TAG, "OTA: %"PRId32" seconds remaining, waiting for upload...",
                     remaining_ms / 1000);
        }
    }

    ESP_LOGW(TAG, "OTA timeout expired — rebooting to Matter mode");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* ---------- Start SoftAP ---------- */

static void run_softap(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "shelly-ota-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    ESP_LOGW(TAG, "SoftAP opening: '%s' (open network)", ssid);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));

    wifi_config_t apc = {
        .ap = {
            .max_connection = 3,
            .authmode = WIFI_AUTH_OPEN,
            .channel = 6,
        },
    };
    strncpy((char*)apc.ap.ssid, ssid, sizeof(apc.ap.ssid));
    apc.ap.ssid_len = strlen(ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apc));
    ESP_ERROR_CHECK(esp_wifi_start());

    web_api_start_httpd();

    ESP_LOGW(TAG, "SoftAP ready. Connect to '%s', open http://192.168.4.1/", ssid);

    wait_for_upload_or_timeout();  /* never returns (reboots) */
}

/* ---------- Button callback during OTA mode ---------- */

static void ota_mode_button_cb(input_id_t id, button_event_t evt)
{
    ESP_LOGI(TAG, "OTA mode: input=%d evt=%d ignored", id, evt);
}

/* ---------- Runtime WiFi enable (alongside Thread/Matter) ---------- */

static void build_ap_ssid(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, len, "shelly-cfg-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static void wifi_runtime_task(void *arg)
{
    (void)arg;
    char ssid[33] = {0}, pass[65] = {0}, url[256] = {0};
    bool have_creds = ota_load_credentials(ssid, sizeof(ssid),
                                           pass, sizeof(pass),
                                           url,  sizeof(url));
    bool from_compile_time = false;

#ifdef DEFAULT_WIFI_SSID
    if (!have_creds) {
#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS ""
#endif
        strncpy(ssid, DEFAULT_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(pass, DEFAULT_WIFI_PASS, sizeof(pass) - 1);
        have_creds = strlen(ssid) > 0;
        from_compile_time = have_creds;
    }
#endif

    esp_netif_init();
    esp_event_loop_create_default();
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"))
        esp_netif_create_default_wifi_sta();
    if (!esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"))
        esp_netif_create_default_wifi_ap();

    {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    }

    s_wifi_evt = xEventGroupCreate();
    s_retry = 0;

    {
        esp_event_handler_instance_t any_id, got_ip;
        esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &any_id);
        esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &got_ip);
    }

    char ap_ssid[32];
    build_ap_ssid(ap_ssid, sizeof(ap_ssid));

    wifi_config_t apc = {
        .ap = {
            .max_connection = 3,
            .authmode = WIFI_AUTH_OPEN,
            .channel = 6,
        },
    };
    strncpy((char*)apc.ap.ssid, ap_ssid, sizeof(apc.ap.ssid));
    apc.ap.ssid_len = strlen(ap_ssid);

    if (have_creds) {
        ESP_LOGI(TAG, "wifi_runtime: APSTA mode — AP '%s' + STA '%s' (source: %s)",
                 ap_ssid, ssid, from_compile_time ? "compile-time" : "NVS");

        wifi_config_t wcfg = { 0 };
        strncpy((char*)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid));
        strncpy((char*)wcfg.sta.password, pass, sizeof(wcfg.sta.password));
        wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apc));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGW(TAG, "wifi_runtime: AP ready '%s', http://192.168.4.1/", ap_ssid);

        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGW(TAG, "wifi_runtime: STA connected — management httpd on both AP and STA");
            if (from_compile_time) {
                ota_save_credentials(ssid, pass, url);
                ESP_LOGI(TAG, "wifi_runtime: compile-time credentials saved to NVS");
            }
        } else {
            ESP_LOGW(TAG, "wifi_runtime: STA failed — AP '%s' still available", ap_ssid);
        }
    } else {
        ESP_LOGW(TAG, "wifi_runtime: no credentials, AP-only mode '%s'", ap_ssid);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apc));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGW(TAG, "wifi_runtime: AP ready '%s', http://192.168.4.1/", ap_ssid);
    }

    web_api_start_httpd();
    vTaskDelete(NULL);
}

static bool s_wifi_runtime_started = false;

void ota_wifi_ensure_netifs(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"))
        esp_netif_create_default_wifi_sta();
    if (!esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"))
        esp_netif_create_default_wifi_ap();
    ESP_LOGI(TAG, "WiFi netifs created (STA + AP)");
}

esp_netif_t *ota_get_wifi_sta_netif(void)
{
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

void ota_enable_wifi_runtime(void)
{
    if (s_wifi_runtime_started) {
        ESP_LOGW(TAG, "WiFi runtime already started, ignoring");
        return;
    }
    s_wifi_runtime_started = true;
    ESP_LOGW(TAG, "Enabling WiFi alongside Thread (runtime, non-persistent)");
    xTaskCreate(wifi_runtime_task, "wifi_rt", 4096, NULL, 5, NULL);
}

/* ---------- Public entrypoints ---------- */

void ota_handle_pending(void)
{
    if (!ota_pending_read_and_clear()) {
        return;
    }
    ESP_LOGW(TAG, "OTA pending — entering DEDICATED OTA-mode (Matter NOT started)");

    button_driver_init(ota_mode_button_cb);
    status_led_set(STATUS_LED_FAST_BLINK);

    char ssid[33] = {0}, pass[65] = {0}, url[256] = {0};
    bool have_creds = ota_load_credentials(ssid, sizeof(ssid),
                                           pass, sizeof(pass),
                                           url,  sizeof(url));

#ifdef DEFAULT_WIFI_SSID
    if (!have_creds) {
        ESP_LOGI(TAG, "No saved WiFi creds — using compile-time defaults");
#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS ""
#endif
#ifndef DEFAULT_OTA_URL
#define DEFAULT_OTA_URL   ""
#endif
        ota_save_credentials(DEFAULT_WIFI_SSID,
                             DEFAULT_WIFI_PASS,
                             DEFAULT_OTA_URL);
        strncpy(ssid, DEFAULT_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(pass, DEFAULT_WIFI_PASS, sizeof(pass) - 1);
        strncpy(url,  DEFAULT_OTA_URL,   sizeof(url) - 1);
        have_creds = strlen(ssid) > 0;
    }
#endif
    if (have_creds) {
        ESP_LOGI(TAG, "trying STA OTA -> ssid=%s url=%s", ssid, url);
        if (wifi_init_sta(ssid, pass) == ESP_OK) {
            web_api_start_httpd();
            ESP_LOGW(TAG, "STA connected. OTA web interface available on local IP");

            if (url[0] && do_ota_from_url(url) == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
            ESP_LOGW(TAG, "URL fetch skipped/failed, waiting for manual upload...");
            wait_for_upload_or_timeout();
        }
        ESP_LOGW(TAG, "STA connection failed, falling back to SoftAP");
        esp_wifi_stop();
        esp_wifi_deinit();
    }
    run_softap();
}

void ota_mark_app_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "current app marked valid (rollback canceled)");
        }
    }
}
