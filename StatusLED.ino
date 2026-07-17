#include <FastLED.h>

CRGB statusLeds[1]; // single onboard addressable status LED
unsigned long statusLedBlueFlashUntil = 0; // millis() deadline while the blue MQTT-activity flash is showing

void initStatusLed() {
  FastLED.addLeds<WS2812, STATUS_LED_PIN, GRB>(statusLeds, 1);
  FastLED.setBrightness(40);
  statusLeds[0] = CRGB::Red; // not ready until Wi-Fi connects
  FastLED.show();
}

void flashStatusLedBlue() {
  statusLedBlueFlashUntil = millis() + 100; // hold blue for 100ms on MQTT activity
}

void updateStatusLed() {
  if (millis() < statusLedBlueFlashUntil) {
    statusLeds[0] = CRGB::Blue;
  } else if (WiFi.status() != WL_CONNECTED) {
    statusLeds[0] = CRGB::Red;
  } else if (isLLMRequestRunning()) {
    statusLeds[0] = CRGB::Magenta;
  } else {
    statusLeds[0] = CRGB::Green;
  }

  FastLED.show();
}
