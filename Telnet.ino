WiFiServer telnetServer(TELNET_SERVER_PORT); // optional single-client telnet listener for a terminal-style ASUKA session
WiFiClient telnetClient; // active telnet connection, if any
String telnetInputBuffer; // accumulates one line of terminal input before submission
size_t telnetLastResponseLength = 0; // tracks how much of the current streamed response has already been mirrored
bool telnetLastBusyState = false; // remembers whether the worker was busy during the previous loop iteration
bool telnetSawCarriageReturn = false; // prevents CRLF line endings from submitting twice

void resetTelnetSessionState() {
  telnetInputBuffer = "";
  telnetLastResponseLength = 0;
  telnetLastBusyState = false;
  telnetSawCarriageReturn = false;
}

bool telnetClientConnected() {
  return telnetClient && telnetClient.connected();
}

void telnetWriteNormalized(const String& text) {
  if (!telnetClientConnected() || text.length() == 0) {
    return;
  }

  for (size_t i = 0; i < text.length(); i++) {
    const char c = text[i];

    if (c == '\r') {
      continue; // normalize all outgoing line endings to CRLF for telnet clients
    }

    if (c == '\n') {
      telnetClient.print("\r\n");
      continue;
    }

    telnetClient.write(static_cast<uint8_t>(c));
  }
}

void telnetWriteLine(const String& text) {
  telnetWriteNormalized(text);
  if (telnetClientConnected()) {
    telnetClient.print("\r\n");
  }
}

void showTelnetPrompt() {
  if (!telnetClientConnected()) {
    return;
  }

  telnetClient.print("asuka> ");
}

String buildTelnetStatusText() {
  String statusText = "Wi-Fi IP: ";
  statusText += WiFi.localIP().toString();
  statusText += "\nLLM endpoint: ";
  statusText += llmHost;
  statusText += ":";
  statusText += String(llmPort);
  statusText += "\nBusy: ";
  statusText += isLLMRequestRunning() ? "yes" : "no";
  statusText += "\nBrave search: ";
  statusText += braveSearchEnabled ? "enabled" : "disabled";
  return statusText;
}

void welcomeTelnetClient() {
  telnetWriteLine("");
  telnetWriteLine("Connected to the ASUKA telnet console.");
  telnetWriteLine("Type a message and press Enter to chat.");
  telnetWriteLine("Commands: /help, /status, /port <1-65535>, /clear, /quit");
  telnetWriteLine("");
  showTelnetPrompt();
}

bool tryParseTelnetPortValue(const String& rawValue, uint16_t& parsedPort) {
  String portValue = rawValue;
  portValue.trim();

  if (portValue.length() == 0) {
    return false; // require a decimal port after the command name
  }

  for (size_t i = 0; i < portValue.length(); i++) {
    if (!isDigit(static_cast<unsigned char>(portValue[i]))) {
      return false; // reject any non-digit character in the telnet port command
    }
  }

  const unsigned long requestedPort = portValue.toInt();
  if (requestedPort == 0 || requestedPort > 65535UL) {
    return false; // keep the accepted range aligned with the web and MQTT port setters
  }

  parsedPort = static_cast<uint16_t>(requestedPort);
  return true;
}

void disconnectTelnetClient(const String& reason) {
  if (telnetClientConnected() && reason.length() > 0) {
    telnetWriteLine("");
    telnetWriteLine(reason);
  }

  if (telnetClient) {
    telnetClient.stop();
  }

  resetTelnetSessionState();
}

void acceptTelnetClient() {
  if (!TELNET_SERVER_ENABLED) {
    return;
  }

  WiFiClient incomingClient = telnetServer.available();
  if (!incomingClient) {
    return;
  }

  if (telnetClientConnected()) {
    incomingClient.print("ASUKA already has an active telnet client.\r\n");
    incomingClient.stop();
    return;
  }

  telnetClient = incomingClient;
  telnetClient.setNoDelay(true);
  resetTelnetSessionState();

  Serial.print("Telnet client connected from ");
  Serial.println(telnetClient.remoteIP());

  welcomeTelnetClient();
}

