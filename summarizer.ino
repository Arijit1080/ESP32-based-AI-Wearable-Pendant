/*
ESP32 Audio Summarizer – Line-wise Code Overview
------------------------------------------------
~1–40     : Includes, global definitions, forward declarations
~41–90    : Pin mapping, RGB LED setup, status indication helpers
~91–160   : Audio configuration, buffers, recording state variables
~161–210  : Transcription data structures and system state machine
~211–280  : FreeRTOS tasks, semaphores, and mutex definitions
~281–360  : SPIFFS init, load/save transcriptions, storage cleanup
~361–470  : Transcription save logic with timestamps & metadata
~471–610  : Q&A and time-based summary using ChatGPT
~611–700  : Graceful shutdown and recovery handling
~701–1200 : Web UI (HTML/CSS/JS) dashboard
~1201–1380: REST API handlers (status, start/stop, query, search, export)
~1381–1510: WiFi config portal (SoftAP, save/load credentials)
~1511–1630: Timezone, NTP sync, timestamp formatting utilities
~1631–1750: I2S microphone init/deinit
~1751–1910: Whisper transcription pipeline
~1911–2030: ChatGPT summarization pipeline
~2031–2180: FreeRTOS transcription & summarization tasks
~2181–2280: setup() – system initialization & auto-start
~2281–END : loop() – audio capture, chunking, task signaling
*/


#include <Arduino.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include "secrets.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// ===== FORWARD DECLARATIONS =====
void saveSettings();
void setupWiFi();
void handleWiFiConfigRoot();
String transcribeSingleChunk(int16_t* chunk_buffer, int chunk_size, int chunk_number);
String formatTimestampLocal(time_t unix_time);
String formatTimestampISO8601(time_t unix_time);
String formatTimestamp12Hour(time_t unix_time);
String formatTimestampRelative(time_t unix_time);

// ===== PIN CONFIGURATION =====
const int CLK_PIN = 42;
const int DATA_PIN = 41;
const int RGB_RED_PIN = 5;
const int RGB_GREEN_PIN = 4;
const int RGB_BLUE_PIN = 3;

// ===== RGB COLOR DEFINITIONS =====
struct RGBColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

#define COLOR_OFF {0, 0, 0}
#define COLOR_IDLE {0, 255, 0}
#define COLOR_TRANSCRIBING {0, 0, 255}

uint8_t led_brightness_idle = 5;
uint8_t led_brightness_transcribing = 5;

// ===== AUDIO & BUFFER CONFIGURATION =====
#define SAMPLE_RATE 8000
#define CHUNK_SECONDS 30
const int chunk_samples = SAMPLE_RATE * CHUNK_SECONDS;
const int audio_buffer_size = chunk_samples * sizeof(int16_t);

int16_t* audio_buffer = NULL;
int16_t* i2s_temp_buffer = NULL;
int16_t* chunk_to_process = NULL;

volatile int samples_recorded = 0;
volatile bool is_recording = false;
volatile unsigned long chunk_start_time = 0;
volatile time_t overall_recording_start_unix = 0;
volatile bool chunk_ready = false;
volatile int chunk_size_to_process = 0;
volatile bool chunk_processing = false;
volatile bool ntp_synced = false;

// ===== TRANSCRIPTION & SUMMARY STORAGE =====
String latest_transcription = "";
String latest_summary = "";

// ===== RGB LED CONTROL =====
void setRGBColor(RGBColor color) {
    analogWrite(RGB_RED_PIN, 0);
    analogWrite(RGB_GREEN_PIN, color.g);
    analogWrite(RGB_BLUE_PIN, color.b);
}


void indicateIdle() {
    RGBColor green = COLOR_IDLE;
    green.g = led_brightness_idle;
    setRGBColor(green);
}

void indicateTranscribing() {
    RGBColor blue = COLOR_TRANSCRIBING;
    blue.b = led_brightness_transcribing;
    setRGBColor(blue);
}

// ===== WiFi & CONFIGURATION SETTINGS =====
const char* WIFI_CONFIG_FILE = "/wifi_config.json";
bool in_config_mode = false;
unsigned long config_mode_timeout = 0;
const unsigned long CONFIG_MODE_DURATION = 300000;

// ===== TIMEZONE & TIME SETTINGS =====
long timezone_offset_seconds = 19800;
String preferred_timezone = "IST";
time_t last_ntp_sync_time = 0;
const unsigned long NTP_SYNC_INTERVAL = 3600000;
bool dark_mode = false;
bool use_12_hour_format = true;
bool use_relative_time = false;

// ===== OPENAI API ENDPOINTS =====
const char* openai_host = "api.openai.com";
const char* whisper_endpoint = "https://api.openai.com/v1/audio/transcriptions";
const char* chatgpt_endpoint = "https://api.openai.com/v1/chat/completions";

// ===== WiFi CREDENTIALS =====
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ===== STORAGE CONFIGURATION =====
const char* TRANSCRIPTIONS_FILE = "/transcriptions.json";
const int IN_MEMORY_CACHE_SIZE = 20;
const unsigned long SPIFFS_WARNING_THRESHOLD = 50000;


// ===== STATE MANAGEMENT =====
enum State { S_IDLE, S_RECORDING, S_TRANSCRIBING, S_SUMMARIZING };
State currentState = S_IDLE;

// ===== WEB SERVER =====
WebServer server(80);

// ===== I2S MICROPHONE =====
I2SClass I2S;
i2s_chan_handle_t rx_channel = NULL;

// ===== TRANSCRIPTION STORAGE STRUCTURE =====
struct TranscriptionEntry {
    String timestamp;
    String timestamp_iso8601;
    String timestamp_12hour;
    String timestamp_relative;
    String transcription;
    String summary;
    String recording_duration;
    String recording_start_time;
    String recording_start_iso8601;
    String timezone_name;
    long unix_timestamp;
};

// ===== GLOBAL STORAGE VECTOR =====
std::vector<TranscriptionEntry> transcriptionStorage;

// ===== FreeRTOS TASK HANDLES =====
TaskHandle_t transcriptionTaskHandle = NULL;
TaskHandle_t summarizationTaskHandle = NULL;

// ===== FreeRTOS SYNCHRONIZATION PRIMITIVES =====
SemaphoreHandle_t audioBufferMutex = NULL;
SemaphoreHandle_t transcriptionReadySemaphore = NULL;
SemaphoreHandle_t summarizationReadySemaphore = NULL;
QueueHandle_t transcriptionQueue = NULL;
SemaphoreHandle_t processingCompleteSemaphore = NULL;

struct TranscriptionMessage {
    String transcription;
    int chunk_number;
    unsigned long timestamp;
};

// ===== USER SETTINGS STRUCTURE =====
struct Settings {
    String timezone_name;
    long timezone_offset;
    bool dark_mode;
    bool use_12_hour;
    bool use_relative_time;
} user_settings = {"IST", 19800, false, true, false};

// ===== WiFi CONFIG STRUCTURE =====
struct WiFiConfig {
    String ssid;
    String password;
} wifi_config;

// ===== FUNCTION PROTOTYPES =====
void createWavHeader(byte* header, int wavDataSize);
void initializeI2S();
void deinitializeI2S();
String transcribeWithWhisper(int audio_len);
String summarizeWithChatGPT(String transcription);
String answerQuestionAboutTranscriptions(String question);
String getTimeBasedSummary(String question, unsigned long timeWindowSeconds);
void saveTranscription(String transcription, String summary);
void loadTranscriptions();
void saveTranscriptionsToFile();
void displayStoredTranscriptions();
void setupWebServer();
void handleRoot();
void handleQuery();
void handleTranscriptions();
void handleStatus();
void handleStartRecording();
void handleStopRecording();
void handleStopRecording();
void processRecoveredChunk();
void gracefulShutdown();
void transcriptionTask(void* parameter);
void summarizationTask(void* parameter);
void initializeFreeRTOSTasks();
void cleanupFreeRTOSTasks();

// ===== SPIFFS INITIALIZATION =====
void initSPIFFS() {
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS Mount Failed");
        return;
    }
    Serial.println("SPIFFS Mounted successfully");
}

// ===== LOAD TRANSCRIPTIONS FROM SPIFFS =====
void loadTranscriptions() {
    if (!SPIFFS.exists(TRANSCRIPTIONS_FILE)) {
        Serial.println("No transcriptions file found, starting fresh");
        return;
    }
    
    File file = SPIFFS.open(TRANSCRIPTIONS_FILE, "r");
    if (!file) {
        Serial.println("Failed to open transcriptions file");
        return;
    }
    
    String content = "";
    while (file.available()) {
        content += (char)file.read();
    }
    file.close();
    
    JsonDocument doc;
    if (deserializeJson(doc, content) != DeserializationError::Ok) {
        Serial.println("Failed to parse transcriptions JSON");
        return;
    }
    
    transcriptionStorage.clear();
    JsonArray arr = doc["transcriptions"];
    int totalCount = arr.size();
    int startIndex = max(0, totalCount - IN_MEMORY_CACHE_SIZE);
    
    // Keep only the latest IN_MEMORY_CACHE_SIZE entries in RAM
    for (int i = startIndex; i < totalCount; i++) {
        JsonObject item = arr[i];
        TranscriptionEntry entry;
        entry.timestamp = item["timestamp"].as<String>();
        entry.transcription = item["transcription"].as<String>();
        entry.summary = item["summary"].as<String>();
        entry.recording_duration = item["recording_duration"].as<String>();
        entry.recording_start_time = item["recording_start_time"].as<String>();
        transcriptionStorage.push_back(entry);
    }
    
    Serial.printf("Loaded %d/%d transcriptions into cache (showing latest %d)\n", 
                  transcriptionStorage.size(), totalCount, transcriptionStorage.size());
}

// ===== CHECK & CLEANUP SPIFFS IF SPACE LOW =====
void checkAndCleanupSPIFFS() {
    unsigned long totalBytes = SPIFFS.totalBytes();
    unsigned long usedBytes = SPIFFS.usedBytes();
    unsigned long freeSpace = totalBytes - usedBytes;
    
    Serial.printf("SPIFFS: %lu bytes used, %lu bytes free\n", usedBytes, freeSpace);
    
    if (freeSpace < SPIFFS_WARNING_THRESHOLD) {
        Serial.println("SPIFFS low on space, cleaning up oldest transcriptions...");
        
        File file = SPIFFS.open(TRANSCRIPTIONS_FILE, "r");
        if (!file) return;
        
        String content = "";
        while (file.available()) {
            content += (char)file.read();
        }
        file.close();
        
        JsonDocument doc;
        if (deserializeJson(doc, content) != DeserializationError::Ok) return;
        
        JsonArray arr = doc["transcriptions"];
        if (arr.size() > 100) {
            JsonArray newArr = doc.createNestedArray("transcriptions_new");
            int startIdx = arr.size() - 100;
            for (int i = startIdx; i < arr.size(); i++) {
                newArr.add(arr[i]);
            }
            
            doc.remove("transcriptions");
            JsonArray finalArr = doc.createNestedArray("transcriptions");
            for (JsonObject item : newArr) {
                finalArr.add(item);
            }
            
            File writeFile = SPIFFS.open(TRANSCRIPTIONS_FILE, "w");
            serializeJson(doc, writeFile);
            writeFile.close();
            
            Serial.printf("Cleanup complete: kept last 100 entries\n");
        }
    }
}

// ===== SAVE TRANSCRIPTIONS TO FILE =====
void saveTranscriptionsToFile() {
    checkAndCleanupSPIFFS();
}

// ===== SAVE NEW TRANSCRIPTION =====
void saveTranscription(String transcription, String summary) {
    time_t now = time(nullptr);
    
    // Format timestamps in multiple formats
    String timestamp_local = formatTimestampLocal(now);
    String timestamp_iso = formatTimestampISO8601(now);
    String timestamp_12hr = use_12_hour_format ? formatTimestamp12Hour(now) : timestamp_local;
    String timestamp_rel = use_relative_time ? formatTimestampRelative(now) : "";
    
    // Calculate recording duration
    unsigned long chunk_elapsed_ms = millis() - chunk_start_time;
    int seconds = (chunk_elapsed_ms / 1000) % 60;
    int minutes = (chunk_elapsed_ms / 60000) % 60;
    
    char duration_str[30];
    sprintf(duration_str, "%dm %ds", minutes, seconds);
    
    // Get recording start time in local timezone
    String start_time_local = formatTimestampLocal(overall_recording_start_unix);
    String start_time_iso = formatTimestampISO8601(overall_recording_start_unix);
    
    TranscriptionEntry entry;
    entry.timestamp = timestamp_local;
    entry.timestamp_iso8601 = timestamp_iso;
    entry.timestamp_12hour = timestamp_12hr;
    entry.timestamp_relative = timestamp_rel;
    entry.transcription = transcription;
    entry.summary = summary;
    entry.recording_duration = String(duration_str);
    entry.recording_start_time = start_time_local;
    entry.recording_start_iso8601 = start_time_iso;
    entry.timezone_name = preferred_timezone;
    entry.unix_timestamp = now;
    
    // Add to in-memory cache
    transcriptionStorage.push_back(entry);
    
    // Keep only the latest IN_MEMORY_CACHE_SIZE entries in RAM
    if (transcriptionStorage.size() > IN_MEMORY_CACHE_SIZE) {
        transcriptionStorage.erase(transcriptionStorage.begin());
    }
    
    // Append to file (all entries go to persistent storage)
    File file = SPIFFS.open(TRANSCRIPTIONS_FILE, "r");
    JsonDocument fullDoc;
    
    if (file) {
        String content = "";
        while (file.available()) {
            content += (char)file.read();
        }
        file.close();
        deserializeJson(fullDoc, content);
    }
    
    JsonArray arr = fullDoc["transcriptions"].isNull() ? 
                    fullDoc.createNestedArray("transcriptions") : 
                    fullDoc["transcriptions"];
    
    JsonObject newItem = arr.createNestedObject();
    newItem["timestamp"] = entry.timestamp;
    newItem["timestamp_iso8601"] = entry.timestamp_iso8601;
    newItem["timestamp_12hour"] = entry.timestamp_12hour;
    newItem["timestamp_relative"] = entry.timestamp_relative;
    newItem["transcription"] = entry.transcription;
    newItem["summary"] = entry.summary;
    newItem["recording_duration"] = entry.recording_duration;
    newItem["recording_start_time"] = entry.recording_start_time;
    newItem["recording_start_iso8601"] = entry.recording_start_iso8601;
    newItem["timezone_name"] = entry.timezone_name;
    newItem["unix_timestamp"] = entry.unix_timestamp;
    
    // Write all transcriptions back
    File writeFile = SPIFFS.open(TRANSCRIPTIONS_FILE, "w");
    if (writeFile) {
        serializeJson(fullDoc, writeFile);
        writeFile.close();
    }
    
    // Check if cleanup is needed
    checkAndCleanupSPIFFS();
}

