#pragma once

#include <Arduino.h>
#include <FS.h>

struct PlayerState {
  bool connected;
  char name[32];
  float x;
  float y;
  float z;
  float yaw;
  float pitch;
  uint16_t health;
  uint16_t hunger;
};

class World {
public:
  bool begin(fs::FS &fs);
  bool save();
  bool regenerate();

  uint8_t getBlock(int x, int y, int z) const;
  bool setBlock(int x, int y, int z, uint8_t block);

  uint32_t seed() const { return worldSeed; }
  uint32_t gameTime() const { return tickTime; }
  void setGameTime(uint32_t t) { tickTime = t % 24000UL; }
  void tick();

private:
  fs::FS *storage = nullptr;
  uint32_t worldSeed = 0x5EED1234;
  uint32_t tickTime = 0;

  int terrainHeight(int x, int z) const;
  String worldPath() const;
};
