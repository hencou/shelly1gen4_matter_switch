/*
 * OTA implementation for Shelly 1 Gen4 custom Matter firmware.
 *
 * Two paths, both via WiFi:
 *   1) STA mode with saved creds            -> direct esp_https_ota fetch.
 *   2) SoftAP mode + HTTP upload form       -> user uploads .bin directly from browser.
 *
 * During OTA mode the Matter stack is NOT started. This avoids
 * coexistence overhead and keeps the firmware size during OTA minimal.
 *
 * Partition layout requires ota_0 + ota_1 + otadata. See partitions.csv.
 */

#include "ota.h"
#include "app_config.h"
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
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "nvs.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/uart.h"
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

#define OTA_BUF_SIZE        1024

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
    /* password may be empty (open network) */
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

/* ---------- HTTPS OTA fetch (STA path: fetch URL) ---------- */

static esp_err_t do_ota_from_url(const char *url)
{
    ESP_LOGI(TAG, "starting OTA from %s", url);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .crt_bundle_attach = NULL,  /* HTTPS optional */
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


/* ---------- DS18B20 sensor test (dual-pin 1-Wire via ISO7221A) ---------- */

#define OTA_OW_TX  PIN_ONEWIRE_TX
#define OTA_OW_RX  PIN_ONEWIRE_RX

static inline void ota_ow_tx_low(void)  { gpio_set_level(OTA_OW_TX, 0); }
static inline void ota_ow_tx_high(void) { gpio_set_level(OTA_OW_TX, 1); }
static inline int  ota_ow_rx_rd(void)   { return gpio_get_level(OTA_OW_RX); }

static bool ota_ow_reset(void)
{
    uint8_t retries = 125;
    do {
        if (--retries == 0) return false;
        esp_rom_delay_us(2);
    } while (!ota_ow_rx_rd());

    ota_ow_tx_low();  esp_rom_delay_us(480);
    ota_ow_tx_high(); esp_rom_delay_us(70);
    bool present = !ota_ow_rx_rd();
    esp_rom_delay_us(410);
    return present;
}

static void ota_ow_write_bit(int b)
{
    ota_ow_tx_low();
    if (b) { esp_rom_delay_us(10); ota_ow_tx_high(); esp_rom_delay_us(55); }
    else   { esp_rom_delay_us(65); ota_ow_tx_high(); esp_rom_delay_us(5);  }
}

static int ota_ow_read_bit(void)
{
    ota_ow_tx_low();  esp_rom_delay_us(3);
    ota_ow_tx_high(); esp_rom_delay_us(9);
    int v = ota_ow_rx_rd();
    esp_rom_delay_us(53);
    return v;
}

static void ota_ow_write_byte(uint8_t b)
{
    for (int i = 0; i < 8; i++) { ota_ow_write_bit(b & 1); b >>= 1; }
}

static uint8_t ota_ow_read_byte(void)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) v |= (ota_ow_read_bit() << i);
    return v;
}

