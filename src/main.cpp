#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ==========================================
// CONFIGURATION PARAMETERS
// ==========================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Base API endpoint on backend
const char* BASE_URL      = "http://192.168.1.100:5000/api/cutting/esp32";

// Search filter state
char activeSearchQuery[64] = "IF2082-BULK-A-45-V4"; // Set default or leave "" for all

// Polling Interval
const unsigned long POLL_INTERVAL = 3000;
unsigned long lastFetchTime = 0;

struct CuttingTarget {
    char marker[64];
    float width;
    float start;
    float end;
};

// Simple URL Encoder for query parameter escaping
String urlEncode(const String& msg) {
    String encodedMsg = "";
    char buf[4];
    for (size_t i = 0; i < msg.length(); i++) {
        char c = msg.charAt(i);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encodedMsg += c;
        } else {
            sprintf(buf, "%%%02X", c);
            encodedMsg += buf;
        }
    }
    return encodedMsg;
}

void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.print("[WiFi] Connecting to ");
    Serial.println(WIFI_SSID);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("\n[WiFi] Connected successfully!");
    Serial.print("[WiFi] ESP32 IP Address: ");
    Serial.println(WiFi.localIP());
}

void fetchCuttingTargets(const char* searchQuery) {
    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
        return;
    }

    // Build URL with optional search filter query parameter
    String fullUrl = String(BASE_URL);
    if (searchQuery != NULL && strlen(searchQuery) > 0) {
        fullUrl += "?search=" + urlEncode(String(searchQuery));
    }

    HTTPClient http;
    http.begin(fullUrl);
    http.setTimeout(3000);

    Serial.print("[HTTP] Requesting targets from: ");
    Serial.println(fullUrl);

    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        
        DynamicJsonDocument doc(8192);
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            Serial.print("[JSON] Deserialization failed: ");
            Serial.println(error.c_str());
            http.end();
            return;
        }

        int count = doc["count"];
        JsonArray targets = doc["targets"].as<JsonArray>();

        Serial.println("==========================================");
        Serial.printf("[SYSTEM] Found %d Record(s) for Filter: '%s'\n", count, searchQuery);
        Serial.println("==========================================");

        for (JsonObject target : targets) {
            CuttingTarget t;
            strncpy(t.marker, target["marker"] | "Unknown", sizeof(t.marker) - 1);
            t.width = target["width"] | 0.0f;
            t.start = target["start"] | 0.0f;
            t.end   = target["end"]   | 0.0f;

            Serial.printf("  [MARKER] %-22s | Start: %6.2fm | End: %6.2fm\n", 
                          t.marker, t.start, t.end);
        }
        Serial.println("==========================================");

    } else {
        Serial.printf("[HTTP] GET request failed, status code: %d (%s)\n", 
                      httpCode, http.errorToString(httpCode).c_str());
    }

    http.end();
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n============================================");
    Serial.println("   FABRIC CUTTING SYSTEM - SEARCH FILTER    ");
    Serial.println("============================================");

    connectWiFi();
}

void loop() {
    unsigned long currentMillis = millis();

    if (currentMillis - lastFetchTime >= POLL_INTERVAL) {
        lastFetchTime = currentMillis;
        fetchCuttingTargets(activeSearchQuery);
    }
}