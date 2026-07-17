bool isLLMRequestRunning() {
  bool requestInFlight = llmRequestInFlight;

  if (chatStateMutex != nullptr) {
    xSemaphoreTake(chatStateMutex, portMAX_DELAY);
    requestInFlight = llmRequestInFlight;
    xSemaphoreGive(chatStateMutex);
  }

  return requestInFlight;
}

void snapshotConversationState(String historySnapshot[6], String& latestResponseSnapshot, bool& requestInFlight) {
  if (chatStateMutex != nullptr) {
    xSemaphoreTake(chatStateMutex, portMAX_DELAY);
  }

  requestInFlight = llmRequestInFlight;
  latestResponseSnapshot = llmOutput;

  for (int i = 0; i < 6; i++) {
    historySnapshot[i] = contextWindow[i];
  }

  if (chatStateMutex != nullptr) {
    xSemaphoreGive(chatStateMutex);
  }
}

String buildControlsHtml() {
  const bool requestInFlight = isLLMRequestRunning();
  const String disabledAttribute = requestInFlight ? " disabled" : "";

  String html = // control panel pinned to the lower section of the page
    "<section class=\"controls-panel\">"
    "<div class=\"controls-grid\">"
    "<form class=\"control-card message-form\" action=\"/\" method=\"GET\">"
    "  <label class=\"message-label\" for=\"msg\">Message</label>"
    "  <input class=\"message-input\" type=\"text\" id=\"msg\" name=\"msg\" placeholder=\"Ask something...\"" + disabledAttribute + ">"
    "  <input id=\"send-button\" type=\"submit\" value=\"Send\"" + disabledAttribute + ">"
    "</form>"
    "</div>"
    "</section>";

  return html; // return the finished controls markup block
}

String buildLogPanelHtml() {
  String html =
    "<section class=\"log-panel\">"
    "<div class=\"log-shell\">"
    "<div class=\"log-heading\">"
    "  <h2>Server Log</h2>"
    "  <p id=\"log-status\">Polling every 5s.</p>"
    "</div>"
    "<pre id=\"sakura-log-output\" class=\"log-output\">Waiting for log data...</pre>"
    "</div>"
    "</section>";

  return html;
}

String buildSettingsHtml() {
  String html =
    "<details class=\"settings-menu\">"
    "<summary>Settings</summary>"
    "<div class=\"settings-body\">"
    "<form class=\"settings-form\" action=\"/\" method=\"GET\">"
    "  <label for=\"port\">LLM Port</label>"
    "  <div class=\"settings-row\">"
    "    <input type=\"number\" id=\"port\" name=\"port\" min=\"1\" max=\"65535\" placeholder=\"9090\" value=\"";

  html += String(llmPort); // show the current configured port in the compact settings control
  html +=
    "\">"
    "    <input type=\"submit\" value=\"Save\">"
    "  </div>"
    "</form>"
    "<form class=\"settings-form\" action=\"/toggle-brave\" method=\"GET\">"
    "  <label>Brave Search</label>"
    "  <div class=\"settings-toggle\"><strong>";

  html += braveSearchEnabled ? "Enabled" : "Disabled"; // show the current Brave feature-toggle state
  html +=
    "</strong>"
    "    <input type=\"submit\" value=\"";
  html += braveSearchEnabled ? "Disable" : "Enable"; // keep the dropdown button label compact
  html +=
    "\">"
    "  </div>"
    "</form>"
    "</div>"
    "</details>";

  return html; // return the compact top-of-page settings dropdown
}

