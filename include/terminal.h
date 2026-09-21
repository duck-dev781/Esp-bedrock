#pragma once

#include <Arduino.h>
#include "world.h"

class SerialTerminal {
public:
  void begin(World &world);
  void update();

private:
  World *worldPtr = nullptr;
  String line;

  void execute(const String &command);
  void printHelp();
  void cmdStatus();
  void cmdWorld();
  void cmdSave();
  void cmdRegenerate();
  void cmdSay(const String &text);
  void cmdSetTime(const String &value);
};
