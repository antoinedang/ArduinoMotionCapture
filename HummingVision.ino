#include "esp_camera.h"
#include "global_vars.h"
#include <WiFi.h>
#include <ESP_Mail_Client.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <EEPROM.h>
#include <TimeLib.h>
#define EEPROM_SIZE 10
#define BOOLEAN_KEY 69
#define CAMERA_MODEL_WROVER_KIT
#define LED_BUILTIN 2
#define PIR_GPIO 13
#define SECONDARY_PIR_GPIO 12
#include "camera_pins.h"
// FOR VIDEO ENCODING
#define videoFrameLength 10  // 10 seconds * 20 FPS
#define frameSkipAmt 3
int frame_count = 0;

bool is_daytime;
int numSequentialMotionDetections = 0;
int cam_contrast;
int cam_brightness;
int cam_exposure;
int cam_saturation;
unsigned long lastDaytimeUpdateMillis = 0;
const unsigned long timeBetweenDaytimeChecks = 600000;

// FOR CAMERA
camera_config_t camera_config;
void startCameraServer();

// FOR EMAILS
#define emailAccount "visionhumming@gmail.com"
#define emailSenderPassword "kqzt zltm xtkx jmpq"
#define smtpServer "smtp.gmail.com"
#define smtpServerPort 465
SMTPSession smtp;
void smtpCallback(SMTP_Status status);
Session_Config smtp_config;

// FOR WIFI
#define ssid_Router "JKLM"
#define password_Router "9494990303"

// #define ssid_Router "BELL 2.4"
// #define password_Router "123456789"

void connectToWifi() {
  WiFi.begin(ssid_Router, password_Router);
  while (WiFi.isConnected() != true) {
    digitalWrite(LED_BUILTIN, LOW);  // turn the LED off
    delay(300);
    Serial.print(".");
    digitalWrite(LED_BUILTIN, HIGH);  // turn the LED on
    delay(300);
  }
  Serial.println("");
  Serial.println("WiFi connected");
}

void connectToEmail() {
  // Enable the debug via Serial port
  smtp.debug(0);
  smtp.callback(smtpCallback);

  /*Set the NTP config time
  For times east of the Prime Meridian use 0-12
  For times west of the Prime Meridian add 12 to the offset.
  Ex. American/Denver GMT would be -6. 6 + 12 = 18
  See https://en.wikipedia.org/wiki/Time_zone for a list of the GMT/UTC timezone offsets
  */
  smtp_config.time.ntp_server = F("pool.ntp.org,time.nist.gov");
  smtp_config.time.gmt_offset = 20;  // -8 => 8 + 12 = 20
  smtp_config.time.day_light_offset = 0;
  smtp_config.server.host_name = smtpServer;
  smtp_config.server.port = smtpServerPort;
  smtp_config.login.email = emailAccount;
  smtp_config.login.password = emailSenderPassword;
  smtp_config.login.user_domain = F("127.0.0.1");

  /* Connect to server with the session config */
  if (!smtp.connect(&smtp_config)) {
    Serial.printf("Connection error, Status Code: %d, Error Code: %d, Reason: %s\n", smtp.statusCode(), smtp.errorCode(), smtp.errorReason().c_str());
    ESP.restart();
  }
}

void sendStartupEmail(String local_IP) {
  SMTP_Message message;
  message.enable.chunking = true;
  message.sender.name = F("HummingVision");
  message.sender.email = emailAccount;
  String subject = "HummingVision Started";
  message.subject = subject;
  message.addRecipient(F("MonitoringEmail"), emailAccount);
  String text = "Your HummingVision is up and running!\nYou can see the live feed in your browser at http://" + local_IP + ".";
  message.text.content = text;
  message.html.charSet = F("utf-8");
  message.html.transfer_encoding = "base64";
  message.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_low;
  message.response.notify = esp_mail_smtp_notify_success | esp_mail_smtp_notify_failure | esp_mail_smtp_notify_delay;

  /* Start sending Email */
  if (!MailClient.sendMail(&smtp, &message, false)) {
    Serial.printf("Error, Status Code: %d, Error Code: %d, Reason: %s\n", smtp.statusCode(), smtp.errorCode(), smtp.errorReason().c_str());
    ESP.restart();
  }
}