// ===== DISPLAY STORED TRANSCRIPTIONS =====
void displayStoredTranscriptions() {
    Serial.println("\n========== STORED TRANSCRIPTIONS ==========");
    if (transcriptionStorage.empty()) {
        Serial.println("No transcriptions stored yet.");
        return;
    }
    
    for (int i = 0; i < transcriptionStorage.size(); i++) {
        Serial.printf("\n[Entry %d] %s\n", i + 1, transcriptionStorage[i].timestamp.c_str());
        Serial.print("Recording Start: ");
        Serial.println(transcriptionStorage[i].recording_start_time);
        Serial.print("Duration: ");
        Serial.println(transcriptionStorage[i].recording_duration);
        Serial.print("Transcription: ");
        Serial.println(transcriptionStorage[i].transcription);
        Serial.print("Summary: ");
        Serial.println(transcriptionStorage[i].summary);
        Serial.println("---");
    }
    Serial.println("==========================================\n");
}

// ===== GET TIME-BASED SUMMARY FROM TRANSCRIPTIONS =====
String getTimeBasedSummary(String question, unsigned long timeWindowSeconds) {
    if (transcriptionStorage.empty()) {
        return "No transcriptions available.";
    }
    
    time_t currentTime = time(nullptr);
    time_t cutoffTime = currentTime - timeWindowSeconds;
    
    String filteredTranscriptions = "";
    int matchCount = 0;
    
    for (int i = 0; i < transcriptionStorage.size(); i++) {
        if (transcriptionStorage[i].unix_timestamp >= cutoffTime && 
            transcriptionStorage[i].unix_timestamp <= currentTime) {
            matchCount++;
            filteredTranscriptions += "Transcription " + String(matchCount) + ":\n";
            filteredTranscriptions += "  Time: " + transcriptionStorage[i].timestamp + "\n";
            filteredTranscriptions += "  Text: " + transcriptionStorage[i].transcription + "\n";
            filteredTranscriptions += "  Summary: " + transcriptionStorage[i].summary + "\n\n";
        }
    }
    
    if (matchCount == 0) {
        return "No transcriptions found within the specified time period.";
    }
    
    StaticJsonDocument<3000> doc;
    doc["model"] = "gpt-4o-mini";
    doc["max_tokens"] = 800;
    
    JsonArray messages = doc.createNestedArray("messages");
    
    JsonObject systemMsg = messages.createNestedObject();
    systemMsg["role"] = "system";
    systemMsg["content"] = "You are a helpful assistant. Provide a comprehensive summary based ONLY on the provided transcriptions from the specified time period. Be concise but informative.";
    
    JsonObject contextMsg = messages.createNestedObject();
    contextMsg["role"] = "user";
    contextMsg["content"] = "Summarize what happened during this time period based on these transcriptions:\n\n" + filteredTranscriptions;
    
    String requestBody;
    serializeJson(doc, requestBody);
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    
    if (!http.begin(client, chatgpt_endpoint)) {
        Serial.println("Failed to start HTTP connection for time-based summary");
        return "Error: Could not connect to ChatGPT API";
    }
    
    http.addHeader("Authorization", "Bearer " + String(OPENAI_API_KEY));
    http.addHeader("Content-Type", "application/json");
    http.setConnectTimeout(10000);
    http.setTimeout(60000);
    
    int httpCode = http.POST(requestBody);
    String answer = "";
    
    if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        JsonDocument responseDoc;
        
        if (deserializeJson(responseDoc, response) == DeserializationError::Ok) {
            if (responseDoc.containsKey("choices") && responseDoc["choices"].size() > 0) {
                answer = responseDoc["choices"][0]["message"]["content"].as<String>();
            } else {
                answer = "Error: Invalid response format from ChatGPT";
            }
        } else {
            answer = "Error: Failed to parse ChatGPT response";
        }
    } else {
        answer = "Error: ChatGPT API request failed (HTTP " + String(httpCode) + ")";
    }
    
    http.end();
    return answer;
}

// ===== ANSWER QUESTIONS ABOUT TRANSCRIPTIONS =====
String answerQuestionAboutTranscriptions(String question) {
    if (transcriptionStorage.empty()) {
        return "No transcriptions available to answer questions about.";
    }
    
    // Check for time-based queries
    unsigned long timeWindow = 0;
    
    if (question.indexOf("last hour") != -1 || question.indexOf("last 1 hour") != -1 || question.indexOf("past hour") != -1) {
        timeWindow = 3600;
    } 
    else if (question.indexOf("last 2 hours") != -1 || question.indexOf("past 2 hours") != -1) {
        timeWindow = 7200;
    } 
    else if (question.indexOf("last 3 hours") != -1 || question.indexOf("past 3 hours") != -1) {
        timeWindow = 10800;
    } 
    else if (question.indexOf("last day") != -1 || question.indexOf("past day") != -1 || question.indexOf("last 24 hours") != -1) {
        timeWindow = 86400;
    } 
    else if (question.indexOf("last week") != -1 || question.indexOf("past week") != -1 || question.indexOf("last 7 days") != -1) {
        timeWindow = 604800;
    } 
    else if (question.indexOf("last 30 minutes") != -1 || question.indexOf("past 30 minutes") != -1) {
        timeWindow = 1800;
    } 
    else if (question.indexOf("last 15 minutes") != -1 || question.indexOf("past 15 minutes") != -1) {
        timeWindow = 900;
    } 
    else if (question.indexOf("last 5 minutes") != -1 || question.indexOf("past 5 minutes") != -1) {
        timeWindow = 300;
    }
    
    if (timeWindow > 0) {
        return getTimeBasedSummary(question, timeWindow);
    }
    
    // General question answering with all transcriptions
    String allTranscriptions = "";
    for (int i = 0; i < transcriptionStorage.size(); i++) {
        allTranscriptions += "Transcription " + String(i + 1) + ":\n";
        allTranscriptions += "  Timestamp: " + transcriptionStorage[i].timestamp + "\n";
        allTranscriptions += "  Recording started: " + transcriptionStorage[i].recording_start_time + "\n";
        allTranscriptions += "  Duration: " + transcriptionStorage[i].recording_duration + "\n";
        allTranscriptions += "  Text: " + transcriptionStorage[i].transcription + "\n";
        allTranscriptions += "  Summary: " + transcriptionStorage[i].summary + "\n\n";
    }
    
    StaticJsonDocument<3000> doc;
    doc["model"] = "gpt-4o-mini";
    doc["max_tokens"] = 500;
    
    JsonArray messages = doc.createNestedArray("messages");
    
    JsonObject systemMsg = messages.createNestedObject();
    systemMsg["role"] = "system";
    systemMsg["content"] = "You are a helpful assistant. Answer the user's question based ONLY on the provided transcriptions and their timing information. Include specific times and durations when relevant. If the answer is not in the transcriptions, say 'Information not found in the transcriptions.'";
    
    JsonObject contextMsg = messages.createNestedObject();
    contextMsg["role"] = "user";
    contextMsg["content"] = "Here are the available transcriptions with timing information:\n\n" + allTranscriptions;
    
    JsonObject queryMsg = messages.createNestedObject();
    queryMsg["role"] = "user";
    queryMsg["content"] = question;
    
    String requestBody;
    serializeJson(doc, requestBody);
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    
    if (!http.begin(client, chatgpt_endpoint)) {
        Serial.println("Failed to start HTTP connection for question answering");
        return "Error: Could not connect to ChatGPT API";
    }
    
    http.addHeader("Authorization", "Bearer " + String(OPENAI_API_KEY));
    http.addHeader("Content-Type", "application/json");
    http.setConnectTimeout(10000);
    http.setTimeout(60000);
    
    int httpCode = http.POST(requestBody);
    String answer = "";
    
    if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        JsonDocument responseDoc;
        
        if (deserializeJson(responseDoc, response) == DeserializationError::Ok) {
            if (responseDoc.containsKey("choices") && responseDoc["choices"].size() > 0) {
                answer = responseDoc["choices"][0]["message"]["content"].as<String>();
            } else {
                answer = "Error: Invalid response format from ChatGPT";
            }
        } else {
            answer = "Error: Failed to parse ChatGPT response";
        }
    } else if (httpCode == -1) {
        answer = "Error: Connection timeout - Check WiFi and API availability";
    } else {
        answer = "Error: ChatGPT API returned " + String(httpCode);
    }
    
    http.end();
    return answer;
}

// ===== POWER LOSS RECOVERY SYSTEM =====
void processRecoveredChunk() {
    Serial.println("[STARTUP] Simple mode - no recovery processing");
}

// ===== GRACEFUL SHUTDOWN HANDLER =====
void gracefulShutdown() {
    Serial.println("\n[SHUTDOWN] Graceful shutdown initiated!");
    
    is_recording = false;
    
    RGBColor yellow = {255, 255, 0};
    setRGBColor(yellow);
    
    Serial.println("[SHUTDOWN] Saving settings...");
    saveSettings();
    
    Serial.println("[SHUTDOWN] Deinitializing I2S...");
    deinitializeI2S();
    
    Serial.println("[SHUTDOWN] Cleaning up FreeRTOS resources...");
    cleanupFreeRTOSTasks();
    
    Serial.println("[SHUTDOWN] Freeing memory buffers...");
    if (audio_buffer != NULL) {
        free(audio_buffer);
        audio_buffer = NULL;
    }
    if (i2s_temp_buffer != NULL) {
        free(i2s_temp_buffer);
        i2s_temp_buffer = NULL;
    }
    if (chunk_to_process != NULL) {
        free(chunk_to_process);
        chunk_to_process = NULL;
    }
    
    Serial.println("[SHUTDOWN] All systems safely shut down!");
    
    while (true) {
        delay(1000);
    }
}

