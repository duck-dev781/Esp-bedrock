#include "terminal.h"
#include "espbedrock_config.h"
#include <ESP.h>

void SerialTerminal::begin(World &world) {
  worldPtr = &world;
  Serial.println("espbedrock> ");
}

void SerialTerminal::printHelp() {
  Serial.println("Commands:");
  Serial.println("  help");
  Serial.println("  status");
  Serial.println("  players");
  Serial.println("  world");
  Serial.println("  save");
  Serial.println("  regen");
  Serial.println("  say <message>");
  Serial.println("  settime <0-23999>");
  Serial.println("  stop");
}

void SerialTerminal::cmdStatus() {
  Serial.printf("Version: %s\n", ESPBEDROCK_VERSION);
  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
  Serial.printf("PSRAM free: %u\n", ESP.getFreePsram());
  Serial.printf("WiFi IP: %s\n", WiFi.softAPIP().toString().c_str());
}

void SerialTerminal::cmdWorld() {
  Serial.printf("Seed: %lu\n", (unsigned long)worldPtr->seed());
  Serial.printf("Time: %lu\n", (unsigned long)worldPtr->gameTime());
  Serial.println("World: procedural voxel prototype");
}

void SerialTerminal::cmdSave() {
  worldPtr->save();
}

void SerialTerminal::cmdRegenerate() {
  worldPtr->regenerate();
}

void SerialTerminal::cmdSay(const String &text) {
  Serial.print("[SERVER] ");
  Serial.println(text);
}

void SerialTerminal::cmdSetTime(const String &value) {
  const long t = value.toInt();
  if (t < 0 || t > 23999) {
    Serial.println("Usage: settime 0-23999");
    return;
  }
  worldPtr->setGameTime((uint32_t)t);
  Serial.printf("Time set to %ld.\n", t);
}

void SerialTerminal::execute(const String &command) {
  String c = command;
  c.trim();

  if (c == "help") printHelp();
  else if (c == "status") cmdStatus();
  else if (c == "players") Serial.println("Players: 0 (protocol layer not attached yet)");
  else if (c == "world") cmdWorld();
  else if (c == "save") cmdSave();
  else if (c == "regen") cmdRegenerate();
  else if (c.startsWith("say ")) cmdSay(c.substring(4));
  else if (c.startsWith("settime ")) cmdSetTime(c.substring(8));
  else if (c == "stop") Serial.println("Stop requested: persistent save is complete; reset the board to restart.");
  else if (c.length()) Serial.println("Unknown command. Type 'help'.");
}

void SerialTerminal::update() {
  while (Serial.available()) {
    const char ch = (char)Serial.read();

    if (ch == '\r') continue;

    if (ch == '\n') {
      execute(line);
      line = "";
      Serial.print("espbedrock> ");
      continue;
    }

    if (line.length() < 160) {
      line += ch;
    }
  }
}