String buildConversationHtml() {
  String historySnapshot[6];
  String latestResponseSnapshot;
  bool requestInFlight = false;

  snapshotConversationState(historySnapshot, latestResponseSnapshot, requestInFlight);

  String html = "<section class=\"conversation-panel\"><div id=\"conversation-scroll\" class=\"conversation-scroll\">"; // scrolling conversation document shown in the upper section

  html += "<div class=\"panel-heading\"><h2>Conversation</h2><p>Recent history and current reply.</p></div>"; // explain the combined conversation view with less vertical space

  String latestHistoryResponse = ""; // keep the most recent LLM history line out of the transcript when it matches the dedicated response block
  if (latestResponseSnapshot.length() > 0) {
    latestHistoryResponse = "LLM: " + latestResponseSnapshot; // match the exact history entry format used when storing model replies
  }

  bool hasHistory = false; // track whether any history entries have been rendered yet
  for (int i = 0; i < 6; i++) {
    if (historySnapshot[i].length() == 0) {
      continue; // skip empty history slots
    }

    if (latestHistoryResponse.length() > 0 &&
        i == 5 &&
        historySnapshot[i] == latestHistoryResponse) {
      continue; // avoid showing the newest LLM answer twice when it is already displayed below
    }

    if (!hasHistory) {
      html += "<div class=\"transcript\">"; // open the transcript container only when the first entry is rendered
      hasHistory = true; // remember that at least one entry exists
    }

    html += "<div class=\"transcript-line\">" + renderTextForHtml(historySnapshot[i]) + "</div>"; // render each saved chat turn as its own line in the transcript
  }

  if (!hasHistory) {
    html += "<p class=\"empty-state\">No conversation yet.</p>"; // show a placeholder before the first exchange
  } else {
    html += "</div>"; // close the transcript container when entries were rendered
  }

  html += "<div class=\"latest-response\">"; // isolate the most recent LLM output within the shared scroll document without a separate heading
  html += requestInFlight
    ? "<p id=\"response-status\" class=\"response-status busy\">Generating</p>"
    : "<p id=\"response-status\" class=\"response-status\">Ready</p>";

  if (latestResponseSnapshot.length() == 0) {
    html += requestInFlight
      ? "<p id=\"latest-response-content\" class=\"empty-state\">Waiting for response...</p>"
      : "<p id=\"latest-response-content\" class=\"empty-state\">No response yet.</p>"; // placeholder before the first model answer
  } else {
    html += "<p id=\"latest-response-content\">" + renderTextForHtml(latestResponseSnapshot) + "</p>"; // show the last answer in the main scroll area
  }

  html += "</div></div></section>"; // close the latest-response block, scroll wrapper, and panel section

  return html; // return the rendered conversation section
}

