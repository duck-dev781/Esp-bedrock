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

#define ESPBEDROCK_VERSION       "0.4.0"
#define ESPBEDROCK_UDP_PORT      19132
#define ESPBEDROCK_HOSTNAME       "esp-bedrock"
#define ESPBEDROCK_MAX_PLAYERS   4
#define RAKNET_PROTOCOL_VERSION  11
#define RAKNET_MAX_MTU           1492
#define BEDROCK_PROTOCOL_VERSION 2193
#define BEDROCK_VERSION_NAME     "1.26.51"
#define BEDROCK_LEVEL_NAME       "ESP World"
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
  struct Peer {
    bool active = false;
    bool openConnection = false;
    uint64_t clientGuid = 0;
    IPAddress ip;
    uint16_t port = 0;
    uint16_t mtu = 1200;
    uint32_t lastSeen = 0;
    uint32_t datagramsReceived = 0;
  };

  bool begin() {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[NET] Wi-Fi is not connected. RakNet listener not started.");
      serverRunning = false;
      return false;
    }

    serverRunning = udp.begin(ESPBEDROCK_UDP_PORT) == 1;

    if (serverRunning) {
      serverGuid = ESP.getEfuseMac();
      Serial.printf("[RAKNET] Listening on UDP %u\\n", ESPBEDROCK_UDP_PORT);
      Serial.printf("[RAKNET] Server GUID: %llu\\n", (unsigned long long)serverGuid);
      Serial.printf("[RAKNET] Protocol: %u\\n", RAKNET_PROTOCOL_VERSION);
    }

    return serverRunning;
  }

  void restartIfNeeded() {
    if (serverRunning && WiFi.status() != WL_CONNECTED) {
      udp.stop();
      serverRunning = false;
      Serial.println("[RAKNET] Wi-Fi lost; listener stopped.");
      return;
    }

    if (!serverRunning && WiFi.status() == WL_CONNECTED) {
      begin();
    }

    cleanupPeers();
  }

  void update() {
    if (!serverRunning) return;

    const int packetSize = udp.parsePacket();
    if (packetSize <= 0) return;

    uint8_t buffer[1536];
    const size_t n = udp.read(buffer, sizeof(buffer));
    if (n == 0) return;

    const IPAddress from = udp.remoteIP();
    const uint16_t port = udp.remotePort();

    packetsReceivedCount++;

    switch (buffer[0]) {
      case 0x01:
      case 0x02:
        handleUnconnectedPing(buffer, n, from, port);
        break;

      case 0x05:
        handleOpenConnectionRequest1(buffer, n, from, port);
        break;

      case 0x07:
        handleOpenConnectionRequest2(buffer, n, from, port);
        break;

      default:
        if (buffer[0] >= 0x80 && buffer[0] <= 0x8D) {
          handleConnectedDatagram(buffer, n, from, port);
        }
        break;
    }
  }

  uint32_t packetsReceived() const { return packetsReceivedCount; }
  uint32_t pingsAnswered() const { return pingsAnsweredCount; }
  uint32_t handshakeSteps() const { return handshakeCount; }
  uint32_t connectedDatagrams() const { return connectedDatagramCount; }

  uint8_t activePeers() const {
    uint8_t count = 0;
    for (const auto &peer : peers) {
      if (peer.active) count++;
    }
    return count;
  }

  uint64_t guid() const { return serverGuid; }

