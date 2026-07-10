// HTML page with a text box and submit button

String HTML_Form = 
  "<!DOCTYPE html><html>"
  "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"></head>"
  "<body>"
  "<h1>ESP32 Text Receiver</h1>"
  "<form action=\"/get-text\" method=\"GET\">"
  "  <label for=\"msg\">Enter text:</label><br>"
  "  <input type=\"text\" id=\"msg\" name=\"msg\">"
  "  <input type=\"submit\" value=\"Submit\">"
  "</form>"
  "<form action=\"/get-text\" method=\"GET\">"
  "  <label for=\"msg\">Enter Port</label><br>"
  "  <input type=\"number\" id=\"port\" name=\"port\">"
  "  <input type=\"submit\" value=\"Submit\">"
  "</form>"
  "</body></html>";

void handleRoot() {
  server.send(200, "text/html", HTML_Form);
}

void handleText() {
  // Check if "msg" parameter exists in the GET request
  if(server.hasArg("port")){
    llmPort = server.arg("port").toInt();
    server.send(200, "text/html", HTML_Form);
  } else if (server.hasArg("msg")) {
    inputLine = server.arg("msg");
    addToHistory("User", inputLine);
    Serial.print("Received Text: ");
    Serial.println(inputLine);
    // Send a confirmation page back to the browser
    collapseHistory();
    contextCollapsed += inputLine;
    askLLM(contextCollapsed);
    addToHistory("LLM", llmOutput);
    server.send(200, "text/html", "<h1>Response: " 
                    "<form action=\"/get-text\" method=\"GET\">"
                    "  <label for=\"msg\">Enter text:</label><br>"
                    "  <input type=\"text\" id=\"msg\" name=\"msg\">"
                    "  <input type=\"submit\" value=\"Submit\">"
                    "</form>"
                + llmOutput + "</h1><br><h2>Context:</h2><br><p>" + contextCollapsed + "<a href='/'>Go back</a>");
    llmOutput = "";
    contextCollapsed = "";
  } else {
    server.send(200, "text/html", "No text sent.");
  }
}
