#include "webconfig.h"
#include <stdint.h>
#include <esp_log.h>
#include <sys/param.h>
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "string.h"
#include "cJSON.h"
#include "esp_http_client.h"

#include "store.h"

static const char *TAG = "WS";
#define SCRATCH_BUFSIZE (1024)

static esp_err_t index_get_handler(httpd_req_t *req);
static esp_err_t post_handler(httpd_req_t *req);

httpd_uri_t uri_get = {
  .uri      = "/*",
  .method   = HTTP_GET,
  .handler  = index_get_handler,
  .user_ctx = NULL
};

httpd_uri_t config_post = {
  .uri      = "/config",
  .method   = HTTP_POST,
  .handler  = post_handler,
  .user_ctx = NULL
};

httpd_handle_t start_webserver() {
  ESP_LOGI(TAG, "Starting web server");
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 32768;
  config.uri_match_fn = httpd_uri_match_wildcard;

  httpd_handle_t server = NULL;

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &uri_get);
    httpd_register_uri_handler(server, &config_post);
  }
  return server;
}

void stop_webserver(httpd_handle_t server) {
  ESP_LOGI(TAG, "Stoping web server");
  httpd_stop(server);
}

static esp_err_t index_get_handler(httpd_req_t *req) {
  if (strcmp(req->uri, "/scan") == 0) {
    uint16_t ap_num = 0;
    wifi_scan_config_t scan_config = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = false,
    };
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));
    esp_wifi_scan_get_ap_num(&ap_num);
    if(ap_num != 0){
      wifi_ap_record_t *ap_records = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t)*ap_num);
      ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_num, ap_records));
      char buf[100];
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send_chunk(req, "{", 1);
      for(int i = 0; i<ap_num; i++) {
        sprintf(buf, "%s\"%d\":{\"ssid\":\"%s\",\"rssi\":%d,\"secured\": %s}", i?",\n ":"\n ", i, ap_records[i].ssid, ap_records[i].rssi, ap_records[i].authmode ? "true":"false");
        httpd_resp_sendstr_chunk(req, buf);
      }
      httpd_resp_send_chunk(req, "\n}", 2);
      httpd_resp_send_chunk(req, NULL, 0);
      free(ap_records);
    }
  }
  else if (strcmp(req->uri, "/esp-tail.png") == 0) {
    extern const unsigned char esp_tail_png_start[] asm("_binary_esp_tail_png_start");
    extern const unsigned char esp_tail_png_end[]   asm("_binary_esp_tail_png_end");
    const size_t esp_tail_png_size = (esp_tail_png_end - esp_tail_png_start);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_send(req, (const char *)esp_tail_png_start, esp_tail_png_size);
  }
  else if (strcmp(req->uri, "/") == 0) {
    extern const unsigned char index_html_start[] asm("_binary_index_html_start");
    extern const unsigned char index_html_end[]   asm("_binary_index_html_end");
    const size_t index_html_size = (index_html_end - index_html_start);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_size);
  } else {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Page does not exist");
  }

  return ESP_OK;
}

static esp_err_t post_handler(httpd_req_t *req) {
  int total_len = req->content_len;
  int cur_len = 0;
  char buf[SCRATCH_BUFSIZE];
  int received = 0;
  if (total_len >= SCRATCH_BUFSIZE) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
    return ESP_FAIL;
  }
  while (cur_len < total_len) {
    received = httpd_req_recv(req, buf + cur_len, total_len);
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
      return ESP_FAIL;
    }
    cur_len += received;
  }

  buf[total_len] = '\0';

  cJSON *root = cJSON_Parse(buf);
  loki_cfg_t loki_cfg;
  strcpy(sta_ssid, cJSON_GetObjectItem(root, "ssid")->valuestring);
  strcpy(sta_password, cJSON_GetObjectItem(root, "key")->valuestring);
  wifi_save_settings();
  char *transport_str = cJSON_GetObjectItem(root, "lokitransport")->valuestring;
  if (!strcmp(transport_str, "tls")) loki_cfg.transport = HTTP_TRANSPORT_OVER_SSL;
  else loki_cfg.transport = HTTP_TRANSPORT_OVER_TCP;
  strcpy(loki_cfg.host, cJSON_GetObjectItem(root, "lokihost")->valuestring);
  char *port_str = cJSON_GetObjectItem(root, "lokiport")->valuestring;
  if (strcmp(port_str, "")) loki_cfg.port = atoi(port_str);
  else loki_cfg.port = loki_cfg.transport == HTTP_TRANSPORT_OVER_SSL ? 443 : 80;
  strcpy(loki_cfg.username, cJSON_GetObjectItem(root, "lokilogin")->valuestring);
  strcpy(loki_cfg.password, cJSON_GetObjectItem(root, "lokipass")->valuestring);
  strcpy(loki_cfg.name, cJSON_GetObjectItem(root, "lokiname")->valuestring);
  set_loki_config(loki_cfg);

  ESP_LOGI(TAG, "SSID: %s", sta_ssid);
  ESP_LOGI(TAG, "Loki Transport: %s", loki_cfg.transport == 2 ? "https" : "http");
  ESP_LOGI(TAG, "Loki Host: %s", loki_cfg.host);
  ESP_LOGI(TAG, "Loki Port: %d", loki_cfg.port);
  ESP_LOGI(TAG, "Loki Login: %s", loki_cfg.username);
  ESP_LOGI(TAG, "Loki Instance: %s", loki_cfg.name);

  const char resp[] = "Done. Rebooting...";
  httpd_resp_send(req, resp, strlen(resp));
  esp_restart();
  return ESP_OK;
}