private:
  static constexpr uint8_t MAGIC[16] = {
    0x00, 0xFF, 0xFF, 0x00,
    0xFE, 0xFE, 0xFE, 0xFE,
    0xFD, 0xFD, 0xFD, 0xFD,
    0x12, 0x34, 0x56, 0x78
  };

  WiFiUDP udp;
  bool serverRunning = false;
  uint64_t serverGuid = 0;

  uint32_t packetsReceivedCount = 0;
  uint32_t pingsAnsweredCount = 0;
  uint32_t handshakeCount = 0;
  uint32_t connectedDatagramCount = 0;

  Peer peers[ESPBEDROCK_MAX_PLAYERS];

  static void writeU16BE(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
  }

  static uint16_t readU16BE(const uint8_t *in) {
    return ((uint16_t)in[0] << 8) | in[1];
  }

  static void writeU64BE(uint8_t *out, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
      out[i] = (uint8_t)(value & 0xFF);
      value >>= 8;
    }
  }

  static uint64_t readU64BE(const uint8_t *in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value = (value << 8) | in[i];
    }
    return value;
  }

  static uint32_t readTriadLE(const uint8_t *in) {
    return ((uint32_t)in[0]) |
           ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16);
  }

  static void writeTriadLE(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
  }

  static bool magicOK(const uint8_t *data, size_t offset, size_t length) {
    return offset + 16 <= length && memcmp(data + offset, MAGIC, 16) == 0;
  }

  static void writeRakAddress(uint8_t *out,
                              const IPAddress &ip,
                              uint16_t port) {
    out[0] = 4;
    for (uint8_t i = 0; i < 4; ++i) {
      out[1 + i] = (uint8_t)~ip[i];
    }
    writeU16BE(out + 5, port);
  }

  Peer *findPeer(const IPAddress &ip, uint16_t port, uint64_t guid = 0) {
    for (auto &peer : peers) {
      if (!peer.active) continue;
      if (guid != 0 && peer.clientGuid == guid) return &peer;
      if (peer.ip == ip && peer.port == port) return &peer;
    }
    return nullptr;
  }

  Peer *allocatePeer(const IPAddress &ip, uint16_t port, uint64_t guid) {
    Peer *peer = findPeer(ip, port, guid);
    if (peer) {
      peer->lastSeen = millis();
      return peer;
    }

    for (auto &candidate : peers) {
      if (!candidate.active) {
        candidate = Peer{};
        candidate.active = true;
        candidate.ip = ip;
        candidate.port = port;
        candidate.clientGuid = guid;
        candidate.lastSeen = millis();
        return &candidate;
      }
    }

    return nullptr;
  }

  void cleanupPeers() {
    const uint32_t now = millis();
    for (auto &peer : peers) {
      if (peer.active && now - peer.lastSeen > 120000UL) {
        peer = Peer{};
      }
    }
  }

  void sendPacket(const uint8_t *data,
                  size_t length,
                  IPAddress ip,
                  uint16_t port) {
    udp.beginPacket(ip, port);
    udp.write(data, length);
    udp.endPacket();
  }

  void handleUnconnectedPing(const uint8_t *data,
                             size_t length,
                             IPAddress ip,
                             uint16_t port) {
    if (length < 33 || !magicOK(data, 9, length)) return;

    const uint64_t pingTime = readU64BE(data + 1);

    const String motd =
      String("MCPE;ESP-Bedrock;") +
      String(BEDROCK_PROTOCOL_VERSION) + ";" +
      String(BEDROCK_VERSION_NAME) + ";" +
      String(activePeers()) + ";" +
      String(ESPBEDROCK_MAX_PLAYERS) + ";" +
      String((unsigned long long)serverGuid) + ";" +
      String(BEDROCK_LEVEL_NAME) + ";Survival;1;" +
      String(ESPBEDROCK_UDP_PORT) + ";19133;";

    uint8_t response[512];
    size_t offset = 0;
    response[offset++] = 0x1C;

    writeU64BE(response + offset, pingTime);
    offset += 8;

    writeU64BE(response + offset, serverGuid);
    offset += 8;

    memcpy(response + offset, MAGIC, sizeof(MAGIC));
    offset += sizeof(MAGIC);

    const uint16_t motdLength = (uint16_t)motd.length();
    writeU16BE(response + offset, motdLength);
    offset += 2;

    memcpy(response + offset, motd.c_str(), motdLength);
    offset += motdLength;

    sendPacket(response, offset, ip, port);
    pingsAnsweredCount++;
  }

  void sendIncompatible(IPAddress ip, uint16_t port) {
    uint8_t response[27];
    response[0] = 0x19;
    response[1] = RAKNET_PROTOCOL_VERSION;
    memcpy(response + 2, MAGIC, sizeof(MAGIC));
    writeU64BE(response + 18, serverGuid);
    sendPacket(response, sizeof(response), ip, port);
  }

  void handleOpenConnectionRequest1(const uint8_t *data,
                                    size_t length,
                                    IPAddress ip,
                                    uint16_t port) {
    if (length < 18 || !magicOK(data, 1, length)) return;

    const uint8_t clientVersion = data[17];

    if (clientVersion != RAKNET_PROTOCOL_VERSION) {
      Serial.printf("[RAKNET] Client protocol %u != server %u\\n",
                    clientVersion, RAKNET_PROTOCOL_VERSION);
      sendIncompatible(ip, port);
      return;
    }

    uint16_t mtu = (uint16_t)min(
      (uint32_t)RAKNET_MAX_MTU,
      (uint32_t)length + 28UL
    );

    if (mtu < 576) mtu = 576;

    uint8_t response[64];
    size_t offset = 0;
    response[offset++] = 0x06;
    memcpy(response + offset, MAGIC, sizeof(MAGIC));
    offset += sizeof(MAGIC);
    writeU64BE(response + offset, serverGuid);
    offset += 8;
    response[offset++] = 0;
    writeU16BE(response + offset, mtu);
    offset += 2;

    sendPacket(response, offset, ip, port);
    handshakeCount++;

    Serial.printf("[RAKNET] Reply1 -> %s:%u mtu=%u\\n",
                  ip.toString().c_str(), (unsigned)port, mtu);
  }

  void handleOpenConnectionRequest2(const uint8_t *data,
                                    size_t length,
                                    IPAddress ip,
                                    uint16_t port) {
    if (length < 34 || !magicOK(data, 1, length)) return;

    const uint16_t clientMtu = readU16BE(data + 24);
    const uint64_t clientGuid = readU64BE(data + 26);

    Peer *peer = allocatePeer(ip, port, clientGuid);

    if (!peer) {
      const uint8_t failed[] = { 0x11 };
      sendPacket(failed, sizeof(failed), ip, port);
      Serial.printf("[RAKNET] Connection slots full for %s:%u\\n",
                    ip.toString().c_str(), (unsigned)port);
      return;
    }

    peer->openConnection = true;
    peer->mtu = min(clientMtu, (uint16_t)RAKNET_MAX_MTU);
    peer->lastSeen = millis();

    uint8_t response[64];
    size_t offset = 0;

    response[offset++] = 0x08;
    memcpy(response + offset, MAGIC, sizeof(MAGIC));
    offset += sizeof(MAGIC);
    writeU64BE(response + offset, serverGuid);
    offset += 8;

    writeRakAddress(response + offset, ip, port);
    offset += 7;

    writeU16BE(response + offset, peer->mtu);
    offset += 2;

    response[offset++] = 0;

    sendPacket(response, offset, ip, port);
    handshakeCount++;

    Serial.printf("[RAKNET] Reply2 -> %s:%u guid=%llu mtu=%u\\n",
                  ip.toString().c_str(),
                  (unsigned)port,
                  (unsigned long long)clientGuid,
                  peer->mtu);
  }

  void handleConnectedDatagram(const uint8_t *data,
                               size_t length,
                               IPAddress ip,
                               uint16_t port) {
    if (length < 4) return;

    Peer *peer = findPeer(ip, port);
    const uint32_t sequence = readTriadLE(data + 1);

    connectedDatagramCount++;

    if (!peer) {
      Serial.printf("[RAKNET] Datagram before handshake from %s:%u seq=%lu\\n",
                    ip.toString().c_str(),
                    (unsigned)port,
                    (unsigned long)sequence);
      return;
    }

    peer->lastSeen = millis();
    peer->datagramsReceived++;
    parseFrames(data + 4, length - 4, peer);
  }

  void parseFrames(const uint8_t *data, size_t length, Peer *peer) {
    size_t offset = 0;

    while (offset + 3 <= length) {
      const uint8_t flags = data[offset++];
      const uint8_t reliability = (flags >> 5) & 0x07;
      const bool isSplit = (flags & 0x10) != 0;

      const uint16_t bitLength =
        ((uint16_t)data[offset] << 8) | data[offset + 1];
      offset += 2;

      const bool reliable =
        reliability == 2 ||
        reliability == 3 ||
        reliability == 4 ||
        reliability == 6 ||
        reliability == 7;

      const bool sequenced = reliability == 1 || reliability == 4;
      const bool ordered = reliability == 3 || reliability == 4 || reliability == 7;

      if (reliable) {
        if (offset + 3 > length) return;
        offset += 3;
      }

      if (sequenced) {
        if (offset + 3 > length) return;
        offset += 3;
      }

      if (ordered) {
        if (offset + 4 > length) return;
        offset += 4;
      }

      if (isSplit) {
        if (offset + 10 > length) return;
        offset += 10;
      }

      const size_t payloadBytes = (bitLength + 7U) / 8U;
      if (offset + payloadBytes > length) return;

      if (payloadBytes > 0) {
        const uint8_t id = data[offset];

        if (id == 0x09 && payloadBytes >= 17) {
          const uint64_t clientGuid = readU64BE(data + offset + 1);
          const uint64_t requestTime = readU64BE(data + offset + 9);

          peer->clientGuid = clientGuid;

          Serial.printf("[RAKNET] ConnectionRequest guid=%llu time=%llu\\n",
                        (unsigned long long)clientGuid,
                        (unsigned long long)requestTime);
          Serial.println("[RAKNET] Reliable session response is next layer.");
        }
        else if (id == 0x00 && payloadBytes >= 9) {
          const uint64_t pingTime = readU64BE(data + offset + 1);
          sendConnectedPong(peer, pingTime);
          Serial.println("[RAKNET] ConnectedPing -> ConnectedPong");
        }
      }

      offset += payloadBytes;
    }
  }

  void sendConnectedPong(Peer *peer, uint64_t pingTime) {
    uint8_t response[32];
    size_t offset = 0;

    response[offset++] = 0x80;
    writeTriadLE(response + offset, 0);
    offset += 3;

    response[offset++] = 0x00;
    writeU16BE(response + offset, 72);
    offset += 2;

    response[offset++] = 0x03;
    writeU64BE(response + offset, pingTime);
    offset += 8;

    sendPacket(response, offset, peer->ip, peer->port);
  }
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
        true,                   // 1-bit mode
        false,                  // NEVER auto-format the world SD card
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
  Serial.println("          ESP-BEDROCK 0.4.0");
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