static esp_err_t sensor_get(httpd_req_t *req)
{
    static char buf[3072];
    int pos = 0;

    uart_driver_delete(UART_NUM_0);

    gpio_config_t tx_cfg = {
        .pin_bit_mask = (1ULL << OTA_OW_TX),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&tx_cfg);
    ota_ow_tx_high();

    gpio_reset_pin(OTA_OW_RX);
    gpio_config_t rx_cfg = {
        .pin_bit_mask = (1ULL << OTA_OW_RX),
        .mode         = GPIO_MODE_INPUT,
    };
    gpio_config(&rx_cfg);

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta http-equiv=refresh content=3>"
        "<title>DS18B20 Test</title>"
        "<style>body{font-family:monospace;max-width:520px;margin:2em auto;padding:1em}"
        "h2{font-family:sans-serif}.ok{color:green}.err{color:red}"
        ".warn{color:orange}.row{margin:.5em 0}a{color:#0066cc}</style></head><body>"
        "<h2>&#127777; DS18B20 Sensor Test</h2>"
        "<p style='color:#555;font-size:.9em'>Page refreshes automatically every 3s.</p>");

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<div class=row><b>Step 1: Reset pulse (TX=GPIO%d, RX=GPIO%d)</b> &mdash; ", OTA_OW_TX, OTA_OW_RX);
    bool present = ota_ow_reset();
    if (!present) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "<span class=err>&#10007; No presence pulse &mdash; sensor not found on bus</span></div>"
            "<div class=row><span class=warn>Check wiring: VCC=3V3, TX=GPIO%d, RX=GPIO%d, GND=GND</span></div>"
            "<p><a href=/>&#8592; Back</a></p></body></html>", OTA_OW_TX, OTA_OW_RX);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, buf, pos);
        return ESP_OK;
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<span class=ok>&#10003; Presence pulse received &mdash; sensor found</span></div>");

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<div class=row><b>Step 2: Convert T (0xCC 0x44)</b> &mdash; ");
    ota_ow_write_byte(0xCC);
    ota_ow_write_byte(0x44);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<span class=ok>&#10003; Command sent</span></div>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, buf, pos);
    pos = 0;
    vTaskDelay(pdMS_TO_TICKS(820));

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<div class=row><b>Step 3: Reset for ReadScratchpad</b> &mdash; ");
    present = ota_ow_reset();
    if (!present) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "<span class=err>&#10007; No presence pulse after conversion</span></div>"
            "<p><a href=/>&#8592; Back</a></p></body></html>");
        httpd_resp_send_chunk(req, buf, pos);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<span class=ok>&#10003; Presence pulse OK</span></div>");

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<div class=row><b>Step 4: ReadScratchpad (0xCC 0xBE)</b><br>");
    ota_ow_write_byte(0xCC);
    ota_ow_write_byte(0xBE);

    uint8_t sc[9];
    for (int i = 0; i < 9; i++) sc[i] = ota_ow_read_byte();

    const char *labels[9] = {"Temp LSB","Temp MSB","TH register","TL register",
                              "Config register","Reserved","Reserved","Reserved","CRC"};
    for (int i = 0; i < 9; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "Byte[%d] = 0x%02X &mdash; %s<br>", i, sc[i], labels[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "</div>");

    int16_t raw   = (int16_t)((sc[1] << 8) | sc[0]);
    int16_t centi = (int16_t)(((int32_t)raw * 100) / 16);
    int deg  = centi / 100;
    int frac = centi < 0 ? -(centi % 100) : (centi % 100);
    if (frac < 0) frac = -frac;
    uint8_t res   = 9 + ((sc[4] >> 5) & 0x03);
    bool sane = (raw != 0x0550) && (centi > -5500) && (centi < 12500);

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<div class=row><b>Step 5: Temperature</b><br>"
        "raw=0x%04X (%d) &rarr; %d.%02d&deg;C &mdash; ",
        (uint16_t)raw, raw, deg, frac);

    if (raw == 0x0550) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "<span class=warn>&#9888; Power-on value 85&deg;C &mdash; conversion failed?</span>");
    } else if (!sane) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "<span class=err>&#10007; Illogical value &mdash; check wiring</span>");
    } else {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "<span class=ok>&#10003; Valid reading: <b>%d.%02d&deg;C</b></span>", deg, frac);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "</div><div class=row><b>Resolution:</b> %d bit</div>"
        "<p><a href=/>&#8592; Back to OTA page</a></p></body></html>", res);

    httpd_resp_send_chunk(req, buf, pos);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---------- GPIO17 (Analog IN / occupancy) diagnostic ---------- */

#include "esp_private/periph_ctrl.h"
#include "soc/periph_defs.h"

