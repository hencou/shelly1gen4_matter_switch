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

/* Optional compile-time WiFi defaults from local secrets.h (not in git).
 * If the file exists, DEFAULT_WIFI_SSID/PASS are pre-loaded into NVS
 * on first OTA boot so you don't need the web form. */
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
#include "driver/temperature_sensor.h"
#include "esp_timer.h"
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

/* Runtime bench mode — initialised from NVS in bench_mode_init(). */
int g_bench_mode = BENCH_MODE;

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
    bool ok = nvs_load_str(h, NVS_KEY_SSID, ssid, ssidlen);
    /* password and URL may be empty */
    if (ok) {
        nvs_load_str(h, NVS_KEY_PASS, pass, passlen);
        nvs_load_str(h, NVS_KEY_URL,  url,  urllen);
    }
    nvs_close(h);
    return ok;
}

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
}

static esp_err_t bench_mode_save(int on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NVS_KEY_BENCH, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    g_bench_mode = on ? 1 : 0;
    ESP_LOGI(TAG, "bench_mode saved: %d", g_bench_mode);
    return ESP_OK;
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

#include "esp_private/periph_ctrl.h"
#include "soc/periph_defs.h"

/* ---------- Management dashboard HTML page ---------- */

static const char MGMT_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Shelly Management</title>"
"<style>"
"*{box-sizing:border-box}body{font-family:sans-serif;max-width:560px;margin:0 auto;padding:1em}"
"h2{margin-bottom:.3em}"
".tabs{display:flex;border-bottom:2px solid #0066cc;margin:1em 0 0}"
".tab{padding:.6em 1.2em;cursor:pointer;border:1px solid #ccc;border-bottom:none;"
"  margin-right:2px;border-radius:6px 6px 0 0;background:#f5f5f5;font-size:.95em}"
".tab.act{background:#0066cc;color:#fff;border-color:#0066cc}"
".pane{display:none;padding:1em 0}.pane.act{display:block}"
"input,select{width:100%;padding:.5em;margin:.3em 0 .8em;border:1px solid #ccc;border-radius:3px}"
"label{font-weight:bold;display:block;margin-top:.3em}"
".btn{padding:.55em 1.2em;color:#fff;border:0;cursor:pointer;font-size:.95em;border-radius:3px;margin:.3em .3em .3em 0}"
".btn-blue{background:#0066cc}.btn-green{background:#28a745}"
".btn-red{background:#dc3545}.btn-gray{background:#6c757d}"
".btn:disabled{background:#999;cursor:default}"
"#bar-wrap{display:none;margin:.8em 0;background:#eee;border-radius:4px;height:22px}"
"#bar{height:22px;background:#0066cc;border-radius:4px;width:0;transition:width .2s}"
"#bar-lbl{text-align:center;margin-top:.2em;font-size:.9em}"
".msg{font-weight:bold;margin-top:.6em}.ok{color:green}.err{color:red}"
"table{border-collapse:collapse;width:100%;margin:.5em 0}"
"td,th{border:1px solid #ddd;padding:6px 10px;text-align:left}"
"th{background:#f0f0f0}.hw-val{font-family:monospace;font-size:1.1em}"
"h4{margin:1.2em 0 .3em;color:#333;border-bottom:1px solid #ddd;padding-bottom:.2em}"
".info{font-size:.85em;color:#555;margin-bottom:.8em}"
"</style></head><body>"

"<h2>Shelly1Gen4 Management</h2>"

/* ── Tab bar ── */
"<div class=tabs>"
"<div class='tab act' onclick=showTab(0)>WiFi &amp; OTA</div>"
"<div class=tab onclick=showTab(1)>Hardware</div>"
"<div class=tab onclick=showTab(2)>Scripts</div>"
"<div class=tab onclick=showTab(3)>Backup</div>"
"</div>"

/* ── Tab 0: WiFi & OTA ── */
"<div class='pane act' id=p0>"
"<h3>WiFi Settings</h3>"
"<p class=info>Credentials are stored in flash. On next OTA boot the device "
"connects to your WiFi automatically.</p>"
"<label>WiFi SSID</label><input id=ssid>"
"<label>WiFi Password</label><input id=pass type=password>"
"<label>Firmware URL (optional)</label>"
"<input id=url placeholder='http://server/shelly1gen4.bin'>"
"<div style='margin:1em 0'>"
"<button class='btn btn-green' onclick=doSaveRestart()>Save &amp; Restart</button>"
"<button class='btn btn-gray' onclick=doRestart()>Restart without saving</button>"
"<button class='btn btn-red' onclick=doFactory()>Factory Reset</button>"
"</div>"
"<div id=wifi-msg class=msg></div>"

"<h3>Upload Firmware</h3>"
"<label for=binfile>Select .bin file</label>"
"<input type=file id=binfile accept='.bin'>"
"<button class='btn btn-blue' id=flashbtn onclick=doUpload()>Flash Firmware</button>"
"<div id=bar-wrap><div id=bar></div></div>"
"<div id=bar-lbl></div>"
"<div id=flash-msg class=msg></div>"
"</div>"

/* ── Tab 1: Hardware ── */
"<div class=pane id=p1>"
"<h3>Hardware Status</h3>"
"<p class=info>Live readings (auto-refresh every 5 s). "
"<button class='btn btn-blue' onclick=loadHW()>Refresh now</button></p>"

"<h4>System</h4>"
"<table><tr><th style='width:45%'>Item</th><th>Value</th></tr>"
"<tr><td>Firmware</td><td class=hw-val id=hw-fw>-</td></tr>"
"<tr><td>Chip</td><td class=hw-val id=hw-chip>-</td></tr>"
"<tr><td>MAC address</td><td class=hw-val id=hw-mac>-</td></tr>"
"<tr><td>WiFi mode</td><td class=hw-val id=hw-wifi>-</td></tr>"
"<tr><td>WiFi RSSI</td><td class=hw-val id=hw-rssi>-</td></tr>"
"<tr><td>Uptime</td><td class=hw-val id=hw-up>-</td></tr>"
"<tr><td>Free heap</td><td class=hw-val id=hw-heap>-</td></tr>"
"<tr><td>Chip temperature</td><td class=hw-val id=hw-ctemp>-</td></tr>"
"<tr><td>Reset reason</td><td class=hw-val id=hw-rst>-</td></tr>"
"<tr><td>Bench mode<br><span style='font-size:.8em;color:#666'>"
"ON = GPIO10 pull-up, sensor tasks skipped (UART0 stays active).<br>"
"OFF = production (230V optocoupler, DS18B20 + occupancy running).</span></td>"
"<td class=hw-val id=hw-bench>-<br>"
"<button class='btn btn-gray' id=bench-btn onclick=toggleBench() style='margin-top:.4em'>Toggle</button>"
"</td></tr>"
"</table>"

"<h4>Inputs</h4>"
"<table><tr><th style='width:45%'>Input</th><th>Value</th></tr>"
"<tr><td>Pushbutton (GPIO10)</td><td class=hw-val id=hw-btn>-</td></tr>"
"<tr><td>PCB button (GPIO4)</td><td class=hw-val id=hw-pcb>-</td></tr>"
"<tr><td>Digital IN (GPIO18)</td><td class=hw-val id=hw-dig>-</td></tr>"
"<tr><td>Analog IN (GPIO17)</td><td class=hw-val id=hw-ana>-</td></tr>"
"</table>"

"<h4>Sensors</h4>"
"<table><tr><th style='width:45%'>Sensor</th><th>Value</th></tr>"
"<tr><td>Temperature (DS18B20)</td><td class=hw-val id=hw-temp>-</td></tr>"
"</table>"
"<div id=hw-msg class=msg></div>"
"</div>"

/* ── Tab 2: Scripts ── */
"<div class=pane id=p2>"
"<h3>Endpoint Scripts</h3>"
"<p class=info>Configure Matter endpoints via Lua scripts. Each slot is one endpoint. "
"Changes require a reboot to recreate endpoints.</p>"

"<label>Slot</label>"
"<select id=sc-slot onchange=loadSlot()>"
"<option value=0>Slot 0</option><option value=1>Slot 1</option>"
"<option value=2>Slot 2</option><option value=3>Slot 3</option>"
"<option value=4>Slot 4</option><option value=5>Slot 5</option>"
"<option value=6>Slot 6</option><option value=7>Slot 7</option>"
"</select>"

"<label>Name</label><input id=sc-name placeholder='e.g. Pushbutton Toggle'>"

"<label>Endpoint Type</label>"
"<select id=sc-type>"
"<option value=0>None (disabled)</option>"
"<option value=1>OnOff Toggle + Dim + Color (client)</option>"
"<option value=2>OnOff State-Follow (client)</option>"
"<option value=4>Occupancy Sensor</option>"
"<option value=5>Illuminance Sensor</option>"
"<option value=6>Temperature Sensor</option>"
"<option value=7>Relay (OnOff Light server)</option>"
"</select>"

"<label>Trigger</label>"
"<select id=sc-trig>"
"<option value=0>Periodic</option>"
"<option value=1>On input change</option>"
"<option value=2>On button event</option>"
"</select>"

"<label>Period (ms, for periodic trigger)</label>"
"<input id=sc-period type=number value=500 min=50 max=60000>"

"<label>Lua Script</label>"
"<textarea id=sc-code style='width:100%;height:200px;font-family:monospace;font-size:.9em;"
"  tab-size:2;white-space:pre;border:1px solid #ccc;border-radius:3px;padding:.5em'"
"  placeholder='-- Write your Lua script here&#10;function run()&#10;  local evt = input.button_event()&#10;  if evt == \"short_press\" then&#10;    endpoint.command(\"toggle\")&#10;  end&#10;end'></textarea>"

"<div style='margin:1em 0'>"
"<button class='btn btn-green' onclick=saveSlot()>Save Slot</button>"
"<button class='btn btn-red' onclick=clearSlot()>Clear Slot</button>"
"<button class='btn btn-gray' onclick=loadSlot()>Reload</button>"
"</div>"
"<div id=sc-msg class=msg></div>"
"<hr style='margin:1.5em 0'>"
"<button class='btn btn-red' onclick=doScriptReboot()>Reboot Device</button>"
"<p class=info>Reboot to apply endpoint type changes.</p>"
"</div>"

/* ── Tab 3: Backup / Restore ── */
"<div class=pane id=p3>"
"<h3>Backup &amp; Restore</h3>"
"<p class=info>Download all saved settings as a backup file, or restore "
"from a previously saved backup.</p>"
"<button class='btn btn-blue' onclick=doBackup()>Download Backup</button>"
"<h3>Restore from file</h3>"
"<input type=file id=restfile accept='.json'>"
"<button class='btn btn-green' onclick=doRestore()>Restore Backup</button>"
"<div id=bk-msg class=msg></div>"
"</div>"

"<script>"
/* Tab switching */
"function showTab(n){"
"  document.querySelectorAll('.tab').forEach(function(t,i){t.className=i==n?'tab act':'tab'});"
"  document.querySelectorAll('.pane').forEach(function(p,i){p.className=i==n?'pane act':'pane'});"
"  if(n==1){loadHW();startHWTimer()}else{stopHWTimer()}"
"  if(n==2){loadSlot()}"
"}"

/* Load saved settings on page load */
"function loadSettings(){"
"  var x=new XMLHttpRequest();"
"  x.onload=function(){"
"    if(x.status==200){"
"      var d=JSON.parse(x.responseText);"
"      document.getElementById('ssid').value=d.ssid||'';"
"      document.getElementById('pass').value=d.pass||'';"
"      document.getElementById('url').value=d.url||'';"
"    }"
"  };"
"  x.open('GET','/api/settings');x.send();"
"}"
"loadSettings();"

/* WiFi save & restart */
"function doSaveRestart(){"
"  var b='ssid='+encodeURIComponent(document.getElementById('ssid').value)"
"    +'&pass='+encodeURIComponent(document.getElementById('pass').value)"
"    +'&url='+encodeURIComponent(document.getElementById('url').value);"
"  var x=new XMLHttpRequest();"
"  x.onload=function(){document.getElementById('wifi-msg').innerHTML="
"    '<span class=ok>Saved! Restarting...</span>'};"
"  x.onerror=function(){document.getElementById('wifi-msg').innerHTML="
"    '<span class=ok>Saved! Restarting...</span>'};"
"  x.open('POST','/ota');x.setRequestHeader('Content-Type','application/x-www-form-urlencoded');"
"  x.send(b);"
"}"

/* Restart without saving */
"function doRestart(){"
"  if(!confirm('Restart device without saving?'))return;"
"  var x=new XMLHttpRequest();"
"  x.onload=function(){document.getElementById('wifi-msg').innerHTML="
"    '<span class=ok>Restarting...</span>'};"
"  x.open('POST','/api/restart');x.send();"
"}"

/* Factory reset */
"function doFactory(){"
"  if(!confirm('WARNING: This erases ALL settings and Matter pairing data. Continue?'))return;"
"  if(!confirm('Are you really sure? This cannot be undone!'))return;"
"  var x=new XMLHttpRequest();"
"  x.onload=function(){document.getElementById('wifi-msg').innerHTML="
"    '<span class=ok>Factory reset done. Restarting...</span>'};"
"  x.open('POST','/api/factory-reset');x.send();"
"}"

/* Firmware upload */
"function doUpload(){"
"  var f=document.getElementById('binfile').files[0];"
"  if(!f){alert('Select a .bin file first');return;}"
"  var btn=document.getElementById('flashbtn');"
"  var wrap=document.getElementById('bar-wrap');"
"  var bar=document.getElementById('bar');"
"  var lbl=document.getElementById('bar-lbl');"
"  var st=document.getElementById('flash-msg');"
"  btn.disabled=true;wrap.style.display='block';st.textContent='';st.className='msg';"
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
"      lbl.textContent='Upload complete';"
"      st.innerHTML='<span class=ok>Flashed! Restarting...</span>';"
"    }else{"
"      st.innerHTML='<span class=err>Error: '+xhr.responseText+'</span>';"
"      btn.disabled=false;"
"    }"
"  };"
"  xhr.onerror=function(){"
"    st.innerHTML='<span class=err>Connection lost</span>';btn.disabled=false;};"
"  xhr.open('POST','/upload');"
"  xhr.setRequestHeader('Content-Type','application/octet-stream');"
"  xhr.send(f);"
"}"

/* Hardware readout + auto-refresh */
"var hwTimer=null;"
"function setHW(id,v){var e=document.getElementById(id);if(e)e.textContent=v||'N/A'}"
"function loadHW(){"
"  document.getElementById('hw-msg').innerHTML='Loading...';"
"  var x=new XMLHttpRequest();"
"  x.onload=function(){"
"    if(x.status==200){"
"      var d=JSON.parse(x.responseText);"
"      setHW('hw-fw',d.firmware);setHW('hw-chip',d.chip);"
"      setHW('hw-mac',d.mac);setHW('hw-wifi',d.wifi_mode);"
"      setHW('hw-rssi',d.wifi_rssi);setHW('hw-up',d.uptime);"
"      setHW('hw-heap',d.free_heap);setHW('hw-ctemp',d.chip_temp);"
"      setHW('hw-rst',d.reset_reason);"
"      var be=document.getElementById('hw-bench');"
"      if(be){be.childNodes[0].textContent=d.bench_mode||'N/A';}"
"      setHW('hw-btn',d.pushbutton);setHW('hw-pcb',d.pcb_button);"
"      setHW('hw-dig',d.digital_in);setHW('hw-ana',d.analog_in);"
"      setHW('hw-temp',d.temperature);"
"      document.getElementById('hw-msg').textContent='';"
"    }else{document.getElementById('hw-msg').innerHTML='<span class=err>Read failed</span>'}"
"  };"
"  x.onerror=function(){document.getElementById('hw-msg').innerHTML='<span class=err>Connection error</span>'};"
"  x.open('GET','/api/hardware');x.send();"
"}"
"function startHWTimer(){if(!hwTimer)hwTimer=setInterval(loadHW,5000)}"
"function stopHWTimer(){if(hwTimer){clearInterval(hwTimer);hwTimer=null}}"
"function toggleBench(){"
"  if(!confirm('Toggle bench mode? Device will restart.'))return;"
"  var x=new XMLHttpRequest();"
"  x.onload=function(){"
"    if(x.status==200){document.getElementById('hw-msg').innerHTML="
"      '<span class=ok>Bench mode changed. Restarting...</span>'}"
"    else{document.getElementById('hw-msg').innerHTML="
"      '<span class=err>Failed to toggle bench mode</span>'}"
"  };"
"  x.onerror=function(){document.getElementById('hw-msg').innerHTML="
"    '<span class=err>Connection error</span>'};"
"  x.open('POST','/api/bench-mode');x.send();"
"}"

/* Scripts */
"function loadSlot(){"
"  var s=document.getElementById('sc-slot').value;"
"  var x=new XMLHttpRequest();"
"  x.onload=function(){"
"    if(x.status==200){"
"      var d=JSON.parse(x.responseText);"
"      document.getElementById('sc-name').value=d.name||'';"
"      document.getElementById('sc-type').value=d.type||0;"
"      document.getElementById('sc-trig').value=d.trigger||0;"
"      document.getElementById('sc-period').value=d.period_ms||500;"
"      document.getElementById('sc-code').value=d.script||'';"
"      document.getElementById('sc-msg').textContent='';"
"    }else{document.getElementById('sc-msg').innerHTML='<span class=err>Load failed</span>'}"
"  };"
"  x.open('GET','/api/script?slot='+s);x.send();"
"}"

"function saveSlot(){"
"  var d={slot:parseInt(document.getElementById('sc-slot').value),"
"    name:document.getElementById('sc-name').value,"
"    type:parseInt(document.getElementById('sc-type').value),"
"    trigger:parseInt(document.getElementById('sc-trig').value),"
"    period_ms:parseInt(document.getElementById('sc-period').value),"
"    script:document.getElementById('sc-code').value};"
"  var x=new XMLHttpRequest();"
"  x.onload=function(){"
"    if(x.status==200){document.getElementById('sc-msg').innerHTML="
"      '<span class=ok>Saved! Reboot to apply endpoint changes.</span>'}"
"    else{document.getElementById('sc-msg').innerHTML="
"      '<span class=err>Error: '+x.responseText+'</span>'}"
"  };"
"  x.open('POST','/api/script');"
"  x.setRequestHeader('Content-Type','application/json');"
"  x.send(JSON.stringify(d));"
"}"

"function clearSlot(){"
"  if(!confirm('Clear this slot? Endpoint will be removed on reboot.'))return;"
"  var s=document.getElementById('sc-slot').value;"
"  var x=new XMLHttpRequest();"
"  x.onload=function(){"
"    if(x.status==200){"
"      document.getElementById('sc-name').value='';"
"      document.getElementById('sc-type').value=0;"
"      document.getElementById('sc-code').value='';"
"      document.getElementById('sc-msg').innerHTML='<span class=ok>Slot cleared</span>';"
"    }"
"  };"
"  x.open('DELETE','/api/script?slot='+s);x.send();"
"}"

"function doScriptReboot(){"
"  if(!confirm('Reboot device now?'))return;"
"  var x=new XMLHttpRequest();"
"  x.open('POST','/api/restart');x.send();"
"  document.getElementById('sc-msg').innerHTML='<span class=ok>Rebooting...</span>';"
"}"

/* Backup */
"function doBackup(){window.location='/api/backup'}"

/* Restore */
"function doRestore(){"
"  var f=document.getElementById('restfile').files[0];"
"  if(!f){alert('Select a backup file first');return;}"
"  var r=new FileReader();"
"  r.onload=function(){"
"    var x=new XMLHttpRequest();"
"    x.onload=function(){"
"      if(x.status==200){document.getElementById('bk-msg').innerHTML="
"        '<span class=ok>Restored! Restarting...</span>'}"
"      else{document.getElementById('bk-msg').innerHTML="
"        '<span class=err>Error: '+x.responseText+'</span>'}"
"    };"
"    x.open('POST','/api/restore');"
"    x.setRequestHeader('Content-Type','application/json');"
"    x.send(r.result);"
"  };"
"  r.readAsText(f);"
"}"
"</script></body></html>";

/* ---------- HTTP handlers ---------- */

static esp_err_t form_get(httpd_req_t *req)
{
    return httpd_resp_send(req, MGMT_HTML, HTTPD_RESP_USE_STRLEN);
}

/* /api/settings — return current NVS WiFi creds as JSON,
 * falling back to compile-time defaults when NVS is empty. */
static esp_err_t api_settings_get(httpd_req_t *req)
{
    char ssid[33] = {0}, pass[65] = {0}, url[256] = {0};
    bool have = ota_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass),
                                     url, sizeof(url));
