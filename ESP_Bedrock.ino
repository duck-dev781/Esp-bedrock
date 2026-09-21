/*
  ESP-Bedrock
  ESP32-WROVER-E single-file prototype

  Goal:
    A lightweight, original Minecraft-inspired survival server/runtime
    foundation for the ESP32-WROVER-E.

  This is NOT the official Minecraft Bedrock server and does not contain
  Microsoft's proprietary client/server code or assets.

  Current features:
    - ESP32-WROVER-E / PSRAM awareness
    - Wi-Fi SoftAP
    - UDP port 19132 foundation
    - SD-card persistence
    - Procedural voxel terrain API
    - World metadata save/load
    - Serial admin terminal
    - VarUInt protocol utility
    - Original block metadata support

  Next protocol/gameplay layers:
    - RakNet session handshake
    - Bedrock login/session packets
    - chunk serialization
    - player movement
    - block break/place
    - inventory/crafting
    - survival state
    - persistent chunk edits/entities

  Serial monitor:
    115200 baud

  Terminal commands:
    help
    status
    players
    world
    save
    regen
    say <message>
    settime <0-23999>
    stop
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>

#define ESPBEDROCK_VERSION       "0.1.0"
#define ESPBEDROCK_UDP_PORT      19132
#define ESPBEDROCK_MAX_PLAYERS   4
#define SD_CS_PIN                5
#define ESPBEDROCK_WORLD_DIR     "/espbedrock/world"
#define ESPBEDROCK_CONFIG_DIR    "/espbedrock/config"
#define ESPBEDROCK_ASSET_DIR     "/espbedrock/assets"
#define ESPBEDROCK_WORLD_FILE    "/espbedrock/world/world.dat"
#define ESPBEDROCK_WORLD_HEIGHT  64

struct PlayerState {
  bool connected = false;
  char name[32] = {};
  float x = 8.0f;
  float y = 28.0f;
  float z = 8.0f;
  float yaw = 0.0f;
  float pitch = 0.0f;
  uint16_t health = 20;
  uint16_t hunger = 20;
};

namespace BedrockProtocol {
size_t writeVarUInt(uint32_t value, uint8_t *out, size_t capacity) {
  size_t i = 0;
  while (true) {
    if (i >= capacity || i >= 5) return 0;
    uint8_t byte = value & 0x7F;
    value >>= 7;
    if (value != 0) byte |= 0x80;
    out[i++] = byte;
    if (value == 0) return i;
  }
}

bool readVarUInt(const uint8_t *data, size_t length, size_t &offset,
                 uint32_t &value) {
  value = 0;
  uint8_t shift = 0;
  while (offset < length && shift < 35) {
    const uint8_t byte = data[offset++];
    value |= (uint32_t)(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}
}

class World {
public:
  bool begin(fs::FS &fs) {
    storage = &fs;
    if (!storage) return false;
    File f = storage->open(ESPBEDROCK_WORLD_FILE, FILE_READ);
    if (f) {
      if (f.size() >= 8) {
        f.read((uint8_t *)&worldSeed, sizeof(worldSeed));
        f.read((uint8_t *)&tickTime, sizeof(tickTime));
        Serial.printf("[WORLD] Loaded seed=%lu time=%lu\n",
                      (unsigned long)worldSeed, (unsigned long)tickTime);
      }
      f.close();
      return true;
    }
    Serial.println("[WORLD] No save found; creating a new world.");
    return save();
  }

  bool save() {
    if (!storage) return false;
    File f = storage->open(ESPBEDROCK_WORLD_FILE, FILE_WRITE);
    if (!f) {
      Serial.println("[WORLD] Could not open save file.");
      return false;
    }
    f.write((const uint8_t *)&worldSeed, sizeof(worldSeed));
    f.write((const uint8_t *)&tickTime, sizeof(tickTime));
    f.close();
    Serial.println("[WORLD] Saved.");
    return true;
  }

  bool regenerate() {
    worldSeed = mix32(worldSeed + 1);
    tickTime = 0;
    Serial.printf("[WORLD] Regenerated seed=%lu\n",
                  (unsigned long)worldSeed);
    return save();
  }

  uint32_t seed() const { return worldSeed; }
  uint32_t gameTime() const { return tickTime; }
  void setGameTime(uint32_t t) { tickTime = t % 24000UL; }
  void tick() { tickTime = (tickTime + 1) % 24000UL; }

  int terrainHeight(int x, int z) const {
    const uint32_t n1 =
      mix32((uint32_t)x * 374761393UL ^
            (uint32_t)z * 668265263UL ^ worldSeed);
    return 22 + (int)(n1 % 9);
  }

  uint8_t getBlock(int x, int y, int z) const {
    if (y < 0 || y >= ESPBEDROCK_WORLD_HEIGHT) return 0;
    const int h = terrainHeight(x, z);
    if (y > h) return 0;
    if (y == h) return 2;
    if (y >= h - 3) return 3;
    return 1;
  }

  bool setBlock(int x, int y, int z, uint8_t block) {
    (void)x; (void)y; (void)z; (void)block;
    return true;
  }

private:
  fs::FS *storage = nullptr;
  uint32_t worldSeed = 0x5EED1234UL;
  uint32_t tickTime = 0;

  static uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dUL;
    x ^= x >> 15;
    x *= 0x846ca68bUL;
    x ^= x >> 16;
    return x;
  }
};

class NetworkServer {
public:
  bool begin() {
    return udp.begin(ESPBEDROCK_UDP_PORT) == 1;
  }

  void update() {
    const int packetSize = udp.parsePacket();
    if (packetSize <= 0) return;

    uint8_t buffer[512];
    const size_t n = udp.read(buffer, sizeof(buffer));
    rxPackets++;

    Serial.printf("[UDP] %u bytes from %s:%u\n",
                  (unsigned)n,
                  udp.remoteIP().toString().c_str(),
                  (unsigned)udp.remotePort());

    if (n > 0) {
      size_t offset = 0;
      uint32_t firstVarUInt = 0;
      if (BedrockProtocol::readVarUInt(buffer, n, offset, firstVarUInt)) {
        Serial.printf("[PROTO] first varuint=0x%08lX\n",
                      (unsigned long)firstVarUInt);
      }
    }
  }

private:
  WiFiUDP udp;
  uint32_t rxPackets = 0;
};

class SerialTerminal {
public:
  void begin(World &w) {
    world = &w;
    Serial.println();
    Serial.println("ESP-Bedrock terminal ready.");
    Serial.println("Type 'help'.");
    Serial.print("espbedrock> ");
  }

  void update() {
    while (Serial.available()) {
      const char ch = (char)Serial.read();
      if (ch == '\r') continue;
      if (ch == '\n') {
        execute(line);
        line = "";
        Serial.print("espbedrock> ");
        continue;
      }
      if (line.length() < 160) line += ch;
    }
  }

private:
  String line;
  World *world = nullptr;

  void execute(String command) {
    command.trim();

    if (command == "help") printHelp();
    else if (command == "status") cmdStatus();
    else if (command == "players")
      Serial.println("Players: 0 (RakNet/Bedrock session layer not attached yet)");
    else if (command == "world") cmdWorld();
    else if (command == "save") world->save();
    else if (command == "regen") world->regenerate();
    else if (command.startsWith("say ")) {
      Serial.print("[SERVER] ");
      Serial.println(command.substring(4));
    }
    else if (command.startsWith("settime ")) {
      const long t = command.substring(8).toInt();
      if (t < 0 || t > 23999) Serial.println("Usage: settime 0-23999");
      else {
        world->setGameTime((uint32_t)t);
        Serial.printf("Time set to %ld.\n", t);
      }
    }
    else if (command == "stop") {
      Serial.println("Saving world before stop...");
      world->save();
      Serial.println("Reset the board to restart the server.");
    }
    else if (command.length() > 0) Serial.println("Unknown command. Type 'help'.");
  }

  void printHelp() {
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

  void cmdStatus() {
    Serial.printf("Version: %s\n", ESPBEDROCK_VERSION);
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("PSRAM total: %u bytes\n", ESP.getPsramSize());
    Serial.printf("PSRAM free: %u bytes\n", ESP.getFreePsram());
    Serial.printf("WiFi AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("UDP port: %u\n", ESPBEDROCK_UDP_PORT);
    Serial.printf("SD: %s\n",
                  SD.cardType() == CARD_NONE ? "not mounted" : "mounted");
  }

  void cmdWorld() {
    Serial.printf("Seed: %lu\n", (unsigned long)world->seed());
    Serial.printf("Time: %lu\n", (unsigned long)world->gameTime());
    Serial.println("Terrain: deterministic procedural voxel world");
  }
};

World world;
NetworkServer network;
SerialTerminal terminal;

bool mountStorage() {
  if (!SD.begin(SD_CS_PIN)) {
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

void printBootInfo() {
  Serial.println();
  Serial.println("======================================");
  Serial.println("          ESP-BEDROCK 0.1.0");
  Serial.println("======================================");
  Serial.println("Target: ESP32-WROVER-E");
  Serial.printf("Chip cores: %d\n", ESP.getChipCores());
  Serial.printf("Flash: %u bytes\n", ESP.getFlashChipSize());
  Serial.printf("PSRAM: %u bytes\n", ESP.getPsramSize());
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("UDP port: %u\n", ESPBEDROCK_UDP_PORT);
  Serial.println("======================================");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(400);

  printBootInfo();

  const bool sdReady = mountStorage();

  if (sdReady) {
    if (!world.begin(SD))
      Serial.println("[WORLD] World initialization failed.");
  } else {
    Serial.println("[WORLD] Running without persistent SD storage.");
  }

  terminal.begin(world);

  WiFi.mode(WIFI_AP);
  if (WiFi.softAP("ESP-Bedrock", "")) {
    Serial.println("[NET] Wi-Fi AP: ESP-Bedrock");
    Serial.print("[NET] AP address: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("[NET] Failed to start SoftAP.");
  }

  if (network.begin())
    Serial.printf("[NET] UDP listener active on %u\n", ESPBEDROCK_UDP_PORT);
  else
    Serial.println("[NET] UDP listener failed.");

  Serial.println();
  Serial.println("[READY] ESP-Bedrock prototype running.");
  Serial.println("[READY] Connect the serial monitor at 115200 baud.");
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
