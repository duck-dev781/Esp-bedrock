#include "world.h"
#include "espbedrock_config.h"

static uint32_t mix32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dUL;
  x ^= x >> 15;
  x *= 0x846ca68bUL;
  x ^= x >> 16;
  return x;
}

String World::worldPath() const {
  return String(ESPBEDROCK_WORLD_FILE);
}

bool World::begin(fs::FS &fs) {
  storage = &fs;

  File f = storage->open(worldPath(), FILE_READ);
  if (f) {
    if (f.size() >= 8) {
      f.read((uint8_t*)&worldSeed, sizeof(worldSeed));
      f.read((uint8_t*)&tickTime, sizeof(tickTime));
      Serial.printf("[WORLD] Loaded seed=%lu time=%lu\n",
                    (unsigned long)worldSeed,
                    (unsigned long)tickTime);
    }
    f.close();
    return true;
  }

  Serial.println("[WORLD] No save found; creating a new world.");
  return save();
}

bool World::save() {
  if (!storage) return false;

  File f = storage->open(worldPath(), FILE_WRITE);
  if (!f) {
    Serial.println("[WORLD] Could not open save file.");
    return false;
  }

  f.write((const uint8_t*)&worldSeed, sizeof(worldSeed));
  f.write((const uint8_t*)&tickTime, sizeof(tickTime));
  f.close();

  Serial.println("[WORLD] Saved.");
  return true;
}

bool World::regenerate() {
  // The terrain is deterministic from the seed, so regeneration only
  // resets the world seed and clock in this first prototype.
  worldSeed = mix32(worldSeed + 1);
  tickTime = 0;
  Serial.printf("[WORLD] Regenerated seed=%lu\n", (unsigned long)worldSeed);
  return save();
}

int World::terrainHeight(int x, int z) const {
  const uint32_t n1 = mix32((uint32_t)x * 374761393UL ^ (uint32_t)z * 668265263UL ^ worldSeed);
  const int hills = (int)(n1 % 9);
  return 22 + hills;
}

uint8_t World::getBlock(int x, int y, int z) const {
  if (y < 0 || y >= ESPBEDROCK_WORLD_HEIGHT) return 0;

  const int h = terrainHeight(x, z);

  if (y > h) return 0;
  if (y == h) return 2;      // grass
  if (y >= h - 3) return 3;  // dirt
  return 1;                   // stone
}

bool World::setBlock(int x, int y, int z, uint8_t block) {
  // Persistent block edits come in a later chunk-storage layer.
  // Keep the API now so the network/protocol layer doesn't change later.
  (void)x;
  (void)y;
  (void)z;
  (void)block;
  return true;
}

void World::tick() {
  tickTime = (tickTime + 1) % 24000UL;
}
