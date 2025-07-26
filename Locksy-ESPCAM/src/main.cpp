#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <FS.h>
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

const char *ssid = "Locksy_Smartbox";
const char *password = "12345678";

IPAddress local_IP(192, 168, 137, 200);
IPAddress gateway(192, 168, 137, 1);
IPAddress subnet(255, 255, 255, 0);

WebServer server(80);
WebSocketsServer webSocket(81);

String photoDir = "/photos";

void startCamera()
{
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sscb_sda = 26;
  config.pin_sscb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 10;
  config.fb_count = 1;
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK)
  {
    Serial.printf("[CAM] Camera init failed with error 0x%x\n", err);
  }
  else
  {
    Serial.println("[CAM] Camera init success");
  }
}

String uploadToStrapi(String path)
{
  File file = SD_MMC.open(path.c_str(), FILE_READ);
  if (!file)
  {
    Serial.println("[UPLOAD] Failed to open file");
    return "";
  }

  size_t fileSize = file.size();
  Serial.println("[UPLOAD] File size: " + String(fileSize) + " bytes");

  // Baca seluruh file ke buffer (pastikan ukuran foto tidak terlalu besar)
  if (fileSize > 200000)
  {
    Serial.println("[UPLOAD] File too large for buffer");
    file.close();
    return "";
  }

  uint8_t *fileBuffer = (uint8_t *)malloc(fileSize);
  if (!fileBuffer)
  {
    Serial.println("[UPLOAD] Failed to allocate memory for file");
    file.close();
    return "";
  }

  size_t bytesRead = file.read(fileBuffer, fileSize);
  file.close();

  if (bytesRead != fileSize)
  {
    Serial.println("[UPLOAD] Failed to read entire file");
    free(fileBuffer);
    return "";
  }

  HTTPClient http;
  WiFiClient client;
  String serverURL = "http://192.168.137.1:1337/api/upload";

  // Mulai koneksi HTTP
  http.begin(client, serverURL);

  // Buat boundary
  String boundary = "----WebKitFormBoundary" + String(random(0xFFFF), HEX);

  // Set header
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  // Buat buffer untuk bagian header dan footer
  String headPart = "--" + boundary + "\r\n";
  headPart += "Content-Disposition: form-data; name=\"files\"; filename=\"image.jpg\"\r\n";
  headPart += "Content-Type: image/jpeg\r\n\r\n";

  String tailPart = "\r\n--" + boundary + "--\r\n";

  // Alokasi memori untuk buffer lengkap yang berisi header + file + footer
  size_t totalSize = headPart.length() + fileSize + tailPart.length();
  uint8_t *buffer = (uint8_t *)malloc(totalSize);

  if (!buffer)
  {
    Serial.println("[UPLOAD] Failed to allocate memory for upload buffer");
    free(fileBuffer);
    return "";
  }

  // Salin semua bagian ke buffer tunggal
  size_t pos = 0;
  memcpy(buffer, headPart.c_str(), headPart.length());
  pos += headPart.length();

  memcpy(buffer + pos, fileBuffer, fileSize);
  pos += fileSize;
  free(fileBuffer); // Bebaskan buffer file karena sudah disalin

  memcpy(buffer + pos, tailPart.c_str(), tailPart.length());

  // Kirim sebagai POST
  Serial.println("[UPLOAD] Sending POST request with data size: " + String(totalSize));
  int httpCode = http.POST(buffer, totalSize);
  free(buffer); // Bebaskan buffer kombinasi

  Serial.println("[UPLOAD] HTTP response code: " + String(httpCode));

  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED)
  {
    String response = http.getString();
    Serial.println("[UPLOAD] Response: " + response);

    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, response);

    if (!error)
    {
      if (doc.is<JsonArray>() && doc.size() > 0)
      {
        int id = doc[0]["id"].as<int>();
        Serial.println("[UPLOAD] Document ID: " + String(id));
        http.end();
        return String(id);
      }
    }
    else
    {
      Serial.println("[UPLOAD] JSON parse error: " + String(error.c_str()));
    }
  }
  else
  {
    Serial.println("[UPLOAD] Upload failed with code: " + String(httpCode));
  }

  http.end();
  return "";
}

