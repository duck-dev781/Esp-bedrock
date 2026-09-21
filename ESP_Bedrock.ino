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
    - FNK0047-compatible SDMMC 1-bit storage
    - Wi-Fi STA/client mode (no SoftAP)
    - Serial Wi-Fi configuration with saved credentials
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
#include <SD_MMC.h>
#include <FS.h>
#include <Preferences.h>
#include <ESPmDNS.h>

#define ESPBEDROCK_VERSION       "0.3.1"
#define ESPBEDROCK_UDP_PORT      19132
#define ESPBEDROCK_HOSTNAME       "esp-bedrock"
#define ESPBEDROCK_MAX_PLAYERS   4
#define SD_MMC_CMD               15  // FNK0047/Freenove fixed pin
#define SD_MMC_CLK               14  // FNK0047/Freenove fixed pin
#define SD_MMC_D0                 2  // FNK0047/Freenove fixed pin
#define SD_MMC_MOUNT_POINT       "/sdcard"
#define SD_MMC_MAX_FILES          5
#define ESPBEDROCK_WORLD_DIR     "/espbedrock/world"
#define ESPBEDROCK_CONFIG_DIR    "/espbedrock/config"
#define ESPBEDROCK_ASSET_DIR     "/espbedrock/assets"
#define ESPBEDROCK_WORLD_FILE    "/espbedrock/world/world.dat"
#define ESPBEDROCK_WORLD_HEIGHT  64
#define WIFI_CONNECT_TIMEOUT_MS 15000

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

class WiFiManager {
public:
  void begin() {
    preferences.begin("espbedrock", false);

    ssid = preferences.getString("ssid", "");
    password = preferences.getString("pass", "");

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);

    Serial.println("[WIFI] STA/client mode only.");
    Serial.println("[WIFI] No SoftAP is created.");

    if (ssid.length() == 0) {
      Serial.println("[WIFI] No saved network.");
      Serial.println("[WIFI] Use: wifi scan");
      Serial.println("[WIFI] Then: wifi set <SSID>|<PASSWORD>");
      Serial.println("[WIFI] Then: wifi connect");
      return;
    }

    Serial.printf("[WIFI] Saved network: %s\n", ssid.c_str());
    connect();
  }

  bool connect() {
    if (ssid.length() == 0) {
      Serial.println("[WIFI] No SSID configured.");
      return false;
    }

    Serial.printf("[WIFI] Connecting to %s", ssid.c_str());

    WiFi.disconnect();
    delay(100);
    WiFi.begin(ssid.c_str(), password.c_str());

    const uint32_t start = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      delay(250);
      Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("[WIFI] Connected. IP: ");
      Serial.println(WiFi.localIP());

      Serial.print("[WIFI] Gateway: ");
      Serial.println(WiFi.gatewayIP());

      Serial.print("[WIFI] RSSI: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");

      startMDNS();
      return true;
    }

    Serial.printf("[WIFI] Connection failed. status=%d\n",
                  (int)WiFi.status());
    return false;
  }

  void setCredentials(const String &newSSID,
                      const String &newPassword) {
    ssid = newSSID;
    password = newPassword;

    preferences.putString("ssid", ssid);
    preferences.putString("pass", password);

    Serial.printf("[WIFI] Saved SSID: %s\n", ssid.c_str());
    Serial.println("[WIFI] Password saved.");
  }

  void clearCredentials() {
    preferences.clear();
    ssid = "";
    password = "";

    WiFi.disconnect(true);
    Serial.println("[WIFI] Saved credentials cleared.");
  }

  void printStatus() const {
    Serial.println("Mode: STA/client");
    Serial.printf("Configured SSID: %s\n",
                  ssid.length() ? ssid.c_str() : "(none)");
    Serial.printf("Status: %s\n", statusText());

    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());

      Serial.print("Port: ");
      Serial.println(ESPBEDROCK_UDP_PORT);

      Serial.print("Hostname: ");
      Serial.print(ESPBEDROCK_HOSTNAME);
      Serial.println(".local");

      Serial.print("Gateway: ");
      Serial.println(WiFi.gatewayIP());

      Serial.print("RSSI: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
    }
  }

  void scan() {
    Serial.println("[WIFI] Scanning...");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    const int count = WiFi.scanNetworks(false, true);

    if (count < 0) {
      Serial.println("[WIFI] Scan failed.");
      return;
    }

    Serial.printf("[WIFI] Found %d network(s):\n", count);

    for (int i = 0; i < count; ++i) {
      Serial.printf(
        "  %d: %s  RSSI=%d  %s\n",
        i + 1,
        WiFi.SSID(i).c_str(),
        WiFi.RSSI(i),
        WiFi.encryptionType(i) == WIFI_AUTH_OPEN
          ? "open"
          : "secured"
      );
    }

    WiFi.scanDelete();
  }

  bool startMDNS() {
    if (WiFi.status() != WL_CONNECTED) return false;

    if (MDNS.begin(ESPBEDROCK_HOSTNAME)) {
      MDNS.addService("minecraft", "udp", ESPBEDROCK_UDP_PORT);
      Serial.print("[LAN] mDNS: ");
      Serial.print(ESPBEDROCK_HOSTNAME);
      Serial.println(".local");
      return true;
    }

    Serial.println("[LAN] mDNS startup failed.");
    return false;
  }

  void printLanInfo() const {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[LAN] ESP32 is not connected to the LAN.");
      return;
    }

    Serial.println("[LAN] Local network information:");
    Serial.print("  IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("  UDP port: ");
    Serial.println(ESPBEDROCK_UDP_PORT);
    Serial.print("  mDNS: ");
    Serial.print(ESPBEDROCK_HOSTNAME);
    Serial.println(".local");
    Serial.println("  Clients on the same LAN can target the ESP32 IP and port.");
  }

  const char *statusText() const {
    switch (WiFi.status()) {
      case WL_CONNECTED:       return "connected";
      case WL_NO_SSID_AVAIL:   return "SSID not found";
      case WL_CONNECT_FAILED:  return "connection failed";
      case WL_CONNECTION_LOST: return "connection lost";
      case WL_DISCONNECTED:    return "disconnected";
      case WL_IDLE_STATUS:     return "idle";
      default:                 return "unknown";
    }
  }

