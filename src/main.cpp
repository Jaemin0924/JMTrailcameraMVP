#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <esp_camera.h>
#include <Wire.h>
#include <RTClib.h>

// WiFi credentials
const char* kWiFiSsid = "LognSteam";
const char* kWiFiPassword = "roboticsisfun!";

// REST endpoint for posting JSON payloads
const char* kPostUrl = "https://drive.google.com/drive/folders/10c4ehFUj7CQ_qneVW_DQPeUOt9HWwfQD";

// Save directory on SD card
const char* kPhotoDirectory = "/photos";

// STM32L0 handshake pin - pulled HIGH when this boot's work is done.
// This tells the gatekeeper it is safe to cut power now instead of
// waiting out the full timeout.
#if defined(D0)
#define DONE_PIN D0
#else
#define DONE_PIN 0
#endif

// Camera pin definitions - verify against your actual board datasheet.
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      8
#define SIOD_GPIO_NUM     11
#define SIOC_GPIO_NUM     12
#define Y9_GPIO_NUM       19
#define Y8_GPIO_NUM       18
#define Y7_GPIO_NUM       17
#define Y6_GPIO_NUM       16
#define Y5_GPIO_NUM       15
#define Y4_GPIO_NUM       14
#define Y3_GPIO_NUM       13
#define Y2_GPIO_NUM       20
#define VSYNC_GPIO_NUM    22
#define HREF_GPIO_NUM     21
#define PCLK_GPIO_NUM     23

RTC_DS3231 rtc;
bool rtcOk = false;

String base64Encode(const uint8_t* data, size_t length) {
  static const char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String encoded;
  encoded.reserve(((length + 2) / 3) * 4);

  for (size_t i = 0; i < length; i += 3) {
    uint32_t value = 0;
    int bytes = 0;

    for (int j = 0; j < 3; ++j) {
      value <<= 8;
      if (i + j < length) {
        value |= data[i + j];
        ++bytes;
      }
    }

    encoded += kBase64Alphabet[(value >> 18) & 0x3F];
    encoded += kBase64Alphabet[(value >> 12) & 0x3F];
    encoded += (bytes > 1) ? kBase64Alphabet[(value >> 6) & 0x3F] : '=';
    encoded += (bytes > 2) ? kBase64Alphabet[value & 0x3F] : '=';
  }

  return encoded;
}

void haltAndSleep(const char* reason) {
  Serial.println(reason);
  pinMode(DONE_PIN, OUTPUT);
  digitalWrite(DONE_PIN, HIGH);
  delay(200);
  esp_deep_sleep_start();
}

bool initWiFi() {
  WiFi.mode(WIFI_STA);
  Serial.printf("Connecting to WiFi '%s'...\n", kWiFiSsid);
  WiFi.begin(kWiFiSsid, kWiFiPassword);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print('.');
  }

  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("Failed to connect to WiFi");
  return false;
}

bool initSDCard() {
  Serial.println("Initializing SD card...");
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD_MMC.begin() failed");
    return false;
  }

  if (!SD_MMC.exists(kPhotoDirectory)) {
    Serial.printf("Creating directory %s\n", kPhotoDirectory);
    if (!SD_MMC.mkdir(kPhotoDirectory)) {
      Serial.println("Failed to create photo directory");
      return false;
    }
  }

  uint64_t cardSize = SD_MMC.cardSize() >> 20;
  Serial.printf("SD card mounted, size: %llu MB\n", cardSize);
  return true;
}

bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_SVGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }

  Serial.println("Camera initialized successfully");
  return true;
}

void discardWarmupFrames() {
  for (int i = 0; i < 2; i++) {
    camera_fb_t* warm = esp_camera_fb_get();
    if (warm) esp_camera_fb_return(warm);
    delay(100);
  }
}

String makePhotoPath() {
  if (rtcOk) {
    DateTime now = rtc.now();
    char buf[48];
    snprintf(buf, sizeof(buf), "%s/photo_%04d%02d%02d_%02d%02d%02d.jpg",
             kPhotoDirectory, now.year(), now.month(), now.day(),
             now.hour(), now.minute(), now.second());
    return String(buf);
  }

  uint32_t r = esp_random();
  return String(kPhotoDirectory) + "/photo_" + String(r) + ".jpg";
}