#ifdef DEFAULT_WIFI_SSID
    if (!have) {
        strncpy(ssid, DEFAULT_WIFI_SSID, sizeof(ssid) - 1);
#ifdef DEFAULT_WIFI_PASS
        strncpy(pass, DEFAULT_WIFI_PASS, sizeof(pass) - 1);
#endif
#ifdef DEFAULT_OTA_URL
        strncpy(url, DEFAULT_OTA_URL, sizeof(url) - 1);
#endif
    }
#endif
    static char json[512];
    snprintf(json, sizeof(json),
        "{\"ssid\":\"%s\",\"pass\":\"%s\",\"url\":\"%s\"}",
        ssid, pass, url);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* reset reason to human-readable string */
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:  return "Power-on";
        case ESP_RST_SW:      return "Software";
        case ESP_RST_PANIC:   return "Panic/exception";
        case ESP_RST_INT_WDT: return "Interrupt watchdog";
        case ESP_RST_TASK_WDT:return "Task watchdog";
        case ESP_RST_WDT:     return "Other watchdog";
        case ESP_RST_DEEPSLEEP:return "Deep sleep";
        case ESP_RST_BROWNOUT:return "Brownout";
        case ESP_RST_SDIO:    return "SDIO";
        default:              return "Unknown";
    }
}

