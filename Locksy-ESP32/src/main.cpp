#include <WiFi.h>
#include <ArduinoHttpClient.h>
#include <Keypad.h>
#include <ESP32Servo.h>
#include <EEPROM.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Fonts/FreeSans12pt7b.h>
#include <WiFiManager.h>

WiFiManager wifiManager;

WebSocketsClient webSocket;

const char *serverAddress = "192.168.137.1";
const char *websocketAddress = "192.168.137.200";
const int serverPort = 1337;

// TFT Display configuration
#define TFT_CS 33
#define TFT_RST 25
#define TFT_DC 27

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// Status variables for display
bool wifiConnected = false;
bool wsConnected = false;

WiFiClient wifi;
// HttpClient client(wifi, serverAddress, serverPort);

const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
    {'1', '2', '3', '['}, // 'A' changed to '[' for input owner ID
    {'4', '5', '6', ']'}, // 'B' changed to ']' for potential future use or current functionality
    {'7', '8', '9', '{'}, // 'C' changed to '{' for reset WiFi
    {'*', '0', '#', '}'}  // 'D' changed to '}' for servo test
};

byte rowPins[ROWS] = {5, 19, 21, 22};
byte colPins[COLS] = {15, 4, 16, 17};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

#define EEPROM_SIZE 10
#define OWNER_ID_ADDR 0
String ownerId = "0";
String currentDocumentId = "";
unsigned long buttonStartTime = 0;
unsigned long lastPressA_millis = 0; // Still used for '[' (original A)
int pressCountA = 0;                 // Still used for '[' (original A)
const unsigned long RAPID_PRESS_INTERVAL = 700;
unsigned long lastPressC_millis = 0; // Still used for '{' (original C)
int pressCountC = 0;                 // Still used for '{' (original C)
// Current input for display
String currentInput = "";
String statusMessage = "Ready";

// --- START: New variables for multi-tap keypad input ---
const unsigned long MULTI_TAP_TIMEOUT = 1000; // Waktu tunggu sebelum karakter dianggap selesai (ms)
unsigned long lastDigitPressTime = 0;
char lastPressedDigit = '\0';
int digitPressCount = 0;

char keypadCharMapFinal[10][5] = {
    // Increased size for 'S' and 'Z'
    //   0   1   2   3   4
    {'0', ' ', '\0', '\0', '\0'},  // 0
    {'1', '\0', '\0', '\0', '\0'}, // 1
    {'2', 'A', 'B', 'C', '\0'},    // 2 (ABC)
    {'3', 'D', 'E', 'F', '\0'},    // 3 (DEF)
    {'4', 'G', 'H', 'I', '\0'},    // 4 (GHI)
    {'5', 'J', 'K', 'L', '\0'},    // 5 (JKL)
    {'6', 'M', 'N', 'O', '\0'},    // 6 (MNO)
    {'7', 'P', 'Q', 'R', 'S'},     // 7 (PQRS)
    {'8', 'T', 'U', 'V', '\0'},    // 8 (TUV)
    {'9', 'W', 'X', 'Y', 'Z'}      // 9 (WXYZ)
};

// --- END: New variables for multi-tap keypad input ---

void updateStatusPenerimaan(String resi, String documentId);
void bukaSmartbox();
void tutupSmartbox();
void saveOwnerId(String id);
String readOwnerId();
void inputOwnerId();
void updateStrapiWithPhotoId(int photoId);
void drawStatusIndicators();
void drawInputField();
void updateInputField(String text);
void updateStatusMessage(String message);
void inputResi(char firstKey); // Declare inputResi here

Servo myServo;
int servoPin = 12;

void resetWiFiSettings()
{
    wifiManager.resetSettings();
    Serial.println("WiFi settings reset");
    updateStatusMessage("WiFi settings reset");
    ESP.restart();
}

