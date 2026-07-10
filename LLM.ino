void askLLM(const String& message) {
  WiFiClient client;
        if(!client.connect(llmHost, llmPort)){
            Serial.println("Connecting to Monolith Failed");
            return;
        }

  client.setTimeout(30000);

  StaticJsonDocument<1024> doc;
  doc["model"] = "model";
  doc["stream"] = true;
  doc["temperature"] = 0.7;
  doc["max_tokens"] = 4096;

  JsonArray messages = doc.createNestedArray("messages");
  JsonObject user = messages.createNestedObject();
  user["role"] = "user";
  user["content"] = message;

  String body;
  serializeJson(doc, body);
    
    client.print(String("POST ") + llmPath + " HTTP/1.1\r\n" + 
                "Host: " + llmHost + "\r\n" + 
                "Content-type:application/json\r\n" +
                "Accept: text/event-stream\r\n" +
                "Connection: close\r\n" +
                "Content-Length: " + body.length() + "\r\n\r\n" +
                body);
  //skip http respo headers since we're streaming - Timeout configured here
  unsigned long timeout = millis();
  while (client.connected() && !client.available()) {
    if(millis() - timeout > 30000){
      server.send(200, "text/html", "<h1>LLM TIMEOUT<'/'h1><br><a href='/'>Go back</a>");
      closeConnection(client);
      return;
    }
    delay(1);
  }
  while (client.connected() || client.available()) {
    String headerLine = client.readStringUntil('\n');
    if (headerLine == "\r" || headerLine.length() == 0) break;
  }
  Serial.print("LLM: ");

  bool done = false;
  while (!done && (client.connected() || client.available())) {
    if (!client.available()) {
      delay(1);
      continue;
    }
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (!line.startsWith("data:")) continue;

    String payload = line.substring(5);
    payload.trim();
 
    if (payload == "[DONE]") {
      done = true;
      break;
    }

    String content;
    if (extractContent(payload, content)) {
      Serial.print(content);
      
    }
  }

  Serial.println();
  closeConnection(client);  

}

void closeConnection(WiFiClient& client) {
  unsigned long start = millis();
  while ((client.connected() || client.available()) && millis() - start < 2000) {
    while (client.available()) client.read();
    delay(1);
  }
  client.stop();
}

bool extractContent(const String& json, String& outContent) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) return false;

  JsonArray choices = doc["choices"].as<JsonArray>();
  if (choices.isNull() || choices.size() == 0) return false;

  JsonObject delta = choices[0]["delta"];
  if (delta.isNull() || !delta.containsKey("content")) return false;

  const char* content = delta["content"];
  if (!content) return false;

  outContent = content;
 llmOutput+= content;
  return true;
}
void addToHistory(String sender, String message){
    for(int i = 0; i < 5; i++){
        contextWindow[i] = contextWindow[i + 1];
    }

    contextWindow[5] = sender + ": " + message;
    printHistory();
}

void printHistory() {
  Serial.println("--- Current Chat History ---");
  for (int i = 0; i < 6; i++) {
    if (contextWindow[i] != "") { // Skip empty slots
      Serial.println(contextWindow[i]);
    }
  }
}
void collapseHistory(){
    for (int i = 0; i < 6; i++) {
        if (contextWindow[i] != "") { // Skip empty slots
         contextCollapsed += contextWindow[i] + "|";
        }
    }
    Serial.println(contextCollapsed);
}