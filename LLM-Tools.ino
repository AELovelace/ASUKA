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
const size_t URL_FETCH_MAX_RESPONSE_BYTES = 12000; // cap raw page downloads to protect ESP32 heap usage
const int URL_FETCH_DEFAULT_MAX_CHARS = 4000; // default amount of extracted text returned to the LLM
const int URL_FETCH_MAX_TEXT_CHARS = 6000; // hard upper bound for extracted text returned to the LLM

bool isWhitespaceChar(const char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v'; // normalize the ASCII whitespace seen in HTTP payloads
}

void decodeHtmlEntities(String& text) {
    text.replace("&nbsp;", " "); // convert common HTML spacing entity into a normal space
    text.replace("&amp;", "&"); // decode ampersands for cleaner natural-language context
    text.replace("&lt;", "<"); // decode less-than signs when pages escape code snippets
    text.replace("&gt;", ">"); // decode greater-than signs when pages escape code snippets
    text.replace("&quot;", "\""); // decode quoted text into its literal form
    text.replace("&#39;", "'"); // decode apostrophes from HTML-escaped pages
}

void collapseWhitespace(String& text) {
    String collapsed; // normalized text with repeated whitespace collapsed to single spaces
    collapsed.reserve(text.length()); // avoid repeated allocations during cleanup

    bool lastWasSpace = true; // trim leading whitespace by pretending a space already exists

    for (size_t i = 0; i < text.length(); i++) {
        const char c = text[i]; // current character being normalized

        if (isWhitespaceChar(c)) {
            if (!lastWasSpace) {
                collapsed += ' '; // keep only one space between words
                lastWasSpace = true; // suppress further adjacent whitespace
            }

            continue; // skip the original whitespace character
        }

        collapsed += c; // keep non-whitespace characters verbatim
        lastWasSpace = false; // permit a separating space later
    }

    collapsed.trim(); // remove any trailing space left by the loop
    text = collapsed; // hand the normalized text back to the caller
}

bool contentTypeLooksTextual(const String& rawContentType) {
    String contentType = rawContentType; // copy so matching can ignore case without mutating caller state
    contentType.toLowerCase(); // normalize header matching for mixed-case servers

    return contentType.startsWith("text/") ||
           contentType.indexOf("json") >= 0 ||
           contentType.indexOf("xml") >= 0 ||
           contentType.indexOf("javascript") >= 0 ||
           contentType.indexOf("x-www-form-urlencoded") >= 0; // allow common text-like payloads and reject binary formats
}

bool readHttpBodyWithLimit(HTTPClient& http, const size_t maxBytes, String& responseBody, bool& truncated) {
    truncated = false; // default to a full-body read unless the cap is hit
    responseBody = ""; // clear any prior caller state before appending bytes
    responseBody.reserve(maxBytes); // minimize reallocations while reading the response stream

    WiFiClient* stream = http.getStreamPtr(); // access the underlying socket stream for incremental reads

    if (stream == nullptr) {
        return false; // stop if the HTTP client did not expose a readable response stream
    }

    int remainingBytes = http.getSize(); // may be -1 when the server does not provide Content-Length
    unsigned long lastActivityAt = millis(); // track idle time so chunked responses cannot stall forever

    while (http.connected() && (remainingBytes > 0 || remainingBytes == -1)) {
        while (stream->available()) {
            const int nextByte = stream->read(); // consume one byte from the response stream

            if (nextByte < 0) {
                break; // stop on an unexpected stream read failure
            }

            responseBody += static_cast<char>(nextByte); // append the raw payload byte into the buffer

            if (remainingBytes > 0) {
                remainingBytes--; // count down fixed-size bodies as bytes are consumed
            }

            lastActivityAt = millis(); // reset the idle timer after each successful read

            if (responseBody.length() >= maxBytes) {
                truncated = true; // signal that the raw body was clipped to the safety limit
                return true; // stop once the configured response budget is exhausted
            }
        }

        if (remainingBytes == 0) {
            break; // the announced content length has been fully consumed
        }

        if (!http.connected()) {
            break; // the server closed the connection after sending the body
        }

        if (millis() - lastActivityAt > 2000) {
            break; // stop waiting after a short idle period on chunked/connection-close bodies
        }

        delay(1); // yield while waiting for the next packet to arrive
    }

    return true; // return any bytes that were collected, even on connection-close bodies
}