void updateStrapiWithPhotoId(int photoId)
{
    if (currentDocumentId.length() == 0)
    {
        Serial.println("[Error] No document ID available for update");
        updateStatusMessage("Error: No document ID");
        return;
    }

    String updatePath = "/api/smartboxes/" + currentDocumentId;

    JsonDocument doc;
    doc["data"]["foto"]["id"] = photoId;
    String requestBody;
    serializeJson(doc, requestBody);

    WiFiClient wifiClient;
    HttpClient client(wifiClient, serverAddress, serverPort);

    client.beginRequest();
    client.put(updatePath);
    client.sendHeader("Content-Type", "application/json");
    client.sendHeader("Content-Length", requestBody.length());
    client.beginBody();
    client.print(requestBody);
    client.endRequest();

    int statusCode = client.responseStatusCode();
    String response = client.responseBody();

    Serial.println("[HTTP] PUT Video ID Status: " + String(statusCode));
    if (statusCode > 0)
    {
        Serial.println("[HTTP] Response: " + response);
        updateStatusMessage("Photo ID updated");
    }
    else
    {
        Serial.println("[HTTP] Failed to send PUT request for Photo ID");
        updateStatusMessage("Failed to update video");
    }

    client.stop();
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case WStype_DISCONNECTED:
        Serial.println("[WebSocket] Disconnected");
        wsConnected = false;
        drawStatusIndicators();
        break;
    case WStype_CONNECTED:
        Serial.println("[WebSocket] Connected to ESP32-CAM");
        wsConnected = true;
        drawStatusIndicators();
        break;
    case WStype_TEXT:
    {
        String message = (char *)payload;
        Serial.print("[WebSocket] Message from CAM: ");
        Serial.println(message);

        if (message.startsWith("STRAPI_ID:"))
        {
            int photoId = message.substring(10).toInt();
            Serial.println("[Main] Received video ID: " + String(photoId));
            updateStatusMessage("Got video ID: " + String(photoId));

            // Update Strapi with video ID
            updateStrapiWithPhotoId(photoId);

            // Now update the status_penerimaan
            if (currentDocumentId.length() > 0)
            {
                Serial.println("📌 Mengupdate status_penerimaan menjadi true...");
                updateStatusMessage("Updating receipt status...");
                updateStatusPenerimaan("", currentDocumentId); // No need for resi here
            }
        }
        break;
    }
    case WStype_ERROR:
        Serial.println("[WebSocket] Error!");
        updateStatusMessage("WebSocket Error");
        wsConnected = false;
        drawStatusIndicators();
        break;
    }
}

void saveOwnerId(String id)
{
    for (int i = 0; i < EEPROM_SIZE; i++)
    {
        EEPROM.write(OWNER_ID_ADDR + i, 0);
    }

    for (int i = 0; i < id.length(); i++)
    {
        EEPROM.write(OWNER_ID_ADDR + i, id[i]);
    }

    EEPROM.commit();
    Serial.println("Owner ID saved: " + id);
    updateStatusMessage("Owner ID saved: " + id);
}

String readOwnerId()
{
    String id = "";
    char ch;

    for (int i = 0; i < EEPROM_SIZE; i++)
    {
        ch = EEPROM.read(OWNER_ID_ADDR + i);
        if (ch == 0)
            break;
        id += ch;
    }

    if (id.length() == 0)
    {
        return "2";
    }

    return id;
}

