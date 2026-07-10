/*
* handle tool calls from llm. 
* Expected input:
* {
*   "tool": "brave_search",
*   "arguments": {
*     "query": "ESP32 WiFi examples",
*     "count": 5
*   }
* }
*/
bool braveSearch(const String& query, int resultCount, String& toolResult) {
    if (query.isEmpty()) {
        Serial.println("Search query cannot be empty."); // reject empty lookups before opening a connection
        toolResult = "{\"error\":\"Search query cannot be empty.\"}"; // return a machine-readable error to the caller
        return false; // stop on invalid input
    }

    // Brave currently limits q to 400 characters and 50 words.
    String trimmedQuery = query.substring(0, 400); // enforce the documented character limit defensively

    String requestUrl = String(BRAVE_SEARCH_URL) + // start from the configured Brave API endpoint
                        "?q=" + urlEncode(trimmedQuery) + // add the encoded search text
                        "&count=" + String(resultCount) + // request the desired number of hits
                        "&country=US" + // bias results toward US market defaults
                        "&search_lang=en" + // prefer English results
                        "&safesearch=moderate"; // keep Brave's moderate safe-search setting

    WiFiClientSecure secureClient; // HTTPS transport for Brave's API

    /*
     * Development shortcut:
     *
     * setInsecure() encrypts the connection but does not verify the remote
     * certificate. For a production device, install the appropriate root CA
     * certificate with secureClient.setCACert(...).
     */
    secureClient.setInsecure(); // skip certificate verification for now

    HTTPClient http; // high-level HTTP wrapper around the secure socket

    Serial.println(); // blank line for readability in serial logs
    Serial.println("Sending Brave Search request..."); // announce outbound search request

    if (!http.begin(secureClient, requestUrl)) {
        Serial.println("Could not initialize HTTPS request."); // fail early if the client cannot start the request
        toolResult = "{\"error\":\"Could not initialize HTTPS request.\"}"; // send back a machine-readable error
        return false; // abort on setup failure
    }

    http.setTimeout(15000); // bound network waits against a slow external API

    http.addHeader("Accept", "application/json"); // request JSON instead of HTML or other formats
    http.addHeader("Accept-Encoding", "identity"); // avoid compressed bodies to simplify memory handling
    http.addHeader("X-Subscription-Token", BRAVE_API_KEY); // authenticate to Brave's API

    const int httpCode = http.GET(); // perform the blocking HTTPS GET request

    if (httpCode <= 0) {
        Serial.print("HTTP request failed: "); // log the transport-level error prefix
        Serial.println(http.errorToString(httpCode)); // log the transport-level error details
        toolResult = "{\"error\":\"HTTP request failed.\"}"; // keep the returned error compact

        http.end(); // release the HTTP client before returning
        return false; // stop on request failure
    }

    if (httpCode != HTTP_CODE_OK) {
        Serial.print("Brave API returned HTTP "); // log non-200 responses for debugging
        Serial.println(httpCode); // include the actual status code

        String errorBody = http.getString(); // capture the response body for diagnostics

        if (!errorBody.isEmpty()) {
            Serial.println("Response:"); // label the API error payload in serial logs
            Serial.println(errorBody); // print the returned error body
        }

        http.end(); // release the client before building the error JSON

        JsonDocument errorOutput; // structured error payload sent back to the LLM layer
        errorOutput["error"] = String("Brave API returned HTTP ") + httpCode; // include the HTTP status text

        if (!errorBody.isEmpty()) {
            errorOutput["details"] = errorBody; // preserve the upstream error body when available
        }

        serializeJson(errorOutput, toolResult); // serialize the error response for the caller
        return false; // stop on non-success status codes
    }

    /*
     * This filter tells ArduinoJson to retain only:
     *
     * web.results[].title
     * web.results[].url
     * web.results[].description
     *
     * Brave returns considerably more data, so filtering helps reduce the
     * ESP32's memory usage.
     */
    JsonDocument filter; // ArduinoJson filter used to discard unneeded response fields

    filter["web"]["results"][0]["title"] = true; // keep only the result title
    filter["web"]["results"][0]["url"] = true; // keep only the result URL
    filter["web"]["results"][0]["description"] = true; // keep only the result snippet

    const int responseLength = http.getSize(); // content length when the server provides one
    String responseBody; // full JSON body from Brave

    if (responseLength > 0) {
        responseBody.reserve(responseLength); // reduce reallocations when the size is known up front
    }

    responseBody = http.getString(); // read the response body into memory

    http.end(); // close the HTTP request as soon as we have the body

    if (responseBody.isEmpty()) {
        Serial.println("Brave API returned an empty response body."); // report unexpected empty bodies
        toolResult = "{\"error\":\"Brave API returned an empty response body.\"}"; // return a compact error JSON
        return false; // stop if there is nothing to parse
    }

    JsonDocument response; // parsed subset of the Brave response

    DeserializationError jsonError = deserializeJson(
        response, // destination document
        responseBody, // full response body string
        DeserializationOption::Filter(filter) // keep only the fields defined above
    );

    if (jsonError) {
        Serial.print("JSON parsing failed: "); // log parsing failures to serial
        Serial.println(jsonError.c_str()); // print the ArduinoJson error text
        toolResult = "{\"error\":\"JSON parsing failed.\"}"; // return a compact parse error payload
        return false; // stop on invalid JSON
    }

    JsonDocument output; // compact tool output returned to the LLM layer
    output["query"] = trimmedQuery; // echo the normalized query back to the caller

    JsonArray outputResults = output["results"].to<JsonArray>(); // destination array for compact results

    JsonArray results = response["web"]["results"].as<JsonArray>(); // filtered Brave results array

    if (results.isNull() || results.size() == 0) {
        Serial.println("No web results were returned."); // log the empty-result case for visibility
        serializeJson(output, toolResult); // still return a valid empty results payload
        return true; // empty results are not an execution error
    }

    // Build a compact response suitable for an LLM tool.

    for (JsonObject result : results) {
        JsonObject item = outputResults.add<JsonObject>(); // append one compact result object per Brave hit

        item["title"] = result["title"] | ""; // copy the title with a safe default
        item["url"] = result["url"] | ""; // copy the result URL with a safe default
        item["description"] = result["description"] | ""; // copy the snippet with a safe default
    }

    Serial.println(); // blank line before dumping result JSON
    Serial.println("--- SEARCH RESULT JSON ---"); // mark the start of the tool payload in serial
    serializeJson(output, Serial); // print the exact JSON returned to the LLM layer
    Serial.println(); // finish the JSON line cleanly
    Serial.println("--- END SEARCH RESULT JSON ---"); // mark the end of the payload dump

    toolResult = ""; // clear any stale caller-provided content before writing the result
    serializeJson(output, toolResult); // serialize the compact result JSON into the output string

    return true; // search completed successfully
}

