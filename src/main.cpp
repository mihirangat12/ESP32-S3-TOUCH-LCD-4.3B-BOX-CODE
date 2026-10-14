#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // ArduinoJson v6
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>
#include <lvgl.h>
#include "driver/twai.h"

// Include EEZ Studio generated header
#include "ui/ui.h"

// ============================================================
// WIDGET NAME MAPPING  (edit these to match src/ui/screens.h)
// ============================================================
#define UI_SEARCH_BOX    objects.ta_search
#define UI_SEARCH_BTN    objects.btn_search
#define UI_RESULTS_BOX   objects.ta_results
#define UI_SEND_BTN      objects.btn_send

// ============================================================
// CAN Bus Configuration to External Microcontroller (ESP32-S3-WROOM-1U)
// ============================================================
#define CAN_TX_PIN      GPIO_NUM_15  // Updated from GPIO 43 to GPIO 15
#define CAN_RX_PIN      GPIO_NUM_16

#define CAN_COMMENT_ID   0x200
#define CAN_RESULT_ID    0x201
#define CAN_STATUS_ID    0x202

bool touchCanReady = false;

// ============================================================
// Display & Touch Hardware Configuration (Waveshare ESP32-S3 Touch LCD 4.3)
// ============================================================
#define LCD_WIDTH   800
#define LCD_HEIGHT  480

#define TOUCH_SDA   8
#define TOUCH_SCL   9
#define TOUCH_INT   4
#define TOUCH_RST  -1

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    5 /* DE */, 3 /* VSYNC */, 46 /* HSYNC */, 7 /* PCLK */,
    1 /* R0 */, 2 /* R1 */, 42 /* R2 */, 41 /* R3 */, 40 /* R4 */,
    39 /* G0 */, 0 /* G1 */, 45 /* G2 */, 48 /* G3 */, 47 /* G4 */, 21 /* G5 */,
    14 /* B0 */, 38 /* B1 */, 18 /* B2 */, 17 /* B3 */, 10 /* B4 */,
    0 /* hsync_polarity */, 40 /* hsync_front_porch */, 48 /* hsync_pulse_width */, 88 /* hsync_back_porch */,
    0 /* vsync_polarity */, 13 /* vsync_front_porch */, 3 /* vsync_pulse_width */, 32 /* vsync_back_porch */,
    1 /* pclk_active_neg */, 16000000 /* prefer_speed */
);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    LCD_WIDTH, LCD_HEIGHT, rgbpanel, 0 /* rotation */, true /* auto_flush */
);

TAMC_GT911 ts = TAMC_GT911(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, LCD_WIDTH, LCD_HEIGHT);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *disp_draw_buf = nullptr;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

static lv_obj_t *kb = nullptr;

// ============================================================
// JSON document allocator: use PSRAM when available, else heap
// ============================================================
struct SpiRamAllocator
{
    void *allocate(size_t size)
    {
        void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = malloc(size);
        return p;
    }
    void deallocate(void *ptr)
    {
        heap_caps_free(ptr);
    }
    void *reallocate(void *ptr, size_t new_size)
    {
        void *p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = realloc(ptr, new_size);
        return p;
    }
};
using SpiRamJsonDocument = BasicJsonDocument<SpiRamAllocator>;

// ============================================================
// LVGL callbacks
// ============================================================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
    lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    ts.read();
    if (ts.isTouched)
    {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = ts.points[0].x;
        data->point.y = ts.points[0].y;
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

void init_display_driver()
{
    Serial.println("[DISPLAY] Initializing I2C & Turning Backlight ON...");
    Wire.begin(TOUCH_SDA, TOUCH_SCL, 100000);
    delay(50);

    Wire.beginTransmission(0x24);
    Wire.write(0x01);
    Wire.endTransmission();

    Wire.beginTransmission(0x38);
    Wire.write(0xFF);
    Wire.endTransmission();

    Wire.beginTransmission(0x20);
    Wire.write(0x03);
    Wire.write(0x00);
    Wire.endTransmission();

    Wire.beginTransmission(0x20);
    Wire.write(0x01);
    Wire.write(0xFF);
    Wire.endTransmission();

    Serial.println("[DISPLAY] Initializing RGB Panel...");
    gfx->begin();
    gfx->fillScreen(BLACK);

    Serial.println("[TOUCH] Initializing GT911...");
    ts.begin();
    ts.setRotation(ROTATION_INVERTED);

    Serial.println("[LVGL] Initializing LVGL core...");
    lv_init();

    disp_draw_buf = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * LCD_WIDTH * 40, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!disp_draw_buf)
    {
        disp_draw_buf = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * LCD_WIDTH * 40, MALLOC_CAP_SPIRAM);
    }
    if (!disp_draw_buf)
    {
        disp_draw_buf = (lv_color_t *)malloc(sizeof(lv_color_t) * LCD_WIDTH * 40);
    }

    lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, LCD_WIDTH * 40);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_WIDTH;
    disp_drv.ver_res = LCD_HEIGHT;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    Serial.println("[DISPLAY] Display and Touch initialized successfully!");
}