bool captureAndSavePhoto(String& outPath) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return false;
  }

  String path = makePhotoPath();
  File file = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (!file) {
    Serial.printf("Failed to open file for writing: %s\n", path.c_str());
    esp_camera_fb_return(fb);
    return false;
  }

  size_t written = file.write(fb->buf, fb->len);
  file.close();
  esp_camera_fb_return(fb);

  if (written != fb->len) {
    Serial.printf("Failed to write complete file. expected=%u written=%u\n", fb->len, written);
    SD_MMC.remove(path.c_str());
    return false;
  }

  Serial.printf("Photo saved to SD: %s (%u bytes)\n", path.c_str(), written);
  outPath = path;
  return true;
}

String findPendingPhoto() {
  File dir = SD_MMC.open(kPhotoDirectory);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return "";
  }

  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      String name = String(entry.name());
      if (name.endsWith(".jpg") || name.endsWith(".jpeg")) {
        entry.close();
        dir.close();
        return name.startsWith("/") ? name : String(kPhotoDirectory) + "/" + name;
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }

  dir.close();
  return "";
}

bool postFileToServer(const String& filePath) {
  File file = SD_MMC.open(filePath.c_str(), FILE_READ);
  if (!file) {
    Serial.printf("Failed to open file for POST: %s\n", filePath.c_str());
    return false;
  }

  size_t fileSize = file.size();
  if (fileSize == 0) {
    Serial.println("File is empty, skipping POST");
    file.close();
    return false;
  }

  uint8_t* buffer = new uint8_t[fileSize];
  size_t readBytes = file.read(buffer, fileSize);
  file.close();

  if (readBytes != fileSize) {
    Serial.println("Failed to read image data from file");
    delete[] buffer;
    return false;
  }

  String payload = "{\"filename\":\"" + filePath + "\",\"image\":\"";
  payload += base64Encode(buffer, fileSize);
  payload += "\"}";
  delete[] buffer;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, kPostUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  Serial.printf("Posting %u bytes to %s\n", payload.length(), kPostUrl);
  int httpCode = http.POST(payload);
  String response;
  if (httpCode > 0) {
    response = http.getString();
    Serial.printf("HTTP %d response: %s\n", httpCode, response.c_str());
  } else {
    Serial.printf("HTTP POST failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();

  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED || httpCode == HTTP_CODE_ACCEPTED) {
    Serial.println("POST successful, deleting local file");
    return SD_MMC.remove(filePath.c_str());
  }

  Serial.println("POST failed, keeping file on SD for retry next trigger");
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(DONE_PIN, OUTPUT);
  digitalWrite(DONE_PIN, LOW);

  Wire.begin();
  rtcOk = rtc.begin();
  if (!rtcOk) {
    Serial.println("RTC not found - falling back to random filenames");
  }

  if (!initCamera()) {
    haltAndSleep("Camera initialization failed. Signaling done, sleeping.");
    return;
  }
  discardWarmupFrames();

  if (!initSDCard()) {
    haltAndSleep("SD card initialization failed. Signaling done, sleeping.");
    return;
  }

  bool wifiOk = initWiFi();

  if (wifiOk) {
    String pending = findPendingPhoto();
    if (pending.length() > 0) {
      Serial.printf("Found pending photo from earlier trigger: %s\n", pending.c_str());
      postFileToServer(pending);
    }
  }

  String newPhotoPath;
  if (captureAndSavePhoto(newPhotoPath)) {
    if (wifiOk) {
      postFileToServer(newPhotoPath);
    } else {
      Serial.println("No WiFi - new photo kept on SD, will retry next trigger");
    }
  }

  SD_MMC.end();
  esp_camera_deinit();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  digitalWrite(DONE_PIN, HIGH);
  delay(300);
  esp_deep_sleep_start();
}

void loop() {
  // never reached
}
