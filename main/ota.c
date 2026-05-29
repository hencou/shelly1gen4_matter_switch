/*
 * OTA implementatie voor Shelly 1 Gen4 custom Zigbee firmware.
 *
 * Twee paden, allebei via WiFi:
 *   1) STA-mode met opgeslagen creds       -> direct esp_https_ota fetch.
 *   2) SoftAP-mode + HTTP captive form     -> user voert creds + url in.
 *
 * Tijdens OTA-mode wordt de Zigbee-stack NIET gestart. Daardoor geen
 * coexistence-werk en is de firmware-grootte tijdens OTA minimaal.
 *
 * Partitielayout vereist ota_0 + ota_1 + otadata. Zie partitions.csv.
 */

#include "ota.h"
#include "app_config.h"
#include "button.h"
#include "status_led.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
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

static bool ota_load_credentials(char *ssid, size_t ssidlen,
                                 char *pass, size_t passlen,
                                 char *url,  size_t urllen)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_load_str(h, NVS_KEY_SSID, ssid, ssidlen) &&
              nvs_load_str(h, NVS_KEY_URL,  url,  urllen);
    /* password mag leeg zijn (open netwerk) */
    if (ok) nvs_load_str(h, NVS_KEY_PASS, pass, passlen);
    nvs_close(h);
    return ok;
}

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

/* ---------- HTTPS OTA fetch ---------- */

static esp_err_t do_ota_from_url(const char *url)
{
    ESP_LOGI(TAG, "starting OTA from %s", url);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .crt_bundle_attach = NULL,  /* HTTPS optioneel */
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

/* ---------- SoftAP + HTTP form ---------- */

static const char FORM_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8><title>Shelly OTA</title>"
"<style>body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:1em}"
"input{width:100%;padding:.5em;margin:.3em 0}label{font-weight:bold}"
"button{padding:.6em 1.2em;background:#0066cc;color:#fff;border:0;cursor:pointer}"
"</style></head><body><h2>Shelly Zigbee Switch — OTA</h2>"
"<form method=POST action=/ota>"
"<label>WiFi SSID</label><input name=ssid required>"
"<label>WiFi password</label><input name=pass type=password>"
"<label>Firmware URL (.bin)</label>"
"<input name=url required value='http://homeassistant.local:8123/local/shelly1gen4.bin'>"
"<p><button type=submit>Save & flash</button></p></form></body></html>";

static esp_err_t form_get(httpd_req_t *req)
{
    return httpd_resp_send(req, FORM_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '+') { *w++ = ' '; r++; }
        else if (*r == '%' && r[1] && r[2]) {
            char h[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(h, NULL, 16); r += 3;
        } else *w++ = *r++;
    }
    *w = 0; return ESP_OK;
}

static bool form_field(const char *body, const char *key, char *out, size_t outlen)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    p += strlen(needle);
    const char *e = strchr(p, '&');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n); out[n] = 0;
    url_decode(out);
    return true;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    char body[512] = { 0 };
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no body");
        return ESP_FAIL;
    }
    char ssid[33] = {0}, pass[65] = {0}, url[256] = {0};
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));
    form_field(body, "url",  url,  sizeof(url));
    if (!ssid[0] || !url[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid+url required");
        return ESP_FAIL;
    }
    ota_save_credentials(ssid, pass, url);
    const char *msg = "Saved. Rebooting and starting OTA...\n";
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    /* Re-trigger pending flag; bij boot leest hij creds + start STA-OTA. */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_PENDING, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_restart();
    return ESP_OK;
}

static void run_softap(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "shelly-ota-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    ESP_LOGW(TAG, "no creds, opening SoftAP '%s' (open network)", ssid);

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

    httpd_handle_t srv = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&srv, &hc));
    httpd_uri_t get_root = { "/",    HTTP_GET,  form_get, NULL };
    httpd_uri_t post_ota = { "/ota", HTTP_POST, ota_post, NULL };
    httpd_register_uri_handler(srv, &get_root);
    httpd_register_uri_handler(srv, &post_ota);

    ESP_LOGW(TAG, "SoftAP ready. Connect a phone/laptop to '%s', open"
                  " http://192.168.4.1/", ssid);

    /* Idle hier — POST handler reboot zelf. */
    while (1) vTaskDelay(pdMS_TO_TICKS(60000));
}

/* ---------- Public entrypoints ---------- */

/* ---------- Factory reset (6x klik tijdens OTA mode) ---------- */

/* Wipe nvs + chip_kvs partities, laat chip_factory + fctry intact (device
 * cert / DAC zit daar). Reboot resulteert in schone Matter mode met
 * commissioning open. */
static void full_factory_reset(void)
{
    ESP_LOGW(TAG, "FULL factory reset: wiping nvs + chip_kvs partities");
    /* Eerst chip_kvs (Matter binding/commissioning/ACL). Erase werkt op
     * partitie-level dus zelfs als nvs_flash_init voor deze partitie nog
     * niet is aangeroepen mag dit. */
    esp_err_t err = nvs_flash_erase_partition("chip_kvs");
    ESP_LOGW(TAG, "  chip_kvs erase -> %s", esp_err_to_name(err));

    /* Dan de gewone nvs (OTA pending flag + WiFi creds + app NVS). */
    nvs_flash_deinit();
    err = nvs_flash_erase();
    ESP_LOGW(TAG, "  nvs erase -> %s", esp_err_to_name(err));

    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "rebooting into clean Matter mode");
    esp_restart();
}

/* Callback voor de button-driver tijdens OTA mode.
 * Dedicated OTA mode = geen Matter, geen relais. Alleen MODE_TOGGLE actief
 * (6x klik) om eruit te komen via factory reset. SHORT/LONG_PRESS worden
 * stil genegeerd. */
static void ota_mode_button_cb(input_id_t id, button_event_t evt)
{
    if (evt == BTN_EVT_MODE_TOGGLE) {
        ESP_LOGW(TAG, "MODE_TOGGLE from input %d in OTA mode -> factory reset", id);
        full_factory_reset();   /* never returns */
    } else {
        ESP_LOGI(TAG, "OTA mode: button input=%d evt=%d ignored", id, evt);
    }
}

void ota_handle_pending(void)
{
    if (!ota_pending_read_and_clear()) {
        return;  /* normaal opstarten -> Matter */
    }
    ESP_LOGW(TAG, "OTA pending — entering DEDICATED OTA-mode (Matter NOT started)");

    /* Init button-driver met OTA-mode callback. Hierdoor kunnen alle 3
     * inputs een 6x-klik gesture detecteren om uit OTA mode te knallen. */
    button_driver_init(ota_mode_button_cb);
    /* Visuele indicator: snel knipperen = "wachten op OTA upload" */
    status_led_set(STATUS_LED_FAST_BLINK);

    char ssid[33] = {0}, pass[65] = {0}, url[256] = {0};
    bool have_creds = ota_load_credentials(ssid, sizeof(ssid),
                                           pass, sizeof(pass),
                                           url,  sizeof(url));
    if (have_creds) {
        ESP_LOGI(TAG, "trying STA OTA -> %s", ssid);
        if (wifi_init_sta(ssid, pass) == ESP_OK) {
            if (do_ota_from_url(url) == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }
        ESP_LOGW(TAG, "STA-pad mislukt, val terug naar SoftAP");
        esp_wifi_stop();
        esp_wifi_deinit();
    }
    run_softap();  /* never returns; button-task blijft in achtergrond luisteren */
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