static esp_err_t gpio17_diag_get(httpd_req_t *req)
{
    static char buf[2048];
    int pos = 0;

    /* ---- UART0 teardown (same as sensors_init) ---- */
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    uart_driver_delete(UART_NUM_0);
    periph_module_disable(PERIPH_UART0_MODULE);
    gpio_reset_pin(PIN_LD2410_INPUT);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_LD2410_INPUT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    /* ---- Duty cycle measurement: sample over 100 ms ---- */
    int high = 0, total = 0;
    for (int us = 0; us < 100000; us += 100) {
        if (gpio_get_level(PIN_LD2410_INPUT)) high++;
        total++;
        esp_rom_delay_us(100);
    }
    int duty = (high * 100) / total;
    int occ  = (duty >= 25);

    /* ---- Build HTML ---- */
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta http-equiv=refresh content=2>"
        "<title>GPIO17 Diagnostic</title>"
        "<style>body{font-family:monospace;max-width:560px;margin:2em auto;padding:1em}"
        "h2{font-family:sans-serif}"
        ".hi{color:red;font-weight:bold}.lo{color:green;font-weight:bold}"
        "table{border-collapse:collapse;margin:.5em 0}"
        "td,th{border:1px solid #ccc;padding:4px 10px;text-align:left}"
        "</style></head><body>"
        "<h2>&#128270; GPIO17 (Analog IN) Diagnostic</h2>"
        "<p>Auto-refreshes every 2 s.  Duty cycle sampled over 100 ms (1000 reads).</p>");

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<h3>Duty cycle: <span class=%s>%d%%</span></h3>"
        "<h3>Occupancy: <span class=%s>%s</span></h3>",
        occ ? "hi" : "lo", duty,
        occ ? "hi" : "lo", occ ? "OCCUPIED" : "CLEAR");

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<table>"
        "<tr><th>Parameter</th><th>Value</th></tr>"
        "<tr><td>GPIO</td><td>%d</td></tr>"
        "<tr><td>Samples</td><td>%d (HIGH: %d)</td></tr>"
        "<tr><td>Duty cycle</td><td>%d%%</td></tr>"
        "<tr><td>Threshold</td><td>25%% (\u2248 2.5 V)</td></tr>"
        "<tr><td>Result</td><td>%s</td></tr>"
        "</table>",
        PIN_LD2410_INPUT, total, high, duty,
        occ ? "occupied" : "clear");

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<p><a href=/>&#8592; Back to OTA</a></p></body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

/* ---------- SoftAP HTML page ---------- */

/*
 * Upload page:
 * - Top: direct .bin upload via fetch() -> /upload (no WiFi creds needed)
 * - Bottom: optional WiFi+URL entry for STA fetch on next boot
 */
static const char FORM_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Shelly OTA</title>"
"<style>"
"body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:1em}"
"h2{margin-bottom:.2em}h3{margin-top:1.5em;border-top:1px solid #ccc;padding-top:1em}"
"input{width:100%;box-sizing:border-box;padding:.5em;margin:.3em 0 .8em}"
"label{font-weight:bold;display:block}"
".btn{padding:.6em 1.4em;background:#0066cc;color:#fff;border:0;"
"     cursor:pointer;font-size:1em;border-radius:3px}"
".btn:disabled{background:#999;cursor:default}"
"#bar-wrap{display:none;margin:.8em 0;background:#eee;border-radius:4px;height:22px}"
"#bar{height:22px;background:#0066cc;border-radius:4px;width:0;transition:width .2s}"
"#bar-lbl{text-align:center;margin-top:.2em;font-size:.9em}"
"#status{font-weight:bold;margin-top:.6em}"
".ok{color:green}.err{color:red}"
"</style></head><body>"

"<h2>&#128268; Shelly1Gen4 &mdash; OTA Update</h2>"
"<p><a href=/sensor>&#127777; DS18B20 sensor test</a>"
" &nbsp;|&nbsp; <a href=/gpio17>&#128270; GPIO17 diagnostic</a></p>"

/* ── Section 1: direct upload ── */
"<h3>Upload firmware</h3>"
"<label for=binfile>Select .bin file</label>"
"<input type=file id=binfile accept='.bin'>"
"<button class=btn id=flashbtn onclick=doUpload()>&#9889; Flash Firmware</button>"
"<div id=bar-wrap><div id=bar></div></div>"
"<div id=bar-lbl></div>"
"<div id=status></div>"