String buildMainPageHtml() {
  String html = String( // minimal landing page shown at the sketch root
    "<!DOCTYPE html><html>"
    "<head>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<style>"
    "body{margin:0;font-family:Verdana,sans-serif;background:#fff4f7;color:#341b28;}"
    ".app-shell{height:100vh;height:100dvh;display:flex;flex-direction:column;overflow:hidden;}"
    ".top-bar{display:flex;align-items:flex-start;justify-content:space-between;gap:8px;padding:10px 10px 0 10px;background:#fff4f7;}"
    ".top-bar h1{margin:0;font-size:1.2rem;color:#7d294f;letter-spacing:0.05em;text-transform:uppercase;}"
    ".settings-menu{min-width:156px;border:1px solid #d78ba7;background:#fffafb;color:#341b28;}"
    ".settings-menu summary{list-style:none;cursor:pointer;padding:8px 10px;font-size:0.82rem;font-weight:700;letter-spacing:0.04em;text-transform:uppercase;color:#7d294f;}"
    ".settings-menu summary::-webkit-details-marker{display:none;}"
    ".settings-body{display:flex;flex-direction:column;gap:8px;padding:0 10px 10px 10px;border-top:1px solid #efb5c8;}"
    ".settings-form{display:flex;flex-direction:column;gap:6px;margin:0;padding-top:8px;}"
    ".settings-form label{margin:0;font-size:0.8rem;font-weight:700;color:#9a5572;text-transform:uppercase;letter-spacing:0.04em;}"
    ".settings-row,.settings-toggle{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:6px;align-items:center;}"
    ".settings-toggle strong{font-size:0.92rem;color:#b83268;}"
    ".settings-form input[type=number]{width:100%;padding:7px 8px;border:1px solid #d99ab3;background:#fffdfd;box-sizing:border-box;}"
    ".settings-form input[type=submit]{padding:7px 10px;border:1px solid #8a1f4b;background:#c83e75;color:#fff8fb;font-weight:700;box-sizing:border-box;}"
    ".conversation-panel{height:60vh;height:60dvh;min-height:0;padding:8px 10px 4px 10px;background:linear-gradient(180deg,#ffeaf1 0%,#ffd5e3 100%);box-sizing:border-box;}"
    ".conversation-scroll{height:100%;overflow-y:auto;background:#fffafb;border:2px solid #d78ba7;padding:12px;box-sizing:border-box;box-shadow:inset 0 1px 0 rgba(255,255,255,0.65);}"
    ".panel-heading h2{margin:0 0 4px 0;font-size:1rem;letter-spacing:0.04em;text-transform:uppercase;color:#7d294f;}"
    ".panel-heading p{margin:0 0 10px 0;color:#9a5572;font-size:0.9rem;}"
    ".transcript{display:flex;flex-direction:column;gap:6px;margin-bottom:10px;}"
    ".transcript-line{padding:8px 10px;background:#fff0f5;border-left:4px solid #e05b8c;line-height:1.35;word-break:break-word;}"
    ".latest-response{padding-top:8px;border-top:1px solid #efb5c8;}"
    ".response-status{margin:0 0 8px 0;font-size:0.78rem;font-weight:700;letter-spacing:0.04em;text-transform:uppercase;color:#9a5572;}"
    ".response-status.busy{color:#b83268;}"
    ".response-status.busy::after{content:'...';display:inline-block;width:1.6em;text-align:left;vertical-align:baseline;animation:busyDots 1.2s steps(4,end) infinite;}"
    ".latest-response p{margin:0;line-height:1.4;word-break:break-word;white-space:pre-wrap;background:#fff6f8;border-left:4px solid #b83268;padding:8px 10px;}"
    ".empty-state{color:#a46a83;font-style:italic;margin:0;}"
    ".log-panel{flex:1;min-height:110px;padding:0 10px 6px 10px;background:linear-gradient(180deg,#ffd5e3 0%,#f4bdd0 100%);box-sizing:border-box;}"
    ".log-shell{height:100%;display:flex;flex-direction:column;gap:8px;padding:10px;background:#fffafb;border:2px solid #d78ba7;box-sizing:border-box;box-shadow:inset 0 1px 0 rgba(255,255,255,0.65);}"
    ".log-heading{display:flex;align-items:flex-start;justify-content:space-between;gap:8px;}"
    ".log-heading h2{margin:0;font-size:0.95rem;letter-spacing:0.04em;text-transform:uppercase;color:#7d294f;}"
    ".log-heading p{margin:0;color:#9a5572;font-size:0.78rem;}"
    ".log-output{flex:1;min-height:0;overflow-y:auto;margin:0;padding:10px;background:#341b28;color:#ffeaf1;border:1px solid #7d294f;font:0.78rem/1.4 Consolas,Monaco,monospace;white-space:pre-wrap;word-break:break-word;}"
    ".controls-panel{height:10vh;height:10dvh;min-height:72px;padding:6px 10px 8px 10px;background:#6f163c;box-sizing:border-box;}"
    ".controls-grid{height:100%;display:grid;grid-template-columns:minmax(0,1fr);gap:8px;align-items:center;}"
    ".control-card{display:flex;flex-direction:column;justify-content:space-between;gap:8px;padding:10px;background:#fff7fa;color:#341b28;text-decoration:none;box-sizing:border-box;border:1px solid #e6aac1;}"
    ".message-form{height:100%;min-height:0;display:grid;grid-template-columns:auto minmax(0,1fr) auto;gap:8px;align-items:center;padding:0;background:transparent;border:none;}"
    ".message-label{color:#fff8fb;font-size:0.78rem;letter-spacing:0.04em;text-transform:uppercase;}"
    ".control-card label,.control-card p{margin:0;font-weight:700;}"
    ".control-card strong{font-size:1rem;color:#b83268;}"
    ".control-card input[type=text],.control-card input[type=number]{width:100%;padding:8px 10px;border:1px solid #d99ab3;background:#fffdfd;box-sizing:border-box;}"
    ".message-input{min-width:0;height:100%;min-height:42px;}"
    ".control-card input[type=submit]{height:100%;min-height:42px;padding:8px 14px;border:1px solid #8a1f4b;background:#c83e75;color:#fff8fb;font-weight:700;box-sizing:border-box;}"
    ".toggle-card{align-items:flex-start;}"
    "@keyframes busyDots{0%,25%{color:transparent;}50%{color:rgba(184,50,104,0.45);}75%,100%{color:#b83268;}}"
    "@media (max-width: 720px){.top-bar{flex-direction:column;align-items:stretch;}.settings-menu{width:100%;}.conversation-panel{height:60vh;height:60dvh;padding-bottom:4px;}.log-panel{min-height:96px;padding-bottom:4px;}.log-heading{flex-direction:column;}.controls-panel{height:10vh;height:10dvh;min-height:80px;}.controls-grid{grid-template-columns:1fr;}.message-form{grid-template-columns:minmax(0,1fr) auto;}.message-label{display:none;}.settings-row,.settings-toggle{grid-template-columns:1fr;}}"
    "</style>"
    "</head>"
    "<body><main class=\"app-shell\">"
    "<header class=\"top-bar\"><h1>ASUKA Console</h1>"
  );

  html += buildSettingsHtml(); // surface configuration controls in a compact top dropdown
  html += "</header>";

  html += buildConversationHtml(); // insert the scrollable conversation region above the controls
  html += buildControlsHtml(); // keep the message bar directly below the conversation
  html += buildLogPanelHtml(); // use the remaining vertical space below the message bar for live server logs
  html +=
    "</main>"
    "<script>"
    "const sakuraLogUrl='/api/sakura-log';"
    "const chatStateUrl='/api/chat-state';"
    "const logOutput=document.getElementById('sakura-log-output');"
    "const logStatus=document.getElementById('log-status');"
    "const conversationScroll=document.getElementById('conversation-scroll');"
    "const latestResponse=document.getElementById('latest-response-content');"
    "const responseStatus=document.getElementById('response-status');"
    "const messageInput=document.getElementById('msg');"
    "const sendButton=document.getElementById('send-button');"
    "const defaultMessagePlaceholder='Ask something...';"
    "const conversationScrollSlack=24;"
    "let followConversation=true;"
    "let lastResponseText=latestResponse?latestResponse.textContent:'';"
    "function scrollToBottom(element){if(element){element.scrollTop=element.scrollHeight;}}"
    "function isNearBottom(element){return Boolean(element)&&(element.scrollHeight-element.scrollTop-element.clientHeight)<=conversationScrollSlack;}"
    "function maybeAutoScrollConversation(previousResponseText){"
    "  if(!conversationScroll){return;}"
    "  const currentResponseText=latestResponse?latestResponse.textContent:'';"
    "  const responseGrew=currentResponseText.length>previousResponseText.length&&currentResponseText!==previousResponseText;"
    "  const hasOverflow=conversationScroll.scrollHeight>conversationScroll.clientHeight;"
    "  if(responseGrew&&hasOverflow&&followConversation){scrollToBottom(conversationScroll);}"
    "  followConversation=isNearBottom(conversationScroll);"
    "  lastResponseText=currentResponseText;"
    "}"
    "function applyChatState(payload){"
    "  const busy=Boolean(payload&&payload.busy);"
    "  const responseText=payload&&typeof payload.response==='string'?payload.response:'';"
    "  const previousResponseText=lastResponseText;"
    "  if(messageInput){messageInput.disabled=busy;messageInput.placeholder=busy?'Waiting for current reply...':defaultMessagePlaceholder;}"
    "  if(sendButton){sendButton.disabled=busy;}"
    "  if(responseStatus){responseStatus.textContent=busy?'Generating':'Ready';responseStatus.className=busy?'response-status busy':'response-status';}"
    "  if(latestResponse){"
    "    if(responseText.length){latestResponse.textContent=responseText;}"
    "    else if(busy){latestResponse.textContent='Waiting for response...';}"
    "    else{latestResponse.textContent='No response yet.';}"
    "  }"
    "  maybeAutoScrollConversation(previousResponseText);"
    "}"
    "function tryParseJsonString(value){"
    "  if(typeof value!=='string'){return value;}"
    "  const trimmed=value.trim();"
    "  if(!trimmed){return value;}"
    "  if(trimmed[0]!=='{'&&trimmed[0]!=='['){return value;}"
    "  try{return JSON.parse(trimmed);}catch(parseError){return value;}"
    "}"
    "function pickLogEntry(value){"
    "  if(Array.isArray(value)){return value.length?pickLogEntry(value[0]):null;}"
    "  if(value&&typeof value==='object'){"
    "    const nested=value.lines||value.logs||value.entries||value.items||value.data||value.result;"
    "    if(nested!==undefined){return pickLogEntry(nested);}"
    "  }"
    "  return value;"
    "}"
    "function renderLogValue(value){"
    "  const parsedValue=tryParseJsonString(value);"
    "  if(parsedValue===null||parsedValue===undefined){return 'No log data.';}"
    "  if(typeof parsedValue==='string'){return parsedValue;}"
    "  const entry=pickLogEntry(parsedValue);"
    "  if(!entry||typeof entry!=='object'){return 'No log data.';}"
    "  const ts=entry.ts!==undefined&&entry.ts!==null?String(entry.ts):'unknown';"
    "  const line=entry.line!==undefined&&entry.line!==null?String(entry.line):'No line field.';"
    "  return 'ts: '+ts+'\\nline: '+line;"
    "}"
    "async function pollSakuraLogs(){"
    "  try{"
    "    const response=await fetch(sakuraLogUrl,{cache:'no-store'});"
    "    if(!response.ok){throw new Error('HTTP '+response.status);}" 
    "    const rawText=await response.text();"
    "    let payload=rawText;"
    "    try{payload=JSON.parse(rawText);}catch(parseError){}"
    "    logOutput.textContent=renderLogValue(payload);"
    "    logStatus.textContent='Updated '+new Date().toLocaleTimeString();"
    "    scrollToBottom(logOutput);"
    "  }catch(error){"
    "    logStatus.textContent='Log fetch failed.';"
    "    logOutput.textContent='Unable to load server logs from '+sakuraLogUrl+'\\n'+String(error);"
    "    scrollToBottom(logOutput);"
    "  }"
    "}"
    "async function pollChatState(){"
    "  try{"
    "    const response=await fetch(chatStateUrl,{cache:'no-store'});"
    "    if(!response.ok){throw new Error('HTTP '+response.status);}"
    "    applyChatState(await response.json());"
    "  }catch(error){}"
    "}"
    "if(conversationScroll){scrollToBottom(conversationScroll);followConversation=true;conversationScroll.addEventListener('scroll',function(){followConversation=isNearBottom(conversationScroll);});}"
    "pollChatState();"
    "pollSakuraLogs();"
    "setInterval(pollChatState,500);"
    "setInterval(pollSakuraLogs,5000);"
    "</script>"
    "</body></html>"; // close the layout container and page

  return html; // return the finished page HTML
}

