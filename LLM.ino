String buildToolDecisionPrompt(const String& message) {
  String prompt = "You are an assistant running on an ESP32. "; // base instruction for tool-selection pass
  const bool weatherToolAvailable = OPENWEATHER_API_KEY != nullptr && OPENWEATHER_API_KEY[0] != '\0'; // weather tool is available only when its API key is configured

  if (braveSearchEnabled || weatherToolAvailable) {
    prompt += "If a live tool is needed, respond with only a single JSON object and no markdown. "; // tell the model the exact JSON tool-call shapes when tools are available

    if (braveSearchEnabled) {
      prompt +=
        "For current web information, use exactly this schema: {\"tool\":\"brave_search\",\"arguments\":{\"query\":\"...\",\"count\":5}}. ";
    }

    if (weatherToolAvailable) {
      prompt +=
        "For current weather at the configured location, use exactly this schema: {\"tool\":\"openweather_current\",\"arguments\":{\"units\":\"metric\"}}. "
        "The units field must be either metric or imperial. ";
    }

    prompt += "If no tool is needed, answer normally. ";
  } else {
    prompt += // force a normal natural-language answer when no live tools are available
      "Live tools are unavailable. Do not call any tools and do not respond with JSON. "
      "Answer normally using only built-in knowledge, and say when live lookup is unavailable. ";
  }

  prompt += "User request: "; // prefix the user's text consistently
  return prompt + message; // hand the finished prompt to the LLM request layer
}

String buildToolFollowupPrompt(const String& originalMessage, const String& toolResult) {
  return String( // second-pass prompt that grounds the answer in tool output
    "Answer the user's request using the tool result below. "
    "Do not call any more tools. Be concise and mention useful URLs when relevant. "
    "User request: "
  ) + originalMessage + "\nTool result JSON: " + toolResult; // include both the request and the tool payload
}

bool looksLikeToolCall(const String& response) {
  String trimmed = response; // copy so we can normalize without mutating caller data
  trimmed.trim(); // drop surrounding whitespace from streamed output

  return trimmed.startsWith("{") && trimmed.indexOf("\"tool\"") >= 0; // crude check for a tool-call JSON object
}

bool messageNeedsCurrentWeather(const String& message) {
  String normalized = message; // copy so matching can ignore case without mutating caller state
  normalized.toLowerCase(); // normalize casing for simple substring checks

  return normalized.indexOf("weather") >= 0 ||
         normalized.indexOf("forecast") >= 0 ||
         normalized.indexOf("temperature") >= 0 ||
         normalized.indexOf("rain") >= 0 ||
         normalized.indexOf("snow") >= 0 ||
         normalized.indexOf("wind") >= 0; // route common weather queries directly to the weather tool
}

String getPreferredWeatherUnits(const String& message) {
  String normalized = message; // copy so matching can ignore case without mutating caller state
  normalized.toLowerCase(); // normalize casing for simple substring checks

  if (normalized.indexOf("imperial") >= 0 ||
      normalized.indexOf("fahrenheit") >= 0 ||
      normalized.indexOf(" mph") >= 0) {
    return "imperial"; // prefer US customary units when the user asks for them explicitly
  }

  return "metric"; // otherwise default to metric units
}

bool streamLLMResponse(const String& message, String& responseOut) {
  responseOut = ""; // clear any prior response text before streaming

  WiFiClient client; // plain TCP client for the local LLM endpoint
  if (!client.connect(llmHost, llmPort)) {
    Serial.println("Connecting to Monolith Failed"); // report connection failure on serial
    return false; // stop before attempting to write a request
  }

  client.setTimeout(30000); // protect blocking reads on a slow model backend

  StaticJsonDocument<3072> doc; // request payload document
  doc["model"] = "model"; // model identifier expected by the server
  doc["stream"] = true; // ask for server-sent event style streaming
  doc["temperature"] = 0.7; // moderate creativity
  doc["max_tokens"] = 4096; // upper bound for generation length


  JsonArray messages = doc.createNestedArray("messages"); // chat-format message list

  if (SYSTEM_PROMPT) {
    JsonObject system = messages.createNestedObject(); // prepend system instruction when configured
    system["role"] = "system"; // mark the message as a system role
    system["content"] = SYSTEM_PROMPT; // send the persistent assistant behavior prompt
  }

  JsonObject user = messages.createNestedObject(); // append the caller prompt as the user turn
  user["role"] = "user"; // standard chat completion role
  user["content"] = message; // actual prompt text sent to the model

  String body; // serialized request body
  serializeJson(doc, body); // turn the JSON document into a request string
    
    client.print(String("POST ") + llmPath + " HTTP/1.1\r\n" + // start an HTTP POST to the configured LLM path
                "Host: " + llmHost + "\r\n" + // supply host header expected by the server
                "Content-type:application/json\r\n" + // declare a JSON body
                "Accept: text/event-stream\r\n" + // request streaming chunks
                "Connection: close\r\n" + // let the server close the socket when complete
                "Content-Length: " + body.length() + "\r\n\r\n" + // include content length before payload
                body); // send the request body itself
  //skip http respo headers since we're streaming - Timeout configured here
  unsigned long timeout = millis(); // remember when we started waiting for response bytes
  while (client.connected() && !client.available()) {
    if(millis() - timeout > 30000){
      closeConnection(client); // drain and close the socket on startup timeout
      return false; // surface the timeout to the caller
    }
    delay(1); // yield briefly while waiting for the server to respond
  }
  while (client.connected() || client.available()) {
    String headerLine = client.readStringUntil('\n'); // consume HTTP headers line by line
    if (headerLine == "\r" || headerLine.length() == 0) break; // stop once the blank line after headers is reached
  }
  Serial.print("LLM: "); // begin mirroring streamed tokens to the serial console

  bool done = false; // track the [DONE] sentinel from the stream
  while (!done && (client.connected() || client.available())) {
    if (!client.available()) {
      delay(1); // wait for the next chunk when the socket is temporarily idle
      continue; // re-check connection state
    }
    String line = client.readStringUntil('\n'); // read one server-sent event line
    line.trim(); // normalize whitespace for prefix checks
    if (line.length() == 0) continue; // ignore blank keepalive lines
    if (!line.startsWith("data:")) continue; // ignore non-payload event lines

    String payload = line.substring(5); // strip the SSE field name
    payload.trim(); // normalize before inspecting content
 
    if (payload == "[DONE]") {
      done = true; // stop once the server signals completion
      break; // leave the streaming loop
    }

    String content; // token fragment extracted from this event payload
    if (extractContent(payload, content)) {
      Serial.print(content); // mirror tokens live for debugging
      responseOut += content; // accumulate the final answer string
    }
  }

  Serial.println(); // end the console line after streaming finishes
  closeConnection(client);  // drain any remaining bytes and stop the socket

  return true; // signal success to the caller
}