/* ── Section 2: WiFi + URL for STA boot ── */
"<h3>Save WiFi (optional)</h3>"
"<p style='font-size:.9em;color:#555'>Save WiFi credentials so the device can "
"fetch a URL on the next OTA trigger by itself (without SoftAP).</p>"
"<form method=POST action=/ota>"
"<label>WiFi SSID</label><input name=ssid>"
"<label>WiFi Password</label><input name=pass type=password>"
"<label>Firmware URL (.bin)</label>"
"<input name=url value='http://homeassistant.local:8123/local/shelly1gen4.bin'>"
"<button class=btn type=submit>Save &amp; Restart</button>"
"</form>"

"<script>"
"function doUpload(){"
"  var f=document.getElementById('binfile').files[0];"
"  if(!f){alert('Select a .bin file first');return;}"
"  var btn=document.getElementById('flashbtn');"
"  var wrap=document.getElementById('bar-wrap');"
"  var bar=document.getElementById('bar');"
"  var lbl=document.getElementById('bar-lbl');"
"  var st=document.getElementById('status');"
"  btn.disabled=true;"
"  wrap.style.display='block';"
"  st.textContent='';"
"  st.className='';"
"  var xhr=new XMLHttpRequest();"
"  xhr.upload.onprogress=function(e){"
"    if(e.lengthComputable){"
"      var pct=Math.round(e.loaded/e.total*100);"
"      bar.style.width=pct+'%';"
"      lbl.textContent='Uploading: '+pct+'% ('+Math.round(e.loaded/1024)+'/'+"
"        Math.round(e.total/1024)+' KB)';"
"    }"
"  };"
"  xhr.onload=function(){"
"    if(xhr.status===200){"
"      bar.style.width='100%';"
"      lbl.textContent='Upload complete — device restarting...';"
"      st.textContent='\\u2705 Flashed! Wait 10 seconds and reconnect.';"
"      st.className='ok';"
"    } else {"
"      st.textContent='\\u274C Error: '+xhr.responseText;"
"      st.className='err';"
"      btn.disabled=false;"
"    }"
"  };"
"  xhr.onerror=function(){"
"    st.textContent='\\u274C Connection lost during upload.';"
"    st.className='err';"
"    btn.disabled=false;"
"  };"
"  xhr.open('POST','/upload');"
"  xhr.setRequestHeader('Content-Type','application/octet-stream');"
"  xhr.send(f);"
"}"
"</script></body></html>";

/* ---------- HTTP handlers ---------- */

static esp_err_t form_get(httpd_req_t *req)
{
    return httpd_resp_send(req, FORM_HTML, HTTPD_RESP_USE_STRLEN);
}

/* /upload — receives raw .bin, writes directly to OTA partition */
static esp_err_t upload_post(httpd_req_t *req)
{
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition found");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA begin failed");
        return ESP_FAIL;
    }

    char buf[OTA_BUF_SIZE];
    int remaining = req->content_len;
    int total     = remaining;
    ESP_LOGI(TAG, "upload start: %d bytes to partition %s",
             total, update_part->label);

    while (remaining > 0) {
        int to_read = (remaining < OTA_BUF_SIZE) ? remaining : OTA_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_read);
        if (received < 0) {
            ESP_LOGE(TAG, "httpd_req_recv error: %d", received);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Receive failed");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Flash write failed");
            return ESP_FAIL;
        }
        remaining -= received;
        ESP_LOGD(TAG, "  received %d/%d bytes", total - remaining, total);
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA verification failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Setting boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "upload succeeded (%d bytes), rebooting", total);
    httpd_resp_sendstr(req, "OK");

    /* Wait so the browser receives the response before reboot */
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