void inputOwnerId()
{
    Serial.println("\n=== MODE INPUT OWNER ID ===");
    updateStatusMessage("INPUT OWNER ID MODE");
    currentInput = "";
    updateInputField(currentInput);

    lastDigitPressTime = 0;
    lastPressedDigit = '\0';
    digitPressCount = 0;

    while (true)
    {
        char key = keypad.getKey();
        if (key)
        {
            if (key == '#')
            {
                // FINALISASI KARAKTER MULTI-TAP SEBELUM SUBMIT
                if (lastPressedDigit != '\0' && digitPressCount > 0)
                {
                    int digitIndex = lastPressedDigit - '0';
                    if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[digitIndex][digitPressCount - 1] != '\0')
                    {
                        currentInput += keypadCharMapFinal[digitIndex][digitPressCount - 1];
                    }
                    else if (digitIndex >= 0 && digitIndex <= 9)
                    {
                        currentInput += lastPressedDigit;
                    }
                    // Reset multi-tap states after finalizing
                    lastDigitPressTime = 0;
                    lastPressedDigit = '\0';
                    digitPressCount = 0;
                }
                break; // Exit input loop
            }
            else if (key == '*')
            {
                // FINALISASI KARAKTER MULTI-TAP SEBELUM BACKSPACE
                if (lastPressedDigit != '\0' && digitPressCount > 0)
                {
                    int digitIndex = lastPressedDigit - '0';
                    if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[digitIndex][digitPressCount - 1] != '\0')
                    {
                        currentInput += keypadCharMapFinal[digitIndex][digitPressCount - 1];
                    }
                    else if (digitIndex >= 0 && digitIndex <= 9)
                    {
                        currentInput += lastPressedDigit;
                    }
                    // Reset multi-tap states after finalizing
                    lastDigitPressTime = 0;
                    lastPressedDigit = '\0';
                    digitPressCount = 0;
                }

                // Hapus karakter terakhir dari input
                if (!currentInput.isEmpty())
                {
                    currentInput.remove(currentInput.length() - 1);
                    updateInputField(currentInput);
                    Serial.print("\b \b");
                }
            }
            else if (key == '[' || key == ']' || key == '{' || key == '}')
            {
                // FINALISASI KARAKTER MULTI-TAP SEBELUM TOMBOL SPECIAL
                if (lastPressedDigit != '\0' && digitPressCount > 0)
                {
                    int digitIndex = lastPressedDigit - '0';
                    if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[digitIndex][digitPressCount - 1] != '\0')
                    {
                        currentInput += keypadCharMapFinal[digitIndex][digitPressCount - 1];
                    }
                    else if (digitIndex >= 0 && digitIndex <= 9)
                    {
                        currentInput += lastPressedDigit;
                    }
                }
                // Abaikan tombol ini dalam mode input ID, atau tambahkan jika diinginkan.
                // Untuk Owner ID, kita biasanya hanya ingin angka/huruf.
                // Reset multi-tap states
                lastDigitPressTime = 0;
                lastPressedDigit = '\0';
                digitPressCount = 0;
            }
            else if (isDigit(key)) // Handle digit input with multi-tap logic
            {
                unsigned long currentMillis = millis();
                if (key == lastPressedDigit && (currentMillis - lastDigitPressTime < MULTI_TAP_TIMEOUT))
                {
                    // Multi-tap pada tombol yang sama
                    digitPressCount++;
                    int maxCharsForThisDigit = 0;
                    int digitIndex = key - '0';
                    if (digitIndex >= 0 && digitIndex <= 9)
                    {
                        for (int i = 0; i < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]); ++i)
                        {
                            if (keypadCharMapFinal[digitIndex][i] != '\0')
                            {
                                maxCharsForThisDigit++;
                            }
                        }
                    }
                    if (digitPressCount > maxCharsForThisDigit)
                        digitPressCount = 1;

                    // Hapus karakter sebelumnya yang mungkin sudah ditampilkan (sementara)
                    if (!currentInput.isEmpty() && currentInput.length() > 0)
                    {
                        currentInput.remove(currentInput.length() - 1);
                        Serial.print("\b \b");
                    }
                }
                else
                {
                    // Tombol baru ditekan atau timeout, tambahkan karakter sebelumnya secara permanen
                    if (lastPressedDigit != '\0' && digitPressCount > 0)
                    {
                        int prevDigitIndex = lastPressedDigit - '0';
                        if (prevDigitIndex >= 0 && prevDigitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[prevDigitIndex][digitPressCount - 1] != '\0')
                        {
                            currentInput += keypadCharMapFinal[prevDigitIndex][digitPressCount - 1];
                        }
                        else if (prevDigitIndex >= 0 && prevDigitIndex <= 9)
                        {
                            currentInput += lastPressedDigit;
                        }
                    }
                    // Mulai hitungan baru untuk tombol ini
                    lastPressedDigit = key;
                    digitPressCount = 1;
                }

                // Tampilkan karakter yang dipilih saat ini (sementara)
                int digitIndex = lastPressedDigit - '0';
                char charToShow = '\0';
                if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]))
                {
                    charToShow = keypadCharMapFinal[digitIndex][digitPressCount - 1];
                    if (charToShow == '\0' && digitPressCount == 1)
                    {
                        charToShow = lastPressedDigit;
                    }
                    else if (charToShow == '\0')
                    {
                        charToShow = lastPressedDigit;
                    }
                }
                else
                {
                    charToShow = lastPressedDigit;
                }

                currentInput += charToShow;
                updateInputField(currentInput);
                Serial.print(charToShow);

                lastDigitPressTime = currentMillis;
            }
        }

        // Cek timeout untuk multi-tap (ini yang memfinalisasi karakter multi-tap jika diam)
        if (lastPressedDigit != '\0' && digitPressCount > 0 && (millis() - lastDigitPressTime >= MULTI_TAP_TIMEOUT))
        {
            int digitIndex = lastPressedDigit - '0';
            if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[digitIndex][digitPressCount - 1] != '\0')
            {
                // Karakter sudah ditambahkan di logic di atas, hanya perlu reset state
            }
            else if (digitIndex >= 0 && digitIndex <= 9)
            {
                // Karakter sudah ditambahkan
            }
            Serial.println("\nMulti-tap timeout. Finalizing character.");
            lastDigitPressTime = 0;
            lastPressedDigit = '\0';
            digitPressCount = 0;
        }

        webSocket.loop();
        delay(50);
    }

    // FINAL CHECK: Finalisasi karakter multi-tap terakhir sebelum keluar dari fungsi
    if (lastPressedDigit != '\0' && digitPressCount > 0)
    {
        int digitIndex = lastPressedDigit - '0';
        if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[digitIndex][digitPressCount - 1] != '\0')
        {
            currentInput += keypadCharMapFinal[digitIndex][digitPressCount - 1];
        }
        else if (digitIndex >= 0 && digitIndex <= 9)
        {
            currentInput += lastPressedDigit;
        }
    }

    Serial.println();
    currentInput.trim();

    if (currentInput.length() > 0)
    {
        saveOwnerId(currentInput);
        ownerId = currentInput;
        Serial.println("ID berhasil diperbarui: " + ownerId);
        updateStatusMessage("ID Updated: " + ownerId);
    }
    else
    {
        Serial.println("ID tidak diubah");
        updateStatusMessage("ID unchanged");
    }

    Serial.println("Kembali ke mode normal...");
    delay(1000);
    updateStatusMessage("Ready");
    currentInput = "";
    updateInputField("");
}