String htmlToPlainText(const String& html, const int maxChars) {
    String extractedText; // normalized text excerpt derived from the HTML payload
    extractedText.reserve(maxChars); // keep allocations bounded to the requested output budget

    bool insideTag = false; // track whether the parser is currently inside an HTML tag
    bool insideScript = false; // suppress script contents from the returned text excerpt
    bool insideStyle = false; // suppress stylesheet contents from the returned text excerpt
    bool lastWasSpace = true; // trim leading whitespace and collapse runs between words
    String currentTag; // small scratch buffer used to inspect the current tag name

    for (size_t i = 0; i < html.length(); i++) {
        const char c = html[i]; // current HTML character being processed

        if (insideTag) {
            if (c == '>') {
                insideTag = false; // resume text parsing after the tag closes

                String normalizedTag = currentTag; // copy so we can normalize the tag name for comparison
                normalizedTag.trim(); // remove stray whitespace around the raw tag name
                normalizedTag.toLowerCase(); // compare tag names case-insensitively

                if (normalizedTag.startsWith("script")) {
                    insideScript = true; // drop everything until the matching closing script tag
                } else if (normalizedTag.startsWith("/script")) {
                    insideScript = false; // resume text collection after script content ends
                    if (!lastWasSpace && extractedText.length() < static_cast<size_t>(maxChars)) {
                        extractedText += ' '; // keep script boundaries from merging adjacent words
                        lastWasSpace = true;
                    }
                } else if (normalizedTag.startsWith("style")) {
                    insideStyle = true; // drop stylesheet contents from the returned context
                } else if (normalizedTag.startsWith("/style")) {
                    insideStyle = false; // resume text collection after style content ends
                    if (!lastWasSpace && extractedText.length() < static_cast<size_t>(maxChars)) {
                        extractedText += ' '; // preserve a separator across style boundaries
                        lastWasSpace = true;
                    }
                }

                currentTag = ""; // clear the scratch buffer for the next tag
                continue; // consume the closing angle bracket itself
            }

            if (currentTag.length() < 32) {
                currentTag += c; // capture a small prefix so we can identify script/style tags
            }

            continue; // skip tag markup from the extracted text output
        }

        if (c == '<') {
            insideTag = true; // begin skipping markup until the matching closing bracket
            currentTag = ""; // reset the tag scratch buffer at each opening bracket
            continue; // do not emit the opening bracket into the text output
        }

        if (insideScript || insideStyle) {
            continue; // ignore raw script/style text until the closing tag is encountered
        }

        if (isWhitespaceChar(c)) {
            if (!lastWasSpace && extractedText.length() < static_cast<size_t>(maxChars)) {
                extractedText += ' '; // collapse whitespace runs into single spaces
                lastWasSpace = true; // prevent repeated spaces until another word arrives
            }

            continue; // skip the original whitespace character
        }

        extractedText += c; // preserve visible text content outside of HTML tags
        lastWasSpace = false; // permit one separating space after the current word

        if (extractedText.length() >= static_cast<size_t>(maxChars)) {
            break; // stop once the caller's text budget has been filled
        }
    }

    decodeHtmlEntities(extractedText); // clean up common HTML escapes after text extraction
    collapseWhitespace(extractedText); // ensure the final excerpt is compact and LLM-friendly
    return extractedText; // return the normalized excerpt to the fetch tool
}

