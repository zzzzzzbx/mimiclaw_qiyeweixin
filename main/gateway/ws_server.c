#include "ws_server.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "cJSON.h"

static const char *TAG = "ws";

static httpd_handle_t s_server = NULL;

/* Simple client tracking */
typedef struct {
    int fd;
    char chat_id[32];
    bool active;
} ws_client_t;

static ws_client_t s_clients[MIMI_WS_MAX_CLIENTS];

static const char *s_ui_html =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MimiClaw Web UI</title>"
    "<style>"
    "body{font-family:ui-monospace,Consolas,monospace;background:#f4f7fb;color:#102a43;margin:0;padding:16px;}"
    ".box{max-width:900px;margin:0 auto;background:#fff;border:1px solid #d9e2ec;border-radius:8px;padding:16px;}"
    ".row{display:flex;gap:8px;margin-bottom:8px;flex-wrap:wrap;}"
    "input,button{padding:8px;border:1px solid #bcccdc;border-radius:6px;}"
    "button{background:#0f766e;color:#fff;border:none;cursor:pointer;}"
    "#log{height:320px;overflow:auto;background:#0b1f33;color:#d9e2ec;padding:8px;border-radius:6px;white-space:pre-wrap;}"
    "#status{font-size:13px;line-height:1.5;background:#f0f4f8;padding:8px;border-radius:6px;}"
    "</style></head><body><div class='box'>"
    "<h3>MimiClaw Browser Console</h3>"
    "<div class='row'><input id='chatId' value='browser_1' style='width:160px' placeholder='chat_id' />"
    "<input id='msg' style='flex:1;min-width:220px' placeholder='Type message...' />"
    "<button id='sendBtn'>Send</button></div>"
    "<div id='status'>Loading status...</div><br/><div id='log'></div></div>"
    "<script>"
    "const log=(t)=>{const el=document.getElementById('log');el.textContent+=t+'\\n';el.scrollTop=el.scrollHeight;};"
    "const ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/');"
    "ws.onopen=()=>log('[ws] connected');"
    "ws.onclose=()=>log('[ws] closed');"
    "ws.onmessage=(ev)=>log('[recv] '+ev.data);"
    "document.getElementById('sendBtn').onclick=()=>{"
    "const cid=document.getElementById('chatId').value||'browser_1';"
    "const content=document.getElementById('msg').value.trim();"
    "if(!content)return;"
    "const payload={type:'message',chat_id:cid,content};"
    "ws.send(JSON.stringify(payload));log('[send] '+JSON.stringify(payload));"
    "document.getElementById('msg').value='';};"
    "const refresh=async()=>{"
    "try{const r=await fetch('/api/status');const j=await r.json();"
    "document.getElementById('status').textContent="
    "'wifi='+(j.wifi_connected?'up':'down')+' ip='+j.ip+' ws_clients='+j.ws_clients+"
    "'\\nheap_internal='+j.heap_internal+' heap_psram='+j.heap_psram+' heap_total='+j.heap_total+"
    "'\\nts='+j.ts;}catch(e){document.getElementById('status').textContent='status error: '+e.message;}};"
    "setInterval(refresh,2000);refresh();"
    "</script></body></html>";

static ws_client_t *find_client_by_fd(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *find_client_by_chat_id(const char *chat_id)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && strcmp(s_clients[i].chat_id, chat_id) == 0) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *add_client(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            s_clients[i].fd = fd;
            snprintf(s_clients[i].chat_id, sizeof(s_clients[i].chat_id), "ws_%d", fd);
            s_clients[i].active = true;
            ESP_LOGI(TAG, "Client connected: %s (fd=%d)", s_clients[i].chat_id, fd);
            return &s_clients[i];
        }
    }
    ESP_LOGW(TAG, "Max clients reached, rejecting fd=%d", fd);
    return NULL;
}

static int active_client_count(void)
{
    int count = 0;
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active) {
            count++;
        }
    }
    return count;
}

static void remove_client(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            ESP_LOGI(TAG, "Client disconnected: %s", s_clients[i].chat_id);
            s_clients[i].active = false;
            return;
        }
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket handshake — register client */
        int fd = httpd_req_to_sockfd(req);
        add_client(fd);
        return ESP_OK;
    }

    /* Receive WebSocket frame */
    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    /* Get frame length */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.len == 0) return ESP_OK;

    ws_pkt.payload = calloc(1, ws_pkt.len + 1);
    if (!ws_pkt.payload) return ESP_ERR_NO_MEM;

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(ws_pkt.payload);
        return ret;
    }

    int fd = httpd_req_to_sockfd(req);
    ws_client_t *client = find_client_by_fd(fd);

    /* Parse JSON message */
    cJSON *root = cJSON_Parse((char *)ws_pkt.payload);
    free(ws_pkt.payload);

    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON from fd=%d", fd);
        return ESP_OK;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *content = cJSON_GetObjectItem(root, "content");

    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "message") == 0
        && content && cJSON_IsString(content)) {

        /* Determine chat_id */
        const char *chat_id = client ? client->chat_id : "ws_unknown";
        cJSON *cid = cJSON_GetObjectItem(root, "chat_id");
        if (cid && cJSON_IsString(cid)) {
            chat_id = cid->valuestring;
            /* Update client's chat_id if provided */
            if (client) {
                strncpy(client->chat_id, chat_id, sizeof(client->chat_id) - 1);
            }
        }

        ESP_LOGI(TAG, "WS message from %s: %.40s...", chat_id, content->valuestring);

        /* Push to inbound bus */
        mimi_msg_t msg = {0};
        strncpy(msg.channel, MIMI_CHAN_WEBSOCKET, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
        msg.content = strdup(content->valuestring);
        if (msg.content) {
            message_bus_push_inbound(&msg);
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_manager_is_connected());
    cJSON_AddStringToObject(root, "ip", wifi_manager_get_ip());
    cJSON_AddNumberToObject(root, "heap_internal", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "heap_psram", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "heap_total", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "ws_clients", (double)active_client_count());
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t ui_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, s_ui_html);
}

esp_err_t ws_server_start(void)
{
    memset(s_clients, 0, sizeof(s_clients));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = MIMI_WS_PORT;
    config.ctrl_port = MIMI_WS_PORT + 1;
    config.max_open_sockets = MIMI_WS_MAX_CLIENTS;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register WebSocket URI */
    httpd_uri_t ws_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };
    httpd_register_uri_handler(s_server, &status_uri);

    httpd_uri_t ui_uri = {
        .uri = "/ui",
        .method = HTTP_GET,
        .handler = ui_handler,
    };
    httpd_register_uri_handler(s_server, &ui_uri);

    ESP_LOGI(TAG, "WebSocket server started on port %d", MIMI_WS_PORT);
    return ESP_OK;
}

esp_err_t ws_server_send(const char *chat_id, const char *text)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;

    ws_client_t *client = find_client_by_chat_id(chat_id);
    if (!client) {
        ESP_LOGW(TAG, "No WS client with chat_id=%s", chat_id);
        return ESP_ERR_NOT_FOUND;
    }

    /* Build response JSON */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);

    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (!json_str) return ESP_ERR_NO_MEM;

    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_str,
        .len = strlen(json_str),
    };

    esp_err_t ret = httpd_ws_send_frame_async(s_server, client->fd, &ws_pkt);
    free(json_str);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send to %s: %s", chat_id, esp_err_to_name(ret));
        remove_client(client->fd);
    }

    return ret;
}

esp_err_t ws_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "WebSocket server stopped");
    }
    return ESP_OK;
}