void sendCaptureByEmail() {
  SMTP_Message message;
  message.enable.chunking = true;
  message.sender.name = F("HummingVision");
  message.sender.email = emailAccount;
  String subject = "HummingVision detected movement!";
  message.subject = subject;
  message.addRecipient(F("MonitoringEmail"), emailAccount);
  String text = "See the attached content with what caused the motion detector to trigger:";
  message.text.content = text;
  message.html.charSet = F("utf-8");
  message.html.transfer_encoding = "base64";
  message.html.transfer_encoding = Content_Transfer_Encoding::enc_qp;
  message.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_low;
  message.response.notify = esp_mail_smtp_notify_success | esp_mail_smtp_notify_failure | esp_mail_smtp_notify_delay;

  // add each image taken as an attachment
  for (int i = 0; i < frame_count; i++) {
    SMTP_Attachment att;
    att.descr.filename = String("frame" + String(i) + ".jpg").c_str();
    att.descr.mime = "image/png";
    att.file.path = String("/frame" + String(i) + ".jpg").c_str();
    att.file.storage_type = esp_mail_file_storage_type_flash;
    att.descr.transfer_encoding = Content_Transfer_Encoding::enc_base64;
    message.addAttachment(att);
  }

  frame_count = 0;

  /* Start sending Email */
  if (!MailClient.sendMail(&smtp, &message, false)) {
    Serial.printf("Error, Status Code: %d, Error Code: %d, Reason: %s\n", smtp.statusCode(), smtp.errorCode(), smtp.errorReason().c_str());
    ESP.restart();
  }
}

void saveCaptureToFileSystem() {
  frame_count = 0;
  // Dispose first pictures because of bad quality
  camera_fb_t* fb = NULL;
  // Skip first N frames (increase/decrease number as needed).
  for (int i = 0; i < frameSkipAmt; i++) {
    fb = esp_camera_fb_get();
    esp_camera_fb_return(fb);
    fb = NULL;
  }

  int failed_camera_captures = 0;
  // capture video frame by frame
  for (int i = 0; i < videoFrameLength; i++) {
    // Take a new photo
    fb = NULL;
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      esp_camera_fb_return(fb);
      fb = NULL;
      if (frame_count > 0) break;
      else return;
    }

    File file = LittleFS.open(String("/frame" + String(frame_count) + ".jpg").c_str(), FILE_WRITE);
    Serial.printf("Picture file name: %s\n", "/frame" + String(frame_count) + ".jpg");
    if (!file || LittleFS.totalBytes() - LittleFS.usedBytes() < fb->len) {
      Serial.println("Failed to open file in writing mode");
      esp_camera_fb_return(fb);
      fb = NULL;
      break;
    } else {
      Serial.printf("Writing %d bytes to file\n", (int)fb->len);
      try {
        file.write(fb->buf, fb->len);
        frame_count += 1;
      } catch (const char* errorMessage) {
        Serial.print("Exception caught in file write: ");
        Serial.println(errorMessage);
      }
    }
    Serial.println("Closing file");
    file.close();
    esp_camera_fb_return(fb);
    fb = NULL;
  }
  Serial.printf("Saved %d frames.\n\n", frame_count);
  if (frame_count == 0) ESP.restart();
}

void updateCameraSettings() {
  sensor_t* s = esp_camera_sensor_get();
  if (cam_contrast != (int)s->status.contrast) {
    cam_contrast = (int)s->status.contrast;
    EEPROM.write(2, (uint8_t)(s->status.contrast+10));
    EEPROM.commit();
  }
  if (cam_brightness != (int)s->status.brightness) {
    cam_brightness = (int)s->status.brightness;
    EEPROM.write(3, (uint8_t)(s->status.brightness+10));
    EEPROM.commit();
  }
  if (cam_saturation != (int)s->status.saturation) {
    cam_saturation = (int)s->status.saturation;
    EEPROM.write(4, (uint8_t)(s->status.saturation+10));
    EEPROM.commit();
  }
  if (cam_exposure != (int)s->status.gainceiling) {
    cam_exposure = (int)s->status.gainceiling;
    EEPROM.write(5, (uint8_t)s->status.gainceiling);
    EEPROM.commit();
  }
}