// ===== WEB HANDLER: ROOT PAGE =====
void handleRoot() {
    if (in_config_mode) {
        handleWiFiConfigRoot();
        return;
    }
    
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Audio Summarizer</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css">
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        
        :root {
            --primary: #667eea;
            --secondary: #764ba2;
            --success: #4CAF50;
            --danger: #f44336;
            --bg: #f5f5f5;
            --card: #ffffff;
            --text: #333;
            --text-light: #666;
        }
        
        body.dark-mode {
            --primary: #7a8fee;
            --secondary: #8b5db8;
            --bg: #1e1e2e;
            --card: #2d2d44;
            --text: #e0e0e0;
            --text-light: #b0b0b0;
        }
        
        body {
            font-family: 'Segoe UI', Arial, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 50%, #f093fb 100%);
            background-attachment: fixed;
            background-size: 400% 400%;
            animation: gradientShift 15s ease infinite;
            min-height: 100vh;
            padding: 20px;
            color: var(--text);
            transition: background 0.3s;
            position: relative;
            overflow-x: hidden;
        }
        
        @keyframes gradientShift {
            0% {
                background-position: 0% 50%;
            }
            50% {
                background-position: 100% 50%;
            }
            100% {
                background-position: 0% 50%;
            }
        }
        
        body.dark-mode {
            background: linear-gradient(135deg, #1e1e2e 0%, #2d2d44 50%, #3d3d5c 100%);
            background-attachment: fixed;
            background-size: 400% 400%;
            animation: darkGradientShift 15s ease infinite;
        }
        
        @keyframes darkGradientShift {
            0% {
                background-position: 0% 50%;
            }
            50% {
                background-position: 100% 50%;
            }
            100% {
                background-position: 0% 50%;
            }
        }
        
        /* Floating animation for visual interest */
        @keyframes float {
            0%, 100% {
                transform: translateY(0px);
            }
            50% {
                transform: translateY(-20px);
            }
        }
        
        .container {
            max-width: 1000px;
            margin: 0 auto;
            animation: slideInDown 0.6s ease;
        }
        
        @keyframes slideInDown {
            from {
                opacity: 0;
                transform: translateY(-30px);
            }
            to {
                opacity: 1;
                transform: translateY(0);
            }
        }
        
        header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 30px;
            background: var(--card);
            padding: 20px;
            border-radius: 10px;
            box-shadow: 0 5px 15px rgba(0,0,0,0.1);
            flex-wrap: wrap;
            gap: 15px;
            animation: slideInDown 0.6s ease 0.1s both;
            border: 2px solid rgba(102, 126, 234, 0.1);
            position: relative;
            overflow: hidden;
        }
        
        header::before {
            content: '';
            position: absolute;
            top: -50%;
            right: -50%;
            width: 200px;
            height: 200px;
            background: radial-gradient(circle, rgba(102, 126, 234, 0.1) 0%, transparent 70%);
            animation: float 6s ease-in-out infinite;
        }
        
        h1 {
            font-size: 28px;
            flex: 1;
            min-width: 200px;
            position: relative;
            z-index: 1;
        }
        
        .header-controls {
            display: flex;
            gap: 10px;
            flex-wrap: wrap;
            position: relative;
            z-index: 1;
        }
        
        .icon-btn {
            width: 45px;
            height: 45px;
            border: none;
            border-radius: 50%;
            background: linear-gradient(135deg, var(--primary) 0%, var(--secondary) 100%);
            color: white;
            cursor: pointer;
            font-size: 18px;
            transition: all 0.3s;
            display: flex;
            align-items: center;
            justify-content: center;
            box-shadow: 0 4px 15px rgba(102, 126, 234, 0.3);
            position: relative;
            overflow: hidden;
        }
        
        .icon-btn::before {
            content: '';
            position: absolute;
            top: 50%;
            left: 50%;
            width: 0;
            height: 0;
            border-radius: 50%;
            background: rgba(255, 255, 255, 0.3);
            transform: translate(-50%, -50%);
            transition: width 0.6s, height 0.6s;
        }
        
        .icon-btn:hover::before {
            width: 300px;
            height: 300px;
        }
        
        .icon-btn:hover {
            background: linear-gradient(135deg, var(--secondary) 0%, var(--primary) 100%);
            transform: scale(1.15) rotate(10deg);
            box-shadow: 0 6px 20px rgba(118, 75, 162, 0.4);
        }
        
        .tabs {
            display: flex;
            gap: 0;
            margin-bottom: 20px;
            border-radius: 10px;
            overflow: hidden;
            box-shadow: 0 5px 15px rgba(0,0,0,0.1);
            animation: slideInDown 0.6s ease 0.2s both;
            background: var(--bg);
        }
        
        .tab-btn {
            flex: 1;
            padding: 15px 20px;
            background: var(--bg);
            border: none;
            color: var(--text-light);
            cursor: pointer;
            font-size: 14px;
            font-weight: 600;
            transition: all 0.3s ease;
            position: relative;
            overflow: hidden;
        }
        
        .tab-btn::before {
            content: '';
            position: absolute;
            bottom: 0;
            left: -100%;
            width: 100%;
            height: 3px;
            background: linear-gradient(90deg, transparent, var(--primary), transparent);
            transition: left 0.3s ease;
        }
        
        .tab-btn:hover::before {
            left: 100%;
        }
        
        .tab-btn.active {
            background: linear-gradient(135deg, var(--primary), var(--secondary));
            color: white;
            transform: scale(1.02);
            box-shadow: 0 0 20px rgba(102, 126, 234, 0.3);
        }
        
        .tab-content {
            display: none;
            animation: fadeInScale 0.4s ease;
        }
        
        .tab-content.active {
            display: block;
        }
        
        @keyframes fadeIn {
            from { opacity: 0; }
            to { opacity: 1; }
        }
        
        @keyframes fadeInScale {
            from {
                opacity: 0;
                transform: scale(0.98);
            }
            to {
                opacity: 1;
                transform: scale(1);
            }
        }
        
        .section {
            background: var(--card);
            border-radius: 10px;
            padding: 25px;
            margin-bottom: 20px;
            box-shadow: 0 5px 15px rgba(0,0,0,0.1);
            border-left: 4px solid var(--primary);
            animation: slideInUp 0.6s ease;
            transition: all 0.3s ease;
            position: relative;
            overflow: hidden;
        }
        
        .section::before {
            content: '';
            position: absolute;
            top: -50%;
            right: -50%;
            width: 200px;
            height: 200px;
            background: radial-gradient(circle, rgba(102, 126, 234, 0.05) 0%, transparent 70%);
            animation: float 8s ease-in-out infinite;
        }
        
        @keyframes slideInUp {
            from {
                opacity: 0;
                transform: translateY(30px);
            }
            to {
                opacity: 1;
                transform: translateY(0);
            }
        }
        
        .section:hover {
            transform: translateY(-5px);
            box-shadow: 0 10px 25px rgba(102, 126, 234, 0.2);
        }
        
        .section h2 {
            font-size: 20px;
            margin-bottom: 15px;
            color: var(--primary);
            position: relative;
            z-index: 1;
        }
        
        .section p {
            color: var(--text-light);
            position: relative;
            z-index: 1;
            margin-bottom: 15px;
        }
        
        .form-group {
            margin-bottom: 15px;
        }
        
        .form-group label {
            display: block;
            margin-bottom: 8px;
            font-weight: 600;
            color: var(--text);
        }
        
        input[type="text"],
        input[type="date"],
        input[type="time"],
        input[type="number"],
        select {
            width: 100%;
            padding: 12px;
            border: 2px solid var(--bg);
            border-radius: 5px;
            font-size: 14px;
            background: var(--card);
            color: var(--text);
            transition: border-color 0.3s;
        }
        
        input:focus, select:focus {
            outline: none;
            border-color: var(--primary);
        }
        
        .form-row {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
            gap: 15px;
        }
        
        .checkbox-group {
            display: flex;
            gap: 20px;
            flex-wrap: wrap;
            margin: 15px 0;
        }
        
        .checkbox-label {
            display: flex;
            align-items: center;
            gap: 8px;
            cursor: pointer;
            font-size: 14px;
        }
        
        .checkbox-label input {
            width: auto;
            margin: 0;
        }
        
        .button-group {
            display: flex;
            gap: 10px;
            margin-bottom: 20px;
            flex-wrap: wrap;
        }
        
        .btn {
            padding: 12px 25px;
            background: var(--primary);
            color: white;
            border: none;
            border-radius: 5px;
            cursor: pointer;
            font-size: 14px;
            font-weight: 600;
            transition: all 0.3s;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        
        .btn:hover {
            background: var(--secondary);
            transform: translateY(-2px);
        }
        
        .btn-success {
            background: var(--success);
        }
        
        .btn-success:hover {
            background: #45a049;
        }
        
        .btn-danger {
            background: var(--danger);
        }
        
        .btn-danger:hover {
            background: #da190b;
        }
        
        .btn-small {
            padding: 8px 15px;
            font-size: 12px;
        }
        
        .status-badge {
            display: inline-block;
            padding: 6px 12px;
            border-radius: 20px;
            font-size: 12px;
            font-weight: 600;
            margin-top: 10px;
        }
        
        .status-synced {
            background: rgba(76, 175, 80, 0.2);
            color: var(--success);
        }
        
        .status-unsynced {
            background: rgba(244, 67, 54, 0.2);
            color: var(--danger);
        }
        
        .transcription-item {
            background: var(--bg);
            padding: 15px;
            margin-bottom: 15px;
            border-radius: 5px;
            border-left: 4px solid var(--primary);
            animation: slideIn 0.3s ease;
        }
        
        .transcription-item h4 {
            color: var(--primary);
            margin-bottom: 10px;
        }
        
        .transcription-item p {
            margin: 8px 0;
            font-size: 13px;
            color: var(--text-light);
        }
        
        .result {
            background: var(--bg);
            padding: 15px;
            border-radius: 5px;
            margin-top: 15px;
            border-left: 4px solid var(--success);
            display: none;
        }
        
        .result.show {
            display: block;
            animation: slideIn 0.3s ease;
        }
        
        .result.error {
            border-left-color: var(--danger);
        }
        
        .loading {
            text-align: center;
            color: var(--primary);
            font-weight: bold;
            display: none;
        }
        
        .loading.show {
            display: block;
        }
        
        @keyframes slideIn {
            from { opacity: 0; transform: translateY(-10px); }
            to { opacity: 1; transform: translateY(0); }
        }
        
        .no-data {
            text-align: center;
            padding: 40px 20px;
            color: var(--text-light);
        }
        
        @media (max-width: 600px) {
            header {
                flex-direction: column;
                align-items: flex-start;
            }
            
            h1 {
                font-size: 22px;
            }
            
            .tabs {
                flex-direction: column;
            }
            
            .tab-btn {
                text-align: left;
            }
            
            .form-row {
                grid-template-columns: 1fr;
            }
            
            input, select {
                font-size: 16px;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1><i class="fas fa-microphone"></i> Summarizing Pendant</h1>
            <div class="header-controls">
                <button class="icon-btn" id="darkModeBtn" onclick="toggleDarkMode()" title="Dark Mode"><i class="fas fa-moon"></i></button>
                <button class="icon-btn" onclick="openSettings()" title="Settings"><i class="fas fa-cog"></i></button>
            </div>
        </header>
        
        <!-- LIVE STATUS SECTION -->
        <div id="statusSection" class="section" style="background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; border-left: none;">
            <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 20px;">
                <div style="flex: 1; min-width: 200px;">
                    <h3 style="margin-bottom: 10px; color: white;"><i class="fas fa-circle-notch fa-spin"></i> Status</h3>
                    <div style="font-size: 24px; font-weight: bold; margin-bottom: 5px;">
                        <span id="liveState" style="color: #4CAF50;"><i class="fas fa-circle" style="display: inline-block; margin-right: 5px; font-size: 10px;"></i>READY</span>
                    </div>
                    <div style="font-size: 12px; opacity: 0.9;">
                        <div>Recording: <span id="recordingStatus">Waiting...</span></div>
                        <div>WiFi: <span id="wifiStatus">Checking...</span></div>
                        <div id="recordingBar" style="margin-top: 8px; height: 8px; background: rgba(255,255,255,0.2); border-radius: 4px; overflow: hidden;">
                            <div id="recordingProgress" style="height: 100%; width: 0%; background: #4CAF50; transition: width 0.3s;"></div>
                        </div>
                        <div id="recordingPercent" style="font-size: 11px; margin-top: 3px;">0% complete</div>
                    </div>
                </div>
                <div style="display: flex; gap: 10px; flex-wrap: wrap;">
                    <button class="btn" id="toggleBtn" onclick="toggleRecording()" style="white-space: nowrap; background: #4CAF50;" title="Toggle recording">
                        <i class="fas fa-play-circle"></i> <span id="btnText">START</span>
                    </button>
                </div>
            </div>
        </div>
        
        <!-- RESULTS SECTION (Last Transcription & Summary) -->
        <div id="resultsSection" class="section" style="display: none; background: #f0f4ff; border-left: 4px solid #2196F3;">
            <h3 style="color: #1976D2; margin-top: 0;"><i class="fas fa-lightbulb"></i> Latest Results</h3>
            
            <div style="margin-bottom: 20px;">
                <h4 style="color: #333; margin-bottom: 8px;"><i class="fas fa-file-audio"></i> Transcription</h4>
                <div id="lastTranscription" style="background: white; padding: 12px; border-radius: 4px; border-left: 3px solid #2196F3; color: #333; line-height: 1.6; font-size: 14px;">
                    <em style="color: #999;">No transcription yet...</em>
                </div>
            </div>
            
            <div>
                <h4 style="color: #333; margin-bottom: 8px;"><i class="fas fa-brain"></i> Summary</h4>
                <div id="lastSummary" style="background: white; padding: 12px; border-radius: 4px; border-left: 3px solid #FF9800; color: #333; line-height: 1.6; font-size: 14px;">
                    <em style="color: #999;">Generating summary...</em>
                </div>
            </div>
        </div>
        
        <div class="tabs">
            <button class="tab-btn active" onclick="switchTab(event, 'qa')"><i class="fas fa-comments"></i> Ask</button>
            <button class="tab-btn" onclick="switchTab(event, 'transcriptions')"><i class="fas fa-list"></i> Transcriptions</button>
            <button class="tab-btn" onclick="switchTab(event, 'search')"><i class="fas fa-search"></i> Search</button>
            <button class="tab-btn" onclick="switchTab(event, 'settings')"><i class="fas fa-sliders-h"></i> Settings</button>
        </div>
        
        <!-- Q&A Tab -->
        <div id="qa" class="tab-content active">
            <div class="section">
                <h2>Ask a Question</h2>
                <p>Ask anything about your transcriptions with context-aware answers.</p>
                <div style="display: flex; gap: 10px; margin-top: 15px;">
                    <input type="text" id="queryInput" placeholder="e.g., 'What did I record at 3 PM?'" style="flex: 1;">
                    <button class="btn" onclick="askQuestion()"><i class="fas fa-paper-plane"></i> Ask</button>
                </div>
                <div id="loading" class="loading"><i class="fas fa-spinner fa-spin"></i> Processing...</div>
                <div id="result" class="result">
                    <strong>Answer:</strong>
                    <p id="resultText"></p>
                </div>
            </div>
        </div>
        
        <!-- Transcriptions Tab -->
        <div id="transcriptions" class="tab-content">
            <div class="section">
                <h2>Recorded Transcriptions</h2>
                <p>Manage and view all your recorded transcriptions.</p>
                <div class="button-group" style="margin-top: 15px;">
                    <button class="btn btn-success" onclick="reloadTranscriptions()"><i class="fas fa-sync-alt"></i> Reload</button>
                    <button class="btn" onclick="exportToCSV()"><i class="fas fa-download"></i> Export CSV</button>
                    <button class="btn btn-danger" onclick="clearAllTranscriptions()"><i class="fas fa-trash-alt"></i> Clear All</button>
                </div>
                <div id="transcriptionsContainer" style="margin-top: 20px;">
                    <p class="no-data"><i class="fas fa-info-circle"></i> Click Reload to load transcriptions</p>
                </div>
            </div>
        </div>
        
        <!-- Search Tab -->
        <div id="search" class="tab-content">
            <div class="section">
                <h2>Advanced Search & Filter</h2>
                <p>Search transcriptions by keyword, date, or time range.</p>
                
                <div class="form-row">
                    <div class="form-group">
                        <label>Search Keyword</label>
                        <input type="text" id="searchKeyword" placeholder="e.g., meeting, notes...">
                    </div>
                </div>
                
                <div class="form-row">
                    <div class="form-group">
                        <label>Date From</label>
                        <input type="date" id="filterDateFrom">
                    </div>
                    <div class="form-group">
                        <label>Date To</label>
                        <input type="date" id="filterDateTo">
                    </div>
                </div>
                
                <div class="form-row">
                    <div class="form-group">
                        <label>Time From (HH:MM)</label>
                        <input type="time" id="filterTimeFrom">
                    </div>
                    <div class="form-group">
                        <label>Time To (HH:MM)</label>
                        <input type="time" id="filterTimeTo">
                    </div>
                </div>
                
                <button class="btn" onclick="performSearch()"><i class="fas fa-search"></i> Search</button>
                
                <div id="searchResults" style="margin-top: 20px;">
                </div>
            </div>
        </div>
        
        <!-- Settings Tab -->
        <div id="settings" class="tab-content">
            <div class="section">
                <h2>Settings</h2>
                <p>Configure timezone, display format, and other preferences.</p>
                
                <div class="form-group">
                    <label>Timezone</label>
                    <select id="settingsTimezone">
                        <option value="UTC">UTC (UTC+0)</option>
                        <option value="EST">Eastern Standard Time (UTC-5)</option>
                        <option value="CST">Central Standard Time (UTC-6)</option>
                        <option value="MST">Mountain Standard Time (UTC-7)</option>
                        <option value="PST">Pacific Standard Time (UTC-8)</option>
                        <option value="GMT">Greenwich Mean Time (UTC+0)</option>
                        <option value="CET">Central European Time (UTC+1)</option>
                        <option value="IST" selected>Indian Standard Time (UTC+5:30)</option>
                        <option value="SGT">Singapore Time (UTC+8)</option>
                        <option value="AEST">Australian Eastern Standard Time (UTC+10)</option>
                    </select>
                </div>
                
                <div class="checkbox-group">
                    <label class="checkbox-label">
                        <input type="checkbox" id="settings12Hour" checked> 12-Hour Time Format
                    </label>
                    <label class="checkbox-label">
                        <input type="checkbox" id="settingsRelativeTime"> Relative Time (e.g., "2 hours ago")
                    </label>
                    <label class="checkbox-label">
                        <input type="checkbox" id="settingsDarkMode"> Dark Mode
                    </label>
                </div>
                
                <button class="btn btn-success" onclick="saveSettings()"><i class="fas fa-save"></i> Save Settings</button>
                

            </div>
        </div>
    </div>

    <script>
        // ===== LIVE STATUS UPDATE =====
        function updateLiveStatus() {
            fetch('/status')
                .then(r => r.json())
                .then(data => {
                    // Update state indicator
                    const liveState = document.getElementById('liveState');
                    let stateText = '<i class="fas fa-circle" style="display: inline-block; margin-right: 5px; font-size: 10px;"></i>IDLE';
                    let stateColor = '#999999';
                    
                    if (data.is_recording) {
                        stateText = '<i class="fas fa-circle" style="display: inline-block; margin-right: 5px; font-size: 10px;"></i>RECORDING';
                        stateColor = '#ff4444';
                    } else if (data.is_processing) {
                        stateText = '<i class="fas fa-circle" style="display: inline-block; margin-right: 5px; font-size: 10px;"></i>PROCESSING';
                        stateColor = '#4CAF50';
                    } else {
                        stateText = '<i class="fas fa-circle" style="display: inline-block; margin-right: 5px; font-size: 10px;"></i>READY';
                        stateColor = '#4CAF50';
                    }
                    
                    if (liveState) {
                        liveState.innerHTML = stateText;
                        liveState.style.color = stateColor;
                    }
                    
                    // Update recording status
                    const recordingStatus = document.getElementById('recordingStatus');
                    if (recordingStatus) {
                        recordingStatus.textContent = data.is_recording ? 'ON' : 'OFF';
                        recordingStatus.style.color = data.is_recording ? '#4CAF50' : '#999999';
                    }
                    
                    // Update WiFi status
                    const wifiStatus = document.getElementById('wifiStatus');
                    if (wifiStatus) {
                        wifiStatus.innerHTML = data.wifi_connected ? '<i class="fas fa-check"></i> Connected' : '<i class="fas fa-times"></i> Disconnected';
                        wifiStatus.style.color = data.wifi_connected ? '#4CAF50' : '#ff4444';
                    }
                    
                    // Update progress bar
                    const recordingProgress = document.getElementById('recordingProgress');
                    const recordingPercent = document.getElementById('recordingPercent');
                    if (recordingProgress && recordingPercent) {
                        recordingProgress.style.width = data.recording_percentage + '%';
                        recordingPercent.textContent = data.recording_percentage + '% complete (' + data.samples_recorded + '/' + data.max_samples + ' samples)';
                    }
                    
                    // Update stop button state
                    const stopBtn = document.getElementById('stopBtn');
                    if (stopBtn) {
                        stopBtn.style.opacity = data.is_recording ? '1' : '0.5';
                        stopBtn.style.cursor = data.is_recording ? 'pointer' : 'not-allowed';
                        stopBtn.disabled = !data.is_recording;
                    }
                    
                    // Update toggle button state and appearance
                    const toggleBtn = document.getElementById('toggleBtn');
                    const btnText = document.getElementById('btnText');
                    if (toggleBtn && btnText) {
                        toggleBtn.setAttribute('data-recording', data.is_recording);
                        
                        if (data.is_recording) {
                            toggleBtn.style.background = '#f44336';  // Red for STOP
                            toggleBtn.innerHTML = '<i class="fas fa-stop-circle"></i> <span id="btnText">STOP</span>';
                            toggleBtn.title = 'Stop recording';
                        } else {
                            toggleBtn.style.background = '#4CAF50';  // Green for START
                            toggleBtn.innerHTML = '<i class="fas fa-play-circle"></i> <span id="btnText">START</span>';
                            toggleBtn.title = 'Start recording';
                        }
                    }
                    
                    // Display last transcription and summary if available
                    if (data.last_transcription || data.last_summary) {
                        const resultsSection = document.getElementById('resultsSection');
                        if (resultsSection) {
                            resultsSection.style.display = 'block';
                            
                            // Update transcription
                            if (data.last_transcription) {
                                document.getElementById('lastTranscription').innerHTML = data.last_transcription || '<em style="color: #999;">No transcription yet...</em>';
                            }
                            
                            // Update summary
                            if (data.last_summary) {
                                document.getElementById('lastSummary').innerHTML = data.last_summary || '<em style="color: #999;">Generating summary...</em>';
                            }
                        }
                    }
                })
                .catch(e => console.log('Status update failed:', e));
        }
        
        // Update status every 500ms
        window.addEventListener('load', () => {
            updateLiveStatus();
            setInterval(updateLiveStatus, 500);
        });
        
        // Stop Recording Function
        function stopRecording() {
            if (!confirm('Stop recording now? Any in-process chunks will complete but no new recording will start.')) {
                return;
            }
            
            fetch('/stop', {method: 'POST'})
                .then(r => r.json())
                .then(data => {
                    alert('Recording stopped successfully');
                    updateLiveStatus();
                })
                .catch(e => alert('Error stopping recording: ' + e));
        }
        
        // Start Recording Function
        function startRecording() {
            fetch('/start', {method: 'POST'})
                .then(r => r.json())
                .then(data => {
                    alert('Recording started');
                    updateLiveStatus();
                })
                .catch(e => alert('Error starting recording: ' + e));
        }
        
        // Toggle Recording Function (START/STOP)
        function toggleRecording() {
            const isRecording = document.getElementById('toggleBtn').getAttribute('data-recording') === 'true';
            
            if (isRecording) {
                stopRecording();
            } else {
                startRecording();
            }
        }
        
        // Dark Mode with icon toggling
        function toggleDarkMode() {
            document.body.classList.toggle('dark-mode');
            const isDarkMode = document.body.classList.contains('dark-mode');
            localStorage.setItem('darkMode', isDarkMode);
            
            // Update icon
            const darkModeBtn = document.getElementById('darkModeBtn');
            if (darkModeBtn) {
                darkModeBtn.innerHTML = isDarkMode ? '<i class="fas fa-sun"></i>' : '<i class="fas fa-moon"></i>';
                darkModeBtn.title = isDarkMode ? 'Light Mode' : 'Dark Mode';
            }
        }
        
        // Load dark mode preference and set initial icon
        window.addEventListener('load', () => {
            const isDarkMode = localStorage.getItem('darkMode') === 'true';
            const darkModeBtn = document.getElementById('darkModeBtn');
            
            if (isDarkMode) {
                document.body.classList.add('dark-mode');
            }
            
            if (darkModeBtn) {
                darkModeBtn.innerHTML = isDarkMode ? '<i class="fas fa-sun"></i>' : '<i class="fas fa-moon"></i>';
                darkModeBtn.title = isDarkMode ? 'Light Mode' : 'Dark Mode';
            }
        });
        
        // Tab Switching
        function switchTab(event, tabName) {
            document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
            document.getElementById(tabName).classList.add('active');
            event.target.classList.add('active');
        }
        
        // Settings Panel
        function openSettings() {
            switchTab({target: document.querySelector('[onclick*="settings"]')}, 'settings');
            loadSettings();
        }
        
        function loadSettings() {
            fetch('/settings')
                .then(r => r.json())
                .then(data => {
                    document.getElementById('settingsTimezone').value = data.timezone_name || 'IST';
                    document.getElementById('settings12Hour').checked = data.use_12_hour !== false;
                    document.getElementById('settingsRelativeTime').checked = data.use_relative_time || false;
                    document.getElementById('settingsDarkMode').checked = data.dark_mode || false;
                });
        }
        
        function saveSettings() {
            const settings = {
                timezone_name: document.getElementById('settingsTimezone').value,
                timezone_offset: getTimezoneOffset(document.getElementById('settingsTimezone').value),
                use_12_hour: document.getElementById('settings12Hour').checked,
                use_relative_time: document.getElementById('settingsRelativeTime').checked,
                dark_mode: document.getElementById('settingsDarkMode').checked
            };
            
            fetch('/settings', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify(settings)
            })
            .then(r => r.json())
            .then(data => {
                alert('Settings saved!');
                if (settings.dark_mode) {
                    document.body.classList.add('dark-mode');
                } else {
                    document.body.classList.remove('dark-mode');
                }
            });
        }
        
        function getTimezoneOffset(tz) {
            const offsets = {
                'UTC': 0, 'EST': -18000, 'CST': -21600, 'MST': -25200, 'PST': -28800,
                'GMT': 0, 'CET': 3600, 'IST': 19800, 'SGT': 28800, 'AEST': 36000
            };
            return offsets[tz] || 0;
        }
        
        function resyncNTP() {
            alert('NTP Re-sync initiated. Scheduled to complete in background.');
        }
        
        // Q&A
        function askQuestion() {
            const query = document.getElementById('queryInput').value.trim();
            if (!query) {
                alert('Please enter a question');
                return;
            }
            document.getElementById('loading').classList.add('show');
            document.getElementById('result').classList.remove('show');
            
            fetch('/query?q=' + encodeURIComponent(query))
                .then(r => r.json())
                .then(data => {
                    document.getElementById('loading').classList.remove('show');
                    document.getElementById('resultText').textContent = data.answer;
                    const resultDiv = document.getElementById('result');
                    resultDiv.classList.add('show');
                    if (data.answer.includes('Error')) {
                        resultDiv.classList.add('error');
                    } else {
                        resultDiv.classList.remove('error');
                    }
                });
        }
        
        document.getElementById('queryInput').addEventListener('keypress', e => {
            if (e.key === 'Enter') askQuestion();
        });
        
        // Transcriptions
        function reloadTranscriptions() {
            const container = document.getElementById('transcriptionsContainer');
            container.innerHTML = '<p class="no-data"><i class="fas fa-spinner fa-spin"></i> Loading...</p>';
            
            fetch('/transcriptions')
                .then(r => r.json())
                .then(data => {
                    if (data.transcriptions.length === 0) {
                        container.innerHTML = '<p class="no-data"><i class="fas fa-info-circle"></i> No transcriptions yet</p>';
                        return;
                    }
                    let html = '';
                    data.transcriptions.forEach((item, i) => {
                        html += `<div class="transcription-item">
                            <h4><i class="fas fa-file-alt"></i> Recording ${i + 1}</h4>
                            <p><strong><i class="fas fa-clock"></i> Saved:</strong> ${item.timestamp}</p>
                            <p><strong><i class="fas fa-play"></i> Started:</strong> ${item.recording_start_time}</p>
                            <p><strong><i class="fas fa-hourglass-half"></i> Duration:</strong> ${item.recording_duration}</p>
                            <p><strong><i class="fas fa-microphone"></i> Transcription:</strong> ${item.transcription}</p>
                            <p><strong><i class="fas fa-lightbulb"></i> Summary:</strong> ${item.summary}</p>
                        </div>`;
                    });
                    container.innerHTML = html;
                });
        }
        
        function exportToCSV() {
            window.location.href = '/export';
        }
        
        function clearAllTranscriptions() {
            if (!confirm('Delete ALL transcriptions? This cannot be undone!')) return;
            fetch('/clear', {method: 'POST'})
                .then(r => r.json())
                .then(() => {
                    alert('All transcriptions cleared');
                    document.getElementById('transcriptionsContainer').innerHTML = '<p class="no-data"><i class="fas fa-info-circle"></i> No transcriptions yet</p>';
                });
        }
        
        // Search
        function performSearch() {
            const params = new URLSearchParams();
            if (document.getElementById('searchKeyword').value) {
                params.append('keyword', document.getElementById('searchKeyword').value);
            }
            if (document.getElementById('filterDateFrom').value) {
                params.append('date_from', document.getElementById('filterDateFrom').value);
            }
            if (document.getElementById('filterDateTo').value) {
                params.append('date_to', document.getElementById('filterDateTo').value);
            }
            if (document.getElementById('filterTimeFrom').value) {
                params.append('time_from', document.getElementById('filterTimeFrom').value);
            }
            if (document.getElementById('filterTimeTo').value) {
                params.append('time_to', document.getElementById('filterTimeTo').value);
            }
            
            document.getElementById('searchResults').innerHTML = '<p class="no-data"><i class="fas fa-spinner fa-spin"></i> Searching...</p>';
            
            fetch('/search?' + params)
                .then(r => r.json())
                .then(data => {
                    const container = document.getElementById('searchResults');
                    if (data.results.length === 0) {
                        container.innerHTML = '<p class="no-data"><i class="fas fa-search"></i> No results found</p>';
                        return;
                    }
                    let html = `<h3>Found ${data.total_matches} result(s)</h3>`;
                    data.results.forEach((item, i) => {
                        html += `<div class="transcription-item">
                            <h4><i class="fas fa-file-alt"></i> Match ${i + 1}</h4>
                            <p><strong>Saved:</strong> ${item.timestamp}</p>
                            <p><strong>Started:</strong> ${item.recording_start_time}</p>
                            <p><strong>Duration:</strong> ${item.recording_duration}</p>
                            <p><strong>Transcription:</strong> ${item.transcription}</p>
                            <p><strong>Summary:</strong> ${item.summary}</p>
                        </div>`;
                    });
                    container.innerHTML = html;
                });
        }
    </script>
</body>
</html>
    )rawliteral";
    
    server.send(200, "text/html", html);
}

// ===== WEB HANDLER: QUERY ENDPOINT =====
void handleQuery() {
    if (!server.hasArg("q")) {
        server.send(400, "application/json", "{\"error\":\"Missing query parameter\"}");
        return;
    }
    
    String question = server.arg("q");
    String answer = answerQuestionAboutTranscriptions(question);
    
    StaticJsonDocument<2048> responseDoc;
    responseDoc["question"] = question;
    responseDoc["answer"] = answer;
    
    String jsonResponse;
    serializeJson(responseDoc, jsonResponse);
    server.send(200, "application/json", jsonResponse);
}

// ===== WEB HANDLER: GET TRANSCRIPTIONS =====
void handleTranscriptions() {
    StaticJsonDocument<4096> doc;
    JsonArray arr = doc.createNestedArray("transcriptions");
    
    for (int i = transcriptionStorage.size() - 1; i >= 0; i--) {
        JsonObject item = arr.createNestedObject();
        item["timestamp"] = transcriptionStorage[i].timestamp;
        item["transcription"] = transcriptionStorage[i].transcription;
        item["summary"] = transcriptionStorage[i].summary;
        item["recording_start_time"] = transcriptionStorage[i].recording_start_time;
        item["recording_duration"] = transcriptionStorage[i].recording_duration;
    }
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// ===== WEB HANDLER: CLEAR TRANSCRIPTIONS =====
void handleClear() {
    transcriptionStorage.clear();
    
    if (SPIFFS.exists(TRANSCRIPTIONS_FILE)) {
        SPIFFS.remove(TRANSCRIPTIONS_FILE);
    }
    
    Serial.println("All transcriptions cleared!");
    
    StaticJsonDocument<256> doc;
    doc["message"] = "All transcriptions have been cleared successfully";
    doc["status"] = "success";
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// ===== WEB HANDLER: SEARCH TRANSCRIPTIONS =====
void handleSearch() {
    String keyword = server.hasArg("keyword") ? server.arg("keyword") : "";
    String date_from = server.hasArg("date_from") ? server.arg("date_from") : "";
    String date_to = server.hasArg("date_to") ? server.arg("date_to") : "";
    String time_from = server.hasArg("time_from") ? server.arg("time_from") : "";
    String time_to = server.hasArg("time_to") ? server.arg("time_to") : "";
    
    StaticJsonDocument<4096> doc;
    JsonArray arr = doc.createNestedArray("results");
    
    int matches = 0;
    
    // Iterate in reverse order (newest first)
    for (int i = transcriptionStorage.size() - 1; i >= 0 && matches < 100; i--) {
        TranscriptionEntry& entry = transcriptionStorage[i];
        bool matches_filters = true;
        
        // Keyword filter (case-insensitive)
        if (keyword.length() > 0) {
            String lower_transcription = entry.transcription;
            String lower_keyword = keyword;
            lower_transcription.toLowerCase();
            lower_keyword.toLowerCase();
            if (lower_transcription.indexOf(lower_keyword) == -1 && 
                entry.summary.indexOf(lower_keyword) == -1) {
                matches_filters = false;
            }
        }
        
        // Date range filter
        if (matches_filters && date_from.length() > 0) {
            if (entry.timestamp.substring(0, 10) < date_from) {
                matches_filters = false;
            }
        }
        if (matches_filters && date_to.length() > 0) {
            if (entry.timestamp.substring(0, 10) > date_to) {
                matches_filters = false;
            }
        }
        
        // Time range filter
        if (matches_filters && time_from.length() > 0) {
            if (entry.timestamp.substring(11, 16) < time_from) {
                matches_filters = false;
            }
        }
        if (matches_filters && time_to.length() > 0) {
            if (entry.timestamp.substring(11, 16) > time_to) {
                matches_filters = false;
            }
        }
        
        if (matches_filters) {
            JsonObject item = arr.createNestedObject();
            item["timestamp"] = entry.timestamp;
            item["timestamp_iso8601"] = entry.timestamp_iso8601;
            item["recording_start_time"] = entry.recording_start_time;
            item["recording_duration"] = entry.recording_duration;
            item["transcription"] = entry.transcription;
            item["summary"] = entry.summary;
            item["timezone_name"] = entry.timezone_name;
            matches++;
        }
    }
    
    doc["total_matches"] = matches;
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// ===== WEB HANDLER: EXPORT TO CSV =====
void handleExport() {
    String csv = "Timestamp,Recording Start,Duration,Timezone,Transcription,Summary\n";
    
    for (int i = transcriptionStorage.size() - 1; i >= 0; i--) {
        TranscriptionEntry& entry = transcriptionStorage[i];
        
        String transcript_safe = entry.transcription;
        transcript_safe.replace("\"", "\"\"");
        String summary_safe = entry.summary;
        summary_safe.replace("\"", "\"\"");
        
        csv += "\"" + entry.timestamp + "\",";
        csv += "\"" + entry.recording_start_time + "\",";
        csv += "\"" + entry.recording_duration + "\",";
        csv += "\"" + entry.timezone_name + "\",";
        csv += "\"" + transcript_safe + "\",";
        csv += "\"" + summary_safe + "\"\n";
    }
    
    server.sendHeader("Content-Disposition", "attachment; filename=transcriptions.csv");
    server.send(200, "text/csv", csv);
}

// ===== WEB HANDLER: GET SETTINGS =====
void handleGetSettings() {
    StaticJsonDocument<512> doc;
    doc["timezone_name"] = user_settings.timezone_name;
    doc["timezone_offset"] = user_settings.timezone_offset;
    doc["dark_mode"] = user_settings.dark_mode;
    doc["use_12_hour"] = user_settings.use_12_hour;
    doc["use_relative_time"] = user_settings.use_relative_time;
    doc["synced"] = ntp_synced;
    doc["last_sync"] = formatTimestampLocal(last_ntp_sync_time);
    doc["current_time"] = formatTimestampLocal(time(nullptr));
    doc["timezone"] = user_settings.timezone_name;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// ===== WEB HANDLER: SAVE SETTINGS =====
void handleSaveSettings() {
    if (server.method() == HTTP_POST) {
        JsonDocument doc;
        String body = server.arg("plain");
        
        if (deserializeJson(doc, body) == DeserializationError::Ok) {
            user_settings.timezone_name = doc["timezone_name"] | "UTC";
            user_settings.timezone_offset = doc["timezone_offset"] | 0;
            user_settings.dark_mode = doc["dark_mode"] | false;
            user_settings.use_12_hour = doc["use_12_hour"] | false;
            user_settings.use_relative_time = doc["use_relative_time"] | false;
            
            // Apply to globals
            timezone_offset_seconds = user_settings.timezone_offset;
            preferred_timezone = user_settings.timezone_name;
            dark_mode = user_settings.dark_mode;
            use_12_hour_format = user_settings.use_12_hour;
            use_relative_time = user_settings.use_relative_time;
            
            saveSettings();
            
            StaticJsonDocument<256> response;
            response["status"] = "success";
            response["message"] = "Settings saved";
            String resp;
            serializeJson(response, resp);
            server.send(200, "application/json", resp);
            return;
        }
    }
    
    server.send(400, "application/json", "{\"error\":\"Invalid request\"}");
}

// ===== WEB HANDLER: NTP STATUS =====
void handleNTPStatus() {
    StaticJsonDocument<512> doc;
    doc["synced"] = ntp_synced;
    doc["last_sync"] = formatTimestampLocal(last_ntp_sync_time);
    doc["current_time"] = formatTimestampLocal(time(nullptr));
    doc["timezone"] = preferred_timezone;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// ===== WEB HANDLER: LIVE STATUS =====
void handleStatus() {
    StaticJsonDocument<512> doc;
    
    String state = "idle";
    if (is_recording) {
        state = "recording";
    } else if (chunk_processing) {
        state = "processing";
    }
    
    doc["state"] = state;
    doc["is_recording"] = (bool)is_recording;
    doc["is_processing"] = (bool)chunk_processing;
    doc["samples_recorded"] = (int)samples_recorded;
    doc["max_samples"] = audio_buffer_size / sizeof(int16_t);
    doc["recording_percentage"] = (int)((float)samples_recorded / (audio_buffer_size / sizeof(int16_t)) * 100);
    doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
    
    doc["last_transcription"] = latest_transcription;
    doc["last_summary"] = latest_summary;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// ===== WEB HANDLER: STOP RECORDING =====
void handleStopRecording() {
    is_recording = false;
    
    if (samples_recorded > 0) {
        Serial.printf("\n[WEB] Recording stopped with %d samples (%.1f sec)\n", samples_recorded, (float)samples_recorded / SAMPLE_RATE);
        Serial.println("[WEB] Processing incomplete chunk immediately...");
        
        if (xSemaphoreTake(audioBufferMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            memcpy(chunk_to_process, audio_buffer, samples_recorded * sizeof(int16_t));
            chunk_size_to_process = samples_recorded;
            chunk_processing = true;
            chunk_ready = true;
            
            Serial.printf("[WEB] Chunk copied to processor (%d samples)\n", samples_recorded);
            
            xSemaphoreGive(audioBufferMutex);
            
            xSemaphoreGive(transcriptionReadySemaphore);
        }
    }
    
    indicateIdle();
    
    Serial.println("[WEB] Recording stopped by user");
    Serial.println("[WEB] Transcription and summary will appear on webpage");
    
    StaticJsonDocument<256> doc;
    doc["status"] = "success";
    doc["message"] = "Recording stopped";
    doc["is_recording"] = false;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

// ===== WEB HANDLER: START RECORDING =====
void handleStartRecording() {
    is_recording = true;
    samples_recorded = 0;
    chunk_ready = false;
    chunk_start_time = millis();
    
    Serial.println("\n[WEB] Recording started by user");
    Serial.println("[WEB] Audio recording resuming...");
    
    StaticJsonDocument<256> doc;
    doc["status"] = "success";
    doc["message"] = "Recording started";
    doc["is_recording"] = true;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void setupWebServer() {
    server.on("/", handleRoot);
    server.on("/query", handleQuery);
    server.on("/transcriptions", handleTranscriptions);
    server.on("/clear", HTTP_POST, handleClear);
    server.on("/search", handleSearch);
    server.on("/export", handleExport);
    server.on("/settings", HTTP_GET, handleGetSettings);
    server.on("/settings", HTTP_POST, handleSaveSettings);
    server.on("/ntp_status", handleNTPStatus);
    server.on("/status", handleStatus);
    server.on("/start", HTTP_POST, handleStartRecording);
    server.on("/stop", HTTP_POST, handleStopRecording);
    
    server.begin();
    Serial.println("Web server started at http://" + WiFi.localIP().toString());
}

// ===== WiFi CONFIGURATION FUNCTIONS =====
void loadWiFiConfig() {
    File file = SPIFFS.open(WIFI_CONFIG_FILE, "r");
    if (!file) {
        Serial.println("[WiFi Config] No saved credentials found, using defaults from secrets.h");
        wifi_config.ssid = String(WIFI_SSID);
        wifi_config.password = String(WIFI_PASSWORD);
        return;
    }
    
    String content = "";
    while (file.available()) {
        content += (char)file.read();
    }
    file.close();
    
    JsonDocument doc;
    if (deserializeJson(doc, content) != DeserializationError::Ok) {
        Serial.println("[WiFi Config] Failed to parse WiFi config, using defaults");
        wifi_config.ssid = String(WIFI_SSID);
        wifi_config.password = String(WIFI_PASSWORD);
        return;
    }
    
    wifi_config.ssid = doc["ssid"] | WIFI_SSID;
    wifi_config.password = doc["password"] | WIFI_PASSWORD;
    
    Serial.printf("[WiFi Config] Loaded: SSID=%s\n", wifi_config.ssid.c_str());
}

// ===== SAVE WiFi CREDENTIALS TO SPIFFS =====
void saveWiFiConfig() {
    JsonDocument doc;
    doc["ssid"] = wifi_config.ssid;
    doc["password"] = wifi_config.password;
    
    File file = SPIFFS.open(WIFI_CONFIG_FILE, "w");
    if (file) {
        serializeJson(doc, file);
        file.close();
        Serial.printf("[WiFi Config] Saved: SSID=%s\n", wifi_config.ssid.c_str());
    } else {
        Serial.println("[WiFi Config] Failed to save credentials");
    }
}

// ===== WiFi CONFIGURATION MODE HANDLER =====
void handleWiFiConfigRoot() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>WiFi Configuration</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body {
            font-family: Arial, sans-serif;
            max-width: 600px;
            margin: 50px auto;
            padding: 20px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
        }
        .container {
            background: white;
            border-radius: 10px;
            padding: 40px;
            box-shadow: 0 10px 30px rgba(0,0,0,0.3);
        }
        h1 {
            color: #667eea;
            text-align: center;
            margin-bottom: 10px;
        }
        .subtitle {
            text-align: center;
            color: #666;
            margin-bottom: 30px;
            font-size: 14px;
        }
        .form-group {
            margin-bottom: 20px;
        }
        label {
            display: block;
            margin-bottom: 8px;
            font-weight: bold;
            color: #333;
        }
        input {
            width: 100%;
            padding: 12px;
            border: 2px solid #ddd;
            border-radius: 5px;
            font-size: 14px;
            box-sizing: border-box;
            transition: border-color 0.3s;
        }
        input:focus {
            outline: none;
            border-color: #667eea;
        }
        .button-group {
            display: flex;
            gap: 10px;
            margin-top: 30px;
        }
        button {
            flex: 1;
            padding: 14px;
            border: none;
            border-radius: 5px;
            font-size: 16px;
            font-weight: bold;
            cursor: pointer;
            transition: all 0.3s;
        }
        .btn-save {
            background: #667eea;
            color: white;
        }
        .btn-save:hover {
            background: #764ba2;
            transform: translateY(-2px);
        }
        .btn-cancel {
            background: #f44336;
            color: white;
        }
        .btn-cancel:hover {
            background: #da190b;
            transform: translateY(-2px);
        }
        .info {
            background: #f0f0f0;
            border-left: 4px solid #667eea;
            padding: 15px;
            margin-bottom: 20px;
            border-radius: 5px;
            font-size: 13px;
            color: #555;
        }
        .warning {
            background: #fff3cd;
            border-left: 4px solid #ffc107;
            padding: 15px;
            margin-bottom: 20px;
            border-radius: 5px;
            font-size: 13px;
            color: #856404;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>WiFi Configuration</h1>
        <p class="subtitle">Configure your WiFi connection</p>
        
        <div class="info">
            <strong>LED Status:</strong> The LED will blink while in configuration mode. It will stop blinking once you save and reconnect.
        </div>
        
        <div class="warning">
            <strong>Note:</strong> Configuration mode will close in 5 minutes. Press GPIO 4 for 5 seconds again to re-enter.
        </div>
        
        <form id="wifiForm">
            <div class="form-group">
                <label for="ssid">WiFi Network Name (SSID)</label>
                <input type="text" id="ssid" name="ssid" placeholder="Enter WiFi network name" required>
            </div>
            
            <div class="form-group">
                <label for="password">WiFi Password</label>
                <input type="password" id="password" name="password" placeholder="Enter WiFi password" required>
            </div>
            
            <div class="button-group">
                <button type="button" class="btn-save" onclick="saveWiFi()">Save & Reconnect</button>
                <button type="button" class="btn-cancel" onclick="cancelConfig()">Cancel</button>
            </div>
        </form>
    </div>

    <script>
        // Load current WiFi credentials
        window.addEventListener('load', () => {
            fetch('/wifi_current')
                .then(r => r.json())
                .then(data => {
                    document.getElementById('ssid').value = data.ssid || '';
                    document.getElementById('password').value = data.password || '';
                })
                .catch(e => console.log('Could not load current config'));
        });
        
        function saveWiFi() {
            const ssid = document.getElementById('ssid').value.trim();
            const password = document.getElementById('password').value.trim();
            
            if (!ssid || !password) {
                alert('Please enter both SSID and password');
                return;
            }
            
            const config = {ssid, password};
            
            fetch('/wifi_save', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify(config)
            })
            .then(r => r.json())
            .then(data => {
                alert('WiFi credentials saved! Device will reconnect...');
                setTimeout(() => window.close(), 2000);
            })
            .catch(e => alert('Error: ' + e));
        }
        
        function cancelConfig() {
            if (confirm('Exit configuration mode?')) {
                fetch('/wifi_cancel', {method: 'POST'})
                    .then(() => window.close());
            }
        }
    </script>
</body>
</html>
    )rawliteral";
    
    server.send(200, "text/html", html);
}

void handleWiFiCurrent() {
    StaticJsonDocument<256> doc;
    doc["ssid"] = wifi_config.ssid;
    doc["password"] = wifi_config.password;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleWiFiSave() {
    if (server.method() == HTTP_POST) {
        JsonDocument doc;
        String body = server.arg("plain");
        
        if (deserializeJson(doc, body) == DeserializationError::Ok) {
            wifi_config.ssid = doc["ssid"].as<String>();
            wifi_config.password = doc["password"].as<String>();
            
            saveWiFiConfig();
            
            StaticJsonDocument<256> response;
            response["status"] = "success";
            response["message"] = "WiFi credentials saved, reconnecting...";
            
            String resp;
            serializeJson(response, resp);
            server.send(200, "application/json", resp);
            
            // Exit config mode and reconnect
            in_config_mode = false;
            indicateIdle();  // Set LED to idle state
            
            // Reconnect WiFi with new credentials
            delay(1000);
            setupWiFi();
            
            return;
        }
    }
    
    server.send(400, "application/json", "{\"error\":\"Invalid request\"}");
}

void handleWiFiCancel() {
    if (server.method() == HTTP_POST) {
        in_config_mode = false;
        indicateIdle();  // Set LED to idle state
        
        StaticJsonDocument<256> response;
        response["status"] = "success";
        response["message"] = "Configuration mode closed";
        
        String resp;
        serializeJson(response, resp);
        server.send(200, "application/json", resp);
        
        return;
    }
    
    server.send(400, "application/json", "{\"error\":\"Invalid request\"}");
}

// ===== ENTER WiFi CONFIGURATION MODE =====
void enterWiFiConfigMode() {
    Serial.println("\n>>> ENTERING WiFi CONFIGURATION MODE <<<");
    Serial.println("Connect to WiFi network: ESP32_CONFIG");
    Serial.println("Open http://192.168.4.1 in your browser");
    Serial.println("Configuration will close in 5 minutes or when saved");
    
    in_config_mode = true;
    config_mode_timeout = millis();
    
    WiFi.softAP("ESP32_CONFIG", "12345678");
    
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
    
    server.on("/", handleWiFiConfigRoot);
    server.on("/wifi_current", HTTP_GET, handleWiFiCurrent);
    server.on("/wifi_save", HTTP_POST, handleWiFiSave);
    server.on("/wifi_cancel", HTTP_POST, handleWiFiCancel);
    
    indicateTranscribing();
}

// ===== EXIT WiFi CONFIGURATION MODE =====
void exitWiFiConfigMode() {
    Serial.println(">>> EXITING WiFi Configuration Mode");
    in_config_mode = false;
    WiFi.softAPdisconnect(true);
    
    setupWiFi();
}

// ===== TIME FORMATTING UTILITIES =====
String formatTimestampLocal(time_t unix_time) {
    time_t local_time = unix_time + timezone_offset_seconds;
    struct tm* timeinfo = gmtime(&local_time);
    
    char buffer[30];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    return String(buffer);
}

String getTimezoneAbbr() {
    if (preferred_timezone.length() > 0) {
        return preferred_timezone;
    }
    return "UTC";
}

String formatTimestampISO8601(time_t unix_time) {
    time_t local_time = unix_time + timezone_offset_seconds;
    struct tm* timeinfo = gmtime(&local_time);
    
    char buffer[35];
    int offset_hours = timezone_offset_seconds / 3600;
    int offset_mins = (timezone_offset_seconds % 3600) / 60;
    
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", timeinfo);
    String result = String(buffer);
    
    if (offset_hours >= 0) {
        result += String("+");
    }
    if (offset_hours < 10 && offset_hours > -10) {
        result += "0";
    }
    result += String(offset_hours);
    result += ":";
    if (offset_mins < 10) {
        result += "0";
    }
    result += String(offset_mins);
    
    return result;
}

String formatTimestamp12Hour(time_t unix_time) {
    time_t local_time = unix_time + timezone_offset_seconds;
    struct tm* timeinfo = gmtime(&local_time);
    
    char buffer[25];
    strftime(buffer, sizeof(buffer), "%I:%M %p", timeinfo);
    
    return String(buffer) + " " + getTimezoneAbbr();
}

String formatTimestampRelative(time_t unix_time) {
    time_t now = time(nullptr);
    long diff_seconds = now - unix_time;
    
    if (diff_seconds < 60) {
        return "now";
    } else if (diff_seconds < 3600) {
        int mins = diff_seconds / 60;
        return String(mins) + (mins == 1 ? " minute ago" : " minutes ago");
    } else if (diff_seconds < 86400) {
        int hours = diff_seconds / 3600;
        return String(hours) + (hours == 1 ? " hour ago" : " hours ago");
    } else if (diff_seconds < 604800) {
        int days = diff_seconds / 86400;
        return String(days) + (days == 1 ? " day ago" : " days ago");
    } else if (diff_seconds < 2592000) {
        int weeks = diff_seconds / 604800;
        return String(weeks) + (weeks == 1 ? " week ago" : " weeks ago");
    } else {
        int months = diff_seconds / 2592000;
        return String(months) + (months == 1 ? " month ago" : " months ago");
    }
}

// ===== SETUP WiFi CONNECTION =====
void setupWiFi() {
    WiFi.disconnect(true);
    delay(500);
    
    Serial.println("\n[WiFi] Connecting to WiFi...");
    Serial.printf("[WiFi] SSID: %s\n", wifi_config.ssid.c_str());
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_config.ssid.c_str(), wifi_config.password.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WiFi] Connected!");
        Serial.print("[WiFi] IP address: ");
        Serial.println(WiFi.localIP());
        
        Serial.println("[NTP] Configuring NTP servers...");
        configTime(0, 0, "pool.ntp.org", "time.nist.gov", "pool.ntp.org");
        
        Serial.println("[NTP] Waiting for initial NTP time sync...");
        time_t now = time(nullptr);
        int ntp_attempts = 0;
        
        while (now < 1577836800 && ntp_attempts < 30) {
            delay(500);
            Serial.print(".");
            now = time(nullptr);
            ntp_attempts++;
        }
        Serial.println();
        
        if (now > 1577836800) {
            ntp_synced = true;
            last_ntp_sync_time = now;
            Serial.printf("[NTP] Initial sync successful! Current time: %s\n", formatTimestampLocal(now).c_str());
        } else {
            ntp_synced = false;
            Serial.println("[NTP] Initial sync timeout - will retry in background");
        }
    } else {
        Serial.println("\n[WiFi] Failed to connect to WiFi");
        ntp_synced = false;
    }
}

// ===== RE-SYNC NTP TIME PERIODICALLY =====
void resyncNTPTime() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[NTP] WiFi disconnected, cannot sync");
        ntp_synced = false;
        return;
    }
    
    Serial.println("[NTP] Starting NTP re-sync...");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    
    time_t now = time(nullptr);
    int attempts = 0;
    int max_attempts = 20;
    
    while (now < 1577836800 && attempts < max_attempts) {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
        attempts++;
    }
    Serial.println();
    
    if (now > 1577836800) {
        ntp_synced = true;
        last_ntp_sync_time = now;
        Serial.printf("[NTP] Sync successful! Time: %s\n", formatTimestampLocal(now).c_str());
    } else {
        ntp_synced = false;
        Serial.printf("[NTP] Sync failed after %d attempts\n", attempts);
    }
}

// ===== LOAD SETTINGS FROM SPIFFS =====
void loadSettings() {
    File file = SPIFFS.open("/settings.json", "r");
    if (!file) {
        Serial.println("No settings file found, using defaults (IST, 12-hour format)");
        user_settings.timezone_name = "IST";
        user_settings.timezone_offset = 19800;
        user_settings.dark_mode = false;
        user_settings.use_12_hour = true;
        user_settings.use_relative_time = false;
        return;
    }
    
    String content = "";
    while (file.available()) {
        content += (char)file.read();
    }
    file.close();
    
    JsonDocument doc;
    if (deserializeJson(doc, content) != DeserializationError::Ok) {
        Serial.println("Failed to parse settings JSON");
        return;
    }
    
    user_settings.timezone_name = doc["timezone_name"] | "IST";
    user_settings.timezone_offset = doc["timezone_offset"] | 19800;
    user_settings.dark_mode = doc["dark_mode"] | false;
    user_settings.use_12_hour = doc["use_12_hour"] | true;
    user_settings.use_relative_time = doc["use_relative_time"] | false;
    
    timezone_offset_seconds = user_settings.timezone_offset;
    preferred_timezone = user_settings.timezone_name;
    dark_mode = user_settings.dark_mode;
    use_12_hour_format = user_settings.use_12_hour;
    use_relative_time = user_settings.use_relative_time;
    
    Serial.printf("Settings loaded: TZ=%s, Offset=%ld, 12Hour=%s, DarkMode=%s\n", 
                  user_settings.timezone_name.c_str(), user_settings.timezone_offset,
                  user_settings.use_12_hour ? "yes" : "no",
                  user_settings.dark_mode ? "on" : "off");
}

// ===== SAVE SETTINGS TO SPIFFS =====
void saveSettings() {
    JsonDocument doc;
    doc["timezone_name"] = user_settings.timezone_name;
    doc["timezone_offset"] = user_settings.timezone_offset;
    doc["dark_mode"] = user_settings.dark_mode;
    doc["use_12_hour"] = user_settings.use_12_hour;
    doc["use_relative_time"] = user_settings.use_relative_time;
    
    File file = SPIFFS.open("/settings.json", "w");
    if (file) {
        serializeJson(doc, file);
        file.close();
        Serial.println("Settings saved");
    }
}

// ===== CREATE WAV HEADER FOR AUDIO =====
void createWavHeader(byte* header, int wavDataSize) {
    header[0] = 'R';
    header[1] = 'I';
    header[2] = 'F';
    header[3] = 'F';
    
    int fileSize = wavDataSize + 36;
    header[4] = fileSize & 0xff;
    header[5] = (fileSize >> 8) & 0xff;
    header[6] = (fileSize >> 16) & 0xff;
    header[7] = (fileSize >> 24) & 0xff;
    
    header[8] = 'W';
    header[9] = 'A';
    header[10] = 'V';
    header[11] = 'E';
    
    header[12] = 'f';
    header[13] = 'm';
    header[14] = 't';
    header[15] = ' ';
    
    header[16] = 16;
    header[17] = 0;
    header[18] = 0;
    header[19] = 0;
    
    header[20] = 1;
    header[21] = 0;
    
    header[22] = 1;
    header[23] = 0;
    
    int sampleRate = SAMPLE_RATE;
    header[24] = sampleRate & 0xff;
    header[25] = (sampleRate >> 8) & 0xff;
    header[26] = (sampleRate >> 16) & 0xff;
    header[27] = (sampleRate >> 24) & 0xff;
    
    int byteRate = SAMPLE_RATE * 2;
    header[28] = byteRate & 0xff;
    header[29] = (byteRate >> 8) & 0xff;
    header[30] = (byteRate >> 16) & 0xff;
    header[31] = (byteRate >> 24) & 0xff;
    
    header[32] = 2;
    header[33] = 0;
    
    header[34] = 16;
    header[35] = 0;
    
    header[36] = 'd';
    header[37] = 'a';
    header[38] = 't';
    header[39] = 'a';
    
    header[40] = wavDataSize & 0xff;
    header[41] = (wavDataSize >> 8) & 0xff;
    header[42] = (wavDataSize >> 16) & 0xff;
    header[43] = (wavDataSize >> 24) & 0xff;
}

// ===== INITIALIZE I2S MICROPHONE =====
void initializeI2S() {
    I2S.end();
    
    i2s_chan_config_t ch_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&ch_cfg, NULL, &rx_channel);
    
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = (gpio_num_t)CLK_PIN,
            .din = (gpio_num_t)DATA_PIN,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    
    i2s_channel_init_pdm_rx_mode(rx_channel, &pdm_rx_cfg);
    i2s_channel_enable(rx_channel);
    
    is_recording = true;
    Serial.println("I2S microphone initialized on I2S0");
}

// ===== DEINITIALIZE I2S MICROPHONE =====
void deinitializeI2S() {
    if (rx_channel != NULL) {
        i2s_channel_disable(rx_channel);
        i2s_del_channel(rx_channel);
        rx_channel = NULL;
    }
    is_recording = false;
    Serial.println("I2S microphone deinitialized");
}

// ===== TRANSCRIBE SINGLE CHUNK WITH WHISPER API =====
String transcribeSingleChunk(int16_t* chunk_buffer, int chunk_size, int chunk_number) {
    if (chunk_size <= 0) {
        Serial.println("No audio in chunk");
        return "";
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("ERROR: WiFi not connected! Reconnecting...");
        WiFi.reconnect();
        delay(2000);
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("ERROR: WiFi reconnection failed!");
            return "";
        }
    }
    
    uint32_t freeHeap = ESP.getFreeHeap();
    Serial.printf("DEBUG: Free heap before API call: %u bytes\n", freeHeap);
    if (freeHeap < 80000) {
        Serial.printf("WARNING: Low memory! Only %u bytes free\n", freeHeap);
    }
    
    Serial.printf("\nTranscribing chunk %d (%d samples)...\n", chunk_number, chunk_size);
    
    int audioDataSize = chunk_size * 2;
    byte* wavHeader = (byte*)malloc(44);
    if (wavHeader == NULL) {
        Serial.println("Failed to allocate WAV header");
        return "";
    }
    
    createWavHeader(wavHeader, audioDataSize);
    
    String chunk_transcription = "";
    int maxRetries = 3;
    int retryCount = 0;
    
    while (retryCount < maxRetries && chunk_transcription.length() == 0) {
        if (retryCount > 0) {
            Serial.printf("Retry attempt %d/%d...\n", retryCount, maxRetries - 1);
            delay(2000);
            
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("WiFi disconnected during retry, reconnecting...");
                WiFi.reconnect();
                delay(2000);
            }
        }
        
        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(90000);
        
        HTTPClient http;
        
        if (!http.begin(client, whisper_endpoint)) {
            Serial.println("Failed to start HTTP connection to Whisper");
            retryCount++;
            continue;
        }
        
        http.addHeader("Authorization", "Bearer " + String(OPENAI_API_KEY));
        
        String boundary = "----WHISPER_BOUNDARY_" + String(millis() + chunk_number + retryCount);
        String contentType = "multipart/form-data; boundary=" + boundary;
        http.addHeader("Content-Type", contentType);
        http.setTimeout(90000);
        
        String formStart = "--" + boundary + "\r\n";
        formStart += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
        formStart += "whisper-1\r\n";
        
        String formLanguage = "--" + boundary + "\r\n";
        formLanguage += "Content-Disposition: form-data; name=\"language\"\r\n\r\n";
        formLanguage += "en\r\n";
        
        String formAudio = "--" + boundary + "\r\n";
        formAudio += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
        formAudio += "Content-Type: audio/wav\r\n\r\n";
        
        String formEnd = "\r\n--" + boundary + "--\r\n";
        
        int totalSize = formStart.length() + formLanguage.length() + 44 + audioDataSize + formAudio.length() + formEnd.length();
        
        Serial.printf("Request size: %d bytes\n", totalSize);
        
        uint8_t* request_body = (uint8_t*)malloc(totalSize);
        if (request_body == NULL) {
            Serial.println("Failed to allocate request body for chunk");
            http.end();
            retryCount++;
            continue;
        }
        
        int offset = 0;
        memcpy(request_body + offset, formStart.c_str(), formStart.length());
        offset += formStart.length();
        
        memcpy(request_body + offset, formLanguage.c_str(), formLanguage.length());
        offset += formLanguage.length();
        
        memcpy(request_body + offset, formAudio.c_str(), formAudio.length());
        offset += formAudio.length();
        
        memcpy(request_body + offset, wavHeader, 44);
        offset += 44;
        
        memcpy(request_body + offset, (uint8_t*)chunk_buffer, audioDataSize);
        offset += audioDataSize;
        
        memcpy(request_body + offset, formEnd.c_str(), formEnd.length());
        
        Serial.println("Sending request to Whisper API...");
        int httpCode = http.POST(request_body, totalSize);
        
        Serial.printf("HTTP Response code: %d\n", httpCode);
        
        if (httpCode == HTTP_CODE_OK) {
            String response = http.getString();
            JsonDocument doc;
            
            if (deserializeJson(doc, response) == DeserializationError::Ok) {
                chunk_transcription = doc["text"].as<String>();
                Serial.printf("Chunk %d: %s\n", chunk_number, chunk_transcription.c_str());
                http.end();
                free(request_body);
                break;
            } else {
                Serial.println("Failed to parse Whisper response");
                Serial.println("Response: " + response);
            }
        } else if (httpCode > 0) {
            Serial.printf("Whisper API error: %d\n", httpCode);
            String response = http.getString();
            Serial.println("Response: " + response);
        } else {
            Serial.printf("Connection error: %d\n", httpCode);
            Serial.println("This could be a timeout or WiFi issue");
        }
        
        http.end();
        free(request_body);
        retryCount++;
    }
    
    free(wavHeader);
    
    if (chunk_transcription.length() > 0) {
        Serial.println("[OK] Transcription successful!");
    } else {
        Serial.printf("[ERROR] Transcription failed after %d attempts\n", maxRetries);
    }
    
    return chunk_transcription;
}

// ===== TRANSCRIBE WITH WHISPER API =====
String transcribeWithWhisper(int audio_len) {
    currentState = S_TRANSCRIBING;
    Serial.println("\nTranscribing audio with Whisper API...");
    
    if (audio_len <= 0) {
        Serial.println("No audio to transcribe");
        currentState = S_IDLE;
        return "";
    }
    
    Serial.printf("Audio length: %d samples (%.1f seconds)\n", audio_len, (float)audio_len / SAMPLE_RATE);
    
    String full_transcription = transcribeSingleChunk(audio_buffer, audio_len, 1);
    
    currentState = S_IDLE;
    return full_transcription;
}

// ===== SUMMARIZE WITH CHATGPT =====
String summarizeWithChatGPT(String transcription) {
    if (transcription.length() == 0) {
        Serial.println("No transcription to summarize");
        return "";
    }
    
    currentState = S_SUMMARIZING;
    Serial.println("Sending transcription to ChatGPT for summarization...");
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("ERROR: WiFi not connected!");
        currentState = S_IDLE;
        return "[WiFi disconnected - cannot connect to ChatGPT]";
    }
    
    uint32_t freeHeap = ESP.getFreeHeap();
    Serial.printf("DEBUG: Free heap: %u bytes\n", freeHeap);
    if (freeHeap < 50000) {
        Serial.println("ERROR: Low memory - heap too small");
        currentState = S_IDLE;
        return "[Memory error - cannot connect to ChatGPT]";
    }
    
    if (String(OPENAI_API_KEY).length() == 0) {
        Serial.println("ERROR: OpenAI API key not configured!");
        currentState = S_IDLE;
        return "[API key error]";
    }
    
    StaticJsonDocument<2048> doc;
    doc["model"] = "gpt-4o-mini";
    doc["max_tokens"] = 300;
    
    JsonArray messages = doc.createNestedArray("messages");
    
    JsonObject systemMsg = messages.createNestedObject();
    systemMsg["role"] = "system";
    systemMsg["content"] = "You are a concise summarization assistant. Summarize the given text in 2-3 sentences, capturing the main points clearly and concisely.";
    
    JsonObject userMsg = messages.createNestedObject();
    userMsg["role"] = "user";
    userMsg["content"] = "Please summarize this transcription:\n\n" + transcription;
    
    String requestBody;
    serializeJson(doc, requestBody);
    Serial.printf("DEBUG: Request body size: %d bytes\n", requestBody.length());
    
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(30000);
    HTTPClient http;
    
    Serial.printf("DEBUG: Connecting to %s\n", chatgpt_endpoint);
    if (!http.begin(client, chatgpt_endpoint)) {
        Serial.println("ERROR: Failed to start HTTP connection to ChatGPT");
        http.end();
        currentState = S_IDLE;
        return "[Connection failed]";
    }
    
    http.addHeader("Authorization", "Bearer " + String(OPENAI_API_KEY));
    http.addHeader("Content-Type", "application/json");
    http.setConnectTimeout(10000);
    http.setTimeout(60000);
    
    Serial.println("DEBUG: Sending POST request...");
    int httpCode = http.POST(requestBody);
    
    String summary = "";
    
    Serial.printf("DEBUG: HTTP response code: %d\n", httpCode);
    
    if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        Serial.printf("DEBUG: Response length: %d bytes\n", response.length());
        JsonDocument responseDoc;
        
        if (deserializeJson(responseDoc, response) == DeserializationError::Ok) {
            if (responseDoc.containsKey("choices") && responseDoc["choices"].size() > 0) {
                summary = responseDoc["choices"][0]["message"]["content"].as<String>();
                Serial.println("Summary: " + summary);
            } else {
                Serial.println("ERROR: Invalid response format from ChatGPT");
                Serial.println("Response: " + response);
            }
        } else {
            Serial.println("ERROR: Failed to parse ChatGPT response");
            Serial.println("Response: " + response);
        }
    } else if (httpCode == -1) {
        Serial.println("ERROR: Connection timeout or network error");
        Serial.println("Possible causes: WiFi weak, API server unreachable, SSL issue");
        String response = http.getString();
        if (response.length() > 0) {
            Serial.println("Response: " + response);
        }
        summary = "[Network timeout - check WiFi connection]";
    } else {
        Serial.printf("ERROR: ChatGPT API error: %d\n", httpCode);
        String response = http.getString();
        if (response.length() > 0) {
            Serial.println("Response: " + response);
        }
        summary = "[API error " + String(httpCode) + "]";
    }
    
    http.end();
    currentState = S_IDLE;
    
    return summary;
}