/* /ota POST — save WiFi creds + URL, reboot for STA OTA (original path) */
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
    const char *msg = "Saved. Device restarting and fetching firmware...\n";
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_PENDING, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_restart();
    return ESP_OK;
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

    /* Increase max body size for the upload handler */
    httpd_handle_t srv = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.recv_wait_timeout  = 30;   /* seconds waiting for data */
    hc.send_wait_timeout  = 10;
    hc.max_open_sockets   = 3;
    ESP_ERROR_CHECK(httpd_start(&srv, &hc));

    httpd_uri_t get_root    = { "/",       HTTP_GET,  form_get,        NULL };
    httpd_uri_t post_upload = { "/upload", HTTP_POST, upload_post,     NULL };
    httpd_uri_t post_ota    = { "/ota",    HTTP_POST, ota_post,        NULL };
    httpd_uri_t get_sensor  = { "/sensor", HTTP_GET,  sensor_get,      NULL };
    httpd_uri_t get_gpio17  = { "/gpio17", HTTP_GET,  gpio17_diag_get, NULL };

    httpd_register_uri_handler(srv, &get_root);
    httpd_register_uri_handler(srv, &post_upload);
    httpd_register_uri_handler(srv, &post_ota);
    httpd_register_uri_handler(srv, &get_sensor);
    httpd_register_uri_handler(srv, &get_gpio17);

    ESP_LOGW(TAG, "SoftAP ready. Connect to '%s', open http://192.168.4.1/", ssid);

    /* Wait up to OTA_SOFTAP_TIMEOUT_MS. If no upload or form post happens,
     * we simply reboot back to Matter mode. The NVS flag has already been
     * cleared upon entering ota_handle_pending(), so after reboot Matter
     * starts automatically. */
#define OTA_SOFTAP_TIMEOUT_MS   (10 * 60 * 1000)   /* 10 minutes */
#define OTA_SOFTAP_TICK_MS      5000                /* log every 5 seconds */

    int32_t remaining_ms = OTA_SOFTAP_TIMEOUT_MS;
    while (remaining_ms > 0) {
        int32_t sleep = (remaining_ms < OTA_SOFTAP_TICK_MS)
                        ? remaining_ms : OTA_SOFTAP_TICK_MS;
        vTaskDelay(pdMS_TO_TICKS(sleep));
        remaining_ms -= sleep;
        if (remaining_ms > 0) {
            ESP_LOGW(TAG, "OTA SoftAP: %"PRId32" seconds remaining, waiting for upload...",
                     remaining_ms / 1000);
        }
    }

    ESP_LOGW(TAG, "OTA timeout expired — rebooting to Matter mode");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* ---------- Factory reset ---------- */

static void full_factory_reset(void)
{
    ESP_LOGW(TAG, "FULL factory reset: wiping nvs + chip_kvs partities");
    esp_err_t err = nvs_flash_erase_partition("chip_kvs");
    ESP_LOGW(TAG, "  chip_kvs erase -> %s", esp_err_to_name(err));
    nvs_flash_deinit();
    err = nvs_flash_erase();
    ESP_LOGW(TAG, "  nvs erase -> %s", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "rebooting into clean Matter mode");
    esp_restart();
}

/* ---------- Button callback during OTA mode ---------- */

static void ota_mode_button_cb(input_id_t id, button_event_t evt)
{
    if (evt == BTN_EVT_MODE_TOGGLE) {
        ESP_LOGW(TAG, "MODE_TOGGLE in OTA mode -> factory reset");
        full_factory_reset();
    } else {
        ESP_LOGI(TAG, "OTA mode: input=%d evt=%d ignored", id, evt);
    }
}

/* ---------- Public entrypoints ---------- */

void ota_handle_pending(void)
{
    if (!ota_pending_read_and_clear()) {
        return;  /* normal boot -> Matter */
    }
    ESP_LOGW(TAG, "OTA pending — entering DEDICATED OTA-mode (Matter NOT started)");

    button_driver_init(ota_mode_button_cb);
    status_led_set(STATUS_LED_FAST_BLINK);

    char ssid[33] = {0}, pass[65] = {0}, url[256] = {0};
    bool have_creds = ota_load_credentials(ssid, sizeof(ssid),
                                           pass, sizeof(pass),
                                           url,  sizeof(url));
    if (have_creds) {
        ESP_LOGI(TAG, "trying STA OTA -> ssid=%s url=%s", ssid, url);
        if (wifi_init_sta(ssid, pass) == ESP_OK) {
            if (do_ota_from_url(url) == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }
        ESP_LOGW(TAG, "STA path failed, falling back to SoftAP");
        esp_wifi_stop();
        esp_wifi_deinit();
    }
    run_softap();  /* never returns */
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