/* /api/hardware — read sensors and system info, return JSON */
static esp_err_t api_hardware_get(httpd_req_t *req)
{
    static char json[1536];
    int pos = 0;

    /* ── System info ── */

    /* Firmware version */
    const esp_app_desc_t *app = esp_app_get_description();

    /* Chip info */
    esp_chip_info_t ci;
    esp_chip_info(&ci);

    /* MAC address */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    /* WiFi mode + RSSI */
    wifi_mode_t wmode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&wmode);
    const char *wmode_str = (wmode == WIFI_MODE_STA) ? "STA" :
                            (wmode == WIFI_MODE_AP)  ? "SoftAP" : "N/A";
    char rssi_str[24] = "N/A (SoftAP)";
    if (wmode == WIFI_MODE_STA) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(rssi_str, sizeof(rssi_str), "%d dBm", ap.rssi);
        }
    }

    /* Uptime */
    int64_t up_us = esp_timer_get_time();
    int up_s  = (int)(up_us / 1000000);
    int up_h  = up_s / 3600;
    int up_m  = (up_s % 3600) / 60;
    int up_ss = up_s % 60;

    /* Free heap */
    uint32_t heap = esp_get_free_heap_size();

    /* Internal chip temperature */
    char ctemp_str[32] = "N/A";
    {
        temperature_sensor_handle_t tsens = NULL;
        temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
        if (temperature_sensor_install(&tc, &tsens) == ESP_OK) {
            temperature_sensor_enable(tsens);
            float t;
            if (temperature_sensor_get_celsius(tsens, &t) == ESP_OK) {
                snprintf(ctemp_str, sizeof(ctemp_str), "%.1f C", t);
            }
            temperature_sensor_disable(tsens);
            temperature_sensor_uninstall(tsens);
        }
    }

    /* Reset reason */
    const char *rst = reset_reason_str(esp_reset_reason());

    /* ── Inputs ── */

    /* Pushbutton — GPIO10 */
    int btn_level = gpio_get_level(PIN_SWITCH_INPUT);
    int btn_active = g_bench_mode ? !btn_level : btn_level;

    /* PCB button — GPIO4 (always active-low with internal pull-up) */
    int pcb_level = gpio_get_level(4);
    int pcb_active = !pcb_level;

    /* Digital IN — GPIO18 (TTP223 touch) */
    int dig_level = gpio_get_level(PIN_TOUCH_INPUT);

    /* Analog IN — GPIO17 duty cycle
     * In bench_mode GPIO9/16/17 are UART0 pins — do NOT reconfigure them
     * or serial output dies. */
    char ana_str[32];
    char temp_str[32] = "N/A (bench mode)";
    if (g_bench_mode) {
        snprintf(ana_str, sizeof(ana_str), "N/A (bench mode)");
    } else {
        {
            uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
            uart_driver_delete(UART_NUM_0);
            periph_module_disable(PERIPH_UART0_MODULE);
            gpio_reset_pin(PIN_LD2410_INPUT);
            gpio_config_t cfg = {
                .pin_bit_mask = (1ULL << PIN_LD2410_INPUT),
                .mode         = GPIO_MODE_INPUT,
                .pull_down_en = GPIO_PULLDOWN_ENABLE,
            };
            gpio_config(&cfg);
            int high = 0, total = 0;
            for (int us = 0; us < 100000; us += 100) {
                if (gpio_get_level(PIN_LD2410_INPUT)) high++;
                total++;
                esp_rom_delay_us(100);
            }
            int duty = (high * 100) / total;
            snprintf(ana_str, sizeof(ana_str), "%d%% duty (%s)",
                     duty, duty >= 25 ? "occupied" : "clear");
        }

        /* ── Sensors ── */

        /* Temperature — DS18B20 via 1-Wire */
        strcpy(temp_str, "sensor not found");
        {
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

            if (ota_ow_reset()) {
                ota_ow_write_byte(0xCC);
                ota_ow_write_byte(0x44);
                vTaskDelay(pdMS_TO_TICKS(820));
                if (ota_ow_reset()) {
                    ota_ow_write_byte(0xCC);
                    ota_ow_write_byte(0xBE);
                    uint8_t sc[9];
                    for (int i = 0; i < 9; i++) sc[i] = ota_ow_read_byte();
                    int16_t raw = (int16_t)((sc[1] << 8) | sc[0]);
                    int16_t centi = (int16_t)(((int32_t)raw * 100) / 16);
                    int deg  = centi / 100;
                    int frac = centi < 0 ? -(centi % 100) : (centi % 100);
                    if (frac < 0) frac = -frac;
                    if (raw != 0x0550 && centi > -5500 && centi < 12500) {
                        snprintf(temp_str, sizeof(temp_str), "%d.%02d C", deg, frac);
                    } else if (raw == 0x0550) {
                        snprintf(temp_str, sizeof(temp_str), "85.00 C (power-on default)");
                    } else {
                        snprintf(temp_str, sizeof(temp_str), "error (%d.%02d C)", deg, frac);
                    }
                }
            }
        }
    }

    /* ── Build JSON response ── */
    pos = snprintf(json, sizeof(json),
        "{\"firmware\":\"%s (%s %s)\","
        "\"chip\":\"ESP32-C6 rev %d, %d core(s)\","
        "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"wifi_mode\":\"%s\","
        "\"wifi_rssi\":\"%s\","
        "\"uptime\":\"%dh %02dm %02ds\","
        "\"free_heap\":\"%lu bytes\","
        "\"chip_temp\":\"%s\","
        "\"reset_reason\":\"%s\","
        "\"bench_mode\":\"%s\","
        "\"pushbutton\":\"%s (GPIO%d=%d)\","
        "\"pcb_button\":\"%s (GPIO4=%d)\","
        "\"digital_in\":\"%s (GPIO%d=%d)\","
        "\"analog_in\":\"%s\","
        "\"temperature\":\"%s\"}",
        app->version, app->date, app->time,
        ci.revision, ci.cores,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        wmode_str,
        rssi_str,
        up_h, up_m, up_ss,
        (unsigned long)heap,
        ctemp_str,
        rst,
        g_bench_mode ? "ON" : "OFF",
        btn_active ? "PRESSED" : "RELEASED", PIN_SWITCH_INPUT, btn_level,
        pcb_active ? "PRESSED" : "RELEASED", pcb_level,
        dig_level ? "HIGH" : "LOW", PIN_TOUCH_INPUT, dig_level,
        ana_str,
        temp_str);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, pos);
}