bool fetchSakuraLatestLog(String& responseBody, String& errorMessage) {
  HTTPClient http; // plain HTTP client for the Sakura log endpoint
  const char* requestUrl = "http://100.66.64.45:8086/api/sakura/logs?limit=1";

  if (!http.begin(requestUrl)) {
    errorMessage = "Could not initialize Sakura log request.";
    return false;
  }

  http.setTimeout(5000); // keep the UI poll from hanging too long on upstream delays
  http.addHeader("Accept", "application/json"); // prefer JSON when the upstream supports it
  http.addHeader("Accept-Encoding", "identity"); // avoid compressed bodies to keep parsing simple

  const int httpCode = http.GET(); // fetch the latest log entry from the upstream server

  if (httpCode <= 0) {
    errorMessage = String("Sakura log request failed: ") + http.errorToString(httpCode);
    http.end();
    return false;
  }

  responseBody = http.getString(); // capture the upstream response body for passthrough

  if (httpCode != HTTP_CODE_OK) {
    errorMessage = String("Sakura log server returned HTTP ") + httpCode;
    http.end();
    return false;
  }

  http.end();
  return true;
}

void handleSakuraLogProxy() {
  String responseBody;
  String errorMessage;

  if (!fetchSakuraLatestLog(responseBody, errorMessage)) {
    JsonDocument errorPayload;
    errorPayload["error"] = errorMessage;

    if (!responseBody.isEmpty()) {
      errorPayload["details"] = responseBody;
    }

    String serializedError;
    serializeJson(errorPayload, serializedError);
    server.send(502, "application/json", serializedError);
    return;
  }

  server.send(200, "application/json", responseBody);
}