bool openWeatherCurrent(const String& requestedUnits, String& toolResult) {
    if (OPENWEATHER_API_KEY == nullptr || OPENWEATHER_API_KEY[0] == '\0') {
        Serial.println("OpenWeatherMap API key is not configured."); // reject requests when the API key is missing
        toolResult = "{\"error\":\"OpenWeatherMap API key is not configured.\"}"; // return a machine-readable configuration error
        return false; // stop when the feature is unavailable
    }

    String units = requestedUnits; // copy so normalization does not mutate caller state
    units.trim(); // drop accidental whitespace around the units argument
    units.toLowerCase(); // normalize casing for validation

    if (units.isEmpty()) {
        units = "metric"; // default to metric units when the model omits the field
    }

    if (units != "metric" && units != "imperial") {
        Serial.println("Weather units must be metric or imperial."); // reject unsupported unit systems
        toolResult = "{\"error\":\"Weather units must be metric or imperial.\"}"; // return a machine-readable validation error
        return false; // stop on invalid units
    }
    const double lat = OPENWEATHER_LAT; // latitude for the fixed current-weather lookup
    const double lon = OPENWEATHER_LON; // longitude for the fixed current-weather lookup

    HTTPClient http; // high-level HTTP wrapper around the secure socket

    String weatherUrl = String(OPENWEATHER_WEATHER_URL) + // start from the configured current-weather endpoint
                        "?lat=" + String(lat, 6) + // provide the resolved latitude
                        "&lon=" + String(lon, 6) + // provide the resolved longitude
                        "&appid=" + OPENWEATHER_API_KEY + // authenticate to OpenWeatherMap
                        "&units=" + units; // request the caller's preferred unit system

    WiFiClientSecure weatherClient; // HTTPS transport for the current-weather request
    weatherClient.setInsecure(); // skip certificate verification for now

    Serial.println("Sending OpenWeatherMap current weather request..."); // announce the second outbound weather request

    if (!http.begin(weatherClient, weatherUrl)) {
        Serial.println("Could not initialize OpenWeatherMap current weather request."); // fail early if the client cannot start the request
        toolResult = "{\"error\":\"Could not initialize OpenWeatherMap current weather request.\"}"; // send back a machine-readable error
        return false; // abort on setup failure
    }

    http.setTimeout(15000); // bound network waits against a slow external API
    http.addHeader("Accept", "application/json"); // request JSON instead of HTML or other formats
    http.addHeader("Accept-Encoding", "identity"); // avoid compressed bodies to simplify memory handling

    int httpCode = http.GET(); // perform the blocking HTTPS GET request

    if (httpCode <= 0) {
        Serial.print("OpenWeatherMap current weather request failed: "); // log the transport-level error prefix
        Serial.println(http.errorToString(httpCode)); // log the transport-level error details
        toolResult = "{\"error\":\"OpenWeatherMap current weather request failed.\"}"; // keep the returned error compact

        http.end(); // release the HTTP client before returning
        return false; // stop on request failure
    }

    if (httpCode != HTTP_CODE_OK) {
        Serial.print("OpenWeatherMap current weather returned HTTP "); // log non-200 responses for debugging
        Serial.println(httpCode); // include the actual status code

        String errorBody = http.getString(); // capture the response body for diagnostics

        if (!errorBody.isEmpty()) {
            Serial.println("Response:"); // label the API error payload in serial logs
            Serial.println(errorBody); // print the returned error body
        }

        http.end(); // release the client before building the error JSON

        JsonDocument errorOutput; // structured error payload sent back to the LLM layer
        errorOutput["error"] = String("OpenWeatherMap current weather returned HTTP ") + httpCode; // include the HTTP status text

        if (!errorBody.isEmpty()) {
            errorOutput["details"] = errorBody; // preserve the upstream error body when available
        }

        serializeJson(errorOutput, toolResult); // serialize the error response for the caller
        return false; // stop on non-success status codes
    }

    String weatherBody = http.getString(); // read the weather response body into memory

    http.end(); // close the request as soon as we have the body

    if (weatherBody.isEmpty()) {
        Serial.println("OpenWeatherMap current weather returned an empty response body."); // report unexpected empty bodies
        toolResult = "{\"error\":\"OpenWeatherMap current weather returned an empty response body.\"}"; // return a compact error JSON
        return false; // stop if there is nothing to parse
    }

    JsonDocument weatherResponse; // parsed current-weather response object
    DeserializationError jsonError = deserializeJson(weatherResponse, weatherBody); // parse the weather JSON

    if (jsonError) {
        Serial.print("OpenWeatherMap weather JSON parsing failed: "); // log parsing failures to serial
        Serial.println(jsonError.c_str()); // print the ArduinoJson error text
        toolResult = "{\"error\":\"OpenWeatherMap weather JSON parsing failed.\"}"; // return a compact parse error payload
        return false; // stop on invalid JSON
    }

    JsonObject currentWeather = weatherResponse["weather"][0]; // OpenWeatherMap exposes headline conditions in the first weather entry

    JsonDocument output; // compact tool output returned to the LLM layer
    output["requested_location"] = OPENWEATHER_LOCATION_LABEL; // identify the configured location back to the caller
    output["units"] = units; // echo the chosen unit system back to the caller

    JsonObject locationOutput = output["location"].to<JsonObject>(); // configured location metadata
    locationOutput["label"] = OPENWEATHER_LOCATION_LABEL; // copy the configured location label
    locationOutput["lat"] = lat; // include the configured latitude for traceability
    locationOutput["lon"] = lon; // include the configured longitude for traceability

    output["conditions"] = currentWeather["main"] | ""; // copy the high-level condition summary with a safe default
    output["description"] = currentWeather["description"] | ""; // copy the detailed condition text with a safe default

    JsonObject temperatureOutput = output["temperature"].to<JsonObject>(); // normalized temperature block for the caller
    temperatureOutput["current"] = weatherResponse["main"]["temp"] | 0.0; // current temperature in the requested units
    temperatureOutput["feels_like"] = weatherResponse["main"]["feels_like"] | 0.0; // apparent temperature in the requested units
    temperatureOutput["unit"] = units == "imperial" ? "F" : "C"; // identify the temperature unit explicitly

    output["humidity_percent"] = weatherResponse["main"]["humidity"] | 0; // copy humidity percentage with a safe default

    JsonObject windOutput = output["wind"].to<JsonObject>(); // normalized wind block for the caller
    windOutput["speed"] = weatherResponse["wind"]["speed"] | 0.0; // copy wind speed in the requested unit system
    windOutput["unit"] = units == "imperial" ? "mph" : "m/s"; // identify the wind-speed unit explicitly

    Serial.println(); // blank line before dumping result JSON
    Serial.println("--- WEATHER RESULT JSON ---"); // mark the start of the tool payload in serial
    serializeJson(output, Serial); // print the exact JSON returned to the LLM layer
    Serial.println(); // finish the JSON line cleanly
    Serial.println("--- END WEATHER RESULT JSON ---"); // mark the end of the payload dump

    toolResult = ""; // clear any stale caller-provided content before writing the result
    serializeJson(output, toolResult); // serialize the compact result JSON into the output string

    return true; // current weather lookup completed successfully
}

