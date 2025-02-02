// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "global_vars.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "driver/ledc.h"
#include "cJSON.h"

#include "sdkconfig.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#define TAG ""
#else
#include "esp_log.h"
static const char *TAG = "camera_httpd";
#endif

typedef struct
{
  httpd_req_t *req;
  size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

typedef struct
{
  size_t size;   // number of values used for filtering
  size_t index;  // current value index
  size_t count;  // value count
  int sum;
  int *values;  // array to be filled with values
} ra_filter_t;

static ra_filter_t ra_filter;

static ra_filter_t *ra_filter_init(ra_filter_t *filter, size_t sample_size) {
  memset(filter, 0, sizeof(ra_filter_t));

  filter->values = (int *)malloc(sample_size * sizeof(int));
  if (!filter->values) {
    return NULL;
  }
  memset(filter->values, 0, sample_size * sizeof(int));

  filter->size = sample_size;
  return filter;
}

static int ra_filter_run(ra_filter_t *filter, int value) {
  if (!filter->values) {
    return value;
  }
  filter->sum -= filter->values[filter->index];
  filter->values[filter->index] = value;
  filter->sum += filter->values[filter->index];
  filter->index++;
  filter->index = filter->index % filter->size;
  if (filter->count < filter->size) {
    filter->count++;
  }
  return filter->sum / filter->count;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  struct timeval _timestamp;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char *part_buf[128];

  static int64_t last_frame = 0;
  if (!last_frame) {
    last_frame = esp_timer_get_time();
  }

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "10");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      ESP_LOGE(TAG, "Camera capture failed");
      res = ESP_FAIL;
    } else {
      _timestamp.tv_sec = fb->timestamp.tv_sec;
      _timestamp.tv_usec = fb->timestamp.tv_usec;
      if (fb->format != PIXFORMAT_JPEG) {
        bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!jpeg_converted) {
          ESP_LOGE(TAG, "JPEG compression failed");
          res = ESP_FAIL;
        }
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;
      }
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf((char *)part_buf, 128, _STREAM_PART, _jpg_buf_len, _timestamp.tv_sec, _timestamp.tv_usec);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if (res != ESP_OK) {
      ESP_LOGI(TAG, "res != ESP_OK : %d , break!", res);
      break;
    }
    int64_t fr_end = esp_timer_get_time();

    int64_t frame_time = fr_end - last_frame;
    last_frame = fr_end;
    frame_time /= 1000;
    uint32_t avg_frame_time = ra_filter_run(&ra_filter, frame_time);
    ESP_LOGI(TAG, "MJPG: %uB %ums (%.1ffps), AVG: %ums (%.1ffps)",
             (uint32_t)(_jpg_buf_len),
             (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time,
             avg_frame_time, 1000.0 / avg_frame_time);
  }
  ESP_LOGI(TAG, "Stream exit!");
  last_frame = 0;
  return res;
}

static esp_err_t parse_get(httpd_req_t *req, char **obuf) {
  char *buf = NULL;
  size_t buf_len = 0;

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char *)malloc(buf_len);
    if (!buf) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      *obuf = buf;
      return ESP_OK;
    }
    free(buf);
  }
  httpd_resp_send_404(req);
  return ESP_FAIL;
}