private:
  Preferences preferences;
  String ssid;
  String password;
};

WiFiManager wifiManager;

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
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[NET] Wi-Fi is not connected. UDP server not started.");
      serverRunning = false;
      return false;
    }

    serverRunning = udp.begin(ESPBEDROCK_UDP_PORT) == 1;

    if (serverRunning) {
      Serial.printf("[NET] UDP listener active on %u\n",
                    ESPBEDROCK_UDP_PORT);
    }

    return serverRunning;
  }

  void restartIfNeeded() {
    if (serverRunning && WiFi.status() != WL_CONNECTED) {
      udp.stop();
      serverRunning = false;
      Serial.println("[NET] Wi-Fi lost; UDP server stopped.");
      return;
    }

    if (!serverRunning && WiFi.status() == WL_CONNECTED) {
      begin();
    }
  }

  void update() {
    if (!serverRunning) return;

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

  uint32_t packetsReceived() const {
    return rxPackets;
  }

private:
  WiFiUDP udp;
  bool serverRunning = false;
  uint32_t rxPackets = 0;
};

class SerialTerminal {
public:
  void begin(World &w, NetworkServer &n) {
    world = &w;
    network = &n;
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
  NetworkServer *network = nullptr;

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
    else if (command == "wifi status") {
      wifiManager.printStatus();
    }
    else if (command == "wifi scan") {
      wifiManager.scan();
    }
    else if (command == "wifi connect") {
      wifiManager.connect();
      network->restartIfNeeded();
    }
    else if (command == "wifi clear") {
      wifiManager.clearCredentials();
      network->restartIfNeeded();
    }
    else if (command == "lan") {
      wifiManager.printLanInfo();
    }
    else if (command == "sd status") {
      cmdSDStatus();
    }
    else if (command == "sd ls") {
      cmdSDList();
    }
    else if (command.startsWith("wifi set ")) {
      cmdWifiSet(command.substring(9));
    }
    else if (command == "stop") {
      Serial.println("Saving world before stop...");
      world->save();
      Serial.println("Reset the board to restart the server.");
    }
    else if (command.length() > 0) Serial.println("Unknown command. Type 'help'.");
  }

  void cmdWifiSet(const String &value) {
    const int split = value.indexOf('|');

    if (split <= 0) {
      Serial.println("Usage: wifi set <SSID>|<PASSWORD>");
      Serial.println("Open network example: wifi set MyNetwork|");
      return;
    }

    const String newSSID = value.substring(0, split);
    const String newPassword = value.substring(split + 1);

    if (newSSID.length() == 0) {
      Serial.println("SSID cannot be empty.");
      return;
    }

    wifiManager.setCredentials(newSSID, newPassword);
    Serial.println("Credentials saved. Use 'wifi connect'.");
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
    Serial.println("  wifi status");
    Serial.println("  wifi scan");
    Serial.println("  wifi set <SSID>|<PASSWORD>");
    Serial.println("  wifi connect");
    Serial.println("  wifi clear");
    Serial.println("  lan");
    Serial.println("  sd status");
    Serial.println("  sd ls");
    Serial.println("  stop");
  }

  void cmdSDStatus() {
    const uint8_t type = SD_MMC.cardType();

    if (type == CARD_NONE) {
      Serial.println("[SD] Not mounted.");
      return;
    }

    Serial.println("[SD] FNK0047 SDMMC 1-bit mode");
    Serial.println("[SD] CLK=GPIO14 CMD=GPIO15 D0=GPIO2");
    Serial.printf("[SD] Capacity: %llu MB\n",
                  SD_MMC.cardSize() / (1024ULL * 1024ULL));
    Serial.printf("[SD] Total: %llu MB\n",
                  SD_MMC.totalBytes() / (1024ULL * 1024ULL));
    Serial.printf("[SD] Used: %llu MB\n",
                  SD_MMC.usedBytes() / (1024ULL * 1024ULL));
    Serial.println("[SD] Root directories:");
    Serial.println("  /espbedrock/world");
    Serial.println("  /espbedrock/config");
    Serial.println("  /espbedrock/assets");
  }

