#pragma once

#include "esp_err.h"

/**
 * Initialize WeCom bot settings from build-time defaults and NVS overrides.
 */
esp_err_t wecom_bot_init(void);

/**
 * Start WeCom bot runtime tasks.
 * Current implementation is outbound-only, so this is a no-op.
 */
esp_err_t wecom_bot_start(void);

/**
 * Send a text message to WeCom group robot webhook.
 * chat_id is optional and only used as a prefix tag in message body.
 */
esp_err_t wecom_send_message(const char *chat_id, const char *text);

/**
 * Save WeCom webhook URL to NVS.
 */
esp_err_t wecom_set_webhook(const char *webhook_url);