// ============================================================
// Initialize CAN Driver on Touch LCD
// ============================================================
void setupTouchCAN() {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        if (twai_start() == ESP_OK) {
            touchCanReady = true;
            Serial.println("[TOUCH CAN] CAN Driver Started Successfully at 500 kbps");
        } else {
            Serial.println("[TOUCH CAN] Driver start failed");
            twai_driver_uninstall();
        }
    } else {
        Serial.println("[TOUCH CAN] Driver install failed");
    }
}

// Little-endian serialization helper
void touchWriteUint32(uint8_t *data, uint32_t value) {
    data[0] = value & 0xFF;
    data[1] = (value >> 8) & 0xFF;
    data[2] = (value >> 16) & 0xFF;
    data[3] = (value >> 24) & 0xFF;
}

// ============================================================
// Wi-Fi Configuration
// ============================================================
const char *WIFI_SSID     = "IoT_Dev";
const char *WIFI_PASSWORD = "masiotnet";

// ============================================================
// Backend API Endpoint
// ============================================================
const char *SERVER_BASE_URL = "http://20.20.22.23:5130/api/cutting/esp32";

// ============================================================
// Helpers
// ============================================================
static void set_result(const char *text)
{
    if (UI_RESULTS_BOX)
    {
        lv_textarea_set_text(UI_RESULTS_BOX, text);
    }
}

static String urlEncode(const String &s)
{
    String out;
    out.reserve(s.length() * 3);
    char buf[4];
    for (size_t i = 0; i < s.length(); i++)
    {
        uint8_t c = (uint8_t)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += (char)c;
        }
        else
        {
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// ============================================================
// On-Screen Keyboard Helpers
// ============================================================
static void show_keyboard(lv_obj_t *ta)
{
    if (kb == nullptr)
    {
        kb = lv_keyboard_create(lv_scr_act());
        lv_obj_set_size(kb, LCD_WIDTH, 220);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    }

    lv_keyboard_set_textarea(kb, ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);
}

static void hide_keyboard()
{
    if (kb != nullptr)
    {
        lv_keyboard_set_textarea(kb, NULL);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);

    if (code == LV_EVENT_CLICKED)
    {
        show_keyboard(ta);
    }
    else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
    {
        hide_keyboard();
    }
}

// ============================================================
// Fetch Marker Details From .NET API
// ============================================================
void fetchMarkerDetails(const char *searchKeyword)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        WiFi.disconnect();
        WiFi.reconnect();
        int retry = 0;
        while (WiFi.status() != WL_CONNECTED && retry < 20)
        {
            delay(500);
            retry++;
        }
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        set_result("Error: Wi-Fi Not Connected");
        return;
    }

    HTTPClient http;
    String url = String(SERVER_BASE_URL);

    if (searchKeyword != nullptr && strlen(searchKeyword) > 0)
    {
        url += "?search=" + urlEncode(String(searchKeyword));
    }

    if (!http.begin(url))
    {
        set_result("Error: HTTP Init Failed");
        return;
    }

    http.setTimeout(8000);
    http.useHTTP10(true);

    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK)
    {
        StaticJsonDocument<256> filter;
        filter["targets"][0]["start"] = true;
        filter["targets"][0]["end"]   = true;
        filter["fabricComments"]      = true;
        filter["fabricComment"]       = true;
        filter["fabric_comment"]      = true;
        filter["comment"]             = true;
        filter["comments"]            = true;

        SpiRamJsonDocument doc(32768);

        DeserializationError error = deserializeJson(
            doc,
            http.getStream(),
            DeserializationOption::Filter(filter));

        if (error)
        {
            String msg = String("JSON Error: ") + error.c_str();
            set_result(msg.c_str());
            http.end();
            return;
        }

        JsonArray targets = doc["targets"].as<JsonArray>();

        String fabricComment = "";
        if (doc.containsKey("fabricComments") && !doc["fabricComments"].isNull())
        {
            fabricComment = doc["fabricComments"].as<String>();
        }
        else if (doc.containsKey("fabricComment") && !doc["fabricComment"].isNull())
        {
            fabricComment = doc["fabricComment"].as<String>();
        }
        else if (doc.containsKey("fabric_comment") && !doc["fabric_comment"].isNull())
        {
            fabricComment = doc["fabric_comment"].as<String>();
        }
        else if (doc.containsKey("comment") && !doc["comment"].isNull())
        {
            fabricComment = doc["comment"].as<String>();
        }

        if ((targets.isNull() || targets.size() == 0) && fabricComment.length() == 0)
        {
            set_result("No matching records found.");
            http.end();
            return;
        }

        String resultText;
        resultText.reserve(512);

        if (fabricComment.length() > 0)
        {
            resultText += "Comment: " + fabricComment + "\n";
        }

        if (!targets.isNull() && targets.size() > 0)
        {
            for (JsonObject target : targets)
            {
                float start = target["start"] | 0.0f;
                float end   = target["end"]   | 0.0f;

                if (start == (int)start && end == (int)end)
                {
                    resultText += String((int)start) + "," + String((int)end) + "\n";
                }
                else
                {
                    resultText += String(start, 2) + "," + String(end, 2) + "\n";
                }
            }
        }

        set_result(resultText.c_str());
    }
    else
    {
        String errMsg = "HTTP Error: " + String(httpCode);
        set_result(errMsg.c_str());
    }

    http.end();
}