// TODO
const char index_web[] =
  "\
<html>\
  <head>\
    <title>HummingVision</title>\
    <style>\
      body {\
        text-align: center; \
        font-family: 'Courier', sans-serif; \
        background-color: #262626; \
        color: #fff; \
      }\
      h1 {\
        font-weight: bold; \
        color: #00ffff; \
      }\
      img {\
        width: 50%; \
        transform: rotate(180deg); \
        border: 2px solid #00ffff; \
        border-radius: 10px; \
        box-sizing: border-box; \
      }\
      label {\
        display: block;\
        margin: 10px 0;\
      }\
      input {\
        width: 15%;\
        margin-top: 5px;\
      }\
      input[type=\"range\"] {\
        -webkit-appearance: none;\
        appearance: none;\
        width: 50%;\
        background: transparent;\
        margin-top: 5px;\
      }\
      input[type=\"range\"]::-webkit-slider-runnable-track {\
        width: 100%;\
        height: 10px;\
        cursor: pointer;\
        background: transparent;\
        border-radius: 5px;\
        border: 1px solid #00ffff;\
      }\
      input[type=\"range\"]::-webkit-slider-thumb {\
        -webkit-appearance: none;\
        appearance: none;\
        width: 20px;\
        height: 20px;\
        border-radius: 50%;\
        background: #fff;\
        margin-top: -5px;\
      }\
      input[type=\"range\"]::-moz-range-track {\
        width: 100%;\
        height: 10px;\
        cursor: pointer;\
        background: transparent;\
        border-radius: 5px;\
        border: 1px solid #00ffff;\
      }\
      input[type=\"range\"]::-moz-range-thumb {\
        width: 20px;\
        height: 20px;\
        border-radius: 50%;\
        background: #fff;\
      }\
    </style>\
  </head>\
  <body>\
    <br>\
    <h1>HummingVision v2.0</h1>\
    <br>\
    <label for=\"sensitivity\">Sensitivity (after _ seconds of detecting motion take a photo):</label>\
    <input type=\"range\" id=\"sensitivity\" name=\"sensitivity\" min=\"0.5\" max=\"25\" step=\"0.5\" value=\"10\" oninput=\"updateText()\">\
    <br>\
    <input type=\"text\" id=\"sensitivityTextInput\" name=\"sensitivityTextInput\" value=\"10\" onchange=\"updateSlider()\">\
    <label for=\"brightness\">Brightness:</label>\
    <input type=\"range\" id=\"brightness\" name=\"brightness\" min=\"-2\" max=\"2\" step=\"1\" value=\"0\">\
    <label for=\"contrast\">Contrast:</label>\
    <input type=\"range\" id=\"contrast\" name=\"contrast\" min=\"-2\" max=\"2\" step=\"1\" value=\"0\">\
    <label for=\"exposure\">Exposure:</label>\
    <input type=\"range\" id=\"exposure\" name=\"exposure\" min=\"0\" max=\"2\" step=\"1\" value=\"0\">\
    <label for=\"saturation\">Saturation:</label>\
    <input type=\"range\" id=\"saturation\" name=\"saturation\" min=\"-2\" max=\"0\" step=\"1\" value=\"0\">\
    <br>\
    <br>\
    <br>\
    <br>\
    <img id=\"videoStream\" src=\"\"/>\
    <script>\
      function updateText() {\
          var slider = document.getElementById(\"sensitivity\");\
          var inputBox = document.getElementById(\"sensitivityTextInput\");\
          inputBox.value = slider.value;\
      }\
      function updateSlider() {\
          var slider = document.getElementById(\"sensitivity\");\
          var inputBox = document.getElementById(\"sensitivityTextInput\");\
          var value = parseFloat(inputBox.value);\
          if (!isNaN(value) && value >= parseFloat(slider.min) && value <= parseFloat(slider.max)) {\
              slider.value = value;\
              sendRequest(\"sensitivity\", value);\
          } else {\
              inputBox.value = slider.value;\
          }\
      }\
      document.addEventListener('DOMContentLoaded', function (event) {\
        var baseHost = document.location.origin;\
        const videoStream = document.getElementById('videoStream');\
        var streamUrl = baseHost + ':81';\
        videoStream.src = `${streamUrl}/stream`;\
        const sliders = document.querySelectorAll('input[type=\"range\"]');\
        fetch(`${baseHost}/settings`)\
          .then(response => {\
            if (!response.ok) {\
              console.error('HTTP error! Status: ', response.status);\
              throw new Error('Failed to fetch settings');\
            }\
            return response.json();\
          })\
          .then(data => {\
            sliders.forEach(slider => {\
              slider.value = data[slider.name];\
              if (slider.name == \"sensitivity\") {\
                updateText();\
              }\
            });\
          })\
          .catch(error => {\
            console.error('Error:', error.message);\
          });\
        sliders.forEach(slider => {\
          slider.addEventListener('input', function() {\
            sendRequest(this.name, this.value);\
          });\
        });\
        function sendRequest(variable, value) {\
          const url = `${baseHost}/control?var=${variable}&val=${value}`;\
          fetch(url)\
            .then(response => {\
              if (!response.ok) {\
                console.error('HTTP error! Status: ', response.status);\
              } else {\
                console.log('Control command executed successfully.');\
              }\
            })\
            .catch(error => {\
              console.error('Error:', error.message);\
            });\
        }\
      })\
    </script>\
  </body>\
</html>\
";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  // httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL) {
    // return httpd_resp_sendstr(req, (const char *)index_web);
    return httpd_resp_send(req, (const char *)index_web, sizeof(index_web));
    // return httpd_resp_send(req, (const char *)index_ov2640_html_gz, index_ov2640_html_gz_len);
  } else {
    ESP_LOGE(TAG, "Camera sensor not found");
    return httpd_resp_send_500(req);
  }
}