/* /api/bench-mode — toggle bench mode, save to NVS and reboot */
static esp_err_t api_bench_mode_post(httpd_req_t *req)
{
    int new_val = g_bench_mode ? 0 : 1;
    bench_mode_save(new_val);
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* /api/restart — reboot without saving */
static esp_err_t api_restart_post(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* /api/factory-reset — erase NVS + Matter data and reboot */
static esp_err_t api_factory_reset_post(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "OK");
    ESP_LOGW(TAG, "Factory reset via web: wiping nvs + chip_kvs");
    nvs_flash_erase_partition("chip_kvs");
    nvs_flash_deinit();
    nvs_flash_erase();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* /api/backup — export NVS settings as JSON download */
static esp_err_t api_backup_get(httpd_req_t *req)
{
    char ssid[33] = {0}, pass[65] = {0}, url[256] = {0};
    ota_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass),
                         url, sizeof(url));

    static char json[512];
    snprintf(json, sizeof(json),
        "{\"version\":1,"
        "\"ota\":{\"ssid\":\"%s\",\"pass\":\"%s\",\"url\":\"%s\"}}",
        ssid, pass, url);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"shelly1gen4_backup.json\"");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* /api/restore — import JSON backup and reboot */
static esp_err_t api_restore_post(httpd_req_t *req)
{
    char body[512] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }

    /* Simple JSON extraction (no external parser needed) */
    char ssid[33] = {0}, pass[65] = {0}, url_buf[256] = {0};

    /* Extract values between quotes after key names */
    const char *p;
    p = strstr(body, "\"ssid\":\"");
    if (p) { p += 8; const char *e = strchr(p, '"'); if (e) { size_t n = e-p; if (n > 32) n = 32; memcpy(ssid, p, n); } }
    p = strstr(body, "\"pass\":\"");
    if (p) { p += 8; const char *e = strchr(p, '"'); if (e) { size_t n = e-p; if (n > 64) n = 64; memcpy(pass, p, n); } }
    p = strstr(body, "\"url\":\"");
    if (p) { p += 7; const char *e = strchr(p, '"'); if (e) { size_t n = e-p; if (n > 255) n = 255; memcpy(url_buf, p, n); } }

    if (!ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no ssid in backup");
        return ESP_FAIL;
    }

    ota_save_credentials(ssid, pass, url_buf);
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
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
    if (!ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    ota_save_credentials(ssid, pass, url);
    const char *msg = "Saved. Device restarting into OTA mode on your WiFi...\n";
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

/* ---------- Script API handlers ---------- */

#include "script_engine.h"

/* GET /api/script?slot=N — return slot config as JSON */
static esp_err_t api_script_get(httpd_req_t *req)
{
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing slot param");
        return ESP_FAIL;
    }
    char val[4] = {0};
    httpd_query_key_value(query, "slot", val, sizeof(val));
    int slot = atoi(val);
    if (slot < 0 || slot >= SCRIPT_MAX_SLOTS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
        return ESP_FAIL;
    }

    script_slot_config_t cfg;
    script_slot_get((uint8_t)slot, &cfg);

    /* Build JSON — need to escape script content */
    static char json[3072];
    int pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos,
        "{\"slot\":%d,\"type\":%d,\"trigger\":%d,\"period_ms\":%u,\"name\":\"",
        slot, (int)cfg.type, (int)cfg.trigger, cfg.period_ms);

    /* Escape name */
    for (const char *p = cfg.name; *p && pos < (int)sizeof(json) - 10; p++) {
        if (*p == '"' || *p == '\\') json[pos++] = '\\';
        json[pos++] = *p;
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "\",\"script\":\"");

    /* Escape script */
    for (const char *p = cfg.script; *p && pos < (int)sizeof(json) - 10; p++) {
        if (*p == '"' || *p == '\\') { json[pos++] = '\\'; json[pos++] = *p; }
        else if (*p == '\n') { json[pos++] = '\\'; json[pos++] = 'n'; }
        else if (*p == '\r') { json[pos++] = '\\'; json[pos++] = 'r'; }
        else if (*p == '\t') { json[pos++] = '\\'; json[pos++] = 't'; }
        else json[pos++] = *p;
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "\"}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, pos);
}