bool isDaytime() {
  HTTPClient http;
  String serverPath = "https://api.openweathermap.org/data/2.5/weather?lat=49.4593&lon=-123.2341&appid=4b09e2ce0f29629d4f92f50fc88d3cdd";
  http.begin(serverPath.c_str());
  int httpResponseCode = http.GET();
  if (httpResponseCode > 0) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      Serial.print("Failed to parse weather JSON: ");
      Serial.println(error.c_str());
      return is_daytime;
    }
    long currentDt = doc["dt"].as<long>();
    long currentSunrise = doc["sys"]["sunrise"].as<long>();
    long currentSunset = doc["sys"]["sunset"].as<long>();
    
    int currenttime_hour = hour(currentDt)-7;
    if (currenttime_hour < 0) currenttime_hour += 24;
    int sunrise_hour = hour(currentSunrise)-8;
    if (sunrise_hour < 0) sunrise_hour += 24;
    int sunset_hour = hour(currentSunset)-8;
    if (sunset_hour < 0) sunset_hour += 24;

    int currenttime_minutes = minute(currentDt);
    int sunrise_minutes = minute(currentSunrise);
    int sunset_minutes = minute(currentSunset);
    
    int currenttime_in_minutes = currenttime_minutes + 60*currenttime_hour;
    int sunrise_in_minutes = sunrise_minutes + 60*sunrise_hour;
    int sunset_in_minutes = sunset_minutes + 60*sunset_hour;

    Serial.println("Time: ");
    Serial.print(currenttime_hour);
    Serial.print(":");
    Serial.println(currenttime_minutes);
    // Serial.println(currenttime_in_minutes);
    
    Serial.print("Sunrise: ");
    Serial.print(sunrise_hour);
    Serial.print(":");
    Serial.println(sunrise_minutes);
    // Serial.println(sunrise_in_minutes);
    
    Serial.print("Sunset: ");
    Serial.print(sunset_hour);
    Serial.print(":");
    Serial.println(sunset_minutes);
    // Serial.println(sunset_in_minutes);

    if (currenttime_in_minutes < sunrise_in_minutes || currenttime_in_minutes > sunset_in_minutes) return false;
    else return true;
  }
  else {
    Serial.print("Weather API Error code: ");
    Serial.println(httpResponseCode);
    return true;
  }
  http.end();
}