void cekResi(String resi)
{
    updateStatusMessage("Checking receipt: " + resi);
    String requestPath = "/api/smartboxes?filters[resi][$eq]=" + resi + "&filters[owner][id][$eq]=" + ownerId;
    Serial.println("Mengirim request ke API: " + requestPath);

    WiFiClient wifiClient;
    HttpClient client(wifiClient, serverAddress, serverPort);

    client.get(requestPath);
    int statusCode = client.responseStatusCode();
    String response = client.responseBody();

    if (statusCode == 200)
    {
        if (response.indexOf("\"data\":[]") == -1)
        {
            Serial.println("✅ Resi ditemukan!");
            updateStatusMessage("Receipt found!");

            int docIdStart = response.indexOf("\"documentId\":\"") + 14;
            int docIdEnd = response.indexOf("\"", docIdStart);
            String documentId = response.substring(docIdStart, docIdEnd);

            currentDocumentId = documentId; // simpan untuk digunakan nanti

            if (response.indexOf("\"status_penerimaan\":false") != -1 || response.indexOf("\"status_penerimaan\":null") != -1)
            {
                Serial.println("📦 Paket belum diterima. Membuka smartbox...");
                updateStatusMessage("Opening smartbox...");
                bukaSmartbox();
                delay(5000);
                tutupSmartbox();
                Serial.println("📸 Mengambil foto...");
                updateStatusMessage("Taking photo...");
                webSocket.sendTXT("TAKE_PHOTO");
            }
            else
            {
                Serial.println("⚠️ Paket sudah diterima sebelumnya. Smartbox tidak terbuka.");
                updateStatusMessage("Package already received");
            }
        }
        else
        {
            Serial.println("❌ Resi tidak ditemukan.");
            updateStatusMessage("Receipt not found");
        }
    }
    else
    {
        Serial.print("⚠️ Error HTTP: ");
        Serial.println(statusCode);
        updateStatusMessage("HTTP Error: " + String(statusCode));
    }

    client.stop();
}

void moveServoSmoothly(int startAngle, int endAngle, int delayMs)
{
    if (startAngle < endAngle)
    {
        for (int angle = startAngle; angle <= endAngle; angle++)
        {
            myServo.write(angle);
            delay(delayMs); // Delay untuk mengatur kecepatan
        }
    }
    else
    {
        for (int angle = startAngle; angle >= endAngle; angle--)
        {
            myServo.write(angle);
            delay(delayMs); // Delay untuk mengatur kecepatan
        }
    }
}

void bukaSmartbox()
{
    moveServoSmoothly(45, 0, 20); // Gerakkan servo dari 45° ke 0° dengan delay 20ms
    Serial.println("✅ Smartbox Opened.");
    updateStatusMessage("Smartbox Opened");
}

void tutupSmartbox()
{
    moveServoSmoothly(0, 45, 20); // Gerakkan servo dari 0° ke 45° dengan delay 20ms
    Serial.println("✅ Smartbox tertutup.");
    updateStatusMessage("Smartbox closed");
}

void tesbukaSmartbox()
{
    moveServoSmoothly(45, 0, 20); // Gerakkan servo dari 45° ke 0° dengan delay 20ms
    Serial.println("✅ Smartbox Opened.");
    updateStatusMessage("Smartbox Opened");
}

void testutupSmartbox()
{
    moveServoSmoothly(0, 45, 20); // Gerakkan servo dari 0° ke 45° dengan delay 20ms
    Serial.println("✅ Smartbox tertutup.");
    updateStatusMessage("Smartbox closed");
}

void updateStatusPenerimaan(String resi, String documentId)
{
    String updatePath = "/api/smartboxes/" + documentId;
    String requestBody = "{\"data\":{\"status_penerimaan\":true}}";

    WiFiClient wifiClient;
    HttpClient client(wifiClient, serverAddress, serverPort);

    client.beginRequest();
    client.put(updatePath);
    client.sendHeader("Content-Type", "application/json");
    client.sendHeader("Content-Length", requestBody.length());
    client.beginBody();
    client.print(requestBody);
    client.endRequest();

    int statusCode = client.responseStatusCode();

    if (statusCode == 200)
    {
        Serial.println("✅ Status penerimaan berhasil diperbarui.");
        updateStatusMessage("Receipt status updated");
    }
    else
    {
        Serial.println("⚠️ Gagal mengupdate status penerimaan. Status: " + String(statusCode));
        updateStatusMessage("Failed to update status");
    }

    client.stop();
}

// TFT Display functions
void initDisplay()
{
    tft.init(170, 320);
    tft.setRotation(3);
    tft.fillScreen(ST77XX_BLACK);

    drawStatusIndicators();
    drawInputField();

    // Draw title
    // tft.setFont(&FreeSans12pt7b);
    // tft.setTextColor(ST77XX_CYAN);
    // tft.setCursor(90, 30);
    // tft.println("SMARTBOX");

    updateStatusMessage("Ready");
}

