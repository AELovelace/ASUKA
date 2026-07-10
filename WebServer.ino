String buildControlsHtml() {
  String html = // shared controls used on both the landing page and response page
    "<form action=\"/get-text\" method=\"GET\">"
    "  <label for=\"msg\">Enter text:</label><br>"
    "  <input type=\"text\" id=\"msg\" name=\"msg\">"
    "  <input type=\"submit\" value=\"Submit\">"
    "</form>"
    "<form action=\"/get-text\" method=\"GET\">"
    "  <label for=\"port\">Enter Port</label><br>"
    "  <input type=\"number\" id=\"port\" name=\"port\">"
    "  <input type=\"submit\" value=\"Submit\">"
    "</form>"
    "<form action=\"/toggle-brave\" method=\"GET\">"
    "  <p>Brave Search: <strong>";

  html += braveSearchEnabled ? "Enabled" : "Disabled"; // show the current Brave feature-toggle state
  html +=
    "</strong></p>"
    "  <input type=\"submit\" value=\"";
  html += braveSearchEnabled ? "Disable Brave Search" : "Enable Brave Search"; // label the button with the opposite action
  html +=
    "\">"
    "</form>";

  return html; // return the finished controls markup block
}

String buildMainPageHtml() {
  return String( // minimal landing page shown at the sketch root
    "<!DOCTYPE html><html>"
    "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"></head>"
    "<body>"
    "<h1>ESP32 Text Receiver</h1>"
  ) + buildControlsHtml() + "</body></html>"; // append the shared controls and close the page
}

String buildResponsePageHtml(const String& historyHtml, const String& responseHtml) {
  return String( // page shown after a message or port update round-trip
    "<h2>History: "
  ) + historyHtml + "</h2><br><h1>Response:" + responseHtml + "</h1>" +
    buildControlsHtml() +
    "<a href='/'>Go back</a>"; // include a link back to the landing page
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
  server.send(200, "text/html", buildMainPageHtml()); // serve the main input form
}

void handleToggleBrave() {
  braveSearchEnabled = !braveSearchEnabled; // flip the Brave search feature flag
  server.sendHeader("Location", "/"); // redirect back to the main page after toggling
  server.send(303); // use See Other so the browser performs a fresh GET
}

void handleText() {
  // Check if port is in the request and handle changing ports
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
      server.send(400, "text/html", "<h1>Invalid Port</h1><p>Enter a numeric port between 1 and 65535.</p><a href='/'>Go back</a>"); // reject blank or non-numeric port values
      return; // leave the current llmPort unchanged on invalid input
    }

    unsigned long requestedPort = portValue.toInt(); // convert the validated decimal text into a number

    if (requestedPort == 0 || requestedPort > 65535UL) {
      server.send(400, "text/html", "<h1>Invalid Port</h1><p>Enter a numeric port between 1 and 65535.</p><a href='/'>Go back</a>"); // reject values outside the valid TCP port range
      return; // leave the current llmPort unchanged on out-of-range input
    }

    llmPort = static_cast<uint16_t>(requestedPort); // update the configured LLM port only after validation succeeds

    //confirmation page
    server.send(200, "text/html", "<h1>Port Set: " + portValue + "<br><a href='/'>Go back</a>"); // confirm the updated port in the browser
  } else if (server.hasArg("msg")) {
    inputLine = server.arg("msg"); // read the submitted chat message from the query string
    addToHistory("User", inputLine); // store the user turn in the rolling history window
    Serial.print("Received Text: "); // label the inbound message in serial output
    Serial.println(inputLine); // print the inbound message contents
    // Send a confirmation page back to the browser
    collapseHistory(); // rebuild the quoted chat history string for the prompt
    askLLM(contextCollapsed); // call the LLM with the collapsed conversation context
    addToHistory("LLM", llmOutput); // store the model response in the rolling history window
    String historyHtml = renderTextForHtml(contextCollapsed); // escape the prompt history for browser display
    String responseHtml = renderTextForHtml(llmOutput); // escape the LLM answer for browser display

    server.send(200, "text/html", buildResponsePageHtml(historyHtml, responseHtml)); // return the updated history and answer page
    llmOutput = ""; // clear the last answer after serving it to the browser
    contextCollapsed = ""; // clear the collapsed prompt buffer for the next request
  } else {
    server.send(200, "text/html", "No text sent."); // fallback response when neither known query argument is present
  }
}