void checkIPForStartupEmail() {
  int previousWebsiteIP_el0 = EEPROM.read(1);
  int previousWebsiteIP_el1 = EEPROM.read(7);
  int previousWebsiteIP_el2 = EEPROM.read(8);
  int previousWebsiteIP_el3 = EEPROM.read(9);
  String websiteIP = WiFi.localIP().toString();
  String currentWebsiteIP_el0 = "";
  String currentWebsiteIP_el1 = "";
  String currentWebsiteIP_el2 = "";
  String currentWebsiteIP_el3 = "";
  int el_i = 0;
  for (int i = 0; i < websiteIP.length(); ++i) {
      if (websiteIP[i] != '.') {
        if (el_i == 0) {
          currentWebsiteIP_el0 += websiteIP[i];
        } else if (el_i == 1) {
          currentWebsiteIP_el1 += websiteIP[i];
        } else if (el_i == 2) {
          currentWebsiteIP_el2 += websiteIP[i];
        } else if (el_i == 3) {
          currentWebsiteIP_el3 += websiteIP[i];
        }
      } else {
        el_i += 1;
      }
  }

  Serial.print((uint8_t)currentWebsiteIP_el0.toInt());
  Serial.print(".");
  Serial.print((uint8_t)currentWebsiteIP_el1.toInt());
  Serial.print(".");
  Serial.print((uint8_t)currentWebsiteIP_el2.toInt());
  Serial.print(".");
  Serial.println((uint8_t)currentWebsiteIP_el3.toInt());
  
  Serial.print((uint8_t)previousWebsiteIP_el0);
  Serial.print(".");
  Serial.print((uint8_t)previousWebsiteIP_el1);
  Serial.print(".");
  Serial.print((uint8_t)previousWebsiteIP_el2);
  Serial.print(".");
  Serial.println((uint8_t)previousWebsiteIP_el3);

  if (previousWebsiteIP_el0 != currentWebsiteIP_el0.toInt() || previousWebsiteIP_el1 != currentWebsiteIP_el1.toInt() || previousWebsiteIP_el2 != currentWebsiteIP_el2.toInt() || previousWebsiteIP_el3 != currentWebsiteIP_el3.toInt()) sendStartupEmail(WiFi.localIP().toString());
  EEPROM.write(1, (uint8_t)currentWebsiteIP_el0.toInt());
  EEPROM.write(7, (uint8_t)currentWebsiteIP_el1.toInt());
  EEPROM.write(8, (uint8_t)currentWebsiteIP_el2.toInt());
  EEPROM.write(9, (uint8_t)currentWebsiteIP_el3.toInt());
  EEPROM.commit();
}

void setup() {
  try {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    Serial.println();

    // set up pins
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(PIR_GPIO, INPUT);
    pinMode(SECONDARY_PIR_GPIO, INPUT);

    // Init camera, wifi, server, file system
    if (!LittleFS.begin(true)) {
      Serial.println("LittleFS Mount Failed");
      ESP.restart();
      return;
    }
    EEPROM.begin(EEPROM_SIZE);
    initCamera();
    sensitivity = EEPROM.read(6);
    connectToWifi();
    startCameraServer();
    MailClient.networkReconnect(true);
    connectToEmail();
    
    checkIPForStartupEmail();

    is_daytime = isDaytime();
    Serial.print("isDaytime: ");
    Serial.println(is_daytime);

    digitalWrite(LED_BUILTIN, LOW);  // turn the LED off to indicate setup is complete

  } catch (const char* errorMessage) {
    // Handle the exception
    Serial.print("Exception caught in setup: ");
    Serial.println(errorMessage);
    ESP.restart();
    return;
  }
}

void loop() {
  try {
    updateCameraSettings();
    if(WiFi.status() != WL_CONNECTED) ESP.restart();
    if (millis() - lastDaytimeUpdateMillis > timeBetweenDaytimeChecks) {
      is_daytime = isDaytime();
      lastDaytimeUpdateMillis = millis();
    }

    EEPROM.write(6, (uint8_t)sensitivity);
    EEPROM.commit();
    if (!is_daytime) {
      Serial.print("x");
    } else {
      Serial.print("o");
      if (digitalRead(PIR_GPIO) == HIGH || digitalRead(SECONDARY_PIR_GPIO) == HIGH) {
        numSequentialMotionDetections = numSequentialMotionDetections + 1;
        digitalWrite(LED_BUILTIN, HIGH);  // turn the LED on
        delay(10);
        digitalWrite(LED_BUILTIN, LOW);   // turn the LED off
        delay(90);
      } else {
        numSequentialMotionDetections = 0;
      }
      if (numSequentialMotionDetections >= sensitivity) {
        digitalWrite(LED_BUILTIN, HIGH);  // turn the LED on
        saveCaptureToFileSystem();        // capture hummingbird content
        if (frame_count > 0) sendCaptureByEmail();             // send hummingbird content as attachment in email
        digitalWrite(LED_BUILTIN, LOW);   // turn the LED off
        numSequentialMotionDetections = 0;
      }
    }
    delay(400);
  } catch (const char* errorMessage) {
    // Handle the exception
    Serial.print("Exception caught in loop: ");
    Serial.println(errorMessage);
    ESP.restart();
    return;
  }
}

