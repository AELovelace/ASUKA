String buildToolDecisionPrompt(const String& message) {
  String prompt = "You are an assistant running on an ESP32. "; // base instruction for tool-selection pass
  const bool urlFetchToolAvailable = true; // fetch_url is always callable when the device has outbound network access
  const bool weatherToolAvailable = OPENWEATHER_API_KEY != nullptr && OPENWEATHER_API_KEY[0] != '\0'; // weather tool is available only when its API key is configured
  const bool dateTimeToolAvailable = true; // current_datetime is always callable, though it may report an unsynchronized clock at runtime

  if (braveSearchEnabled || urlFetchToolAvailable || weatherToolAvailable || dateTimeToolAvailable) {
    prompt += "If a live tool is needed, respond with only a single JSON object and no markdown. "; // tell the model the exact JSON tool-call shapes when tools are available

    if (braveSearchEnabled) {
      prompt +=
        "For current web information, use exactly this schema: {\"tool\":\"brave_search\",\"arguments\":{\"query\":\"...\",\"count\":5}}. ";
    }

    if (urlFetchToolAvailable) {
      prompt +=
        "For reading the contents of a specific http or https URL, use exactly this schema: {\"tool\":\"fetch_url\",\"arguments\":{\"url\":\"https://...\",\"max_chars\":4000}}. "
        "Use fetch_url when the user provides a link or asks you to inspect a specific page. ";
    }

    if (weatherToolAvailable) {
      prompt +=
        "For current weather at the configured location, use exactly this schema: {\"tool\":\"openweather_current\",\"arguments\":{\"units\":\"metric\"}}. "
        "The units field must be either metric or imperial. ";
    }

    if (dateTimeToolAvailable) {
      prompt +=
        "For the current local date, time, weekday, or timezone, use exactly this schema: {\"tool\":\"current_datetime\",\"arguments\":{}}. ";
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

String buildToolRoutingPrompt(const String& message) {
  String prompt =
    "You are a lightweight router that decides whether the main assistant needs a live tool before answering. "
    "Reply with exactly one uppercase word: TOOL or DIRECT. "
    "Reply TOOL if the user needs current or changing information, wants you to inspect a specific URL or web page, asks for web search, asks for the current local date or time, or needs current weather. "
    "Reply DIRECT if normal knowledge and reasoning are enough. "
    "If unsure, reply TOOL. User request: ";

  return prompt + message;
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

bool toolRouterIsConfigured() {
  return toolRouterEnabled && toolRouterHost != nullptr && toolRouterHost[0] != '\0' && toolRouterPath != nullptr && toolRouterPath[0] != '\0';
}

bool extractChatMessageContent(const String& json, String& outContent) {
  const int contentKeyIndex = json.indexOf("\"content\""); // locate the assistant content field inside the router response
  if (contentKeyIndex < 0) {
    return false; // response body did not include the expected content field
  }

  int valueStartIndex = json.indexOf(':', contentKeyIndex); // move from the key to the start of the JSON value
  if (valueStartIndex < 0) {
    return false; // malformed JSON field without a separating colon
  }

  valueStartIndex++; // advance past the colon itself
  while (valueStartIndex < json.length() && (json[valueStartIndex] == ' ' || json[valueStartIndex] == '\t' || json[valueStartIndex] == '\r' || json[valueStartIndex] == '\n')) {
    valueStartIndex++; // skip insignificant whitespace before the string literal
  }

  if (valueStartIndex >= json.length() || json[valueStartIndex] != '"') {
    return false; // the content field was not a JSON string
  }

  valueStartIndex++; // start reading the string contents after the opening quote
  outContent = "";

  bool escaping = false; // track JSON backslash escapes while scanning the string literal
  for (int i = valueStartIndex; i < json.length(); i++) {
    const char c = json[i];

    if (escaping) {
      if (c == 'n' || c == 'r' || c == 't') {
        outContent += ' '; // normalize escaped whitespace to a plain space for the router token
      } else {
        outContent += c; // keep simple escaped characters such as \" and \\ as their literal value
      }

      escaping = false;
      continue;
    }

    if (c == '\\') {
      escaping = true;
      continue;
    }

    if (c == '"') {
      return outContent.length() > 0; // closing quote marks the end of the string value
    }

    outContent += c;
  }

  outContent = "";
  return false; // unterminated JSON string
}

bool requestToolRoutingDecision(const String& message, bool& shouldUseTools) {
  shouldUseTools = true; // default fail-open so the existing tool-selection path still runs on errors

  if (!toolRouterIsConfigured()) {
    Serial.println("Route decision: TOOL_SELECTION (router disabled or incomplete)");
    return false; // router is disabled or incomplete, so the caller should use the existing path
  }

  StaticJsonDocument<2048> doc; // request payload document for the router
  doc["model"] = "model"; // model identifier expected by the server
  doc["stream"] = false; // lightweight router returns one short completion
  doc["temperature"] = 0; // keep the classification deterministic
  doc["max_tokens"] = 4; // router must answer with a single short token

  JsonArray messages = doc.createNestedArray("messages");
  JsonObject user = messages.createNestedObject();
  user["role"] = "user";
  user["content"] = buildToolRoutingPrompt(message);

  String body;
  serializeJson(doc, body);

  const String requestUrl = String("http://") + toolRouterHost + ":" + String(toolRouterPort) + toolRouterPath;
  HTTPClient http; // high-level HTTP wrapper for the lightweight router endpoint

  if (!http.begin(requestUrl)) {
    Serial.println("Connecting to tool router failed");
    Serial.println("Route decision: TOOL_SELECTION (router connection failed)");
    return false;
  }

  http.setTimeout(15000); // keep the lightweight routing pass on a shorter leash than the full model
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");

  const int httpStatus = http.POST(body); // send the router request and capture the HTTP status code
  String responseBody;
  bool responseTruncated = false;

  if (httpStatus <= 0) {
    Serial.print("Tool router HTTP request failed: ");
    Serial.println(http.errorToString(httpStatus));
    http.end();
    Serial.println("Route decision: TOOL_SELECTION (router request failed)");
    return false;
  }

  if (!readHttpBodyWithLimit(http, 2048, responseBody, responseTruncated)) {
    Serial.println("Tool router response body read failed");
    http.end();
    Serial.println("Route decision: TOOL_SELECTION (router body read failed)");
    return false;
  }

  http.end();

  if (httpStatus < 200 || httpStatus >= 300) {
    Serial.print("Tool router HTTP status: ");
    Serial.println(httpStatus);
    Serial.print("Tool router body: ");
    Serial.println(responseBody);
    Serial.println("Route decision: TOOL_SELECTION (router returned error status)");
    return false;
  }

  if (responseTruncated) {
    Serial.println("Tool router response body was truncated");
  }

  String routerDecision;
  if (!extractChatMessageContent(responseBody, routerDecision)) {
    Serial.println("Tool router returned an unreadable response");
    Serial.print("Tool router body: ");
    Serial.println(responseBody);
    Serial.println("Route decision: TOOL_SELECTION (router parse failed)");
    return false;
  }

  routerDecision.trim();
  routerDecision.toUpperCase();
  shouldUseTools = routerDecision == "TOOL";

  Serial.print("Tool router decision: ");
  Serial.println(routerDecision);

  return routerDecision == "TOOL" || routerDecision == "DIRECT";
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

String extractFirstHttpUrl(const String& message) {
  int startIndex = message.indexOf("https://"); // prefer secure URLs when both schemes appear in the same prompt

  if (startIndex < 0) {
    startIndex = message.indexOf("http://"); // fall back to plain HTTP URLs when no HTTPS URL is present
  }

  if (startIndex < 0) {
    return ""; // no fetchable URL appears in the user's message
  }

  int endIndex = startIndex; // advance until the URL terminator is reached

  while (endIndex < message.length()) {
    const char c = message[endIndex]; // current character being considered as part of the URL

    if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '"' || c == '\'' || c == '<' || c == '>') {
      break; // stop at common whitespace and quoting delimiters surrounding pasted links
    }

    endIndex++; // keep scanning until a delimiter is found
  }

  String url = message.substring(startIndex, endIndex); // extract the detected URL from the user's message

  while (url.endsWith(",") || url.endsWith(".") || url.endsWith(")") || url.endsWith("]")) {
    url.remove(url.length() - 1); // trim common trailing punctuation from pasted prose links
  }

  return url; // return the normalized URL candidate for direct fetch handling
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

void publishLLMPreview(const String& previewText) {
  if (chatStateMutex != nullptr) {
    xSemaphoreTake(chatStateMutex, portMAX_DELAY);
    llmOutput = previewText;
    xSemaphoreGive(chatStateMutex);
    return;
  }

  llmOutput = previewText;
}

bool streamLLMResponse(const String& message, String& responseOut, bool publishPartialResponse = false) {
  responseOut = ""; // clear any prior response text before streaming

  if (publishPartialResponse) {
    publishLLMPreview(""); // clear the visible preview before the first streamed token arrives
  }

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

      if (publishPartialResponse) {
        publishLLMPreview(responseOut); // expose the current streamed prefix to the chat-state poller
      }
    }
  }

  Serial.println(); // end the console line after streaming finishes
  closeConnection(client);  // drain any remaining bytes and stop the socket

  return true; // signal success to the caller
}

void askLLM(const String& message, String& responseOut) {
  responseOut = ""; // clear the previous answer before starting a new turn

  const String firstUrlInMessage = extractFirstHttpUrl(message); // detect explicit links so we can fetch page context without relying on the model to choose the tool
  const bool weatherToolAvailable = OPENWEATHER_API_KEY != nullptr && OPENWEATHER_API_KEY[0] != '\0'; // weather tool is available only when its API key is configured
  const bool dateTimeToolAvailable = true; // current_datetime is always exposed to the model during tool selection
  const bool toolCallsPossible = braveSearchEnabled || weatherToolAvailable || dateTimeToolAvailable || true; // fetch_url is always available when the device can reach the network
  const bool toolDecisionCanStream = !toolCallsPossible; // safe to stream the first pass only when it cannot emit tool-call JSON

  if (!firstUrlInMessage.isEmpty()) {
    Serial.println("Route decision: DIRECT_FETCH_URL");
    String toolRequest = String("{\"tool\":\"fetch_url\",\"arguments\":{\"url\":\"") + firstUrlInMessage + "\",\"max_chars\":4000}}"; // synthesize a direct tool call for prompts that already include a concrete URL
    String toolResult; // JSON result returned by the URL fetch tool

    if (!handleToolCall(toolRequest, toolResult)) {
      responseOut = toolResult.length() > 0 ? toolResult : "URL fetch failed."; // surface fetch tool failure details when possible
      return; // skip the general tool-decision pass if the direct URL lookup failed
    }

    String finalResponse; // follow-up pass turns the fetched page content into a user-facing answer
    if (!streamLLMResponse(buildToolFollowupPrompt(message, toolResult), finalResponse, true)) {
      responseOut = "LLM follow-up failed or timed out."; // show a separate error for the follow-up call
      return; // stop after a failed second pass
    }

    responseOut = finalResponse; // store the final answer for direct URL requests
    return; // skip the general tool-decision pass once the URL has been handled
  }

  if (weatherToolAvailable && messageNeedsCurrentWeather(message)) {
    Serial.println("Route decision: DIRECT_WEATHER_TOOL");
    String toolRequest = String("{\"tool\":\"openweather_current\",\"arguments\":{\"units\":\"") + getPreferredWeatherUnits(message) + "\"}}"; // synthesize a direct tool call for obvious current-weather requests
    String toolResult; // JSON result returned by the weather tool

    if (!handleToolCall(toolRequest, toolResult)) {
      responseOut = toolResult.length() > 0 ? toolResult : "Weather lookup failed."; // surface weather tool failure details when possible
      return; // skip the LLM pass if the direct weather lookup failed
    }

    String finalResponse; // follow-up pass turns the weather tool output into a user-facing answer
    if (!streamLLMResponse(buildToolFollowupPrompt(message, toolResult), finalResponse, true)) {
      responseOut = "LLM follow-up failed or timed out."; // show a separate error for the follow-up call
      return; // stop after a failed second pass
    }

    responseOut = finalResponse; // store the final answer for direct weather requests
    return; // skip the general tool-decision pass once weather has been handled
  }

  if (toolCallsPossible) {
    bool shouldUseTools = true; // fail-open so the existing tool path still runs if the router cannot answer
    const bool routerAnswered = requestToolRoutingDecision(message, shouldUseTools);

    if (routerAnswered && !shouldUseTools) {
      Serial.println("Route decision: DIRECT_ANSWER");
      if (!streamLLMResponse(message, responseOut, true)) {
        responseOut = "LLM request failed or timed out.";
      }

      return; // router chose a direct answer path, so skip the tool-decision prompt entirely
    }

    if (routerAnswered && shouldUseTools) {
      Serial.println("Route decision: TOOL_SELECTION");
    }
  }

  if (!toolCallsPossible) {
    Serial.println("Route decision: DIRECT_ANSWER (no tools available)");
  }

  String firstResponse; // first pass decides between a direct answer and a tool call
  if (!streamLLMResponse(buildToolDecisionPrompt(message), firstResponse, toolDecisionCanStream)) {
    responseOut = "LLM request failed or timed out."; // show a user-visible failure message
    return; // stop if the initial model request failed
  }

  if (!looksLikeToolCall(firstResponse)) {
    responseOut = firstResponse; // direct answer path when no tool call was requested
    return; // nothing else to do
  }

  String toolResult; // JSON result returned by the requested tool
  if (!handleToolCall(firstResponse, toolResult)) {
    responseOut = toolResult.length() > 0 ? toolResult : "Tool request failed."; // surface tool failure details when possible
    return; // skip the second LLM pass if the tool could not run
  }

  String finalResponse; // second pass turns tool output into a user-facing answer
  if (!streamLLMResponse(buildToolFollowupPrompt(message, toolResult), finalResponse, true)) {
    responseOut = "LLM follow-up failed or timed out."; // show a separate error for the follow-up call
    return; // stop after a failed second pass
  }

  responseOut = finalResponse; // store the final answer for the web response

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
  if (chatStateMutex != nullptr) {
    xSemaphoreTake(chatStateMutex, portMAX_DELAY); // keep history mutations serialized against concurrent page renders
  }

    for(int i = 0; i < 5; i++){
        contextWindow[i] = contextWindow[i + 1]; // shift older messages toward the front of the ring buffer
    }

    contextWindow[5] = sender + ": " + message; // append the newest chat turn at the end
    printHistory(); // log the updated history to serial

  if (chatStateMutex != nullptr) {
    xSemaphoreGive(chatStateMutex);
  }
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
  if (chatStateMutex != nullptr) {
    xSemaphoreTake(chatStateMutex, portMAX_DELAY); // snapshot the current rolling history without racing a background completion
  }

  contextCollapsed = ""; // rebuild the collapsed prompt text from scratch each turn

    for (int i = 0; i < 6; i++) {
        if (contextWindow[i] != "") { // Skip empty slots
     contextCollapsed += "> " + contextWindow[i] + "\n"; // format history as quoted lines for the prompt
        }
    }

    if (chatStateMutex != nullptr) {
      xSemaphoreGive(chatStateMutex);
    }

    Serial.println(contextCollapsed); // show the collapsed prompt history on serial
}