void drawStatusIndicators()
{
    // WiFi status indicator
    tft.fillCircle(15, 15, 5, wifiConnected ? ST77XX_GREEN : ST77XX_RED);
    tft.setFont(NULL); // Use default font for small text
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(25, 12);
    tft.print("WiFi");

    // WebSocket status indicator
    // Clear the previous WebSocket status circle and text area first
    tft.fillRect(70, 7, 70, 15, ST77XX_BLACK); // Adjust coordinates and size as needed to clear the area
                                               // 70 (start x) = 75 (circle x) - 5 (padding)
                                               // 7 (start y) = 15 (circle y) - 8 (padding)
                                               // 70 (width) covers circle + text
                                               // 15 (height) covers circle + text

    tft.fillCircle(75, 15, 5, wsConnected ? ST77XX_GREEN : ST77XX_RED);
    tft.setCursor(85, 12);
    tft.print("WebSocket");

    // Return to default font
    tft.setFont(&FreeSans12pt7b);
}

void drawInputField()
{
    // Draw rounded rectangle for input field
    tft.drawRoundRect(30, 70, 260, 50, 10, ST77XX_WHITE);

    // Input field title
    tft.setFont(NULL);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(35, 55);
    tft.print("Input Resi:");

    // Return to default font
    tft.setFont(&FreeSans12pt7b);
}

void updateInputField(String text)
{
    // Clear previous text
    tft.fillRect(35, 75, 250, 40, ST77XX_BLACK);

    // Display new text
    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(40, 105);
    tft.print(text);
}

void updateStatusMessage(String message)
{
    // Clear previous message
    tft.fillRect(30, 140, 260, 30, ST77XX_BLACK);

    // Display new message
    tft.setFont(NULL);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(35, 155);
    tft.print(message);

    // Return to default font
    tft.setFont(&FreeSans12pt7b);
}