bool fetchUrlContent(const String& requestedUrl, int requestedMaxChars, String& toolResult) {
    String normalizedUrl = requestedUrl; // copy so validation and trimming do not mutate caller state
    normalizedUrl.trim(); // remove surrounding whitespace from model-provided arguments

    if (normalizedUrl.isEmpty()) {
        Serial.println("URL cannot be empty."); // reject empty fetch requests before opening a connection
        toolResult = "{\"error\":\"URL cannot be empty.\"}"; // return a machine-readable validation error
        return false; // stop on invalid input
    }

    if (!normalizedUrl.startsWith("http://") && !normalizedUrl.startsWith("https://")) {
        Serial.println("Only http:// and https:// URLs are supported."); // reject unsupported URI schemes that HTTPClient cannot fetch
        toolResult = "{\"error\":\"Only http:// and https:// URLs are supported.\"}"; // surface the supported schemes clearly
        return false; // stop before attempting a request
    }

    const int maxChars = constrain(requestedMaxChars <= 0 ? URL_FETCH_DEFAULT_MAX_CHARS : requestedMaxChars, 500, URL_FETCH_MAX_TEXT_CHARS); // keep the extracted context within a safe range
    const bool useSecureTransport = normalizedUrl.startsWith("https://"); // pick the matching client implementation for the URL scheme
    const char* headerKeys[] = {"Content-Type", "Location"}; // preserve key response headers for the tool result JSON

    HTTPClient http; // high-level HTTP wrapper around the chosen transport
    http.setTimeout(15000); // bound network waits against slow or stalled origin servers
    http.collectHeaders(headerKeys, 2); // retain the response headers we want to expose upstream

    bool requestStarted = false; // track whether HTTP begin succeeded for the chosen transport
    WiFiClient plainClient; // transport for unencrypted HTTP URLs
    WiFiClientSecure secureClient; // transport for HTTPS URLs

    if (useSecureTransport) {
        secureClient.setInsecure(); // skip certificate verification for now, matching the existing tool behavior
        requestStarted = http.begin(secureClient, normalizedUrl); // initialize an HTTPS request with the secure socket
    } else {
        requestStarted = http.begin(plainClient, normalizedUrl); // initialize a plain HTTP request with the standard socket
    }

    Serial.println(); // blank line for readability in serial logs
    Serial.println("Sending URL fetch request..."); // announce the outbound page fetch in serial logs

    if (!requestStarted) {
        Serial.println("Could not initialize URL fetch request."); // fail early if the client cannot start the request
        toolResult = "{\"error\":\"Could not initialize URL fetch request.\"}"; // return a machine-readable setup error
        return false; // abort on setup failure
    }

    http.addHeader("Accept", "text/plain, text/html, application/json, application/xml;q=0.9, text/xml;q=0.9"); // prefer text-like payloads that can become LLM context
    http.addHeader("Accept-Encoding", "identity"); // avoid compressed bodies to simplify bounded response handling
    http.addHeader("User-Agent", "ASUKA-ESP32/1.0"); // identify the device when origins log or gate requests by client type

    const int httpCode = http.GET(); // perform the blocking fetch request

    if (httpCode <= 0) {
        Serial.print("URL fetch request failed: "); // log the transport-level error prefix
        Serial.println(http.errorToString(httpCode)); // log the transport-level error details
        toolResult = "{\"error\":\"URL fetch request failed.\"}"; // keep the returned error compact

        http.end(); // release the HTTP client before returning
        return false; // stop on request failure
    }

    if (httpCode != HTTP_CODE_OK) {
        Serial.print("URL fetch returned HTTP "); // log non-200 responses for debugging
        Serial.println(httpCode); // include the actual status code

        String errorBody = http.getString(); // capture the response body for diagnostics

        if (!errorBody.isEmpty()) {
            Serial.println("Response:"); // label the upstream error payload in serial logs
            Serial.println(errorBody); // print the returned error body for debugging
        }

        http.end(); // release the client before building the error JSON

        JsonDocument errorOutput; // structured error payload sent back to the LLM layer
        errorOutput["error"] = String("URL fetch returned HTTP ") + httpCode; // include the upstream status code in the returned payload

        if (!errorBody.isEmpty()) {
            errorOutput["details"] = errorBody.substring(0, 500); // cap verbose upstream errors before returning them to the model
        }

        serializeJson(errorOutput, toolResult); // serialize the error response for the caller
        return false; // stop on non-success status codes
    }

    const String contentType = http.header("Content-Type"); // capture the response MIME type for validation and reporting

    if (!contentTypeLooksTextual(contentType)) {
        Serial.print("URL fetch returned unsupported content type: "); // log unexpected binary/media payloads
        Serial.println(contentType); // include the content type for easier debugging

        http.end(); // release the request before returning the validation error

        JsonDocument errorOutput; // structured unsupported-content payload sent back to the LLM layer
        errorOutput["error"] = "URL fetch returned a non-text content type."; // keep the model-facing error concise
        errorOutput["content_type"] = contentType; // tell the caller what type was rejected
        serializeJson(errorOutput, toolResult); // serialize the error response for the caller
        return false; // stop on unsupported content types
    }

    bool bodyTruncated = false; // reports whether the raw response body hit the safety cap
    String responseBody; // raw text-like payload body returned by the server

    if (!readHttpBodyWithLimit(http, URL_FETCH_MAX_RESPONSE_BYTES, responseBody, bodyTruncated)) {
        Serial.println("Could not read the URL fetch response body."); // report unexpected stream-read failures before parsing
        toolResult = "{\"error\":\"Could not read the URL fetch response body.\"}"; // return a compact read failure error
        http.end(); // release the request before returning
        return false; // stop if the response stream could not be consumed
    }

    const String redirectedUrl = http.header("Location"); // expose the last redirect target when the server returned one
    http.end(); // close the HTTP request as soon as we have the body and headers

    if (responseBody.isEmpty()) {
        Serial.println("URL fetch returned an empty response body."); // report unexpected empty bodies
        toolResult = "{\"error\":\"URL fetch returned an empty response body.\"}"; // return a compact error JSON
        return false; // stop if there is nothing to pass to the model
    }

    String extractedContent; // normalized text excerpt returned to the LLM layer
    String loweredContentType = contentType; // copy so matching can ignore case without mutating the original header string
    loweredContentType.toLowerCase(); // normalize the content type for simple substring checks

    if (loweredContentType.indexOf("html") >= 0) {
        extractedContent = htmlToPlainText(responseBody, maxChars); // strip markup and return only visible page text for HTML documents
    } else {
        extractedContent = responseBody.substring(0, maxChars); // use the raw body directly for plain text and structured text formats
        decodeHtmlEntities(extractedContent); // still decode common entities in case a text response is HTML-escaped
        collapseWhitespace(extractedContent); // compact whitespace so the excerpt fits better in the prompt budget
    }

    if (extractedContent.isEmpty()) {
        Serial.println("URL fetch did not yield any usable text content."); // report pages that contained only markup or unsupported text payloads
        toolResult = "{\"error\":\"URL fetch did not yield any usable text content.\"}"; // return a machine-readable extraction failure
        return false; // stop when nothing useful can be forwarded to the LLM
    }

    const bool textTruncated = extractedContent.length() >= static_cast<size_t>(maxChars); // indicate that the visible excerpt hit the caller's text budget

    JsonDocument output; // compact tool output returned to the LLM layer
    output["url"] = normalizedUrl; // echo the normalized URL back to the caller
    output["content_type"] = contentType; // preserve the resolved MIME type for downstream reasoning
    output["http_status"] = httpCode; // include the upstream HTTP status for debugging and trust decisions
    output["requested_max_chars"] = maxChars; // expose the bounded excerpt size requested for this fetch
    output["response_truncated"] = bodyTruncated; // show whether the raw response body hit the device safety cap
    output["text_truncated"] = textTruncated; // show whether the visible excerpt hit the caller's max-char cap
    output["content"] = extractedContent; // include the normalized text excerpt passed to the second LLM prompt

    if (!redirectedUrl.isEmpty()) {
        output["redirect_location"] = redirectedUrl; // expose the redirect target when the origin reported one
    }

    Serial.println(); // blank line before dumping result JSON
    Serial.println("--- FETCH RESULT JSON ---"); // mark the start of the tool payload in serial
    serializeJson(output, Serial); // print the exact JSON returned to the LLM layer
    Serial.println(); // finish the JSON line cleanly
    Serial.println("--- END FETCH RESULT JSON ---"); // mark the end of the payload dump

    toolResult = ""; // clear any stale caller-provided content before writing the result
    serializeJson(output, toolResult); // serialize the compact result JSON into the output string

    return true; // URL fetch completed successfully
}

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