void initCamera() {
  camera_config_t camera_config;
  camera_config.ledc_channel = LEDC_CHANNEL_0;
  camera_config.ledc_timer = LEDC_TIMER_0;
  camera_config.pin_d0 = Y2_GPIO_NUM;
  camera_config.pin_d1 = Y3_GPIO_NUM;
  camera_config.pin_d2 = Y4_GPIO_NUM;
  camera_config.pin_d3 = Y5_GPIO_NUM;
  camera_config.pin_d4 = Y6_GPIO_NUM;
  camera_config.pin_d5 = Y7_GPIO_NUM;
  camera_config.pin_d6 = Y8_GPIO_NUM;
  camera_config.pin_d7 = Y9_GPIO_NUM;
  camera_config.pin_xclk = XCLK_GPIO_NUM;
  camera_config.pin_pclk = PCLK_GPIO_NUM;
  camera_config.pin_vsync = VSYNC_GPIO_NUM;
  camera_config.pin_href = HREF_GPIO_NUM;
  camera_config.pin_sscb_sda = SIOD_GPIO_NUM;
  camera_config.pin_sscb_scl = SIOC_GPIO_NUM;
  camera_config.pin_pwdn = PWDN_GPIO_NUM;
  camera_config.pin_reset = RESET_GPIO_NUM;
  camera_config.xclk_freq_hz = 20000000;
  camera_config.pixel_format = PIXFORMAT_JPEG;
  camera_config.frame_size = FRAMESIZE_HD;
  camera_config.jpeg_quality = 4;
  camera_config.fb_count = 1;


  esp_err_t err = esp_camera_init(&camera_config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    ESP.restart();
    return;
  }
  
  sensor_t* s = esp_camera_sensor_get();
  Serial.print("EEPROM @ 0: ");
  Serial.println(EEPROM.read(0));
  if (EEPROM.read(0) != (uint8_t)BOOLEAN_KEY) {
    EEPROM.write(0, (uint8_t)BOOLEAN_KEY);
    EEPROM.write(1, 0);
    EEPROM.write(7, 0);
    EEPROM.write(8, 0);
    EEPROM.write(9, 0);
    EEPROM.write(5, (uint8_t)s->status.gainceiling);
    EEPROM.write(4, (uint8_t)(s->status.saturation+10));
    EEPROM.write(3, (uint8_t)(s->status.brightness+10));
    EEPROM.write(2, (uint8_t)(s->status.contrast+10));
    EEPROM.write(6, (uint8_t)sensitivity);
    EEPROM.commit();
  } else {
    s->set_contrast(s, ((int)EEPROM.read(2))-10);
    s->set_brightness(s, ((int)EEPROM.read(3))-10);
    s->set_saturation(s, ((int)EEPROM.read(4))-10);
    s->set_gainceiling(s, (gainceiling_t)EEPROM.read(5));
  }
  cam_contrast = (int)s->status.contrast;
  cam_brightness = (int)s->status.brightness;
  cam_exposure = (int)s->status.gainceiling;
  cam_saturation = (int)s->status.saturation;
}

/* Callback function to get the Email sending status */
void smtpCallback(SMTP_Status status) {
  Serial.println(status.info());
  if (status.success()) {
    Serial.println("----------------");
    Serial.printf("Message sent success: %d\n", status.completedCount());
    Serial.printf("Message sent failed: %d\n", status.failedCount());
    Serial.println("----------------\n");
    for (size_t i = 0; i < smtp.sendingResult.size(); i++) {
      SMTP_Result result = smtp.sendingResult.getItem(i);
      Serial.printf("Message No: %d\n", i + 1);
      Serial.printf("Status: %s\n", result.completed ? "success" : "failed");
      Serial.printf("Date/Time: %s\n", MailClient.Time.getDateTimeString(result.timestamp, "%B %d, %Y %H:%M:%S").c_str());
      Serial.printf("Recipient: %s\n", result.recipients.c_str());
      Serial.printf("Subject: %s\n", result.subject.c_str());
    }
    Serial.println("----------------\n");
    smtp.sendingResult.clear();
  }
}
