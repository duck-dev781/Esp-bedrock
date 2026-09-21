#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>

#include "espbedrock_config.h"
#include "world.h"
#include "terminal.h"
#include "net.h"

World world;
SerialTerminal terminal;
NetworkServer network;

static bool mountStorage() {
  if (!SD.begin()) {
    Serial.println("[SD] Mount failed. Server will run without persistence.");
    return false;
  }

  Serial.println("[SD] Mounted.");
  SD.mkdir("/espbedrock");
  SD.mkdir(ESPBEDROCK_WORLD_DIR);
  SD.mkdir(ESPBEDROCK_CONFIG_DIR);
  SD.mkdir(ESPBEDROCK_ASSET_DIR);
  return true;
}

static void printBootInfo() {
  Serial.println();
  Serial.println("================================");
  Serial.println(" ESP-BEDROCK");
  Serial.printf(" version: %s\n", ESPBEDROCK_VERSION);
  Serial.println(" target: ESP32-WROVER-E");
  Serial.printf(" PSRAM: %u bytes\n", ESP.getPsramSize());
  Serial.printf(" free heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf(" UDP port: %u\n", ESPBEDROCK_UDP_PORT);
  Serial.println("================================");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  printBootInfo();

  const bool sdReady = mountStorage();
  if (sdReady) {
    world.begin(SD);
  } else {
    Serial.println("[WORLD] Persistence disabled.");
  }

  terminal.begin(world);

  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP-Bedrock", "");
  Serial.print("[NET] AP address: ");
  Serial.println(WiFi.softAPIP());

  if (!network.begin()) {
    Serial.println("[NET] UDP listener failed.");
  }

  Serial.println("[READY] Type 'help'.");
}

void loop() {
  terminal.update();
  network.update();

  static uint32_t lastTick = 0;
  const uint32_t now = millis();
  if (now - lastTick >= 50) {
    lastTick = now;
    world.tick();
  }

  delay(1);
}