void runLLMWorkerTask(void* parameter) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // sleep until a chat request queues work for the persistent worker

    String promptToSend;

    if (chatStateMutex != nullptr) {
      xSemaphoreTake(chatStateMutex, portMAX_DELAY);
      promptToSend = pendingLLMPrompt;
      pendingLLMPrompt = "";
      xSemaphoreGive(chatStateMutex);
    } else {
      promptToSend = pendingLLMPrompt;
      pendingLLMPrompt = "";
    }

    String responseText;
    askLLM(promptToSend, responseText);

    if (chatStateMutex != nullptr) {
      xSemaphoreTake(chatStateMutex, portMAX_DELAY);
      llmOutput = responseText;
      llmRequestInFlight = false;
      xSemaphoreGive(chatStateMutex);
    } else {
      llmOutput = responseText;
      llmRequestInFlight = false;
    }

    addToHistory("LLM", responseText);
    publishLlmResponseToMqtt(responseText); // mirror every completed reply back out over MQTT
  }
}

bool queueLLMRequest(const String& promptText) {
  if (llmTaskHandle == nullptr) {
    if (chatStateMutex != nullptr) {
      xSemaphoreTake(chatStateMutex, portMAX_DELAY);
      llmOutput = "LLM worker is unavailable.";
      llmRequestInFlight = false;
      xSemaphoreGive(chatStateMutex);
    } else {
      llmOutput = "LLM worker is unavailable.";
      llmRequestInFlight = false;
    }

    return false;
  }

  if (chatStateMutex != nullptr) {
    xSemaphoreTake(chatStateMutex, portMAX_DELAY);
    pendingLLMPrompt = promptText;
    llmOutput = "";
    llmRequestInFlight = true;
    xSemaphoreGive(chatStateMutex);
  } else {
    pendingLLMPrompt = promptText;
    llmOutput = "";
    llmRequestInFlight = true;
  }

  xTaskNotifyGive(llmTaskHandle); // wake the persistent worker to process the queued prompt
  return true;
}

