#include <PubSubClient.h>

WiFiClient mqttNetworkClient; // plain TCP transport for the MQTT connection
PubSubClient mqttClient(mqttNetworkClient);
unsigned long lastMqttReconnectAttempt = 0; // throttle reconnect attempts so a down broker doesn't stall the loop

void handleMqttMessage(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    message += static_cast<char>(payload[i]);
  }
  message.trim();

  flashStatusLedBlue(); // any inbound MQTT message flashes the activity LED
  Serial.print("MQTT message on ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(message);

  const String topicName(topic);

  if (topicName == "ask") {
    if (!submitChatMessage(message, "MQTT")) {
      Serial.println("MQTT ask ignored because the request could not be queued.");
    }
    return;
  }

  if (topicName == "port") {
    bool portIsNumeric = message.length() > 0;
    for (size_t i = 0; i < message.length(); i++) {
      if (!isDigit(static_cast<unsigned char>(message[i]))) {
        portIsNumeric = false;
        break;
      }
    }

    if (!portIsNumeric) {
      Serial.println("MQTT port message was not a valid number, ignoring.");
      return;
    }

    unsigned long requestedPort = message.toInt();
    if (requestedPort == 0 || requestedPort > 65535UL) {
      Serial.println("MQTT port message out of range, ignoring.");
      return;
    }

    llmPort = static_cast<uint16_t>(requestedPort);
    Serial.print("LLM port set via MQTT: ");
    Serial.println(llmPort);
    return;
  }

  if (topicName == "ip") {
    if (message.length() == 0) {
      return;
    }

    llmHost = message;
    Serial.print("LLM host set via MQTT: ");
    Serial.println(llmHost);
    return;
  }

  if (topicName == "clear") {
    clearConversationHistory();
    Serial.println("Context window cleared via MQTT.");
    return;
  }
}

void publishLlmResponseToMqtt(const String& responseText) {
  if (!mqttClient.connected()) {
    Serial.println("Skipping MQTT response publish: broker not connected.");
    return;
  }

  if (mqttClient.publish("answer", responseText.c_str())) {
    flashStatusLedBlue(); // mirror the blue activity flash for outbound messages too
  } else {
    Serial.println("MQTT publish of LLM response failed (payload may exceed buffer size).");
  }
}

void connectToMqttBroker() {
  Serial.print("Connecting to MQTT broker...");
  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    Serial.println(" connected.");
    mqttClient.subscribe("ask");
    mqttClient.subscribe("port");
    mqttClient.subscribe("ip");
    mqttClient.subscribe("clear");
  } else {
    Serial.print(" failed, rc=");
    Serial.println(mqttClient.state());
  }
}

void initMqtt() {
  mqttClient.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  mqttClient.setCallback(handleMqttMessage);
  mqttClient.setBufferSize(4096); // allow prompts/responses longer than PubSubClient's 256-byte default
  connectToMqttBroker();
}

void maintainMqttConnection() {
  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }

  unsigned long now = millis();
  if (now - lastMqttReconnectAttempt < 5000) {
    return; // avoid hammering a broker that is down
  }

  lastMqttReconnectAttempt = now;
  connectToMqttBroker();
}