// ============================================================
// Search button callback
// ============================================================
static void btn_search_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    hide_keyboard();

    String searchText = "";
    if (UI_SEARCH_BOX)
    {
        searchText = String(lv_textarea_get_text(UI_SEARCH_BOX));
    }
    searchText.trim();

    if (searchText.length() == 0)
    {
        set_result("Please enter a marker name!");
        return;
    }

    if (UI_RESULTS_BOX)
    {
        lv_textarea_set_text(UI_RESULTS_BOX, "Loading...");
        lv_refr_now(NULL);
    }

    fetchMarkerDetails(searchText.c_str());
}

// ============================================================
// Send button callback: Transmit target ranges over CAN bus
// ============================================================
static void btn_send_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    hide_keyboard();

    String contentToSend = "";
    if (UI_RESULTS_BOX)
    {
        contentToSend = String(lv_textarea_get_text(UI_RESULTS_BOX));
    }
    contentToSend.trim();

    if (contentToSend.length() == 0 || contentToSend.startsWith("Error:") || contentToSend.startsWith("Loading..."))
    {
        Serial.println("[SEND] Nothing valid to send.");
        return;
    }

    if (!touchCanReady) {
        set_result("Error: CAN Bus Not Ready!");
        return;
    }

    // Parse lines from text area and send each range block over CAN
    int sentCount = 0;
    int lineStart = 0;
    uint32_t dummyId = 101; // Base comment/marker ID identifier

    while (lineStart < contentToSend.length()) {
        int lineEnd = contentToSend.indexOf('\n', lineStart);
        if (lineEnd == -1) lineEnd = contentToSend.length();
        
        String line = contentToSend.substring(lineStart, lineEnd);
        line.trim();
        lineStart = lineEnd + 1;

        int commaIdx = line.indexOf(',');
        if (commaIdx > 0) {
            float startVal = line.substring(0, commaIdx).toFloat();
            float endVal = line.substring(commaIdx + 1).toFloat();

            uint16_t startCm = (uint16_t)(startVal * 100.0f);
            uint16_t endCm = (uint16_t)(endVal * 100.0f);

            twai_message_t tx = {};
            tx.identifier = CAN_COMMENT_ID;
            tx.extd = 0;
            tx.rtr = 0;
            tx.data_length_code = 8;

            tx.data[0] = startCm & 0xFF;
            tx.data[1] = (startCm >> 8) & 0xFF;
            tx.data[2] = endCm & 0xFF;
            tx.data[3] = (endCm >> 8) & 0xFF;
            touchWriteUint32(&tx.data[4], dummyId++);

            if (twai_transmit(&tx, pdMS_TO_TICKS(50)) == ESP_OK) {
                sentCount++;
            }
        }
    }

    if (sentCount > 0) {
        Serial.printf("[TOUCH CAN] Successfully transmitted %d ranges over CAN.\n", sentCount);
        set_result("Ranges Sent to Controller via CAN!");
    } else {
        set_result("No numeric ranges found to send.");
    }
}

// ============================================================
// Wi-Fi Connection Setup
// ============================================================
void connectWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    esp_wifi_set_ps(WIFI_PS_NONE);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30)
    {
        delay(500);
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("\n[Wi-Fi] Connected! IP Address: " + WiFi.localIP().toString());
    }
}

// ============================================================
// SETUP
// ============================================================
void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n==========================================");
    Serial.println(" Waveshare ESP32-S3 Touch LCD 4.3 Starting ");
    Serial.println("==========================================");

    init_display_driver();
    setupTouchCAN();

    Serial.println("[UI] Initializing EEZ Studio UI...");
    ui_init();

    if (UI_SEARCH_BTN)
    {
        lv_obj_add_event_cb(UI_SEARCH_BTN, btn_search_event_cb, LV_EVENT_CLICKED, nullptr);
    }

    if (UI_SEND_BTN)
    {
        lv_obj_add_event_cb(UI_SEND_BTN, btn_send_event_cb, LV_EVENT_CLICKED, nullptr);
    }

    if (UI_SEARCH_BOX)
    {
        lv_textarea_set_one_line(UI_SEARCH_BOX, true);
        lv_obj_add_flag(UI_SEARCH_BOX, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(UI_SEARCH_BOX, ta_event_cb, LV_EVENT_ALL, NULL);
    }

    connectWiFi();
}

// ============================================================
// LOOP
// ============================================================
void loop()
{
    ui_tick();
    lv_timer_handler();

    // Optionally read feedback status/results coming back from WROOM-1U
    if (touchCanReady) {
        twai_message_t rxMsg;
        while (twai_receive(&rxMsg, 0) == ESP_OK) {
            if (rxMsg.identifier == CAN_RESULT_ID) {
                // Handle incoming feedback results if needed
            }
        }
    }

    delay(5);
}