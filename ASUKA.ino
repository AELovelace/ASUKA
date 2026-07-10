#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Replace with your Wi-Fi credentials
const char* ssid = "TailBoard";
const char* password = "esp32router";

WebServer server(80);
const char* llmHost = "100.66.64.45";
uint16_t llmPort = 9090;
const char* llmPath = "/v1/chat/completions";

String inputLine;
String llmOutput;
String contextWindow[6] = {"","","","","",""};
String contextCollapsed = "";

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nConnected to Wi-Fi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Define route handlers
  server.on("/", handleRoot);
  server.on("/get-text", handleText);

  // Start the server
  server.begin();
}

void loop() {
  server.handleClient();
}