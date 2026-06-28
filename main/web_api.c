/*
 * web_api.c - Web management dashboard + OTA + backup/restore
 * Memory-optimized restore handler
 */

#include "web_api.h"
#include "app_config.h"
#include "ota.h"
#include "script_engine.h"
#include "matter_device.h"
#include "status_led.h"
#include "sensors.h"

#include <esp_http_server.h>
#include <cJSON.h>
#include <esp_ota_ops.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "web_api";

// ==================== RESTORE HANDLER (memory optimized) ====================

static esp_err_t restore_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "=== RESTORE STARTED - Memory optimized mode ===");

    esp_task_wdt_reset();
    heap_caps_free(NULL);
    vTaskDelay(pdMS_TO_TICKS(150));

    char *buf = NULL;
    size_t buf_len = req->content_len;

    if (buf_len > 131072) {
        ESP_LOGE(TAG, "Restore JSON too large (%d bytes)", buf_len);
        return httpd_resp_send_500(req);
    }

    buf = malloc(buf_len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "Out of memory allocating restore buffer");
        return httpd_resp_send_500(req);
    }

    int ret = httpd_req_recv(req, buf, buf_len);
    if (ret <= 0) {
        free(buf);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    ESP_LOGI(TAG, "Parsing %d byte restore JSON...", ret);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return httpd_resp_send_500(req);
    }

    // Scripts één voor één
    cJSON *scripts = cJSON_GetObjectItem(root, "scripts");
    if (cJSON_IsArray(scripts)) {
        int count = cJSON_GetArraySize(scripts);
        ESP_LOGI(TAG, "Restoring %d script slots one by one...", count);

        for (int i = 0; i < count && i < 8; i++) {
            cJSON *slot = cJSON_GetArrayItem(scripts, i);
            if (slot) {
                vTaskDelay(pdMS_TO_TICKS(80));
                heap_caps_free(NULL);

                int slot_id = cJSON_GetObjectItem(slot, "slot") ? 
                              cJSON_GetObjectItem(slot, "slot")->valueint : i;

                const char *name = cJSON_GetObjectItem(slot, "name") ? 
                                   cJSON_GetObjectItem(slot, "name")->valuestring : "";

                const char *script = cJSON_GetObjectItem(slot, "script") ? 
                                     cJSON_GetObjectItem(slot, "script")->valuestring : "";

                script_engine_save(slot_id, name, script);   // <--- juiste naam

                ESP_LOGI(TAG, "Restored slot %d: %s", slot_id, name);
            }
        }
    }

    // WiFi credentials
    cJSON *ota = cJSON_GetObjectItem(root, "ota");
    if (ota) {
        cJSON *ssid = cJSON_GetObjectItem(ota, "ssid");
        cJSON *pass = cJSON_GetObjectItem(ota, "pass");
        if (ssid && pass && ssid->valuestring && pass->valuestring) {
            ota_save_credentials(ssid->valuestring, pass->valuestring, "shelly");
            ESP_LOGI(TAG, "Restored WiFi credentials for SSID: %s", ssid->valuestring);
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Restore completed successfully");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Restore successful\"}");
}

// ==================== INIT ====================

esp_err_t web_api_init(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.stack_size = 8192;

    if (httpd_start(&server, &config) == ESP_OK) {
        // Register your URI handlers here
        ESP_LOGI(TAG, "Web API server started successfully");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to start HTTP server");
    return ESP_FAIL;
}