/* POST /api/script — save slot config from JSON body */
static esp_err_t api_script_post(httpd_req_t *req)
{
    static char body[3072];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    /* Minimal JSON parsing — extract fields */
    script_slot_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int slot = 0;

    /* Parse slot */
    char *p = strstr(body, "\"slot\":");
    if (p) slot = atoi(p + 7);
    if (slot < 0 || slot >= SCRIPT_MAX_SLOTS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
        return ESP_FAIL;
    }

    /* Parse type */
    p = strstr(body, "\"type\":");
    if (p) cfg.type = (script_slot_type_t)atoi(p + 7);

    /* Parse trigger */
    p = strstr(body, "\"trigger\":");
    if (p) cfg.trigger = (script_trigger_t)atoi(p + 10);

    /* Parse period_ms */
    p = strstr(body, "\"period_ms\":");
    if (p) cfg.period_ms = (uint16_t)atoi(p + 12);

    /* Parse name (find "name":"...") */
    p = strstr(body, "\"name\":\"");
    if (p) {
        p += 8;
        int i = 0;
        while (*p && *p != '"' && i < SCRIPT_NAME_LEN - 1) {
            if (*p == '\\' && *(p+1)) { p++; }
            cfg.name[i++] = *p++;
        }
        cfg.name[i] = '\0';
    }

    /* Parse script (find "script":"...") — handles escaped chars */
    p = strstr(body, "\"script\":\"");
    if (p) {
        p += 10;
        int i = 0;
        while (*p && i < SCRIPT_MAX_SIZE - 1) {
            if (*p == '\\') {
                p++;
                if (*p == 'n') cfg.script[i++] = '\n';
                else if (*p == 'r') cfg.script[i++] = '\r';
                else if (*p == 't') cfg.script[i++] = '\t';
                else if (*p == '"') cfg.script[i++] = '"';
                else if (*p == '\\') cfg.script[i++] = '\\';
                else cfg.script[i++] = *p;
                p++;
            } else if (*p == '"') {
                break;
            } else {
                cfg.script[i++] = *p++;
            }
        }
        cfg.script[i] = '\0';
    }

    esp_err_t err = script_slot_set((uint8_t)slot, &cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* DELETE /api/script?slot=N — clear a slot */
static esp_err_t api_script_delete(httpd_req_t *req)
{
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing slot param");
        return ESP_FAIL;
    }
    char val[4] = {0};
    httpd_query_key_value(query, "slot", val, sizeof(val));
    int slot = atoi(val);
    if (slot < 0 || slot >= SCRIPT_MAX_SLOTS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
        return ESP_FAIL;
    }

    script_slot_clear((uint8_t)slot);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* ---------- HTTP server (shared by SoftAP and STA modes) ---------- */

#define OTA_TIMEOUT_MS   (10 * 60 * 1000)   /* 10 minutes */
#define OTA_TICK_MS      5000                /* log every 5 seconds */

static void start_httpd(void)
{
    httpd_handle_t srv = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size         = 8192; /* Lua compile in script API needs more stack */
    hc.recv_wait_timeout  = 30;   /* seconds waiting for data */
    hc.send_wait_timeout  = 10;
    hc.max_open_sockets   = 3;
    hc.max_uri_handlers   = 14;
    ESP_ERROR_CHECK(httpd_start(&srv, &hc));

    httpd_uri_t get_root          = { "/",                  HTTP_GET,    form_get,              NULL };
    httpd_uri_t post_upload       = { "/upload",            HTTP_POST,   upload_post,           NULL };
    httpd_uri_t post_ota          = { "/ota",               HTTP_POST,   ota_post,              NULL };
    httpd_uri_t get_settings      = { "/api/settings",      HTTP_GET,    api_settings_get,      NULL };
    httpd_uri_t get_hardware      = { "/api/hardware",      HTTP_GET,    api_hardware_get,      NULL };
    httpd_uri_t post_bench        = { "/api/bench-mode",    HTTP_POST,   api_bench_mode_post,   NULL };
    httpd_uri_t post_restart      = { "/api/restart",       HTTP_POST,   api_restart_post,      NULL };
    httpd_uri_t post_factory      = { "/api/factory-reset", HTTP_POST,   api_factory_reset_post,NULL };
    httpd_uri_t get_backup        = { "/api/backup",        HTTP_GET,    api_backup_get,        NULL };
    httpd_uri_t post_restore      = { "/api/restore",       HTTP_POST,   api_restore_post,      NULL };
    httpd_uri_t get_script        = { "/api/script",        HTTP_GET,    api_script_get,        NULL };
    httpd_uri_t post_script       = { "/api/script",        HTTP_POST,   api_script_post,       NULL };
    httpd_uri_t del_script        = { "/api/script",        HTTP_DELETE, api_script_delete,     NULL };

    httpd_register_uri_handler(srv, &get_root);
    httpd_register_uri_handler(srv, &post_upload);
    httpd_register_uri_handler(srv, &post_ota);
    httpd_register_uri_handler(srv, &get_settings);
    httpd_register_uri_handler(srv, &get_hardware);
    httpd_register_uri_handler(srv, &post_bench);
    httpd_register_uri_handler(srv, &post_restart);
    httpd_register_uri_handler(srv, &post_factory);
    httpd_register_uri_handler(srv, &get_backup);
    httpd_register_uri_handler(srv, &post_restore);
    httpd_register_uri_handler(srv, &get_script);
    httpd_register_uri_handler(srv, &post_script);
    httpd_register_uri_handler(srv, &del_script);
}

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

    start_httpd();

    ESP_LOGW(TAG, "SoftAP ready. Connect to '%s', open http://192.168.4.1/", ssid);

    wait_for_upload_or_timeout();  /* never returns (reboots) */
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
    ESP_LOGI(TAG, "OTA mode: input=%d evt=%d ignored", id, evt);
}

/* ---------- Runtime WiFi enable (alongside Thread/Matter) ---------- */

/* Build the AP SSID from the softAP MAC address */
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

    /* Initialize netif + WiFi (guard against duplicate netif creation) */
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

    /* Always configure and start AP immediately so it is always reachable */
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
        /* APSTA mode: AP broadcasts immediately, STA connects in background */
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
        /* AP-only mode: no credentials to try */
        ESP_LOGW(TAG, "wifi_runtime: no credentials, AP-only mode '%s'", ap_ssid);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apc));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGW(TAG, "wifi_runtime: AP ready '%s', http://192.168.4.1/", ap_ssid);
    }

    start_httpd();
    vTaskDelete(NULL);
}