// ===== FreeRTOS TASK: TRANSCRIPTION =====
// Runs on Core 1 - transcribes audio chunks in parallel with recording
void transcriptionTask(void* parameter) {
    Serial.println("[TRANSCRIPTION TASK] Started on core: " + String(xPortGetCoreID()));
    
    while (true) {
        if (xSemaphoreTake(transcriptionReadySemaphore, portMAX_DELAY) == pdTRUE) {
            if (!xSemaphoreTake(audioBufferMutex, pdMS_TO_TICKS(1000))) {
                Serial.println("[TRANSCRIPTION TASK] Failed to acquire mutex");
                continue;
            }
            
            if (chunk_size_to_process <= 0) {
                xSemaphoreGive(audioBufferMutex);
                continue;
            }
            
            Serial.printf("[TRANSCRIPTION TASK] Processing chunk (%d samples)...\n", chunk_size_to_process);
            
            indicateTranscribing();
            int local_chunk_size = chunk_size_to_process;
            
            xSemaphoreGive(audioBufferMutex);
            
            String transcription = transcribeSingleChunk(chunk_to_process, local_chunk_size, 1);
            
            if (!xSemaphoreTake(audioBufferMutex, pdMS_TO_TICKS(1000))) {
                Serial.println("[TRANSCRIPTION TASK] Failed to acquire mutex for result");
                continue;
            }
            
            latest_transcription = transcription;
            chunk_processing = false;
            
            if (transcription.length() > 0) {
                Serial.printf("[TRANSCRIPTION] %s\n", transcription.c_str());
            }
            
            xSemaphoreGive(summarizationReadySemaphore);
            xSemaphoreGive(audioBufferMutex);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ===== FreeRTOS TASK: SUMMARIZATION =====
// Runs on Core 1 - summarizes transcriptions in parallel
void summarizationTask(void* parameter) {
    Serial.println("[SUMMARIZATION TASK] Started on core: " + String(xPortGetCoreID()));
    
    while (true) {
        if (xSemaphoreTake(summarizationReadySemaphore, portMAX_DELAY) == pdTRUE) {
            if (!xSemaphoreTake(audioBufferMutex, pdMS_TO_TICKS(1000))) {
                Serial.println("[SUMMARIZATION TASK] Failed to acquire mutex");
                continue;
            }
            
            String transcription_to_summarize = latest_transcription;
            
            xSemaphoreGive(audioBufferMutex);
            
            if (transcription_to_summarize.length() == 0) {
                Serial.println("[SUMMARIZATION TASK] No transcription to summarize");
                continue;
            }
            
            Serial.println("[SUMMARIZATION TASK] Summarizing transcription...");
            indicateTranscribing();
            
            String summary = summarizeWithChatGPT(transcription_to_summarize);
            
            if (!xSemaphoreTake(audioBufferMutex, pdMS_TO_TICKS(1000))) {
                Serial.println("[SUMMARIZATION TASK] Failed to acquire mutex for saving");
                continue;
            }
            
            latest_summary = summary;
            saveTranscription(transcription_to_summarize, summary);
            
            if (summary.length() > 0) {
                Serial.printf("[SUMMARY] %s\n", summary.c_str());
            }
            
            xSemaphoreGive(processingCompleteSemaphore);
            xSemaphoreGive(audioBufferMutex);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ===== INITIALIZE FreeRTOS TASKS =====
void initializeFreeRTOSTasks() {
    Serial.println("[FREERTOS] Initializing parallel tasks and synchronization primitives...");
    
    audioBufferMutex = xSemaphoreCreateMutex();
    if (audioBufferMutex == NULL) {
        Serial.println("ERROR: Failed to create audio buffer mutex");
        return;
    }
    
    transcriptionReadySemaphore = xSemaphoreCreateBinary();
    if (transcriptionReadySemaphore == NULL) {
        Serial.println("ERROR: Failed to create transcription semaphore");
        return;
    }
    
    summarizationReadySemaphore = xSemaphoreCreateBinary();
    if (summarizationReadySemaphore == NULL) {
        Serial.println("ERROR: Failed to create summarization semaphore");
        return;
    }
    
    processingCompleteSemaphore = xSemaphoreCreateBinary();
    if (processingCompleteSemaphore == NULL) {
        Serial.println("ERROR: Failed to create processing complete semaphore");
        return;
    }
    
    BaseType_t result1 = xTaskCreatePinnedToCore(
        transcriptionTask,
        "TranscriptionTask",
        16384,
        NULL,
        3,
        &transcriptionTaskHandle,
        1
    );
    
    if (result1 != pdPASS) {
        Serial.println("ERROR: Failed to create transcription task");
    } else {
        Serial.println("[FREERTOS] Transcription task created on Core 1");
    }
    
    BaseType_t result2 = xTaskCreatePinnedToCore(
        summarizationTask,
        "SummarizationTask",
        16384,
        NULL,
        1,
        &summarizationTaskHandle,
        1
    );
    
    if (result2 != pdPASS) {
        Serial.println("ERROR: Failed to create summarization task");
    } else {
        Serial.println("[FREERTOS] Summarization task created on Core 1");
    }
    
    Serial.println("[FREERTOS] Parallel task initialization complete!");
    Serial.println("Recording on Main Loop (Core 0) | Transcription & Summarization on Core 1");
}

// ===== CLEANUP FreeRTOS RESOURCES =====
void cleanupFreeRTOSTasks() {
    if (transcriptionTaskHandle != NULL) {
        vTaskDelete(transcriptionTaskHandle);
        transcriptionTaskHandle = NULL;
    }
    
    if (summarizationTaskHandle != NULL) {
        vTaskDelete(summarizationTaskHandle);
        summarizationTaskHandle = NULL;
    }
    
    if (audioBufferMutex != NULL) {
        vSemaphoreDelete(audioBufferMutex);
        audioBufferMutex = NULL;
    }
    
    if (transcriptionReadySemaphore != NULL) {
        vSemaphoreDelete(transcriptionReadySemaphore);
        transcriptionReadySemaphore = NULL;
    }
    
    if (summarizationReadySemaphore != NULL) {
        vSemaphoreDelete(summarizationReadySemaphore);
        summarizationReadySemaphore = NULL;
    }
    
    if (processingCompleteSemaphore != NULL) {
        vSemaphoreDelete(processingCompleteSemaphore);
        processingCompleteSemaphore = NULL;
    }
    
    Serial.println("[FREERTOS] Tasks and synchronization resources cleaned up");
}

// ===== SETUP =====
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\nESP32 Audio Summarizer - Auto-Start with RGB Status LED");
    Serial.println("=======================================================");
    
    pinMode(RGB_RED_PIN, OUTPUT);
    pinMode(RGB_GREEN_PIN, OUTPUT);
    pinMode(RGB_BLUE_PIN, OUTPUT);
    
    indicateIdle();
    
    initSPIFFS();
    loadWiFiConfig();
    loadSettings();
    setupWiFi();
    loadTranscriptions();
    setupWebServer();
    
    audio_buffer = (int16_t*)malloc(audio_buffer_size);
    i2s_temp_buffer = (int16_t*)malloc(256 * sizeof(int16_t));
    chunk_to_process = (int16_t*)malloc(audio_buffer_size);
    
    if (audio_buffer == NULL || i2s_temp_buffer == NULL || chunk_to_process == NULL) {
        Serial.println("Failed to allocate buffers");
        return;
    }
    
    Serial.println("\nSetup complete!");
    Serial.println("==================== AUTO-START RECORDING ====================");
    Serial.println("Recording starts automatically on power-on");
    Serial.println("Recording stops automatically on power-off");
    Serial.println("Every 30 seconds: auto-transcribe, summarize, and send to server (in background)");
    Serial.println("Access web interface at http://<ESP32_IP>");
    Serial.println("\nRGB LED Status Indicators:");
    Serial.println("  - Green: Idle/Standby");
    Serial.println("  - Red: Recording in progress");
    Serial.println("  - Blue: Transcribing/Processing");
    Serial.println("\nFeatures:");
    Serial.println("  - Timezone Support (IST default)");
    Serial.println("  - 12-Hour Time Format (default)");
    Serial.println("  - Search & Filter by keyword, date, time");
    Serial.println("  - Export to CSV");
    Serial.println("  - Dark Mode Toggle");
    Serial.println("  - NTP Auto Re-sync (every 60 minutes)");
    Serial.println("==============================================================");
    
    Serial.println("\n[POWER LOSS RECOVERY] Checking for recovered chunks...");
    processRecoveredChunk();
    
    initializeFreeRTOSTasks();
    
    initializeI2S();
    is_recording = true;
    samples_recorded = 0;
    chunk_start_time = millis();
    chunk_ready = false;
    overall_recording_start_unix = time(nullptr);
}

// ===== MAIN LOOP =====
void loop() {
    server.handleClient();
    
    if (is_recording) {
        RGBColor off = COLOR_OFF;
        setRGBColor(off);
    } else if (chunk_processing) {
        indicateTranscribing();
    } else {
        indicateIdle();
    }
    
    if (!is_recording) {
        delay(50);
        return;
    }
    
    // Read audio samples from I2S0 PDM Rx
    size_t bytes_read = 0;
    if (rx_channel != NULL) {
        i2s_channel_read(rx_channel, i2s_temp_buffer, 256 * sizeof(int16_t), &bytes_read, 100);
    }
    int samples_in_chunk = bytes_read / sizeof(int16_t);
    
    int max_samples = audio_buffer_size / sizeof(int16_t);
    
    for (int i = 0; i < samples_in_chunk; i++) {
        audio_buffer[samples_recorded] = i2s_temp_buffer[i];
        samples_recorded++;
        
        if (samples_recorded >= max_samples) {
            samples_recorded = max_samples;
            chunk_ready = true;
        }
    }
    
    // Print progress every 5 seconds
    static unsigned long lastPrintTime = 0;
    unsigned long currentTime = millis();
    if (currentTime - lastPrintTime >= 5000) {
        float elapsed = (currentTime - chunk_start_time) / 1000.0;
        Serial.printf("Recording... %.1f sec, %d/%d samples\n", 
                     elapsed, samples_recorded, max_samples);
        lastPrintTime = currentTime;
    }
    
    // Auto-transcribe every 30 seconds with FreeRTOS parallel tasking
    if (chunk_ready) {
        chunk_ready = false;
        
        if (xSemaphoreTake(audioBufferMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            memcpy(chunk_to_process, audio_buffer, samples_recorded * sizeof(int16_t));
            chunk_size_to_process = samples_recorded;
            chunk_processing = true;
            
            Serial.printf("\n>>> 30-SECOND CHUNK READY! (%.1f seconds) Signaling transcription task...\n", 
                         (float)samples_recorded / SAMPLE_RATE);
            
            samples_recorded = 0;
            chunk_start_time = millis();
            
            xSemaphoreGive(audioBufferMutex);
            
            xSemaphoreGive(transcriptionReadySemaphore);
        } else {
            Serial.println("Warning: Could not acquire mutex for chunk handoff");
        }
        
        delay(100);
    }
    
    delay(1);
}