void askLLM(const String& message) {
  llmOutput = ""; // clear the previous answer before starting a new turn

  const bool weatherToolAvailable = OPENWEATHER_API_KEY != nullptr && OPENWEATHER_API_KEY[0] != '\0'; // weather tool is available only when its API key is configured
  if (weatherToolAvailable && messageNeedsCurrentWeather(message)) {
    String toolRequest = String("{\"tool\":\"openweather_current\",\"arguments\":{\"units\":\"") + getPreferredWeatherUnits(message) + "\"}}"; // synthesize a direct tool call for obvious current-weather requests
    String toolResult; // JSON result returned by the weather tool

    if (!handleToolCall(toolRequest, toolResult)) {
      llmOutput = toolResult.length() > 0 ? toolResult : "Weather lookup failed."; // surface weather tool failure details when possible
      return; // skip the LLM pass if the direct weather lookup failed
    }

    String finalResponse; // follow-up pass turns the weather tool output into a user-facing answer
    if (!streamLLMResponse(buildToolFollowupPrompt(message, toolResult), finalResponse)) {
      llmOutput = "LLM follow-up failed or timed out."; // show a separate error for the follow-up call
      return; // stop after a failed second pass
    }

    llmOutput = finalResponse; // store the final answer for direct weather requests
    return; // skip the general tool-decision pass once weather has been handled
  }

  String firstResponse; // first pass decides between a direct answer and a tool call
  if (!streamLLMResponse(buildToolDecisionPrompt(message), firstResponse)) {
    llmOutput = "LLM request failed or timed out."; // show a user-visible failure message
    return; // stop if the initial model request failed
  }

  if (!looksLikeToolCall(firstResponse)) {
    llmOutput = firstResponse; // direct answer path when no tool call was requested
    return; // nothing else to do
  }

  String toolResult; // JSON result returned by the requested tool
  if (!handleToolCall(firstResponse, toolResult)) {
    llmOutput = toolResult.length() > 0 ? toolResult : "Tool request failed."; // surface tool failure details when possible
    return; // skip the second LLM pass if the tool could not run
  }

  String finalResponse; // second pass turns tool output into a user-facing answer
  if (!streamLLMResponse(buildToolFollowupPrompt(message, toolResult), finalResponse)) {
    llmOutput = "LLM follow-up failed or timed out."; // show a separate error for the follow-up call
    return; // stop after a failed second pass
  }

  llmOutput = finalResponse; // store the final answer for the web response

}

void closeConnection(WiFiClient& client) {
  unsigned long start = millis(); // bound how long we spend draining the socket
  while ((client.connected() || client.available()) && millis() - start < 2000) {
    while (client.available()) client.read(); // consume unread bytes so the socket can close cleanly
    delay(1); // yield while waiting for the peer to finish closing
  }
  client.stop(); // release the underlying client connection
}

bool extractContent(const String& json, String& outContent) {
  StaticJsonDocument<512> doc; // scratch document for one streamed SSE chunk
  DeserializationError err = deserializeJson(doc, json); // parse the chunk as JSON
  if (err) return false; // ignore malformed chunks

  JsonArray choices = doc["choices"].as<JsonArray>(); // OpenAI-style choice array
  if (choices.isNull() || choices.size() == 0) return false; // no usable content in this event

  JsonObject delta = choices[0]["delta"]; // incremental token payload
  if (delta.isNull() || !delta.containsKey("content")) return false; // skip role-only or finish events

  const char* content = delta["content"]; // raw text fragment from the chunk
  if (!content) return false; // guard against null strings

  outContent = content; // copy the fragment back to the caller
  return true; // content extraction succeeded
}
void addToHistory(String sender, String message){
    for(int i = 0; i < 5; i++){
        contextWindow[i] = contextWindow[i + 1]; // shift older messages toward the front of the ring buffer
    }

    contextWindow[5] = sender + ": " + message; // append the newest chat turn at the end
    printHistory(); // log the updated history to serial
}

void printHistory() {
  Serial.println("--- Current Chat History ---"); // header for serial debugging
  for (int i = 0; i < 6; i++) {
    if (contextWindow[i] != "") { // Skip empty slots
      Serial.println(contextWindow[i]); // print each populated history entry
    }
  }
}
void collapseHistory(){
  contextCollapsed = ""; // rebuild the collapsed prompt text from scratch each turn

    for (int i = 0; i < 6; i++) {
        if (contextWindow[i] != "") { // Skip empty slots
     contextCollapsed += "> " + contextWindow[i] + "\n"; // format history as quoted lines for the prompt
        }
    }

    Serial.println(contextCollapsed); // show the collapsed prompt history on serial
}