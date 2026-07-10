#include <WiFi.h>               // connect wifi
#include <WebServer.h>          // webserver
#include <HTTPClient.h>         // unencrypted http
#include <WiFiClientSecure.h>   // https
#include <ArduinoJson.h>        // json aprsing
#include "config.h"            // local secrets and endpoint settings

bool braveSearchEnabled = true;

WebServer server(80);                           // port server listens on
String inputLine;                               // input from text box on website
String llmOutput;                               // Output from LLM
String contextWindow[6] = {"","","","","",""};  // context window array
String contextCollapsed = ""; // collapsed context for the bottom row and for prompting

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

  // Define route handlers
  server.on("/", handleRoot); // serve the main page
  server.on("/get-text", handleText); // handle message and port form submissions
  server.on("/toggle-brave", handleToggleBrave); // toggle live web search on or off

  // Start the server
  server.begin(); // start listening for HTTP requests
}

void loop() {
  server.handleClient(); // process one pending HTTP client at a time
}