void inputResi(char firstKey)
{
    Serial.println("Masukkan nomor resi (tekan # untuk submit): ");
    updateStatusMessage("Enter receipt number");

    currentInput = "";

    lastDigitPressTime = 0;
    lastPressedDigit = '\0';
    digitPressCount = 0;

    // Handle the first key press
    if (firstKey != ' ') // Only process firstKey if it's not a dummy space
    {
        if (isDigit(firstKey))
        {
            lastPressedDigit = firstKey;
            digitPressCount = 1;
            int digitIndex = lastPressedDigit - '0';
            char charToShow = '\0';
            if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]))
            {
                charToShow = keypadCharMapFinal[digitIndex][digitPressCount - 1];
                if (charToShow == '\0' && digitPressCount == 1)
                {
                    charToShow = lastPressedDigit;
                }
                else if (charToShow == '\0')
                {
                    charToShow = lastPressedDigit;
                }
            }
            else
            {
                charToShow = lastPressedDigit;
            }

            currentInput += charToShow;
            updateInputField(currentInput);
            Serial.print(charToShow);
            lastDigitPressTime = millis();
        }
        else
        {
            currentInput += firstKey;
            updateInputField(currentInput);
            Serial.print(firstKey);
        }
    }

    while (true)
    {
        webSocket.loop();
        char k = keypad.getKey();

        if (k)
        {
            if (k == '#')
            {
                // FINALISASI KARAKTER MULTI-TAP SEBELUM SUBMIT
                if (lastPressedDigit != '\0' && digitPressCount > 0)
                {
                    int digitIndex = lastPressedDigit - '0';
                    if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[digitIndex][digitPressCount - 1] != '\0')
                    {
                        currentInput += keypadCharMapFinal[digitIndex][digitPressCount - 1];
                    }
                    else if (digitIndex >= 0 && digitIndex <= 9)
                    {
                        currentInput += lastPressedDigit;
                    }
                    // Reset multi-tap states after finalizing
                    lastDigitPressTime = 0;
                    lastPressedDigit = '\0';
                    digitPressCount = 0;
                }
                break;
            }
            else if (k == '*')
            {
                // FINALISASI KARAKTER MULTI-TAP SEBELUM BACKSPACE
                if (lastPressedDigit != '\0' && digitPressCount > 0)
                {
                    int digitIndex = lastPressedDigit - '0';
                    if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[digitIndex][digitPressCount - 1] != '\0')
                    {
                        currentInput += keypadCharMapFinal[digitIndex][digitPressCount - 1];
                    }
                    else if (digitIndex >= 0 && digitIndex <= 9)
                    {
                        currentInput += lastPressedDigit;
                    }
                    // Reset multi-tap states after finalizing
                    lastDigitPressTime = 0;
                    lastPressedDigit = '\0';
                    digitPressCount = 0;
                }

                if (!currentInput.isEmpty())
                {
                    currentInput.remove(currentInput.length() - 1);
                    updateInputField(currentInput);
                    Serial.print("\b \b");
                }
            }
            else if (isDigit(k))
            {
                unsigned long currentMillis = millis();
                if (k == lastPressedDigit && (currentMillis - lastDigitPressTime < MULTI_TAP_TIMEOUT))
                {
                    digitPressCount++;
                    int maxCharsForThisDigit = 0;
                    int digitIndex = k - '0';
                    if (digitIndex >= 0 && digitIndex <= 9)
                    {
                        for (int i = 0; i < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]); ++i)
                        {
                            if (keypadCharMapFinal[digitIndex][i] != '\0')
                            {
                                maxCharsForThisDigit++;
                            }
                        }
                    }
                    if (digitPressCount > maxCharsForThisDigit)
                        digitPressCount = 1;

                    if (!currentInput.isEmpty() && currentInput.length() > 0)
                    {
                        currentInput.remove(currentInput.length() - 1);
                        Serial.print("\b \b");
                    }
                }
                else
                {
                    if (lastPressedDigit != '\0' && digitPressCount > 0)
                    {
                        int prevDigitIndex = lastPressedDigit - '0';
                        if (prevDigitIndex >= 0 && prevDigitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[prevDigitIndex][digitPressCount - 1] != '\0')
                        {
                            currentInput += keypadCharMapFinal[prevDigitIndex][digitPressCount - 1];
                        }
                        else if (prevDigitIndex >= 0 && prevDigitIndex <= 9)
                        {
                            currentInput += lastPressedDigit;
                        }
                    }
                    lastPressedDigit = k;
                    digitPressCount = 1;
                }

                int digitIndex = lastPressedDigit - '0';
                char charToShow = '\0';
                if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]))
                {
                    charToShow = keypadCharMapFinal[digitIndex][digitPressCount - 1];
                    if (charToShow == '\0' && digitPressCount == 1)
                    {
                        charToShow = lastPressedDigit;
                    }
                    else if (charToShow == '\0')
                    {
                        charToShow = lastPressedDigit;
                    }
                }
                else
                {
                    charToShow = lastPressedDigit;
                }

                currentInput += charToShow;
                updateInputField(currentInput);
                Serial.print(charToShow);

                lastDigitPressTime = currentMillis;
            }
            else // Non-digit keys ( [, ], {, } ) in input mode
            {
                // FINALISASI KARAKTER MULTI-TAP SEBELUM TOMBOL NON-DIGIT
                if (lastPressedDigit != '\0' && digitPressCount > 0)
                {
                    int digitIndex = lastPressedDigit - '0';
                    if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[digitIndex][digitPressCount - 1] != '\0')
                    {
                        currentInput += keypadCharMapFinal[digitIndex][digitPressCount - 1];
                    }
                    else if (digitIndex >= 0 && digitIndex <= 9)
                    {
                        currentInput += lastPressedDigit;
                    }
                }
                currentInput += k; // Add the non-digit key directly
                updateInputField(currentInput);
                Serial.print(k);
                // Reset multi-tap states
                lastDigitPressTime = 0;
                lastPressedDigit = '\0';
                digitPressCount = 0;
            }
        }

        // Check for multi-tap timeout
        if (lastPressedDigit != '\0' && digitPressCount > 0 && (millis() - lastDigitPressTime >= MULTI_TAP_TIMEOUT))
        {
            int digitIndex = lastPressedDigit - '0';
            if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[digitIndex][digitPressCount - 1] != '\0')
            {
                // Character already added in the logic above
            }
            else if (digitIndex >= 0 && digitIndex <= 9)
            {
                // Character already added
            }
            Serial.println("\nMulti-tap timeout. Finalizing character.");
            lastDigitPressTime = 0;
            lastPressedDigit = '\0';
            digitPressCount = 0;
        }

        delay(50);
    }

    // FINAL CHECK: Finalisasi karakter multi-tap terakhir sebelum keluar dari fungsi
    if (lastPressedDigit != '\0' && digitPressCount > 0)
    {
        int digitIndex = lastPressedDigit - '0';
        if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[digitIndex][digitPressCount - 1] != '\0')
        {
            currentInput += keypadCharMapFinal[digitIndex][digitPressCount - 1];
        }
        else if (digitIndex >= 0 && digitIndex <= 9)
        {
            currentInput += lastPressedDigit;
        }
    }

    Serial.println();
    currentInput.trim();

    if (currentInput.length() > 0)
    {
        cekResi(currentInput);
    }

    delay(2000);
    updateStatusMessage("Ready");
    updateInputField("");
}

