/*
 * HTTP API handlers + dashboard for the Shelly management web interface.
 * Extracted from ota.c for maintainability.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the HTTP server with all management API routes registered. */
void web_api_start_httpd(void);

#ifdef __cplusplus
}
#endif