  void cmdSDList() {
    if (SD_MMC.cardType() == CARD_NONE) {
      Serial.println("[SD] Not mounted.");
      return;
    }

    listSDDirectory("/", 0);
  }

  void listSDDirectory(const char *dirname, uint8_t depth) {
    if (depth > 3) return;

    File root = SD_MMC.open(dirname);
    if (!root || !root.isDirectory()) {
      Serial.printf("[SD] Cannot open %s\n", dirname);
      return;
    }

    File entry = root.openNextFile();

    while (entry) {
      for (uint8_t i = 0; i < depth; ++i) {
        Serial.print("  ");
      }

      Serial.print(entry.name());

      if (entry.isDirectory()) {
        Serial.println("/");
        const String child = entry.name();
        entry.close();
        listSDDirectory(child.c_str(), depth + 1);
      } else {
        Serial.printf("  (%llu bytes)\n",
                      (unsigned long long)entry.size());
        entry.close();
      }

      entry = root.openNextFile();
    }

    root.close();
  }

  void cmdStatus() {
    Serial.printf("Version: %s\n", ESPBEDROCK_VERSION);
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("PSRAM total: %u bytes\n", ESP.getPsramSize());
    Serial.printf("PSRAM free: %u bytes\n", ESP.getFreePsram());
    Serial.println("Wi-Fi mode: STA/client ONLY");
    wifiManager.printStatus();
    Serial.printf("UDP packets received: %lu\n",
                  (unsigned long)network->packetsReceived());
    Serial.printf("UDP port: %u\n", ESPBEDROCK_UDP_PORT);
    Serial.printf("SDMMC: %s\n",
                  SD_MMC.cardType() == CARD_NONE ? "not mounted" : "mounted");
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
  // Freenove FNK0047 / ESP32-WROVER uses the built-in SDMMC slot.
  // Freenove documents the 1-bit bus as:
  //   CLK = GPIO14
  //   CMD = GPIO15
  //   D0  = GPIO2
  // They specifically use the 1-bit SDMMC configuration.
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);

  if (!SD_MMC.begin(
        SD_MMC_MOUNT_POINT,
        false,                  // NEVER auto-format the world SD card
        true,                   // 1-bit mode
        SDMMC_FREQ_DEFAULT,     // Freenove's documented setting
        SD_MMC_MAX_FILES)) {
    Serial.println("[SD] SDMMC mount failed.");
    return false;
  }

  const uint8_t cardType = SD_MMC.cardType();

  if (cardType == CARD_NONE) {
    Serial.println("[SD] No SDMMC card detected.");
    SD_MMC.end();
    return false;
  }

  Serial.print("[SD] Card type: ");

  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  }
  else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  }
  else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  }
  else {
    Serial.println("UNKNOWN");
  }

  Serial.printf(
    "[SD] Capacity: %llu MB\n",
    SD_MMC.cardSize() / (1024ULL * 1024ULL)
  );

  Serial.printf(
    "[SD] Total: %llu MB\n",
    SD_MMC.totalBytes() / (1024ULL * 1024ULL)
  );

  Serial.printf(
    "[SD] Used: %llu MB\n",
    SD_MMC.usedBytes() / (1024ULL * 1024ULL)
  );

  SD_MMC.mkdir("/espbedrock");
  SD_MMC.mkdir(ESPBEDROCK_WORLD_DIR);
  SD_MMC.mkdir(ESPBEDROCK_CONFIG_DIR);
  SD_MMC.mkdir(ESPBEDROCK_ASSET_DIR);

  return true;
}

void printBootInfo() {
  Serial.println();
  Serial.println("======================================");
  Serial.println("          ESP-BEDROCK 0.3.1");
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
    if (!world.begin(SD_MMC)) {
      Serial.println("[WORLD] World initialization failed.");
    }
  } else {
    Serial.println("[WORLD] Running without persistent SD storage.");
  }

  wifiManager.begin();

  terminal.begin(world, network);

  // The server only binds its UDP socket after a normal Wi-Fi connection.
  if (WiFi.status() == WL_CONNECTED) {
    wifiManager.startMDNS();
    network.begin();
  }

  Serial.println();
  Serial.println("[READY] ESP-Bedrock prototype running.");
  Serial.println("[READY] Serial monitor: 115200 baud.");
  Serial.println("[READY] Wi-Fi: normal STA/client mode. No SoftAP.");
}

void loop() {
  terminal.update();
  network.restartIfNeeded();
  network.update();

  static uint32_t lastTick = 0;
  const uint32_t now = millis();

  if (now - lastTick >= 50) {
    lastTick = now;
    world.tick();
  }

  delay(1);
}