void setup()
{
    myServo.attach(servoPin);
    moveServoSmoothly(0, 45, 20); // Gerakkan servo dari 45° ke 0° dengan delay 20ms

    // myServo.write(45);
    Serial.begin(115200);
    EEPROM.begin(EEPROM_SIZE);
    ownerId = readOwnerId();
    Serial.println("Owner ID loaded: " + ownerId);

    // Initialize TFT display
    initDisplay();
    updateStatusMessage("Connecting to WiFi...");

    // WiFiManager configuration
    wifiManager.setConfigPortalTimeout(180); // 3 minutes timeout
    wifiManager.setAPCallback([](WiFiManager *myWiFiManager)
                              {
    Serial.println("Entered config mode");
    Serial.println(WiFi.softAPIP());
    Serial.println(myWiFiManager->getConfigPortalSSID());
    updateStatusMessage("WiFi Config Mode");
    updateInputField(myWiFiManager->getConfigPortalSSID()); });

    // Attempt to connect to saved WiFi or create AP for configuration
    if (!wifiManager.autoConnect("Locksy_Setup"))
    {
        Serial.println("Failed to connect and hit timeout");
        updateStatusMessage("WiFi Setup Failed");
        delay(3000);
        ESP.restart();
    }

    // If we get here, we're connected to WiFi
    Serial.println("Connected to WiFi");
    wifiConnected = true;
    drawStatusIndicators();
    updateStatusMessage("WiFi Connected");

    webSocket.begin(websocketAddress, 81, "/"); // Ganti dengan IP ESP32-CAM Anda
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    updateStatusMessage("Connecting to WebSocket...");

    // myServo.attach(servoPin);
    //  myServo.write(0); // Pastikan servo di posisi awal (tertutup)

    delay(1000);
    updateStatusMessage("Ready");
}