void handleChatState() {
  bool requestInFlight = false;
  String latestResponseSnapshot;

  if (chatStateMutex != nullptr) {
    xSemaphoreTake(chatStateMutex, portMAX_DELAY);
    requestInFlight = llmRequestInFlight;
    latestResponseSnapshot = llmOutput;
    xSemaphoreGive(chatStateMutex);
  } else {
    requestInFlight = llmRequestInFlight;
    latestResponseSnapshot = llmOutput;
  }

  JsonDocument responsePayload;
  responsePayload["busy"] = requestInFlight;
  responsePayload["response"] = latestResponseSnapshot;

  String serializedResponse;
  serializeJson(responsePayload, serializedResponse);
  server.send(200, "application/json", serializedResponse);
}

String renderTextForHtml(const String& text) {
  String html; // escaped HTML output buffer
  html.reserve(text.length() + 32); // reduce reallocations while converting text

  for (size_t i = 0; i < text.length(); i++) {
    const char c = text[i]; // current input character being escaped

    if (c == '\r') {
      if (i + 1 < text.length() && text[i + 1] == '\n') {
        i++; // consume the following line-feed in CRLF pairs
      }

      html += "<br>"; // convert carriage returns into line breaks
      continue; // move on to the next character
    }

    if (c == '\n') {
      html += "<br>"; // convert line-feeds into line breaks
      continue; // move on to the next character
    }

    if (c == '&') {
      html += "&amp;"; // escape ampersands first so later escapes stay valid
      continue; // move on after writing the entity
    }

    if (c == '<') {
      html += "&lt;"; // escape opening angle brackets
      continue; // move on after writing the entity
    }

    if (c == '>') {
      html += "&gt;"; // escape closing angle brackets
      continue; // move on after writing the entity
    }

    html += c; // copy safe characters through unchanged
  }

  return html; // hand back HTML-safe text for the page templates
}

