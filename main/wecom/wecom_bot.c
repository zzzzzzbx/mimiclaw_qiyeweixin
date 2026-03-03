#include "wecom_bot.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "wecom";

#define WECOM_WEBHOOK_MAX_LEN 320

static char s_webhook_url[WECOM_WEBHOOK_MAX_LEN] = MIMI_SECRET_WECOM_WEBHOOK;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (!resp) {
        return ESP_OK;
    }
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t need = resp->len + evt->data_len + 1;
        if (need > resp->cap) {
            size_t new_cap = resp->cap * 2;
            if (new_cap < need) {
                new_cap = need;
            }
            char *tmp = realloc(resp->buf, new_cap);
            if (!tmp) {
                return ESP_ERR_NO_MEM;
            }
            resp->buf = tmp;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
    }
    return ESP_OK;
}

static bool wecom_response_ok(const char *body)
{
    if (!body || body[0] == '\0') {
        return false;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return false;
    }

    cJSON *errcode = cJSON_GetObjectItem(root, "errcode");
    bool ok = cJSON_IsNumber(errcode) && (errcode->valueint == 0);
    cJSON_Delete(root);
    return ok;
}

static esp_err_t wecom_post_payload(const char *payload_json)
{
    http_resp_t resp = {
        .buf = calloc(1, 512),
        .len = 0,
        .cap = 512,
    };
    if (!resp.buf) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = s_webhook_url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = MIMI_WECOM_SEND_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload_json, strlen(payload_json));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP post failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "WeCom API status=%d body=%.200s", status, resp.buf);
        free(resp.buf);
        return ESP_FAIL;
    }

    if (!wecom_response_ok(resp.buf)) {
        ESP_LOGE(TAG, "WeCom API error body=%.200s", resp.buf);
        free(resp.buf);
        return ESP_FAIL;
    }

    free(resp.buf);
    return ESP_OK;
}

static esp_err_t wecom_send_segment(const char *chat_id, const char *segment)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "msgtype", "text");

    cJSON *text = cJSON_CreateObject();
    if (chat_id && chat_id[0] != '\0') {
        size_t prefix_len = strlen(chat_id) + 8;
        size_t seg_len = strlen(segment);
        char *merged = malloc(prefix_len + seg_len + 1);
        if (!merged) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        snprintf(merged, prefix_len + seg_len + 1, "[%s]\n%s", chat_id, segment);
        cJSON_AddStringToObject(text, "content", merged);
        free(merged);
    } else {
        cJSON_AddStringToObject(text, "content", segment);
    }

    cJSON_AddItemToObject(root, "text", text);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = wecom_post_payload(payload);
    free(payload);
    return err;
}

esp_err_t wecom_bot_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_WECOM, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[WECOM_WEBHOOK_MAX_LEN] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_WECOM_WEBHOOK, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_webhook_url, tmp, sizeof(s_webhook_url) - 1);
            s_webhook_url[sizeof(s_webhook_url) - 1] = '\0';
        }
        nvs_close(nvs);
    }

    if (s_webhook_url[0]) {
        ESP_LOGI(TAG, "WeCom webhook loaded");
    } else {
        ESP_LOGW(TAG, "No WeCom webhook. Use CLI: set_wecom_webhook <URL>");
    }
    return ESP_OK;
}

esp_err_t wecom_bot_start(void)
{
    ESP_LOGI(TAG, "WeCom bot started (outbound webhook mode)");
    return ESP_OK;
}

esp_err_t wecom_send_message(const char *chat_id, const char *text)
{
    if (!text || text[0] == '\0') {
        return ESP_OK;
    }
    if (s_webhook_url[0] == '\0') {
        ESP_LOGW(TAG, "Cannot send WeCom message: webhook not configured");
        return ESP_ERR_INVALID_STATE;
    }

    size_t text_len = strlen(text);
    size_t off = 0;
    bool all_ok = true;

    while (off < text_len) {
        size_t n = text_len - off;
        if (n > MIMI_WECOM_MAX_MSG_LEN) {
            n = MIMI_WECOM_MAX_MSG_LEN;
        }

        char *segment = malloc(n + 1);
        if (!segment) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(segment, text + off, n);
        segment[n] = '\0';

        esp_err_t err = wecom_send_segment(chat_id, segment);
        free(segment);
        if (err != ESP_OK) {
            all_ok = false;
        }

        off += n;
    }

    return all_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t wecom_set_webhook(const char *webhook_url)
{
    if (!webhook_url || webhook_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_WECOM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_WECOM_WEBHOOK, webhook_url));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_webhook_url, webhook_url, sizeof(s_webhook_url) - 1);
    s_webhook_url[sizeof(s_webhook_url) - 1] = '\0';
    ESP_LOGI(TAG, "WeCom webhook saved");
    return ESP_OK;
}
