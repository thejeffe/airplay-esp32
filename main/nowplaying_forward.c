#include "nowplaying_forward.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "rtsp_events.h"
#include "settings.h"

static const char *TAG = "nowplaying";

#ifndef CONFIG_NOWPLAYING_ART_MAX_KB
#define CONFIG_NOWPLAYING_ART_MAX_KB 256
#endif

#define NP_JSON_MAX  512
#define NP_QUEUE_LEN 6

typedef enum { NP_MSG_JSON, NP_MSG_ART } np_msg_kind_t;

typedef struct {
  np_msg_kind_t kind;
  char json[NP_JSON_MAX]; // NP_MSG_JSON
  uint8_t *data;          // NP_MSG_ART (owned; freed by the task)
  size_t len;
  char content_type[24];
} np_msg_t;

static QueueHandle_t s_queue;
static char s_last_title[METADATA_STRING_MAX];
static char s_last_artist[METADATA_STRING_MAX];

static bool url_set(char *out, size_t len) {
  if (settings_get_nowplaying_url(out, len) != ESP_OK) {
    return false;
  }
  // trim a trailing slash so "<url>/api/..." is well formed
  size_t n = strlen(out);
  while (n > 0 && out[n - 1] == '/') {
    out[--n] = '\0';
  }
  return n > 0;
}

static void enqueue(np_msg_t *msg) {
  if (xQueueSend(s_queue, msg, 0) != pdTRUE) {
    // Full: drop the oldest so the newest state/art wins.
    np_msg_t old;
    if (xQueueReceive(s_queue, &old, 0) == pdTRUE && old.kind == NP_MSG_ART) {
      free(old.data);
    }
    if (xQueueSend(s_queue, msg, 0) != pdTRUE && msg->kind == NP_MSG_ART) {
      free(msg->data);
    }
  }
}

static void send_json(cJSON *obj) {
  char url[160];
  if (!s_queue || !url_set(url, sizeof(url))) {
    return;
  }
  char name[65];
  settings_get_device_name(name, sizeof(name));
  cJSON_AddStringToObject(obj, "source", name);
  np_msg_t msg = {.kind = NP_MSG_JSON};
  if (!cJSON_PrintPreallocated(obj, msg.json, sizeof(msg.json), false)) {
    ESP_LOGW(TAG, "now-playing JSON too long, skipped");
    return;
  }
  enqueue(&msg);
}

static void send_state(const char *state) {
  cJSON *obj = cJSON_CreateObject();
  cJSON_AddStringToObject(obj, "state", state);
  send_json(obj);
  cJSON_Delete(obj);
}

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)user_data;
  switch (event) {
  case RTSP_EVENT_PLAYING:
    send_state("playing");
    break;
  case RTSP_EVENT_PAUSED:
    send_state("paused");
    break;
  case RTSP_EVENT_DISCONNECTED:
    send_state("stopped");
    s_last_title[0] = s_last_artist[0] = '\0';
    break;
  case RTSP_EVENT_METADATA: {
    // Progress-only updates arrive as METADATA with no title: skip those,
    // and only send when the track actually changed.
    const rtsp_metadata_t *m = data ? &data->metadata : NULL;
    if (!m || m->title[0] == '\0') {
      break;
    }
    if (strcmp(m->title, s_last_title) == 0 &&
        strcmp(m->artist, s_last_artist) == 0) {
      break;
    }
    strlcpy(s_last_title, m->title, sizeof(s_last_title));
    strlcpy(s_last_artist, m->artist, sizeof(s_last_artist));
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "title", m->title);
    cJSON_AddStringToObject(obj, "artist", m->artist);
    cJSON_AddStringToObject(obj, "album", m->album);
    send_json(obj);
    cJSON_Delete(obj);
    break;
  }
  default:
    break;
  }
}

void nowplaying_forward_art(const char *content_type, const uint8_t *data,
                            size_t len) {
  char url[160];
  if (!s_queue || !data || len == 0 || !url_set(url, sizeof(url))) {
    return;
  }
  if (len > (size_t)CONFIG_NOWPLAYING_ART_MAX_KB * 1024) {
    ESP_LOGW(TAG, "artwork %u bytes is over the %d KB limit, not forwarded",
             (unsigned)len, CONFIG_NOWPLAYING_ART_MAX_KB);
    return;
  }
  uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!copy) {
    copy = malloc(len);
  }
  if (!copy) {
    ESP_LOGW(TAG, "no memory for %u bytes of artwork", (unsigned)len);
    return;
  }
  memcpy(copy, data, len);
  np_msg_t msg = {.kind = NP_MSG_ART, .data = copy, .len = len};
  strlcpy(msg.content_type,
          (content_type && strstr(content_type, "png")) ? "image/png"
                                                        : "image/jpeg",
          sizeof(msg.content_type));
  enqueue(&msg);
}

static void post(const char *url, const char *ctype, const char *body,
                 size_t len) {
  esp_http_client_config_t cfg = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 4000,
  };
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) {
    return;
  }
  esp_http_client_set_header(c, "Content-Type", ctype);
  esp_http_client_set_post_field(c, body, (int)len);
  esp_err_t err = esp_http_client_perform(c);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "POST %s failed: %s", url, esp_err_to_name(err));
  } else {
    int status = esp_http_client_get_status_code(c);
    if (status >= 300) {
      ESP_LOGW(TAG, "POST %s -> HTTP %d", url, status);
    }
  }
  esp_http_client_cleanup(c);
}

static void forward_task(void *arg) {
  (void)arg;
  np_msg_t msg;
  char base[160], url[200];
  for (;;) {
    if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (url_set(base, sizeof(base))) {
      if (msg.kind == NP_MSG_JSON) {
        snprintf(url, sizeof(url), "%s/api/nowplaying", base);
        post(url, "application/json", msg.json, strlen(msg.json));
      } else {
        snprintf(url, sizeof(url), "%s/api/nowplaying/art?source=esp32", base);
        post(url, msg.content_type, (const char *)msg.data, msg.len);
        ESP_LOGI(TAG, "forwarded artwork (%u bytes)", (unsigned)msg.len);
      }
    }
    if (msg.kind == NP_MSG_ART) {
      free(msg.data);
    }
  }
}

void nowplaying_forward_init(void) {
  if (s_queue) {
    return;
  }
  s_queue = xQueueCreate(NP_QUEUE_LEN, sizeof(np_msg_t));
  if (!s_queue) {
    ESP_LOGE(TAG, "queue create failed");
    return;
  }
  if (xTaskCreate(forward_task, "np_fwd", 6144, NULL, 3, NULL) != pdPASS) {
    ESP_LOGE(TAG, "task create failed");
    vQueueDelete(s_queue);
    s_queue = NULL;
    return;
  }
  if (rtsp_events_register(on_rtsp_event, NULL) != 0) {
    ESP_LOGW(TAG, "no free RTSP event slot; only artwork will be forwarded");
  }
  char url[160];
  ESP_LOGI(TAG, "now-playing forward %s%s",
           url_set(url, sizeof(url)) ? "to " : "off (no URL set)",
           url_set(url, sizeof(url)) ? url : "");
}