void loop()
{
    webSocket.loop();

    char key = keypad.getKey(); // Dapatkan tombol yang baru ditekan

    // --- LOGIC FOR BUTTON '[' (Original A - Triple-Click for Owner ID) ---
    if (key == '[')
    {
        unsigned long currentMillis = millis();
        if (currentMillis - lastPressA_millis < RAPID_PRESS_INTERVAL)
        {
            pressCountA++;
            Serial.print("[ pressed ");
            Serial.print(pressCountA);
            Serial.println(" times rapidly.");
        }
        else
        {
            pressCountA = 1;
            Serial.println("[ pressed (new sequence).");
        }
        lastPressA_millis = currentMillis;

        if (pressCountA >= 3)
        {
            Serial.println("Tombol [ ditekan 3 kali dengan cepat! Masuk mode input ID.");
            updateStatusMessage("[ 3x tapped! Entering ID mode.");
            inputOwnerId();  // Panggil fungsi yang diinginkan
            pressCountA = 0; // Reset hitungan setelah aksi
        }
        lastDigitPressTime = 0;
        lastPressedDigit = '\0';
        digitPressCount = 0;
        currentInput = ""; // Clear current input when starting a special function
        updateInputField(currentInput);
    }
    // --- END LOGIC FOR BUTTON '[' ---

    // --- LOGIC FOR BUTTON '{' (Original C - Triple-Click for Reset WiFi) ---
    else if (key == '{')
    {
        unsigned long currentMillis = millis();
        if (currentMillis - lastPressC_millis < RAPID_PRESS_INTERVAL)
        {
            pressCountC++;
            Serial.print("{ pressed ");
            Serial.print(pressCountC);
            Serial.println(" times rapidly.");
        }
        else
        {
            pressCountC = 1;
            Serial.println("{ pressed (new sequence).");
        }
        lastPressC_millis = currentMillis;

        if (pressCountC >= 3)
        {
            Serial.println("Tombol { ditekan 3 kali dengan cepat! Resetting WiFi settings...");
            updateStatusMessage("{ 3x tapped! Resetting WiFi.");
            resetWiFiSettings(); // Panggil fungsi yang diinginkan
            pressCountC = 0;     // Reset hitungan setelah aksi
        }
        lastDigitPressTime = 0;
        lastPressedDigit = '\0';
        digitPressCount = 0;
        currentInput = ""; // Clear current input when starting a special function
        updateInputField(currentInput);
    }
    // --- END LOGIC FOR BUTTON '{' ---

    // --- Penanganan tombol ']', '}', '*' dan '#' (tanpa multi-tap) atau angka (dengan multi-tap) ---
    else if (key) // Only proceed if a key was pressed and it's not '[' or '{'
    {
        // Tombol lain ditekan, batalkan hitungan rapid press untuk '[' dan '{'
        pressCountA = 0;
        pressCountC = 0;

        if (key == ']') // Original 'B'
        {
            Serial.println("Button ] pressed. Entering receipt input mode.");
            updateStatusMessage("Entering receipt input.");
            inputResi(' '); // Call inputResi with a dummy char to signify start
            lastDigitPressTime = 0;
            lastPressedDigit = '\0';
            digitPressCount = 0;
            currentInput = ""; // Clear current input before calling inputResi
            updateInputField(currentInput);
        }
        else if (key == '}') // Original 'D'
        {
            updateStatusMessage("Testing servo...");
            Serial.println("Testing servo movement");
            tesbukaSmartbox();
            delay(2000);
            testutupSmartbox();
            updateStatusMessage("Servo test complete");
            delay(1000);
            updateStatusMessage("Ready");
            lastDigitPressTime = 0;
            lastPressedDigit = '\0';
            digitPressCount = 0;
            currentInput = ""; // Clear current input after special function
            updateInputField(currentInput);
        }
        else if (key == '*')
        {
            // FINALISASI KARAKTER MULTI-TAP SEBELUM BACKSPACE DARI MAIN LOOP
            if (lastPressedDigit != '\0' && digitPressCount > 0)
            {
                int digitIndex = lastPressedDigit - '0';
                if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[digitIndex][digitPressCount - 1] != '\0')
                {
                    currentInput += keypadCharMapFinal[digitIndex][digitPressCount - 1];
                }
                else if (digitIndex >= 0 && digitIndex <= 9)
                {
                    currentInput += lastPressedDigit;
                }
            }
            if (!currentInput.isEmpty())
            {
                currentInput.remove(currentInput.length() - 1);
                updateInputField(currentInput);
                Serial.print("\b \b");
            }
            else
            {
                Serial.println("Button * pressed (no current input to delete).");
                updateStatusMessage("Press * to clear or start input.");
            }
            lastDigitPressTime = 0;
            lastPressedDigit = '\0';
            digitPressCount = 0;
        }
        else if (key == '#')
        {
            // FINALISASI KARAKTER MULTI-TAP SEBELUM SUBMIT DARI MAIN LOOP
            if (lastPressedDigit != '\0' && digitPressCount > 0)
            {
                int digitIndex = lastPressedDigit - '0';
                if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[digitIndex][digitPressCount - 1] != '\0')
                {
                    currentInput += keypadCharMapFinal[digitIndex][digitPressCount - 1];
                }
                else if (digitIndex >= 0 && digitIndex <= 9)
                {
                    currentInput += lastPressedDigit;
                }
            }
            // Jika ada input, kirimkan
            if (!currentInput.isEmpty())
            {
                Serial.println("Submitting current input: " + currentInput);
                updateStatusMessage("Submitting: " + currentInput);
                cekResi(currentInput);
                currentInput = ""; // Clear after submission
                updateInputField(currentInput);
                lastDigitPressTime = 0;
                lastPressedDigit = '\0';
                digitPressCount = 0;
            }
            else
            {
                Serial.println("Button # pressed (no current input). Entering receipt input mode.");
                updateStatusMessage("Entering receipt input.");
                inputResi(' '); // Call inputResi with a dummy char to signify start
                lastDigitPressTime = 0;
                lastPressedDigit = '\0';
                digitPressCount = 0;
                currentInput = ""; // Ensure it's clear
                updateInputField(currentInput);
            }
        }
        else if (isDigit(key)) // This is where the new multi-tap logic for digits starts
        {
            // If no input function is active, pressing a digit should start inputResi
            // We will *always* go into inputResi if a digit is pressed
            // So, this block here only handles initial input and then delegates
            // to inputResi for subsequent input.
            inputResi(key);
            // After inputResi returns, it will have cleared currentInput and reset multi-tap states
        }
    }

    // --- Penting: Bersihkan hitungan jika terlalu lama tidak ada penekanan ---
    unsigned long currentMillis = millis();
    if (pressCountA > 0 && currentMillis - lastPressA_millis >= RAPID_PRESS_INTERVAL)
    {
        Serial.println("[ rapid press sequence timed out.");
        pressCountA = 0;
    }
    if (pressCountC > 0 && currentMillis - lastPressC_millis >= RAPID_PRESS_INTERVAL)
    {
        Serial.println("{ rapid press sequence timed out.");
        pressCountC = 0;
    }

    // --- Check for multi-tap timeout for digit keys in the main loop ---
    // This block is now less critical as inputResi/inputOwnerId handle their own timeouts
    // However, if the user starts typing digits in the main loop without entering
    // inputResi/inputOwnerId, this will finalize the character.
    if (lastPressedDigit != '\0' && digitPressCount > 0 && (currentMillis - lastDigitPressTime >= MULTI_TAP_TIMEOUT))
    {
        int digitIndex = lastPressedDigit - '0';
        if (digitIndex >= 0 && digitIndex <= 9 && (digitPressCount - 1) < sizeof(keypadCharMapFinal[0]) / sizeof(keypadCharMapFinal[0][0]) && keypadCharMapFinal[digitIndex][digitPressCount - 1] != '\0')
        {
            currentInput += keypadCharMapFinal[digitIndex][digitPressCount - 1];
        }
        else if (digitIndex >= 0 && digitIndex <= 9)
        {
            currentInput += lastPressedDigit;
        }

        Serial.println("\nMulti-tap sequence timed out in main loop. Finalizing character.");
        updateInputField(currentInput); // Make sure display is updated
        lastDigitPressTime = 0;
        lastPressedDigit = '\0';
        digitPressCount = 0;
    }

    // Perintah dari Serial (manual)
    if (Serial.available())
    {
        char command = Serial.read();
        if (command == '1')
        {
            webSocket.sendTXT("TAKE_PHOTO");
            Serial.println("Sent: TAKE_PHOTO to ESP32-CAM");
            updateStatusMessage("Taking photo manually");
        }
    }

    delay(100);
}