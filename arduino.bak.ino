#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "esp_camera.h"

// copied from CameraWebServer/camera_pins.h
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// 4 for flash led or 33 for normal led
#define LED_GPIO_NUM 4


const char *ssid = "";
const char *password = "";
const char *linuxBoxIP = "";
const int udpPort = 5000;

WiFiUDP udp;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

void setupLedFlash() {
#if defined(LED_GPIO_NUM)
  ledcAttach(LED_GPIO_NUM, 5000, 8);
#else
  log_i("LED flash is disabled -> LED_GPIO_NUM undefined");
#endif
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, "esp32/cam/start") == 0) {
    Serial.println("received cam start cmd");
  } else if (strcmp(topic, "esp32/cam/stop") == 0) {
    Serial.println("received cam stop cmd");
  } else if (strcmp(topic, "esp32/motors/forward") == 0) {
    Serial.println("received command forward cmd");
  } else if (strcmp(topic, "esp32/motors/backward") == 0) {
    Serial.println("received command backward cmd");
  } else if (strcmp(topic, "esp32/motors/left") == 0) {
    Serial.println("received command left cmd");
  } else if (strcmp(topic, "esp32/motors/right") == 0) {
    Serial.println("received command right cmd");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  camera_config_t config;
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
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    // Limit the frame size when PSRAM is not available
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // drop down frame size for higher initial frame rate
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

// Setup LED FLash if LED pin is defined
#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  Serial.println("cam setup done");

  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");

  mqttClient.setServer(linuxBoxIP, 1883);
  mqttClient.setCallback(mqttCallback);

  udp.begin(WiFi.localIP(), udpPort);

  Serial.println("WifiUdp setup done");
}

void chunkedUdpSend(camera_fb_t *fb) {
  const int maxPacketSize = 1400;  // mtu udp == 1500 bytes
  int bytesSent = 0;
  int totalSize = fb->len;

  // chunk the frames so the chunks fit in a udp packet
  while (bytesSent < totalSize) {
    int chunkSize = min(maxPacketSize, totalSize - bytesSent);
    udp.beginPacket(linuxBoxIP, udpPort);
    udp.write(fb->buf + bytesSent, chunkSize);
    udp.endPacket();
    bytesSent += chunkSize;
    delay(1);  // delay to not overwhelm
  }
  // Serial.printf("Sent frame in %d packets\n", (totalSize + maxPacketSize - 1) / maxPacketSize);
}

void naiveUdpSend(camera_fb_t *fb) {
  // debug: print frame
  Serial.printf(
    "Frame: %d bytes, width: %d, height: %d, format: %d\n",
    fb->len, fb->width, fb->height, fb->format);

  // send full frame with udp => frame too large?
  udp.beginPacket(linuxBoxIP, udpPort);
  udp.write(fb->buf, fb->len);
  udp.endPacket();
}

void loop() {
  if (!mqttClient.connected()) {
    mqttClient.connect("esp32_client");
    mqttClient.subscribe("esp32/cam/#");
    mqttClient.subscribe("esp32/motors/#");
  }
  mqttClient.loop();

  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) {
    //naiveUdpSend(fb);
    chunkedUdpSend(fb);
    esp_camera_fb_return(fb);
  } else {
    Serial.println("Failed to capture frame!");
  }

  delay(100);  // 10 fps
}
