#include <WiFi.h>               // connect wifi
#include <WebServer.h>          // webserver
#include <HTTPClient.h>         // unencrypted http
#include <WiFiClientSecure.h>   // https
#include <ArduinoJson.h>        // json aprsing
#include <time.h>               // NTP-backed wall-clock time for the datetime tool
#include <freertos/FreeRTOS.h>  // task and mutex primitives for async LLM requests
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "config.h"            // local secrets and endpoint settings

bool braveSearchEnabled = true;

WebServer server(80);                           // port server listens on
String inputLine;                               // input from text box on website
String llmOutput;                               // Output from LLM
String contextWindow[6] = {"","","","","",""};  // context window array
String contextCollapsed = ""; // collapsed context for the bottom row and for prompting
String pendingLLMPrompt = ""; // queued prompt handed off to the background LLM task
SemaphoreHandle_t chatStateMutex = nullptr; // guards shared chat state across the web thread and LLM task
TaskHandle_t llmTaskHandle = nullptr; // tracks the currently running background LLM task
volatile bool llmRequestInFlight = false; // reports whether a background LLM request is still running

bool deviceClockReady() {
  return time(nullptr) >= 1700000000; // treat obviously pre-NTP epochs as unsynchronized
}

void syncDeviceClock() {
  configTzTime(DEVICE_TIME_ZONE, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY); // start SNTP with the configured local timezone rules

  struct tm localTimeInfo;
  if (getLocalTime(&localTimeInfo, 10000)) {
    Serial.print("Clock synchronized: "); // log the resolved local wall-clock time for verification
    Serial.println(&localTimeInfo, "%A, %Y-%m-%d %H:%M:%S %Z");
    return;
  }

  Serial.println("Clock synchronization did not complete before timeout."); // keep booting even if NTP is temporarily unavailable
}

void setup() {
  Serial.begin(115200);         //start serial 115200
  WiFi.begin(ssid, password);   //start wifi
  
  while (WiFi.status() != WL_CONNECTED) {   //while wifi not connected
    delay(500);         //wait 500 millis
    Serial.print(".");  //print a dot each time
  }
  
  Serial.println("\nConnected to Wi-Fi"); //alert terminal connection succeeded
  Serial.print("IP Address: "); //print ip
  Serial.println(WiFi.localIP()); //print ip (contd)

  syncDeviceClock(); // initialize wall-clock time before any LLM tool requests use it

  chatStateMutex = xSemaphoreCreateMutex(); // protect shared UI/chat state when the LLM runs off the main loop
  xTaskCreate(runLLMWorkerTask, "llm-worker", 12288, nullptr, 1, &llmTaskHandle); // keep one persistent worker instead of creating/deleting a task per chat turn

  // Define route handlers
  server.on("/", handleRoot); // serve the main page
  server.on("/api/chat-state", handleChatState); // expose chat progress without forcing a full page reload
  server.on("/api/sakura-log", handleSakuraLogProxy); // proxy the latest Sakura log line through the ESP32
  server.on("/get-text", handleText); // handle message and port form submissions
  server.on("/toggle-brave", handleToggleBrave); // toggle live web search on or off

  // Start the server
  server.begin(); // start listening for HTTP requests
}

void loop() {
  server.handleClient(); // process one pending HTTP client at a time
}