bool handleToolCall(const String& jsonInput, String& toolResult) {
    toolResult = ""; // clear prior tool output before parsing a new request

    // This document will contain the parsed JSON.
    JsonDocument toolCallDocument; // decoded tool-call request from the LLM

    // Convert the incoming JSON text into an ArduinoJson document.
    DeserializationError jsonError =
        deserializeJson(toolCallDocument, jsonInput); // parse the incoming JSON string

    // Stop if the input was not valid JSON.
    if (jsonError) {
        Serial.print("Invalid tool-call JSON: "); // log malformed model output
        Serial.println(jsonError.c_str()); // include the parser error details
        toolResult = "{\"error\":\"Invalid tool-call JSON.\"}"; // return a simple error payload

        return false; // stop on invalid JSON
    }

    /*
     * Extract the tool name.
     *
     * The | "" means:
     * use an empty string if the field does not exist.
     */
    const char* toolName =
        toolCallDocument["tool"] | ""; // extract the requested tool name with a safe default

    // Make sure a tool name was supplied.
    if (strlen(toolName) == 0) {
        Serial.println("Tool call is missing the 'tool' field."); // reject requests without a tool selector
        toolResult = "{\"error\":\"Tool call is missing the tool field.\"}"; // return a machine-readable validation error

        return false; // stop when the tool name is absent
    }

    /*
     * Check which tool the LLM requested.
     */
    if (strcmp(toolName, "brave_search") == 0) {
        if (!braveSearchEnabled) {
            Serial.println("brave_search is currently disabled."); // reject tool calls while the feature toggle is off
            toolResult = "{\"error\":\"brave_search is currently disabled.\"}"; // propagate the disabled-state error

            return false; // stop when the feature is disabled
        }

        /*
         * Extract the search query.
         */
        const char* query =
            toolCallDocument["arguments"]["query"] | ""; // extract the search query text

        /*
         * Extract the requested result count.
         *
         * Default to 5 if the field is missing.
         */
        int count =
            toolCallDocument["arguments"]["count"] | 5; // default to five results when omitted

        // Validate the query.
        if (strlen(query) == 0) {
            Serial.println("brave_search requires a query."); // reject empty search requests

            return false; // stop when the required argument is missing
        }

        // Keep the result count within a reasonable range.
        count = constrain(count, 1, 10); // bound request size for latency and memory safety

        Serial.print("Tool requested: "); // log the chosen tool name
        Serial.println(toolName); // print the chosen tool name value

        Serial.print("Search query: "); // label the query in serial output
        Serial.println(query); // print the raw query text

        Serial.print("Result count: "); // label the requested result count
        Serial.println(count); // print the bounded result count

        // Call your existing Brave Search function.
        return braveSearch(String(query), count, toolResult); // execute the tool and pass its JSON result back up
    }

    if (strcmp(toolName, "openweather_current") == 0) {
        if (OPENWEATHER_API_KEY == nullptr || OPENWEATHER_API_KEY[0] == '\0') {
            Serial.println("openweather_current is currently unavailable."); // reject tool calls while the API key is missing
            toolResult = "{\"error\":\"openweather_current is currently unavailable.\"}"; // propagate the unavailable-state error

            return false; // stop when the feature is unavailable
        }

        const char* units =
            toolCallDocument["arguments"]["units"] | "metric"; // default to metric units when omitted

        Serial.print("Tool requested: "); // log the chosen tool name
        Serial.println(toolName); // print the chosen tool name value

        Serial.print("Configured weather location: "); // label the fixed location in serial output
        Serial.println(OPENWEATHER_LOCATION_LABEL); // print the configured location label

        Serial.print("Units: "); // label the requested units
        Serial.println(units); // print the requested units value

        return openWeatherCurrent(String(units), toolResult); // execute the weather tool and pass its JSON result back up
    }

    /*
     * The tool name did not match any supported tool.
     */
    Serial.print("Unknown tool: "); // log unsupported tool names from the model
    Serial.println(toolName); // print the unsupported tool name

    toolResult = "{\"error\":\"Unknown tool.\"}"; // return a compact unsupported-tool error

    return false; // reject unknown tools
}


String urlEncode(const String& input) {
    String encoded; // output string for percent-encoded text
    encoded.reserve(input.length() * 3); // worst case every byte expands to %XX

    const char hex[] = "0123456789ABCDEF"; // lookup table for percent-encoding nibbles

    for (size_t i = 0; i < input.length(); i++) {
        const unsigned char c = input[i]; // current byte from the input string

        if (
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' ||
            c == '_' ||
            c == '.' ||
            c == '~'
        ) {
            encoded += static_cast<char>(c); // keep unreserved URL characters as-is
        } else {
            encoded += '%'; // start a percent-encoded escape sequence
            encoded += hex[(c >> 4) & 0x0F]; // write the high nibble as uppercase hex
            encoded += hex[c & 0x0F]; // write the low nibble as uppercase hex
        }
    }

    return encoded; // hand the encoded query string back to the caller
}