String capturePhotoAndUpload(uint8_t clientNum)
{
  for (int i = 0; i < 3; i++)
  {
    camera_fb_t *temp = esp_camera_fb_get();
    if (temp)
      esp_camera_fb_return(temp);
    delay(500);
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb)
  {
    Serial.println("[CAM] Failed to capture frame");
    return "";
  }
  String filename = photoDir + "/" + String(millis()) + ".jpg";
  File file = SD_MMC.open(filename.c_str(), FILE_WRITE);
  if (!file)
  {
    Serial.println("[SD] Failed to open file");
    esp_camera_fb_return(fb);
    return "";
  }
  file.write(fb->buf, fb->len);
  file.close();
  esp_camera_fb_return(fb);
  delay(300);
  Serial.println("[CAM] Saved: " + filename);

  String docId = uploadToStrapi(filename);
  if (docId != "")
  {
    String message = "STRAPI_ID:" + docId;
    webSocket.sendTXT(clientNum, message);
    Serial.println("[WebSocket] Sent documentId: " + docId);
  }
  return filename;
}

void handleRoot()
{
  String html = "<html><head><title>Galeri Foto</title></head><body>";
  html += "<h2>Galeri Foto</h2>";
  File dir = SD_MMC.open(photoDir);
  if (!dir || !dir.isDirectory())
  {
    html += "<p>Folder tidak ditemukan</p></body></html>";
    server.send(200, "text/html", html);
    return;
  }
  File file = dir.openNextFile();
  while (file)
  {
    if (!file.isDirectory())
    {
      String path = String(file.name());
      if (!path.startsWith("/photos/"))
      {
        path = "/photos/" + path;
      }
      html += "<div><img src=\"" + path + "\" width=\"240\"></div><br>";
    }
    file = dir.openNextFile();
  }
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleNotFound()
{
  String path = server.uri();
  Serial.println("[Web] Request file: " + path);
  if (!SD_MMC.begin())
  {
    server.send(500, "text/plain", "SD Card error");
    return;
  }
  File file = SD_MMC.open(path.c_str());
  if (!file || file.isDirectory())
  {
    server.send(404, "text/plain", "File Not Found");
    return;
  }
  server.streamFile(file, "image/jpeg");
  file.close();
}

void setup()
{
  Serial.begin(115200);
  WiFi.config(local_IP, gateway, subnet);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected to: " + String(WiFi.localIP()));

  if (!SD_MMC.begin())
  {
    Serial.println("[SD] Card mount failed");
    return;
  }
  if (!SD_MMC.exists(photoDir))
  {
    SD_MMC.mkdir(photoDir);
  }
  startCamera();
  server.on("/", handleRoot);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[WebServer] Started on port 80");

  webSocket.begin();
  webSocket.onEvent([](uint8_t num, WStype_t type, uint8_t *payload, size_t length)
                    {
    switch (type) {
      case WStype_CONNECTED: {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[WebSocket] Client %u connected from %s\n", num, ip.toString().c_str());
        webSocket.sendTXT(num, "Connected to ESP32-CAM!");
        break;
      }
      case WStype_DISCONNECTED:
        Serial.printf("[WebSocket] Client %u disconnected\n", num);
        break;
      case WStype_TEXT: {
        String msg = (const char*)payload;
        Serial.printf("[WebSocket] Received from client %u: %s\n", num, msg.c_str());
        if (msg == "TAKE_PHOTO") {
          String result = capturePhotoAndUpload(num);
          Serial.printf("[CAM] Photo captured and saved: %s\n", result.c_str());
          webSocket.sendTXT(num, "Captured: " + result);
        }
        break;
      }
    } });
  Serial.println("[WebSocket] Server started on port 81");

  server.begin();
  Serial.println("[WebServer] Started on port 80");

  webSocket.begin();
  Serial.println("[WebSocket] Server started on port 81");
}

void loop()
{
  webSocket.loop();
  server.handleClient();
}