void mirrorTelnetResponsePreview() {
  if (!telnetClientConnected()) {
    return;
  }

  String latestResponseSnapshot;
  bool requestInFlight = false;

  if (chatStateMutex != nullptr) {
    xSemaphoreTake(chatStateMutex, portMAX_DELAY);
    latestResponseSnapshot = llmOutput;
    requestInFlight = llmRequestInFlight;
    xSemaphoreGive(chatStateMutex);
  } else {
    latestResponseSnapshot = llmOutput;
    requestInFlight = llmRequestInFlight;
  }

  if (requestInFlight && !telnetLastBusyState) {
    telnetWriteLine("");
    telnetWriteLine("ASUKA:");
    telnetLastResponseLength = 0;
  }

  if (latestResponseSnapshot.length() < telnetLastResponseLength) {
    telnetLastResponseLength = 0; // recover cleanly if the shared preview buffer was reset
  }

  if (latestResponseSnapshot.length() > telnetLastResponseLength) {
    telnetWriteNormalized(latestResponseSnapshot.substring(telnetLastResponseLength));
    telnetLastResponseLength = latestResponseSnapshot.length();
  }

  if (!requestInFlight && telnetLastBusyState) {
    telnetWriteLine("");
    telnetWriteLine("");
    showTelnetPrompt();
  }

  telnetLastBusyState = requestInFlight;
}

void handleTelnetCommand(const String& commandText) {
  if (commandText == "/help") {
    telnetWriteLine("Commands: /help, /status, /port <1-65535>, /clear, /quit");
    showTelnetPrompt();
    return;
  }

  if (commandText == "/status") {
    telnetWriteLine(buildTelnetStatusText());
    showTelnetPrompt();
    return;
  }

  if (commandText == "/clear") {
    clearConversationHistory();
    telnetWriteLine("Conversation history cleared.");
    showTelnetPrompt();
    return;
  }

  if (commandText == "/port") {
    telnetWriteLine("Current LLM port: " + String(llmPort));
    telnetWriteLine("Usage: /port <1-65535>");
    showTelnetPrompt();
    return;
  }

  if (commandText.startsWith("/port ")) {
    uint16_t requestedPort = 0;
    if (!tryParseTelnetPortValue(commandText.substring(6), requestedPort)) {
      telnetWriteLine("Invalid port. Enter a numeric port between 1 and 65535.");
      showTelnetPrompt();
      return;
    }

    llmPort = requestedPort;
    telnetWriteLine("LLM port set to " + String(llmPort) + ".");
    showTelnetPrompt();
    return;
  }

  if (commandText == "/quit" || commandText == "exit") {
    disconnectTelnetClient("Closing telnet session.");
    return;
  }

  if (!submitChatMessage(commandText, "Telnet")) {
    telnetWriteLine("ASUKA is busy or unavailable right now.");
    showTelnetPrompt();
    return;
  }

  telnetLastResponseLength = 0;
  telnetLastBusyState = false; // let the next preview sync emit the ASUKA header once the worker goes busy
  telnetWriteLine("Thinking...");
}

void handleTelnetInputLine() {
  String submittedLine = telnetInputBuffer;
  telnetInputBuffer = "";
  submittedLine.trim();

  telnetWriteLine("");

  if (submittedLine.length() == 0) {
    showTelnetPrompt();
    return;
  }

  handleTelnetCommand(submittedLine);
}

void pumpTelnetInput() {
  if (!telnetClientConnected()) {
    return;
  }

  while (telnetClient.available()) {
    const char incomingChar = static_cast<char>(telnetClient.read());

    if (incomingChar == '\r') {
      telnetSawCarriageReturn = true;
      handleTelnetInputLine();
      continue;
    }

    if (incomingChar == '\n') {
      if (telnetSawCarriageReturn) {
        telnetSawCarriageReturn = false;
        continue; // CRLF already submitted the line on the carriage return
      }

      handleTelnetInputLine();
      continue;
    }

    telnetSawCarriageReturn = false;

    if (incomingChar == 8 || incomingChar == 127) {
      if (telnetInputBuffer.length() > 0) {
        telnetInputBuffer.remove(telnetInputBuffer.length() - 1);
        telnetClient.print("\b \b");
      }
      continue;
    }

    if (!isPrintable(static_cast<unsigned char>(incomingChar)) && incomingChar != '\t') {
      continue; // ignore telnet control bytes and other non-printable characters
    }

    telnetInputBuffer += incomingChar;
    telnetClient.write(static_cast<uint8_t>(incomingChar));
  }
}

void initTelnetServer() {
  if (!TELNET_SERVER_ENABLED) {
    Serial.println("Telnet console disabled in config.");
    return;
  }

  telnetServer.begin();
  telnetServer.setNoDelay(true);
  Serial.print("Telnet console listening on port ");
  Serial.println(TELNET_SERVER_PORT);
}

void handleTelnetServer() {
  if (!TELNET_SERVER_ENABLED) {
    return;
  }

  acceptTelnetClient();

  if (!telnetClientConnected()) {
    return;
  }

  if (!telnetClient.connected()) {
    disconnectTelnetClient("");
    return;
  }

  pumpTelnetInput();
  mirrorTelnetResponsePreview();
}