static esp_err_t settings_handler(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();
  cJSON *jsonRoot = cJSON_CreateObject();
  if (!jsonRoot) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
  }

  cJSON_AddNumberToObject(jsonRoot, "brightness", s->status.brightness);
  cJSON_AddNumberToObject(jsonRoot, "contrast", s->status.contrast);
  cJSON_AddNumberToObject(jsonRoot, "exposure", s->status.gainceiling);
  cJSON_AddNumberToObject(jsonRoot, "saturation", s->status.saturation);
  cJSON_AddNumberToObject(jsonRoot, "sensitivity", ((float) sensitivity) / 2.0);

  char *jsonStr = cJSON_Print(jsonRoot);
  cJSON_Delete(jsonRoot);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");  // Allow CORS
  httpd_resp_send(req, jsonStr, strlen(jsonStr));

  free(jsonStr);
  return ESP_OK;
}

static esp_err_t cmd_handler(httpd_req_t *req) {
    char *buf = NULL;
  char variable[32];
  char value[32];
  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK ||
    httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);


  int val = atoi(value);
  int res = 0;

  sensor_t *s = esp_camera_sensor_get();
  if (!strcmp(variable, "contrast"))
    res = s->set_contrast(s, val); // -2 to 2
  else if (!strcmp(variable, "brightness"))
    res = s->set_brightness(s, val); // -2 to 2
  else if (!strcmp(variable, "saturation"))
    res = s->set_saturation(s, val); // -2 to 2
  else if (!strcmp(variable, "exposure"))
    res = s->set_gainceiling(s, (gainceiling_t)val); // 0 to 6
  else if (!strcmp(variable, "sensitivity"))
    res = 0;
    float fval = atof(value);
    sensitivity = (int) (2.0 * fval); // 1 to inf.


  if (res < 0) {
      return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 16;

  httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL
  };

  httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
  };

  httpd_uri_t cmd_uri = {
    .uri = "/control",
    .method = HTTP_GET,
    .handler = cmd_handler,
    .user_ctx = NULL
  };

  httpd_uri_t settings_uri = {
    .uri = "/settings",
    .method = HTTP_GET,
    .handler = settings_handler,
    .user_ctx = NULL
  };

  ra_filter_init(&ra_filter, 20);

  ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &cmd_uri);
    // httpd_register_uri_handler(camera_httpd, &stream_uri);
    httpd_register_uri_handler(camera_httpd, &settings_uri);
  }

  config.server_port += 1;
  config.ctrl_port += 1;
  ESP_LOGI(TAG, "Starting stream server on port: '%d'", config.server_port);
  if (httpd_start(&stream_httpd, &config) == ESP_OK)
  {
      httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}