bool currentDateTime(String& toolResult) {
    if (!deviceClockReady()) {
        Serial.println("current_datetime is unavailable because the device clock is not synchronized."); // reject requests until SNTP has established a real wall-clock time
        toolResult = "{\"error\":\"Device clock is not synchronized yet.\"}"; // return a machine-readable clock-state error
        return false; // stop when current wall-clock time is unavailable
    }

    const time_t now = time(nullptr); // capture one consistent timestamp for all derived fields

    struct tm localTimeInfo;
    struct tm utcTimeInfo;

    if (localtime_r(&now, &localTimeInfo) == nullptr || gmtime_r(&now, &utcTimeInfo) == nullptr) {
        Serial.println("Could not convert the current timestamp into calendar fields."); // report conversion failures before building the response JSON
        toolResult = "{\"error\":\"Could not convert the current timestamp.\"}"; // return a machine-readable conversion error
        return false; // stop when the timestamp cannot be rendered safely
    }

    char localIso8601[32];
    char utcIso8601[32];
    char localDate[16];
    char localTimeOfDay[16];
    char weekdayName[16];
    char monthName[16];
    char timezoneName[16];

    strftime(localIso8601, sizeof(localIso8601), "%Y-%m-%dT%H:%M:%S%z", &localTimeInfo); // emit a stable local timestamp for the LLM layer
    strftime(utcIso8601, sizeof(utcIso8601), "%Y-%m-%dT%H:%M:%SZ", &utcTimeInfo); // emit a stable UTC timestamp with explicit Z suffix
    strftime(localDate, sizeof(localDate), "%Y-%m-%d", &localTimeInfo); // extract the local calendar date for day/date questions
    strftime(localTimeOfDay, sizeof(localTimeOfDay), "%H:%M:%S", &localTimeInfo); // extract the local time of day for concise answers
    strftime(weekdayName, sizeof(weekdayName), "%A", &localTimeInfo); // extract the local weekday name directly from the synchronized clock
    strftime(monthName, sizeof(monthName), "%B", &localTimeInfo); // include the month name so the model can speak more naturally when needed
    strftime(timezoneName, sizeof(timezoneName), "%Z", &localTimeInfo); // include the resolved timezone abbreviation from the active TZ rules

    JsonDocument output; // compact tool output returned to the LLM layer
    output["unix_epoch"] = static_cast<long long>(now); // preserve the raw timestamp for unambiguous comparisons
    output["local_iso8601"] = localIso8601; // fully-qualified local timestamp with numeric offset
    output["utc_iso8601"] = utcIso8601; // fully-qualified UTC timestamp
    output["local_date"] = localDate; // local calendar date
    output["local_time"] = localTimeOfDay; // local time of day
    output["weekday"] = weekdayName; // local weekday name
    output["month"] = monthName; // local month name
    output["year"] = localTimeInfo.tm_year + 1900; // expose numeric year without extra parsing
    output["day_of_month"] = localTimeInfo.tm_mday; // expose numeric day-of-month without extra parsing
    output["timezone"] = timezoneName; // resolved local timezone abbreviation
    output["timezone_config"] = DEVICE_TIME_ZONE; // echo the configured POSIX TZ string used for conversion
    output["source"] = "ntp"; // identify that the timestamp comes from synchronized wall-clock time
    output["uptime_ms"] = millis(); // include uptime so clock freshness can be judged when debugging

    Serial.println(); // blank line before dumping result JSON
    Serial.println("--- DATETIME RESULT JSON ---"); // mark the start of the tool payload in serial
    serializeJson(output, Serial); // print the exact JSON returned to the LLM layer
    Serial.println(); // finish the JSON line cleanly
    Serial.println("--- END DATETIME RESULT JSON ---"); // mark the end of the payload dump

    toolResult = ""; // clear any stale caller-provided content before writing the result
    serializeJson(output, toolResult); // serialize the compact result JSON into the output string

    return true; // current datetime lookup completed successfully
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

    if (strcmp(toolName, "current_datetime") == 0) {
        Serial.print("Tool requested: "); // log the chosen tool name
        Serial.println(toolName); // print the chosen tool name value

        return currentDateTime(toolResult); // execute the datetime tool and pass its JSON result back up
    }

    if (strcmp(toolName, "fetch_url") == 0) {
        const char* url =
            toolCallDocument["arguments"]["url"] | ""; // extract the requested URL with a safe default
        int maxChars =
            toolCallDocument["arguments"]["max_chars"] | URL_FETCH_DEFAULT_MAX_CHARS; // default to the standard excerpt budget when omitted

        if (strlen(url) == 0) {
            Serial.println("fetch_url requires a url."); // reject empty fetch requests before opening a connection
            toolResult = "{\"error\":\"fetch_url requires a url.\"}"; // return a machine-readable validation error
            return false; // stop when the required argument is missing
        }

        Serial.print("Tool requested: "); // log the chosen tool name
        Serial.println(toolName); // print the chosen tool name value

        Serial.print("URL: "); // label the requested URL in serial output
        Serial.println(url); // print the raw URL text

        Serial.print("Max chars: "); // label the requested text budget
        Serial.println(maxChars); // print the requested text budget value

        return fetchUrlContent(String(url), maxChars, toolResult); // execute the URL fetch tool and pass its JSON result back up
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