static bool s_wifi_runtime_started = false;

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
        return;  /* normal boot -> Matter */
    }
    ESP_LOGW(TAG, "OTA pending — entering DEDICATED OTA-mode (Matter NOT started)");

    button_driver_init(ota_mode_button_cb);
    status_led_set(STATUS_LED_FAST_BLINK);

    char ssid[33] = {0}, pass[65] = {0}, url[256] = {0};
    bool have_creds = ota_load_credentials(ssid, sizeof(ssid),
                                           pass, sizeof(pass),
                                           url,  sizeof(url));

#ifdef DEFAULT_WIFI_SSID
    /* Auto-save compile-time defaults on first boot (when NVS is empty) */
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
            /* STA connected — start HTTP server so user can reach the
             * OTA web interface on the router-assigned IP address */
            start_httpd();
            ESP_LOGW(TAG, "STA connected. OTA web interface available on local IP");

            if (url[0] && do_ota_from_url(url) == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
            /* URL fetch failed or no URL — keep HTTP server running
             * so user can upload manually via browser */
            ESP_LOGW(TAG, "URL fetch skipped/failed, waiting for manual upload...");
            wait_for_upload_or_timeout();  /* never returns (reboots) */
        }
        ESP_LOGW(TAG, "STA connection failed, falling back to SoftAP");
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