//receive requests to server root
void handleRoot() {
  if (server.hasArg("port")) {
    String portValue = server.arg("port"); // read the submitted port text from the query string
    portValue.trim(); // drop accidental whitespace around the port value

    bool portIsNumeric = portValue.length() > 0; // require at least one digit before conversion

    for (size_t i = 0; i < portValue.length(); i++) {
      if (!isDigit(static_cast<unsigned char>(portValue[i]))) {
        portIsNumeric = false; // reject any non-digit character in the port field
        break; // stop scanning once the value is known to be invalid
      }
    }

    if (!portIsNumeric) {
      llmOutput = "Invalid port. Enter a numeric port between 1 and 65535."; // surface port validation failures on the main page
      server.send(400, "text/html", buildMainPageHtml()); // render the validation error on the root page
      return; // leave the current llmPort unchanged on invalid input
    }

    unsigned long requestedPort = portValue.toInt(); // convert the validated decimal text into a number

    if (requestedPort == 0 || requestedPort > 65535UL) {
      llmOutput = "Invalid port. Enter a numeric port between 1 and 65535."; // surface range validation failures on the main page
      server.send(400, "text/html", buildMainPageHtml()); // render the validation error on the root page
      return; // leave the current llmPort unchanged on out-of-range input
    }

    llmPort = static_cast<uint16_t>(requestedPort); // update the configured LLM port only after validation succeeds
    llmOutput = "Port set to " + portValue + "."; // keep the confirmation visible on the main page
  } else if (server.hasArg("msg")) {
    if (isLLMRequestRunning()) {
      server.send(200, "text/html", buildMainPageHtml()); // leave the current request running and re-render the live page
      return;
    }

    inputLine = server.arg("msg"); // read the submitted chat message from the query string

    if (inputLine.length() == 0) {
      server.send(200, "text/html", buildMainPageHtml());
      return;
    }

    addToHistory("User", inputLine); // store the user turn in the rolling history window
    Serial.print("Received Text: "); // label the inbound message in serial output
    Serial.println(inputLine); // print the inbound message contents

    collapseHistory(); // rebuild the quoted chat history string for the prompt
    String promptToSend = contextCollapsed; // snapshot the collapsed prompt before the background task clears it
    contextCollapsed = ""; // clear the collapsed prompt buffer after the request completes

    if (!queueLLMRequest(promptToSend)) {
      server.send(500, "text/html", buildMainPageHtml());
      return;
    }
  }

  server.send(200, "text/html", buildMainPageHtml()); // serve the main page, optionally with updated conversation state
}

void handleToggleBrave() {
  braveSearchEnabled = !braveSearchEnabled; // flip the Brave search feature flag
  server.sendHeader("Location", "/"); // redirect back to the main page after toggling
  server.send(303); // use See Other so the browser performs a fresh GET
}

void handleText() {
  server.sendHeader("Location", "/"); // keep the legacy endpoint as a compatibility redirect to the main page
  server.send(303); // use See Other so the browser performs a fresh GET on the root route
}
