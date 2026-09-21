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
    - RakNet split-frame reassembly
    - Bedrock login/session packet parsing
    - P-384/ECDH/AES-CTR session encryption
    - resource-pack handshake with no bundled packs
    - StartGame/world initialization
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
#include <mbedtls/aes.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/md.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>

#define ESPBEDROCK_VERSION       "0.6.0"
#define ESPBEDROCK_UDP_PORT      19132
#define ESPBEDROCK_HOSTNAME       "esp-bedrock"
#define ESPBEDROCK_MAX_PLAYERS   4
#define RAKNET_PROTOCOL_VERSION  11
#define RAKNET_MAX_MTU           1492
#define RAKNET_SYSTEM_ADDRESS_COUNT 20
#define RAKNET_RELIABLE_CACHE      4
#define RAKNET_SPLIT_SLOT           1480
#define RAKNET_MAX_SPLITS           512
#define RAKNET_MAX_SPLIT_MEMORY      (768 * 1024)
#define BEDROCK_PROTOCOL_VERSION 2193
#define BEDROCK_VERSION_NAME     "1.26.51"
#define BEDROCK_LEVEL_NAME       "ESP World"
#define BEDROCK_REQUEST_NETWORK_SETTINGS_ID 193
#define BEDROCK_NETWORK_SETTINGS_ID 143
#define BEDROCK_GAME_PACKET_ID 0xFE
#define BEDROCK_BATCH_NONE 0xFF
#define BEDROCK_PLAY_STATUS_ID 2
#define BEDROCK_SET_TIME_ID 10
#define BEDROCK_SET_DIFFICULTY_ID 60
#define BEDROCK_REQUEST_CHUNK_RADIUS_ID 69
#define BEDROCK_CHUNK_RADIUS_UPDATED_ID 70
#define BEDROCK_NETWORK_CHUNK_PUBLISHER_UPDATE_ID 121
#define BEDROCK_SERVER_TO_CLIENT_HANDSHAKE_ID 3
#define BEDROCK_CLIENT_TO_SERVER_HANDSHAKE_ID 4
#define BEDROCK_RESOURCE_PACKS_INFO_ID 6
#define BEDROCK_RESOURCE_PACK_STACK_ID 7
#define BEDROCK_RESOURCE_PACK_CLIENT_RESPONSE_ID 8
#define BEDROCK_START_GAME_ID 11
// Stable 1.26.51 uses the pre-1.26.60 enum value for "None".
#define BEDROCK_COMPRESSION_NONE 2
#define SD_MMC_CMD               15  // FNK0047/Freenove fixed pin
#define SD_MMC_CLK               14  // FNK0047/Freenove fixed pin
#define SD_MMC_D0                 2  // FNK0047/Freenove fixed pin
#define SD_MMC_MOUNT_POINT       "/sdcard"
#define SD_MMC_MAX_FILES          5

static const uint8_t RAKNET_MAGIC[16] = {
  0x00, 0xFF, 0xFF, 0x00,
  0xFE, 0xFE, 0xFE, 0xFE,
  0xFD, 0xFD, 0xFD, 0xFD,
  0x12, 0x34, 0x56, 0x78
};

static String u64ToDecimal(uint64_t value) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%llu",
           (unsigned long long)value);
  return String(buffer);
}
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

size_t writeVarUInt64(uint64_t value, uint8_t *out, size_t capacity) {
  size_t i = 0;
  while (true) {
    if (i >= capacity || i >= 10) return 0;
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

bool writeString(const char *value, uint8_t *out, size_t capacity, size_t &written) {
  const size_t len = strlen(value);
  uint8_t prefix[5];
  const size_t prefixLen = writeVarUInt((uint32_t)len, prefix, sizeof(prefix));
  if (prefixLen == 0 || prefixLen + len > capacity) return false;
  memcpy(out, prefix, prefixLen);
  memcpy(out + prefixLen, value, len);
  written = prefixLen + len;
  return true;
}

bool writeString(const String &value, uint8_t *out, size_t capacity, size_t &written) {
  return writeString(value.c_str(), out, capacity, written);
}
}

static const char BASE64_STD[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static String base64Encode(const uint8_t *data, size_t length, bool urlSafe) {
  String out;
  out.reserve(((length + 2) / 3) * 4);
  for (size_t i = 0; i < length; i += 3) {
    const uint32_t v =
      ((uint32_t)data[i] << 16) |
      ((uint32_t)((i + 1 < length) ? data[i + 1] : 0) << 8) |
      (uint32_t)((i + 2 < length) ? data[i + 2] : 0);
    const uint8_t a = (v >> 18) & 0x3F;
    const uint8_t b = (v >> 12) & 0x3F;
    const uint8_t d = v & 0x3F;
    const uint8_t e = (v >> 6) & 0x3F;
    auto enc = [urlSafe](uint8_t ch) -> char {
      if (!urlSafe) return BASE64_STD[ch];
      if (ch == 62) return '-';
      if (ch == 63) return '_';
      return BASE64_STD[ch];
    };
    out += enc(a);
    out += enc(b);
    if (i + 1 < length) out += enc(e);
    if (i + 2 < length) out += enc(d);
  }
  if (!urlSafe) while ((out.length() % 4) != 0) out += '=';
  return out;
}

static int base64Value(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+' || ch == '-') return 62;
  if (ch == '/' || ch == '_') return 63;
  return -1;
}

static bool base64Decode(const String &input, uint8_t *out, size_t capacity, size_t &written) {
  written = 0;
  uint32_t accumulator = 0;
  uint8_t bits = 0;
  for (size_t i = 0; i < input.length(); ++i) {
    const char ch = input[i];
    if (ch == '=') break;
    if (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
    const int v = base64Value(ch);
    if (v < 0) return false;
    accumulator = ((accumulator << 6) | (uint32_t)v) & 0xFFFFFFUL;
    bits += 6;
    while (bits >= 8) {
      bits -= 8;
      if (written >= capacity) return false;
      out[written++] = (uint8_t)((accumulator >> bits) & 0xFF);
    }
  }
  return true;
}

static bool extractJsonStringField(const uint8_t *data,
                                   size_t length,
                                   const char *key,
                                   String &value) {
  const size_t keyLen = strlen(key);
  for (size_t i = 0; i + keyLen <= length; ++i) {
    if (memcmp(data + i, key, keyLen) != 0) continue;
    size_t p = i + keyLen;
    while (p < length && (data[p] == ' ' || data[p] == '\t' ||
                          data[p] == '\r' || data[p] == '\n')) ++p;
    if (p >= length || data[p] != ':') continue;
    ++p;
    while (p < length && (data[p] == ' ' || data[p] == '\t' ||
                          data[p] == '\r' || data[p] == '\n')) ++p;
    if (p < length && data[p] == '\\') ++p;
    if (p >= length || data[p] != '"') continue;
    ++p;

    String result;
    while (p < length) {
      if (data[p] == '\\' && p + 1 < length) {
        const uint8_t next = data[p + 1];
        if (next == '"' || next == '\\' || next == '/') {
          result += (char)next;
          p += 2;
          continue;
        }
      }
      if (data[p] == '"') {
        value = result;
        return true;
      }
      result += (char)data[p++];
      if (result.length() > 1024) return false;
    }
  }
  return false;
}

static bool extractJwtHeaderX5u(const uint8_t *jwt,
                                size_t jwtLength,
                                String &x5u) {
  size_t dot = 0;
  while (dot < jwtLength && jwt[dot] != '.') ++dot;
  if (dot == 0 || dot >= jwtLength) return false;

  String header;
  header.reserve(dot);
  for (size_t i = 0; i < dot; ++i) header += (char)jwt[i];

  uint8_t decoded[2048];
  size_t decodedLength = 0;
  if (!base64Decode(header, decoded, sizeof(decoded), decodedLength)) return false;
  return extractJsonStringField(decoded, decodedLength, "x5u", x5u);
}

static bool findClientPublicKey(const uint8_t *authJwt,
                                size_t authLength,
                                const uint8_t *clientJwt,
                                size_t clientJwtLength,
                                uint8_t *der,
                                size_t derCapacity,
                                size_t &derLength,
                                String &source) {
  String candidate;
  if (extractJwtHeaderX5u(clientJwt, clientJwtLength, candidate) &&
      base64Decode(candidate, der, derCapacity, derLength)) {
    source = "client JWT x5u";
    return true;
  }

  if ((extractJsonStringField(authJwt, authLength, "identityPublicKey", candidate) ||
       extractJsonStringField(authJwt, authLength, "cpk", candidate)) &&
      base64Decode(candidate, der, derCapacity, derLength)) {
    source = "auth JWT public key claim";
    return true;
  }

  return false;
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

class EspBedrockNetworkServer {
public:
  struct ReliableCache {
    bool active = false;
    uint32_t datagramSequence = 0;
    uint16_t length = 0;
    uint32_t lastSent = 0;
    uint8_t data[1200] = {};
  };

  struct SplitAssembly {
    bool active = false;
    uint16_t splitId = 0;
    uint32_t splitCount = 0;
    uint32_t receivedCount = 0;
    size_t totalBytes = 0;
    uint32_t lastUpdate = 0;
    uint8_t *buffer = nullptr;
    uint8_t received[RAKNET_MAX_SPLITS] = {};
  };

  struct Peer {
    bool active = false;
    bool openConnection = false;
    bool established = false;
    bool networkSettingsSent = false;
    bool loginReceived = false;
    bool handshakeSent = false;
    bool encryptionReady = false;
    bool resourcePacksInfoSent = false;
    bool resourcePackStackSent = false;
    bool startGameSent = false;
    bool playerSpawnSent = false;
    bool aesInitialized = false;

    uint64_t clientGuid = 0;
    uint32_t clientProtocol = 0;
    uint32_t loginAuthBytes = 0;
    uint32_t loginClientJwtBytes = 0;

    IPAddress ip;
    uint16_t port = 0;
    uint16_t mtu = 1200;

    uint32_t lastSeen = 0;
    uint32_t datagramsReceived = 0;

    uint32_t nextDatagramSequence = 0;
    uint32_t nextReliableIndex = 0;
    uint32_t nextOrderedIndex = 0;

    uint32_t expectedDatagramSequence = 0;
    bool haveExpectedDatagramSequence = false;

    SplitAssembly split;
    ReliableCache reliable[RAKNET_RELIABLE_CACHE];

    uint16_t nextSplitId = 1;
    uint64_t sendPacketCounter = 0;
    uint64_t receivePacketCounter = 0;
    uint8_t encryptionSalt[16] = {};
    uint8_t sessionKey[32] = {};
    mbedtls_aes_context aesSend;
    mbedtls_aes_context aesReceive;
    size_t aesSendOffset = 0;
    size_t aesReceiveOffset = 0;
    uint8_t aesSendStream[16] = {};
    uint8_t aesReceiveStream[16] = {};
    uint8_t aesSendNonce[16] = {};
    uint8_t aesReceiveNonce[16] = {};
  };

  static constexpr uint8_t RakNetCacheCount() {
    return RAKNET_RELIABLE_CACHE;
  }

  void attachWorld(World &w) {
    world = &w;
  }

  uint32_t encryptedPacketsSent() const { return encryptedPacketsSentCount; }
  uint32_t encryptedPacketsReceived() const { return encryptedPacketsReceivedCount; }
  uint32_t resourcePackResponses() const { return resourcePackResponsesCount; }
  uint32_t startGamePacketsSent() const { return startGamePacketsSentCount; }

  bool begin() {
    ensureCrypto();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[NET] Wi-Fi is not connected. RakNet listener not started.");
      serverRunning = false;
      return false;
    }

    serverRunning = udp.begin(ESPBEDROCK_UDP_PORT) == 1;

    if (serverRunning) {
      serverGuid = ESP.getEfuseMac();

      Serial.printf("[RAKNET] Listening on UDP %u\n",
                    ESPBEDROCK_UDP_PORT);
      Serial.printf("[RAKNET] Server GUID: %s\n",
                    u64ToDecimal(serverGuid).c_str());
      Serial.printf("[RAKNET] Protocol: %u\n",
                    RAKNET_PROTOCOL_VERSION);
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

    // ESP32/WROVER has plenty of room for the maximum Ethernet-sized UDP
    // datagram, but we deliberately hard-cap receive size.
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

      case 0xA0:
        handleAckNack(buffer, n, from, port, false);
        break;

      case 0xC0:
        handleAckNack(buffer, n, from, port, true);
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
  uint32_t ackPackets() const { return ackPacketsCount; }
  uint32_t nackPackets() const { return nackPacketsCount; }
  uint32_t reliableSent() const { return reliableSentCount; }
  uint32_t networkSettingsRequests() const { return networkSettingsRequestsCount; }
  uint32_t loginPackets() const { return loginPacketsCount; }
  uint32_t loginVersionMatches() const { return loginVersionMatchesCount; }

  uint8_t activePeers() const {
    uint8_t count = 0;

    for (const auto &peer : peers) {
      if (peer.active) count++;
    }

    return count;
  }

  uint64_t guid() const { return serverGuid; }

private:
  WiFiUDP udp;
  bool serverRunning = false;
  uint64_t serverGuid = 0;

  uint32_t packetsReceivedCount = 0;
  uint32_t pingsAnsweredCount = 0;
  uint32_t handshakeCount = 0;
  uint32_t connectedDatagramCount = 0;
  uint32_t ackPacketsCount = 0;
  uint32_t nackPacketsCount = 0;
  uint32_t reliableSentCount = 0;
  uint32_t networkSettingsRequestsCount = 0;
  uint32_t loginPacketsCount = 0;
  uint32_t loginVersionMatchesCount = 0;
  uint32_t encryptedPacketsSentCount = 0;
  uint32_t encryptedPacketsReceivedCount = 0;
  uint32_t resourcePackResponsesCount = 0;
  uint32_t startGamePacketsSentCount = 0;

  World *world = nullptr;

  mbedtls_pk_context serverKey;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  bool cryptoInitialized = false;
  uint8_t serverPubKeyDer[256] = {};
  size_t serverPubKeyDerLength = 0;
  String serverPubKeyBase64;

  Peer peers[ESPBEDROCK_MAX_PLAYERS];

  static void writeU16BE(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
  }

  static uint16_t readU16BE(const uint8_t *in) {
    return ((uint16_t)in[0] << 8) | in[1];
  }

  static uint16_t readU16LE(const uint8_t *in) {
    return ((uint16_t)in[0]) | ((uint16_t)in[1] << 8);
  }

  static void writeU16LE(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
  }

  static void writeU32LE(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
  }

  static void writeU32BE(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
  }

  static uint32_t readU32LE(const uint8_t *in) {
    return ((uint32_t)in[0]) |
           ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
  }

  static uint32_t readU32BE(const uint8_t *in) {
    return ((uint32_t)in[0] << 24) |
           ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) |
           ((uint32_t)in[3]);
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
    return offset + 16 <= length &&
           memcmp(data + offset, RAKNET_MAGIC, 16) == 0;
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

  void handleUnconnectedPing(const uint8_t *data,
                              size_t length,
                              const IPAddress &ip,
                              uint16_t port) {
    if (length < 25) return;
    if (!magicOK(data, 9, length)) return;

    const uint64_t pingTime = readU64BE(data + 1);

    uint8_t response[512];
    size_t offset = 0;

    response[offset++] = 0x1C;
    writeU64BE(response + offset, pingTime);
    offset += 8;
    writeU64BE(response + offset, serverGuid);
    offset += 8;
    memcpy(response + offset, RAKNET_MAGIC, sizeof(RAKNET_MAGIC));
    offset += sizeof(RAKNET_MAGIC);

    const String motd =
      "MCPE;ESP-Bedrock;" +
      String(BEDROCK_PROTOCOL_VERSION) + ";" +
      BEDROCK_VERSION_NAME + ";" +
      String((unsigned)activePeers()) + ";" +
      String(ESPBEDROCK_MAX_PLAYERS) + ";" +
      u64ToDecimal(serverGuid) + ";" +
      BEDROCK_LEVEL_NAME + ";Survival;1;19132;19133;";

    const size_t motdLength = motd.length();

    if (motdLength > 480 || offset + 2 + motdLength > sizeof(response)) {
      return;
    }

    writeU16BE(response + offset, (uint16_t)motdLength);
    offset += 2;

    memcpy(response + offset, motd.c_str(), motdLength);
    offset += motdLength;

    sendPacket(response, offset, ip, port);
    pingsAnsweredCount++;
  }

  void handleOpenConnectionRequest1(const uint8_t *data,
                                    size_t length,
                                    const IPAddress &ip,
                                    uint16_t port) {
    if (length < 18) return;
    if (!magicOK(data, 1, length)) return;

    const uint8_t clientRakNetVersion = data[17];

    if (clientRakNetVersion != RAKNET_PROTOCOL_VERSION) {
      uint8_t response[19];
      size_t offset = 0;

      response[offset++] = 0x19;
      response[offset++] = RAKNET_PROTOCOL_VERSION;
      memcpy(response + offset, RAKNET_MAGIC, sizeof(RAKNET_MAGIC));
      offset += sizeof(RAKNET_MAGIC);

      sendPacket(response, offset, ip, port);
      Serial.printf(
        "[RAKNET] Incompatible protocol %u from %s:%u\n",
        (unsigned)clientRakNetVersion,
        ip.toString().c_str(),
        (unsigned)port
      );
      return;
    }

    size_t discoveredMtuValue = length + 28U;
    if (discoveredMtuValue > (size_t)RAKNET_MAX_MTU) {
      discoveredMtuValue = RAKNET_MAX_MTU;
    }
    const uint16_t discoveredMtu =
      (uint16_t)discoveredMtuValue;

    uint8_t response[32];
    size_t offset = 0;

    response[offset++] = 0x06;
    memcpy(response + offset, RAKNET_MAGIC, sizeof(RAKNET_MAGIC));
    offset += sizeof(RAKNET_MAGIC);
    writeU64BE(response + offset, serverGuid);
    offset += 8;

    // RakNet security is disabled at this layer. Bedrock session encryption
    // is negotiated later through the Bedrock login handshake.
    response[offset++] = 0;
    writeU16BE(response + offset, discoveredMtu);
    offset += 2;

    sendPacket(response, offset, ip, port);
    handshakeCount++;
  }

  void handleOpenConnectionRequest2(const uint8_t *data,
                                    size_t length,
                                    const IPAddress &ip,
                                    uint16_t port) {
    // With server-level RakNet security disabled, Request2 is:
    // id + magic + server address + mtu + client GUID.
    if (length < 34) return;
    if (!magicOK(data, 1, length)) return;

    const uint16_t requestedMtu = readU16BE(data + 24);
    const uint64_t clientGuid = readU64BE(data + 26);

    const uint16_t mtu =
      (uint16_t)constrain(
        (int)requestedMtu,
        576,
        RAKNET_MAX_MTU
      );

    Peer *peer = allocatePeer(ip, port, clientGuid);

    if (!peer) {
      Serial.println("[RAKNET] Peer table full; rejecting Request2.");
      return;
    }

    peer->mtu = mtu;
    peer->openConnection = true;
    peer->lastSeen = millis();

    uint8_t response[40];
    size_t offset = 0;

    response[offset++] = 0x08;
    memcpy(response + offset, RAKNET_MAGIC, sizeof(RAKNET_MAGIC));
    offset += sizeof(RAKNET_MAGIC);

    writeU64BE(response + offset, serverGuid);
    offset += 8;

    writeRakAddress(response + offset, ip, port);
    offset += 7;

    writeU16BE(response + offset, mtu);
    offset += 2;

    response[offset++] = 0;

    sendPacket(response, offset, ip, port);
    handshakeCount++;

    Serial.printf(
      "[RAKNET] OpenConnectionRequest2 guid=%s mtu=%u from %s:%u\n",
      u64ToDecimal(clientGuid).c_str(),
      (unsigned)mtu,
      ip.toString().c_str(),
      (unsigned)port
    );
  }

  Peer *findPeer(const IPAddress &ip, uint16_t port, uint64_t guid = 0) {
    for (auto &peer : peers) {
      if (!peer.active) continue;

      if (guid != 0 && peer.clientGuid == guid) {
        return &peer;
      }

      if (peer.ip == ip && peer.port == port) {
        return &peer;
      }
    }

    return nullptr;
  }

  static void clearSplitAssembly(SplitAssembly &split) {
    if (split.buffer) {
      free(split.buffer);
    }
    split = SplitAssembly{};
  }

  void clearPeerCrypto(Peer &peer) {
    if (peer.aesInitialized) {
      mbedtls_aes_free(&peer.aesSend);
      mbedtls_aes_free(&peer.aesReceive);
      peer.aesInitialized = false;
    }
  }

  void resetPeer(Peer &peer) {
    clearSplitAssembly(peer.split);
    clearPeerCrypto(peer);
    peer = Peer{};
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
      if (!peer.active) continue;

      if (now - peer.lastSeen > 120000UL) {
        resetPeer(peer);
      }

      if (peer.split.active &&
          now - peer.split.lastUpdate > 10000UL) {
        clearSplitAssembly(peer.split);
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

  void sendAck(IPAddress ip, uint16_t port, uint32_t sequence) {
    uint8_t response[8];

    response[0] = 0xC0;
    writeU16BE(response + 1, 1);
    response[3] = 1;
    writeTriadLE(response + 4, sequence);

    sendPacket(response, 7, ip, port);
  }

  void sendFrameSet(Peer &peer,
                    uint8_t reliability,
                    const uint8_t *payload,
                    size_t payloadLength,
                    bool cacheForRetransmit) {
    if (payloadLength == 0 || payloadLength > 1150) return;

    uint8_t response[1200];
    size_t offset = 0;

    const uint32_t datagramSequence = peer.nextDatagramSequence++;

    response[offset++] = 0x80;
    writeTriadLE(response + offset, datagramSequence);
    offset += 3;

    // Reliable ordered frame on channel 0.
    const uint8_t flags =
      (uint8_t)((reliability & 0x07) << 5);

    response[offset++] = flags;

    const uint16_t bitLength = (uint16_t)(payloadLength * 8);
    writeU16BE(response + offset, bitLength);
    offset += 2;

    if (reliability == 2 || reliability == 3 ||
        reliability == 4 || reliability == 6 ||
        reliability == 7) {
      writeTriadLE(response + offset, peer.nextReliableIndex++);
      offset += 3;
    }

    if (reliability == 1 || reliability == 4) {
      writeTriadLE(response + offset, 0);
      offset += 3;
    }

    if (reliability == 1 || reliability == 3 ||
        reliability == 4 || reliability == 7) {
      writeTriadLE(response + offset, peer.nextOrderedIndex++);
      offset += 3;
      response[offset++] = 0;
    }

    memcpy(response + offset, payload, payloadLength);
    offset += payloadLength;

    sendPacket(response, offset, peer.ip, peer.port);

    if (cacheForRetransmit) {
      const uint8_t slot = datagramSequence % RAKNET_RELIABLE_CACHE;
      ReliableCache &cache = peer.reliable[slot];

      cache.active = true;
      cache.datagramSequence = datagramSequence;
      cache.length = (uint16_t)offset;
      cache.lastSent = millis();
      memcpy(cache.data, response, offset);
    }

    reliableSentCount++;
  }

  void sendReliableOrdered(Peer &peer,
                           const uint8_t *payload,
                           size_t length) {
    constexpr size_t MAX_FRAME_PAYLOAD = 1050;
    if (length <= MAX_FRAME_PAYLOAD) {
      sendFrameSet(peer, 3, payload, length, true);
      return;
    }

    const size_t splitCount =
      (length + MAX_FRAME_PAYLOAD - 1) / MAX_FRAME_PAYLOAD;

    if (splitCount > RAKNET_MAX_SPLITS) {
      Serial.printf("[RAKNET] Outbound payload too large: %u frames.\n",
                    (unsigned)splitCount);
      return;
    }

    const uint16_t splitId = peer.nextSplitId++;
    for (size_t index = 0; index < splitCount; ++index) {
      const size_t payloadOffset = index * MAX_FRAME_PAYLOAD;
      const size_t chunk =
        (length - payloadOffset > MAX_FRAME_PAYLOAD)
          ? MAX_FRAME_PAYLOAD
          : (length - payloadOffset);

      uint8_t frame[1200];
      size_t frameOffset = 0;
      const uint32_t datagramSequence = peer.nextDatagramSequence++;

      frame[frameOffset++] = 0x80;
      writeTriadLE(frame + frameOffset, datagramSequence);
      frameOffset += 3;

      frame[frameOffset++] = (uint8_t)((3U << 5) | 0x10U);
      writeU16BE(frame + frameOffset, (uint16_t)(chunk * 8U));
      frameOffset += 2;

      writeTriadLE(frame + frameOffset, peer.nextReliableIndex++);
      frameOffset += 3;
      writeTriadLE(frame + frameOffset, peer.nextOrderedIndex);
      frameOffset += 3;
      frame[frameOffset++] = 0;

      writeU32LE(frame + frameOffset, (uint32_t)splitCount);
      frameOffset += 4;
      writeU16LE(frame + frameOffset, splitId);
      frameOffset += 2;
      writeU32LE(frame + frameOffset, (uint32_t)index);
      frameOffset += 4;

      memcpy(frame + frameOffset, payload + payloadOffset, chunk);
      frameOffset += chunk;

      sendPacket(frame, frameOffset, peer.ip, peer.port);

      const uint8_t slot = datagramSequence % RAKNET_RELIABLE_CACHE;
      ReliableCache &cache = peer.reliable[slot];
      cache.active = true;
      cache.datagramSequence = datagramSequence;
      cache.length = (uint16_t)frameOffset;
      cache.lastSent = millis();
      memcpy(cache.data, frame, frameOffset);

      reliableSentCount++;
    }

    peer.nextOrderedIndex++;
  }

  void retransmit(Peer &peer, uint32_t sequence) {
    for (auto &cache : peer.reliable) {
      if (!cache.active) continue;

      if (cache.datagramSequence == sequence) {
        sendPacket(cache.data, cache.length, peer.ip, peer.port);
        cache.lastSent = millis();
        return;
      }
    }
  }

  void handleAckNack(const uint8_t *data,
                     size_t length,
                     IPAddress ip,
                     uint16_t port,
                     bool isAck) {
    if (length < 4) return;

    Peer *peer = findPeer(ip, port);

    if (!peer) return;

    const uint16_t recordCount = readU16BE(data + 1);
    size_t offset = 3;

    for (uint16_t i = 0; i < recordCount; ++i) {
      if (offset + 4 > length) return;

      const bool single = data[offset++] != 0;

      if (single) {
        const uint32_t sequence = readTriadLE(data + offset);
        offset += 3;

        if (isAck) {
          for (auto &cache : peer->reliable) {
            if (cache.active &&
                cache.datagramSequence == sequence) {
              cache.active = false;
            }
          }
        } else {
          retransmit(*peer, sequence);
        }
      } else {
        if (offset + 6 > length) return;

        const uint32_t start = readTriadLE(data + offset);
        offset += 3;

        const uint32_t end = readTriadLE(data + offset);
        offset += 3;

        for (uint32_t sequence = start; sequence <= end; ++sequence) {
          if (isAck) {
            for (auto &cache : peer->reliable) {
              if (cache.active &&
                  cache.datagramSequence == sequence) {
                cache.active = false;
              }
            }
          } else {
            retransmit(*peer, sequence);
          }

          if (sequence == 0xFFFFFFUL) break;
        }
      }
    }

    if (isAck) {
      ackPacketsCount++;
    } else {
      nackPacketsCount++;
    }
  }

  void sendConnectionRequestAccepted(Peer &peer, uint64_t pingTime) {
    uint8_t payload[180];
    size_t offset = 0;

    payload[offset++] = 0x10;

    writeRakAddress(payload + offset, peer.ip, peer.port);
    offset += 7;

    writeU16BE(payload + offset, 0);
    offset += 2;

    // MCPE/RakLib uses 20 internal addresses.
    for (uint8_t i = 0; i < RAKNET_SYSTEM_ADDRESS_COUNT; ++i) {
      IPAddress dummy(0, 0, 0, 0);
      writeRakAddress(payload + offset, dummy, 0);
      offset += 7;
    }

    writeU64BE(payload + offset, pingTime);
    offset += 8;

    writeU64BE(payload + offset, millis());
    offset += 8;

    sendReliableOrdered(peer, payload, offset);
  }

  void sendNetworkSettings(Peer &peer) {
    uint8_t payload[32];
    size_t offset = 0;

    // NetworkSettingsPacket = varuint packet id + fields.
    uint8_t packetId[5];
    const size_t packetIdLength =
      BedrockProtocol::writeVarUInt(
        BEDROCK_NETWORK_SETTINGS_ID,
        packetId,
        sizeof(packetId)
      );

    memcpy(payload + offset, packetId, packetIdLength);
    offset += packetIdLength;

    // For 1.26.51, None = 2. NetworkSettings itself is not batch-wrapped.
    writeU16LE(payload + offset, 0);
    offset += 2;

    writeU16LE(payload + offset, BEDROCK_COMPRESSION_NONE);
    offset += 2;

    payload[offset++] = 0;
    payload[offset++] = 0;
    float throttleScalar = 1.0f;
    memcpy(payload + offset, &throttleScalar, sizeof(throttleScalar));
    offset += sizeof(throttleScalar);

    uint8_t framed[40] = {};
    size_t framedOffset = 0;
    framed[framedOffset++] = BEDROCK_GAME_PACKET_ID;

    uint8_t lengthBytes[5] = {};
    const size_t lengthBytesCount =
      BedrockProtocol::writeVarUInt(
        (uint32_t)offset,
        lengthBytes,
        sizeof(lengthBytes)
      );

    if (lengthBytesCount == 0 ||
        framedOffset + lengthBytesCount + offset > sizeof(framed)) {
      return;
    }

    memcpy(framed + framedOffset, lengthBytes, lengthBytesCount);
    framedOffset += lengthBytesCount;
    memcpy(framed + framedOffset, payload, offset);
    framedOffset += offset;

    sendReliableOrdered(peer, framed, framedOffset);
    peer.networkSettingsSent = true;
    networkSettingsRequestsCount++;
  }


  bool ensureCrypto() {
    if (cryptoInitialized) return true;

    mbedtls_pk_init(&serverKey);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    const uint8_t personalization[] = "ESP-Bedrock";
    const int seedRc = mbedtls_ctr_drbg_seed(
      &drbg,
      mbedtls_entropy_func,
      &entropy,
      personalization,
      sizeof(personalization) - 1
    );
    if (seedRc != 0) {
      Serial.printf("[CRYPTO] CTR-DRBG init failed: %d\n", seedRc);
      return false;
    }

    const int setupRc = mbedtls_pk_setup(
      &serverKey,
      mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)
    );
    if (setupRc != 0) {
      Serial.printf("[CRYPTO] EC key setup failed: %d\n", setupRc);
      return false;
    }

    const int keyRc = mbedtls_ecp_gen_key(
      MBEDTLS_ECP_DP_SECP384R1,
      mbedtls_pk_ec(serverKey),
      mbedtls_ctr_drbg_random,
      &drbg
    );
    if (keyRc != 0) {
      Serial.printf("[CRYPTO] P-384 key generation failed: %d\n", keyRc);
      return false;
    }

    const int derRc = mbedtls_pk_write_pubkey_der(
      &serverKey,
      serverPubKeyDer,
      sizeof(serverPubKeyDer)
    );
    if (derRc <= 0) {
      Serial.printf("[CRYPTO] Public-key DER export failed: %d\n", derRc);
      return false;
    }

    serverPubKeyDerLength = (size_t)derRc;
    memmove(
      serverPubKeyDer,
      serverPubKeyDer + sizeof(serverPubKeyDer) - serverPubKeyDerLength,
      serverPubKeyDerLength
    );

    serverPubKeyBase64 = base64Encode(
      serverPubKeyDer,
      serverPubKeyDerLength,
      false
    );

    cryptoInitialized = true;
    Serial.println("[CRYPTO] P-384 server key ready.");
    return true;
  }

  bool derivePeerKey(Peer &peer,
                     const uint8_t *clientDer,
                     size_t clientDerLength) {
    if (!cryptoInitialized) return false;

    mbedtls_pk_context clientKey;
    mbedtls_pk_init(&clientKey);

    const int parseRc = mbedtls_pk_parse_public_key(
      &clientKey, clientDer, clientDerLength
    );
    if (parseRc != 0 ||
        mbedtls_pk_get_type(&clientKey) != MBEDTLS_PK_ECKEY) {
      Serial.printf("[CRYPTO] Client public-key parse failed: %d\n", parseRc);
      mbedtls_pk_free(&clientKey);
      return false;
    }

    mbedtls_ecdh_context ecdh;
    mbedtls_ecdh_init(&ecdh);

    int rc = mbedtls_ecdh_get_params(
      &ecdh,
      mbedtls_pk_ec(serverKey),
      MBEDTLS_ECDH_OURS
    );
    if (rc != 0) {
      Serial.printf("[CRYPTO] ECDH server-key import failed: %d\n", rc);
      mbedtls_ecdh_free(&ecdh);
      mbedtls_pk_free(&clientKey);
      return false;
    }

    rc = mbedtls_ecdh_get_params(
      &ecdh,
      mbedtls_pk_ec(clientKey),
      MBEDTLS_ECDH_THEIRS
    );
    if (rc != 0) {
      Serial.printf("[CRYPTO] ECDH client-key import failed: %d\n", rc);
      mbedtls_ecdh_free(&ecdh);
      mbedtls_pk_free(&clientKey);
      return false;
    }

    uint8_t sharedSecret[64] = {};
    size_t sharedLength = 0;

    rc = mbedtls_ecdh_calc_secret(
      &ecdh,
      &sharedLength,
      sharedSecret,
      sizeof(sharedSecret),
      mbedtls_ctr_drbg_random,
      &drbg
    );

    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&clientKey);

    if (rc != 0 || sharedLength == 0 || sharedLength > sizeof(sharedSecret)) {
      Serial.printf("[CRYPTO] ECDH secret calculation failed: %d\n", rc);
      memset(sharedSecret, 0, sizeof(sharedSecret));
      return false;
    }

    uint8_t keyInput[16 + sizeof(sharedSecret)] = {};
    memcpy(keyInput, peer.encryptionSalt, 16);
    memcpy(keyInput + 16, sharedSecret, sharedLength);

    const mbedtls_md_info_t *sha256Info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (sha256Info == nullptr) {
      memset(sharedSecret, 0, sizeof(sharedSecret));
      memset(keyInput, 0, sizeof(keyInput));
      return false;
    }

    const int hashRc = mbedtls_md(
      sha256Info,
      keyInput,
      16 + sharedLength,
      peer.sessionKey
    );

    memset(sharedSecret, 0, sizeof(sharedSecret));
    memset(keyInput, 0, sizeof(keyInput));

    if (hashRc != 0) {
      Serial.printf("[CRYPTO] Session-key SHA-256 failed: %d\n", hashRc);
      return false;
    }

    mbedtls_aes_init(&peer.aesSend);
    mbedtls_aes_init(&peer.aesReceive);

    if (mbedtls_aes_setkey_enc(
          &peer.aesSend, peer.sessionKey, 256) != 0 ||
        mbedtls_aes_setkey_enc(
          &peer.aesReceive, peer.sessionKey, 256) != 0) {
      mbedtls_aes_free(&peer.aesSend);
      mbedtls_aes_free(&peer.aesReceive);
      Serial.println("[CRYPTO] AES-256 setup failed.");
      return false;
    }

    memset(peer.aesSendNonce, 0, sizeof(peer.aesSendNonce));
    memset(peer.aesReceiveNonce, 0, sizeof(peer.aesReceiveNonce));
    memcpy(peer.aesSendNonce, peer.sessionKey, 12);
    memcpy(peer.aesReceiveNonce, peer.sessionKey, 12);
    peer.aesSendNonce[15] = 2;
    peer.aesReceiveNonce[15] = 2;

    peer.aesSendOffset = 0;
    peer.aesReceiveOffset = 0;
    peer.sendPacketCounter = 0;
    peer.receivePacketCounter = 0;
    memset(peer.aesSendStream, 0, sizeof(peer.aesSendStream));
    memset(peer.aesReceiveStream, 0, sizeof(peer.aesReceiveStream));
    peer.aesInitialized = true;
    peer.encryptionReady = true;

    return true;
  }

  bool signHandshakeInput(const String &signingInput,
                          uint8_t rawSignature[96]) {
    uint8_t hash[48] = {};
    const mbedtls_md_info_t *sha384Info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA384);
    if (sha384Info == nullptr) return false;

    if (mbedtls_md(
          sha384Info,
          (const unsigned char *)signingInput.c_str(),
          signingInput.length(),
          hash
        ) != 0) {
      return false;
    }

    uint8_t derSignature[160] = {};
    size_t derLength = 0;
    const int signRc = mbedtls_pk_sign(
      &serverKey,
      MBEDTLS_MD_SHA384,
      hash,
      sizeof(hash),
      derSignature,
      sizeof(derSignature),
      &derLength,
      mbedtls_ctr_drbg_random,
      &drbg
    );

    if (signRc != 0 || derLength < 8) {
      Serial.printf("[CRYPTO] ES384 signing failed: %d\n", signRc);
      return false;
    }

    size_t p = 0;
    if (derSignature[p++] != 0x30 || p >= derLength) return false;

    size_t sequenceLength = 0;
    uint8_t seqLenByte = derSignature[p++];
    if ((seqLenByte & 0x80U) != 0) {
      const uint8_t count = seqLenByte & 0x7FU;
      if (count == 0 || count > 2 || p + count > derLength) return false;
      for (uint8_t i = 0; i < count; ++i) {
        sequenceLength = (sequenceLength << 8) | derSignature[p++];
      }
    } else {
      sequenceLength = seqLenByte;
    }

    if (sequenceLength > derLength - p) return false;
    const size_t sequenceEnd = p + sequenceLength;

    auto readInteger = [&](uint8_t *out) -> bool {
      if (p + 2 > sequenceEnd || derSignature[p++] != 0x02) return false;
      size_t n = derSignature[p++];
      if ((n & 0x80U) != 0) {
        const uint8_t count = n & 0x7FU;
        if (count == 0 || count > 2 || p + count > sequenceEnd) return false;
        n = 0;
        for (uint8_t i = 0; i < count; ++i) {
          n = (n << 8) | derSignature[p++];
        }
      }
      if (n == 0 || n > sequenceEnd - p) return false;

      while (n > 1 && derSignature[p] == 0x00) {
        ++p;
        --n;
      }
      if (n > 48) return false;

      memset(out, 0, 48);
      memcpy(out + (48 - n), derSignature + p, n);
      p += n;
      return true;
    };

    memset(rawSignature, 0, 96);
    if (!readInteger(rawSignature) ||
        !readInteger(rawSignature + 48)) {
      return false;
    }

    return p == sequenceEnd;
  }

  bool createHandshakeJwt(const uint8_t *token,
                          size_t tokenLength,
                          String &jwt) {
    if (!ensureCrypto()) return false;

    const String headerJson =
      String("{\"alg\":\"ES384\",\"x5u\":\"") +
      serverPubKeyBase64 +
      "\"}";

    const String payloadJson =
      String("{\"salt\":\"") +
      base64Encode(token, tokenLength, false) +
      "\"}";

    const String headerB64 =
      base64Encode(
        (const uint8_t *)headerJson.c_str(),
        headerJson.length(),
        true
      );

    const String payloadB64 =
      base64Encode(
        (const uint8_t *)payloadJson.c_str(),
        payloadJson.length(),
        true
      );

    const String signingInput = headerB64 + "." + payloadB64;
    uint8_t rawSignature[96] = {};

    if (!signHandshakeInput(signingInput, rawSignature)) return false;

    jwt = signingInput + "." +
          base64Encode(rawSignature, sizeof(rawSignature), true);
    return true;
  }

  bool encryptBytes(Peer &peer, uint8_t *data, size_t length) {
    if (!peer.encryptionReady || !peer.aesInitialized) return false;

    size_t ncOffset = peer.aesSendOffset;
    const int rc = mbedtls_aes_crypt_ctr(
      &peer.aesSend,
      length,
      &ncOffset,
      peer.aesSendNonce,
      peer.aesSendStream,
      data,
      data
    );
    if (rc != 0) return false;

    peer.aesSendOffset = ncOffset;
    return true;
  }

  bool decryptBytes(Peer &peer, uint8_t *data, size_t length) {
    if (!peer.encryptionReady || !peer.aesInitialized) return false;

    size_t ncOffset = peer.aesReceiveOffset;
    const int rc = mbedtls_aes_crypt_ctr(
      &peer.aesReceive,
      length,
      &ncOffset,
      peer.aesReceiveNonce,
      peer.aesReceiveStream,
      data,
      data
    );
    if (rc != 0) return false;

    peer.aesReceiveOffset = ncOffset;
    return true;
  }

  bool addEncryptionTrailer(Peer &peer,
                            const uint8_t *plaintext,
                            size_t plaintextLength,
                            uint8_t trailer[8]) {
    const size_t inputLength =
      8 + plaintextLength + sizeof(peer.sessionKey);

    uint8_t *input = (uint8_t *)malloc(inputLength);
    if (!input) return false;

    for (uint8_t i = 0; i < 8; ++i) {
      input[i] = (uint8_t)(peer.sendPacketCounter >> (8U * i));
    }

    memcpy(input + 8, plaintext, plaintextLength);
    memcpy(
      input + 8 + plaintextLength,
      peer.sessionKey,
      sizeof(peer.sessionKey)
    );

    uint8_t digest[32] = {};
    const int rc = mbedtls_md(
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
      input,
      inputLength,
      digest
    );
    free(input);

    if (rc != 0) return false;

    memcpy(trailer, digest, 8);
    peer.sendPacketCounter++;
    return true;
  }

  bool validateEncryptionTrailer(Peer &peer,
                                 const uint8_t *plaintext,
                                 size_t plaintextLength,
                                 const uint8_t trailer[8]) {
    const size_t inputLength =
      8 + plaintextLength + sizeof(peer.sessionKey);

    uint8_t *input = (uint8_t *)malloc(inputLength);
    if (!input) return false;

    for (uint8_t i = 0; i < 8; ++i) {
      input[i] = (uint8_t)(peer.receivePacketCounter >> (8U * i));
    }

    memcpy(input + 8, plaintext, plaintextLength);
    memcpy(
      input + 8 + plaintextLength,
      peer.sessionKey,
      sizeof(peer.sessionKey)
    );

    uint8_t digest[32] = {};
    const int rc = mbedtls_md(
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
      input,
      inputLength,
      digest
    );
    free(input);

    if (rc != 0) return false;

    if (memcmp(digest, trailer, 8) != 0) return false;

    peer.receivePacketCounter++;
    return true;
  }

  bool sendEncryptedBedrockPacket(Peer &peer,
                                  const uint8_t *packet,
                                  size_t packetLength) {
    if (!peer.encryptionReady || packetLength == 0) return false;

    uint8_t batch[2048] = {};
    size_t offset = 0;
    batch[offset++] = BEDROCK_BATCH_NONE;

    uint8_t lengthBytes[5] = {};
    const size_t lengthBytesCount =
      BedrockProtocol::writeVarUInt(
        (uint32_t)packetLength,
        lengthBytes,
        sizeof(lengthBytes)
      );

    if (lengthBytesCount == 0 ||
        offset + lengthBytesCount + packetLength + 8 > sizeof(batch)) {
      return false;
    }

    memcpy(batch + offset, lengthBytes, lengthBytesCount);
    offset += lengthBytesCount;
    memcpy(batch + offset, packet, packetLength);
    offset += packetLength;

    uint8_t trailer[8] = {};
    if (!addEncryptionTrailer(
          peer, batch, offset, trailer)) return false;

    memcpy(batch + offset, trailer, sizeof(trailer));
    offset += sizeof(trailer);

    if (!encryptBytes(peer, batch, offset)) return false;

    uint8_t framed[2060] = {};
    framed[0] = BEDROCK_GAME_PACKET_ID;
    memcpy(framed + 1, batch, offset);

    sendReliableOrdered(peer, framed, offset + 1);
    encryptedPacketsSentCount++;
    return true;
  }

  bool sendRawBedrockPacket(Peer &peer,
                            const uint8_t *packet,
                            size_t packetLength) {
    if (packetLength == 0 || packetLength > 1800) return false;

    uint8_t framed[1900] = {};
    size_t offset = 0;
    framed[offset++] = BEDROCK_GAME_PACKET_ID;
    framed[offset++] = BEDROCK_BATCH_NONE;

    uint8_t lengthBytes[5] = {};
    const size_t lengthBytesCount =
      BedrockProtocol::writeVarUInt(
        (uint32_t)packetLength,
        lengthBytes,
        sizeof(lengthBytes)
      );
    if (lengthBytesCount == 0 ||
        offset + lengthBytesCount + packetLength > sizeof(framed)) {
      return false;
    }

    memcpy(framed + offset, lengthBytes, lengthBytesCount);
    offset += lengthBytesCount;
    memcpy(framed + offset, packet, packetLength);
    offset += packetLength;

    sendReliableOrdered(peer, framed, offset);
    return true;
  }

  bool sendPlayStatus(Peer &peer, uint32_t status) {
    uint8_t packet[8] = {};
    size_t offset = 0;

    const size_t packetIdLength =
      BedrockProtocol::writeVarUInt(
        BEDROCK_PLAY_STATUS_ID,
        packet + offset,
        sizeof(packet) - offset
      );
    if (packetIdLength == 0) return false;
    offset += packetIdLength;

    if (offset + 4 > sizeof(packet)) return false;
    writeU32BE(packet + offset, status);
    offset += 4;

    return sendEncryptedBedrockPacket(peer, packet, offset);
  }

  bool sendResourcePacksInfo(Peer &peer) {
    uint8_t packet[64] = {};
    size_t offset = 0;

    const size_t idLength =
      BedrockProtocol::writeVarUInt(
        BEDROCK_RESOURCE_PACKS_INFO_ID,
        packet + offset,
        sizeof(packet) - offset
      );
    if (idLength == 0) return false;
    offset += idLength;

    packet[offset++] = 0; // forcedToAccept
    packet[offset++] = 0; // hasAddonPacks
    packet[offset++] = 0; // scriptingEnabled
    packet[offset++] = 0; // vibrantVisualsForceDisabled

    memset(packet + offset, 0, 16);
    offset += 16;

    size_t written = 0;
    if (!BedrockProtocol::writeString(
          "",
          packet + offset,
          sizeof(packet) - offset,
          written)) return false;
    offset += written;

    uint8_t countBytes[5] = {};
    const size_t countLength =
      BedrockProtocol::writeVarUInt(0, countBytes, sizeof(countBytes));
    if (countLength == 0 || offset + countLength > sizeof(packet)) return false;
    memcpy(packet + offset, countBytes, countLength);
    offset += countLength;

    if (!sendEncryptedBedrockPacket(peer, packet, offset)) return false;

    peer.resourcePacksInfoSent = true;
    return true;
  }

  bool sendResourcePackStack(Peer &peer) {
    uint8_t packet[128] = {};
    size_t offset = 0;

    const size_t idLength =
      BedrockProtocol::writeVarUInt(
        BEDROCK_RESOURCE_PACK_STACK_ID,
        packet + offset,
        sizeof(packet) - offset
      );
    if (idLength == 0) return false;
    offset += idLength;

    packet[offset++] = 0; // forcedToAccept

    uint8_t countBytes[5] = {};
    const size_t countLength =
      BedrockProtocol::writeVarUInt(0, countBytes, sizeof(countBytes));
    if (countLength == 0 || offset + countLength > sizeof(packet)) return false;
    memcpy(packet + offset, countBytes, countLength);
    offset += countLength;

    size_t written = 0;
    if (!BedrockProtocol::writeString(
          BEDROCK_VERSION_NAME,
          packet + offset,
          sizeof(packet) - offset,
          written)) return false;
    offset += written;

    // Empty experiments: int32 LE count, then experimentsPreviouslyToggled and hasEditorPacks.
    writeU32LE(packet + offset, 0);
    offset += 4;
    packet[offset++] = 0;
    packet[offset++] = 0;

    if (!sendEncryptedBedrockPacket(peer, packet, offset)) return false;

    peer.resourcePackStackSent = true;
    return true;
  }

  bool sendStartGame(Peer &peer) {
    if (!world || peer.startGameSent) return false;

    uint8_t packet[1800] = {};
    size_t offset = 0;

    auto putVarUInt = [&](uint32_t v) -> bool {
      uint8_t tmp[5];
      const size_t n = BedrockProtocol::writeVarUInt(v, tmp, sizeof(tmp));
      if (n == 0 || offset + n > sizeof(packet)) return false;
      memcpy(packet + offset, tmp, n);
      offset += n;
      return true;
    };
    auto putVarInt = [&](int32_t v) -> bool { return putVarUInt((uint32_t)v); };
    auto putVarLong = [&](int64_t v) -> bool {
      uint8_t tmp[10];
      const size_t n = BedrockProtocol::writeVarUInt64((uint64_t)v, tmp, sizeof(tmp));
      if (n == 0 || offset + n > sizeof(packet)) return false;
      memcpy(packet + offset, tmp, n);
      offset += n;
      return true;
    };
    auto putU64LE = [&](uint64_t v) -> bool {
      if (offset + 8 > sizeof(packet)) return false;
      for (uint8_t i = 0; i < 8; ++i) packet[offset++] = (uint8_t)(v >> (8U * i));
      return true;
    };
    auto putU16LE = [&](uint16_t v) -> bool {
      if (offset + 2 > sizeof(packet)) return false;
      writeU16LE(packet + offset, v);
      offset += 2;
      return true;
    };
    auto putI32LE = [&](int32_t v) -> bool {
      if (offset + 4 > sizeof(packet)) return false;
      writeU32LE(packet + offset, (uint32_t)v);
      offset += 4;
      return true;
    };
    auto putBool = [&](bool v) -> bool {
      if (offset + 1 > sizeof(packet)) return false;
      packet[offset++] = v ? 1 : 0;
      return true;
    };
    auto putFloatLE = [&](float v) -> bool {
      if (offset + 4 > sizeof(packet)) return false;
      memcpy(packet + offset, &v, sizeof(v));
      offset += sizeof(v);
      return true;
    };
    auto putString = [&](const String &s) -> bool {
      size_t written = 0;
      if (!BedrockProtocol::writeString(
            s, packet + offset, sizeof(packet) - offset, written)) return false;
      offset += written;
      return true;
    };

    const int spawnX = 8;
    const int spawnZ = 8;
    const int spawnY = world->terrainHeight(spawnX, spawnZ) + 1;

    if (!putVarUInt(BEDROCK_START_GAME_ID) ||
        !putVarLong(1) || !putVarLong(1) || !putVarInt(0) ||
        !putFloatLE(spawnX + 0.5f) ||
        !putFloatLE(spawnY + 0.62f) ||
        !putFloatLE(spawnZ + 0.5f) ||
        !putFloatLE(0.0f) || !putFloatLE(0.0f)) return false;

    // LevelSettings through protocol 2193.
    if (!putU64LE(world->seed()) || !putU16LE(0) ||
        !putString("") || !putVarInt(0) || !putVarInt(1) ||
        !putVarInt(0) || !putBool(false) || !putVarInt(1) ||
        !putVarInt(spawnX) || !putVarInt(spawnY) || !putVarInt(spawnZ) ||
        !putBool(true) || !putVarInt(0) || !putBool(false) || !putBool(false) ||
        !putVarInt(-1) || !putVarInt(0) || !putBool(false) || !putString("") ||
        !putFloatLE(0.0f) || !putFloatLE(0.0f) || !putBool(false) ||
        !putBool(true) || !putBool(true) || !putVarInt(0) || !putVarInt(0) ||
        !putBool(true) || !putBool(false) || !putVarUInt(0) || !putVarUInt(0) ||
        !putBool(false) || !putBool(false) || !putVarInt(1) || !putI32LE(4) ||
        !putBool(false) || !putBool(false) || !putBool(false) || !putBool(false) ||
        !putBool(false) || !putBool(false) || !putBool(false) || !putBool(false) ||
        !putBool(false) || !putBool(false) || !putString(BEDROCK_VERSION_NAME) ||
        !putI32LE(0) || !putI32LE(0) || !putBool(false) ||
        !putString("") || !putString("") || !putBool(false) ||
        !putBool(false) || !putBool(false) || !putVarInt(0) || !putBool(false)) return false;

    // Level ID/name/template/trial.
    if (!putString("espbedrock-world") ||
        !putString(BEDROCK_LEVEL_NAME) ||
        !putString("") || !putBool(false)) return false;

    // Authoritative movement mode, current tick, enchantment seed.
    if (!putVarInt(0) || !putU64LE(world->gameTime()) || !putVarInt(0)) return false;

    // Empty block properties; item definitions are a no-op in current codec.
    if (!putVarUInt(0) || !putString("") || !putBool(false) ||
        !putString("ESP-Bedrock")) return false;

    // Empty player-property NBT: TAG_Compound + empty name + TAG_End.
    if (offset + 4 > sizeof(packet)) return false;
    packet[offset++] = 0x0A;
    packet[offset++] = 0x00;
    packet[offset++] = 0x00;
    packet[offset++] = 0x00;

    // Block registry checksum, world-template UUID, client-side generation,
    // hashed block IDs, network permissions.
    if (!putU64LE(0) || !putU64LE(0) ||
        !putBool(false) || !putBool(false) ||
        !putBool(false)) return false;

    // Server configuration join info is absent, followed by four empty IDs.
    if (!putBool(false) || !putString("") || !putString("") ||
        !putString("") || !putString("")) return false;

    if (!sendEncryptedBedrockPacket(peer, packet, offset)) return false;

    peer.startGameSent = true;
    startGamePacketsSentCount++;
    return true;
  }

  void handleBedrockPayload(Peer &peer,
                            const uint8_t *data,
                            size_t length) {
    if (length == 0) return;

    size_t offset = 0;
    uint32_t packetId = 0;
    if (!BedrockProtocol::readVarUInt(data, length, offset, packetId)) return;

    if (packetId == BEDROCK_REQUEST_NETWORK_SETTINGS_ID) {
      if (offset + 4 != length) return;
      const uint32_t clientProtocol = readU32BE(data + offset);
      Serial.printf("[BEDROCK] RequestNetworkSettings version=%lu\n",
                    (unsigned long)clientProtocol);
      if (clientProtocol != BEDROCK_PROTOCOL_VERSION) {
        Serial.printf("[BEDROCK] Client version differs. expected=%u\n",
                      BEDROCK_PROTOCOL_VERSION);
      }
      sendNetworkSettings(peer);
      return;
    }

    if (packetId == 1) {
      handleLoginPacket(peer, data + offset, length - offset);
      return;
    }

    if (packetId == BEDROCK_CLIENT_TO_SERVER_HANDSHAKE_ID) {
      Serial.println("[BEDROCK] ClientToServerHandshake received.");
      if (!peer.encryptionReady) return;
      sendPlayStatus(peer, 0);
      if (!peer.resourcePacksInfoSent) sendResourcePacksInfo(peer);
      return;
    }

    if (packetId == BEDROCK_RESOURCE_PACK_CLIENT_RESPONSE_ID) {
      handleResourcePackClientResponse(peer, data + offset, length - offset);
      return;
    }

    if (packetId == BEDROCK_REQUEST_CHUNK_RADIUS_ID) {
      handleRequestChunkRadius(peer, data + offset, length - offset);
      return;
    }

    if (packetId == 175) {
      handleSubChunkRequest(peer, data + offset, length - offset);
      return;
    }
  }

  bool readVarStringSpan(const uint8_t *data,
                         size_t length,
                         size_t &offset,
                         size_t &stringOffset,
                         uint32_t &stringLength) {
    uint32_t lengthValue = 0;

    if (!BedrockProtocol::readVarUInt(
          data, length, offset, lengthValue)) {
      return false;
    }

    if ((size_t)lengthValue > length - offset) {
      return false;
    }

    stringOffset = offset;
    stringLength = lengthValue;
    offset += lengthValue;
    return true;
  }

  bool readLEStringSpan(const uint8_t *data,
                        size_t length,
                        size_t &offset,
                        size_t &stringOffset,
                        uint32_t &stringLength) {
    if (offset + 4 > length) return false;

    stringLength = (uint32_t)data[offset] |
                   ((uint32_t)data[offset + 1] << 8) |
                   ((uint32_t)data[offset + 2] << 16) |
                   ((uint32_t)data[offset + 3] << 24);
    offset += 4;

    if ((size_t)stringLength > length - offset) {
      return false;
    }

    stringOffset = offset;
    offset += stringLength;
    return true;
  }

  void handleLoginPacket(Peer &peer,
                         const uint8_t *data,
                         size_t length) {
    loginPacketsCount++;

    if (length < 4) {
      Serial.println("[BEDROCK] Login packet too short.");
      return;
    }

    const uint32_t clientProtocol = readU32BE(data);
    size_t offset = 4;
    size_t connectionOffset = 0;
    uint32_t connectionLength = 0;

    if (!readVarStringSpan(
          data, length, offset, connectionOffset, connectionLength)) {
      Serial.println("[BEDROCK] Login connection-request string is malformed.");
      return;
    }

    const uint8_t *connection = data + connectionOffset;
    const size_t connectionBytes = connectionLength;
    size_t innerOffset = 0;
    size_t authOffset = 0;
    uint32_t authLength = 0;
    size_t clientJwtOffset = 0;
    uint32_t clientJwtLength = 0;

    if (!readLEStringSpan(
          connection, connectionBytes,
          innerOffset, authOffset, authLength) ||
        !readLEStringSpan(
          connection, connectionBytes,
          innerOffset, clientJwtOffset, clientJwtLength)) {
      Serial.println("[BEDROCK] Login JWT fields are malformed.");
      return;
    }

    peer.loginReceived = true;
    peer.clientProtocol = clientProtocol;
    peer.loginAuthBytes = authLength;
    peer.loginClientJwtBytes = clientJwtLength;

    if (clientProtocol == BEDROCK_PROTOCOL_VERSION) loginVersionMatchesCount++;

    Serial.printf("[BEDROCK] Login protocol=%lu auth=%lu clientJwt=%lu\n",
                  (unsigned long)clientProtocol,
                  (unsigned long)authLength,
                  (unsigned long)clientJwtLength);

    uint8_t clientPublicKeyDer[256] = {};
    size_t clientPublicKeyLength = 0;
    String keySource;

    if (!findClientPublicKey(
          connection + authOffset,
          authLength,
          connection + clientJwtOffset,
          clientJwtLength,
          clientPublicKeyDer,
          sizeof(clientPublicKeyDer),
          clientPublicKeyLength,
          keySource)) {
      Serial.println("[CRYPTO] No client P-384 public key found in Login.");
      return;
    }

    esp_fill_random(peer.encryptionSalt, sizeof(peer.encryptionSalt));

    if (!derivePeerKey(
          peer, clientPublicKeyDer, clientPublicKeyLength)) {
      Serial.println("[CRYPTO] ECDH/AES setup failed.");
      return;
    }

    String handshakeJwt;
    if (!createHandshakeJwt(
          peer.encryptionSalt,
          sizeof(peer.encryptionSalt),
          handshakeJwt)) {
      clearPeerCrypto(peer);
      peer.encryptionReady = false;
      Serial.println("[CRYPTO] ServerToClientHandshake JWT creation failed.");
      return;
    }

    uint8_t packet[768] = {};
    size_t packetOffset = 0;
    const size_t idLength =
      BedrockProtocol::writeVarUInt(
        BEDROCK_SERVER_TO_CLIENT_HANDSHAKE_ID,
        packet, sizeof(packet));
    if (idLength == 0) return;
    packetOffset += idLength;

    size_t stringWritten = 0;
    if (!BedrockProtocol::writeString(
          handshakeJwt,
          packet + packetOffset,
          sizeof(packet) - packetOffset,
          stringWritten)) return;

    packetOffset += stringWritten;
    peer.handshakeSent = sendRawBedrockPacket(peer, packet, packetOffset);

    Serial.printf(
      "[CRYPTO] Session key ready from %s; handshake sent=%s\n",
      keySource.c_str(),
      peer.handshakeSent ? "yes" : "no");
  }

  void handleResourcePackClientResponse(Peer &peer,
                                        const uint8_t *data,
                                        size_t length) {
    size_t offset = 0;
    uint32_t statusOrdinal = 0;
    if (!BedrockProtocol::readVarUInt(data, length, offset, statusOrdinal)) return;

    size_t typeOffset = 0;
    uint32_t typeLength = 0;
    if (!readVarStringSpan(data, length, offset, typeOffset, typeLength)) return;

    String response;
    for (uint32_t i = 0; i < typeLength; ++i) {
      char ch = (char)data[typeOffset + i];
      if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
      response += ch;
    }

    resourcePackResponsesCount++;
    Serial.printf("[BEDROCK] ResourcePackClientResponse status=%lu type=%s\n",
                  (unsigned long)statusOrdinal, response.c_str());

    if ((response == "downloadingfinished" || response == "haveallpacks") &&
        !peer.resourcePackStackSent) {
      sendResourcePackStack(peer);
      return;
    }

    if (response == "resourcepackstackfinished" || response == "completed") {
      if (!peer.resourcePackStackSent) {
        sendResourcePackStack(peer);
      }
      if (peer.resourcePackStackSent && !peer.startGameSent) {
        if (sendStartGame(peer)) {
          sendWorldBootstrap(peer, 4);
          sendPlayStatus(peer, 3);
          peer.playerSpawnSent = true;
        }
      }
    }
  }

  bool sendSimpleVarIntPacket(Peer &peer,
                              uint32_t packetId,
                              int32_t value) {
    uint8_t packet[16] = {};
    size_t offset = 0;

    const size_t idLen = BedrockProtocol::writeVarUInt(
      packetId, packet + offset, sizeof(packet) - offset);
    if (idLen == 0) return false;
    offset += idLen;

    const size_t valueLen = BedrockProtocol::writeVarUInt(
      (uint32_t)value, packet + offset, sizeof(packet) - offset);
    if (valueLen == 0) return false;
    offset += valueLen;

    return sendEncryptedBedrockPacket(peer, packet, offset);
  }

  bool sendNetworkChunkPublisherUpdate(Peer &peer, uint32_t radius) {
    uint8_t packet[32] = {};
    size_t offset = 0;

    const size_t idLen = BedrockProtocol::writeVarUInt(
      BEDROCK_NETWORK_CHUNK_PUBLISHER_UPDATE_ID,
      packet + offset, sizeof(packet) - offset);
    if (idLen == 0) return false;
    offset += idLen;

    const int publisherY = world ? world->terrainHeight(8, 8) + 1 : 24;
    writeU32LE(packet + offset, 8); offset += 4;
    writeU32LE(packet + offset, (uint32_t)publisherY); offset += 4;
    writeU32LE(packet + offset, 8); offset += 4;

    const size_t radiusLen = BedrockProtocol::writeVarUInt(
      radius, packet + offset, sizeof(packet) - offset);
    if (radiusLen == 0) return false;
    offset += radiusLen;

    return sendEncryptedBedrockPacket(peer, packet, offset);
  }

  void sendWorldBootstrap(Peer &peer, int32_t requestedRadius) {
    const int32_t radius =
      requestedRadius < 2 ? 2 :
      requestedRadius > 8 ? 8 :
      requestedRadius;

    sendSimpleVarIntPacket(peer, BEDROCK_CHUNK_RADIUS_UPDATED_ID, radius);
    sendSimpleVarIntPacket(
      peer, BEDROCK_SET_TIME_ID,
      world ? (int32_t)world->gameTime() : 0);
    sendSimpleVarIntPacket(peer, BEDROCK_SET_DIFFICULTY_ID, 1);
    sendNetworkChunkPublisherUpdate(peer, (uint32_t)radius);
  }

  void handleRequestChunkRadius(Peer &peer,
                                const uint8_t *data,
                                size_t length) {
    size_t offset = 0;
    uint32_t encodedRadius = 0;
    if (!BedrockProtocol::readVarUInt(
          data, length, offset, encodedRadius)) return;

    const int32_t requestedRadius = (int32_t)encodedRadius;
    Serial.printf("[BEDROCK] RequestChunkRadius radius=%ld\n",
                  (long)requestedRadius);
    sendWorldBootstrap(peer, requestedRadius);
  }

  void handleSubChunkRequest(Peer &peer,
                             const uint8_t *data,
                             size_t length) {
    size_t offset = 0;
    uint32_t dimension = 0;
    if (!BedrockProtocol::readVarUInt(data, length, offset, dimension)) return;

    uint32_t requestCount = 0;
    if (!BedrockProtocol::readVarUInt(data, length, offset, requestCount)) return;
    if (requestCount > 96) return;
    if (offset + (size_t)requestCount * 3 + 12 > length) return;

    const size_t offsetsStart = offset;
    offset += (size_t)requestCount * 3;

    const int32_t centerX = (int32_t)readU32LE(data + offset);
    const int32_t centerY = (int32_t)readU32LE(data + offset + 4);
    const int32_t centerZ = (int32_t)readU32LE(data + offset + 8);
    (void)dimension;
    (void)centerY;

    uint8_t packet[8192] = {};
    size_t out = 0;
    const size_t idLength =
      BedrockProtocol::writeVarUInt(174, packet + out, sizeof(packet) - out);
    if (idLength == 0) return;
    out += idLength;

    packet[out++] = 0; // cache enabled
    if (out + 1 + 12 + 5 > sizeof(packet)) return;
    packet[out++] = 0; // dimension 0
    writeU32LE(packet + out, (uint32_t)centerX); out += 4;
    writeU32LE(packet + out, (uint32_t)centerY); out += 4;
    writeU32LE(packet + out, (uint32_t)centerZ); out += 4;

    uint8_t countBytes[5];
    const size_t countLength =
      BedrockProtocol::writeVarUInt(requestCount, countBytes, sizeof(countBytes));
    if (countLength == 0 || out + countLength > sizeof(packet)) return;
    memcpy(packet + out, countBytes, countLength);
    out += countLength;

    for (uint32_t i = 0; i < requestCount; ++i) {
      const uint8_t *requestOffset = data + offsetsStart + i * 3;
      packet[out++] = requestOffset[0];
      packet[out++] = requestOffset[1];
      packet[out++] = requestOffset[2];
      packet[out++] = 6; // SUCCESS_ALL_AIR
      packet[out++] = 0; // optional subchunk data absent
      packet[out++] = 0; // HeightMapDataType.NO_DATA
      packet[out++] = 0; // optional height map absent
      packet[out++] = 0; // Render height map type NO_DATA
      packet[out++] = 0; // optional render height map absent
      packet[out++] = 0; // optional blob id absent
      if (out + 10 > sizeof(packet)) break;
    }

    sendEncryptedBedrockPacket(peer, packet, out);
  }

  void handleGameBatch(Peer &peer,
                       const uint8_t *data,
                       size_t length) {
    if (length < 2 || data[0] != BEDROCK_GAME_PACKET_ID) return;

    // ServerToClientHandshake and Login are sent in plaintext. Before
    // encryption, Bedrock uses FE + either:
    //   VarUInt(length) + packet (pre-NetworkSettings), or
    //   FF + VarUInt(length) + packet (after NetworkSettings).
    if (!peer.encryptionReady) {
      size_t offset = 1;
      const bool hasBatchMarker =
        (data[offset] == BEDROCK_BATCH_NONE);

      if (hasBatchMarker) ++offset;

      while (offset < length) {
        uint32_t packetLength = 0;
        if (!BedrockProtocol::readVarUInt(
              data, length, offset, packetLength)) return;

        if (packetLength == 0 ||
            packetLength > 64 * 1024 ||
            offset + packetLength > length) {
          return;
        }

        handleBedrockPayload(
          peer,
          data + offset,
          packetLength
        );
        offset += packetLength;
      }
      return;
    }

    if (length < 10) return;

    const size_t encryptedLength = length - 1;
    uint8_t *plaintext = (uint8_t *)malloc(encryptedLength);
    if (!plaintext) return;

    memcpy(plaintext, data + 1, encryptedLength);

    if (!decryptBytes(peer, plaintext, encryptedLength) ||
        encryptedLength < 9) {
      free(plaintext);
      return;
    }

    const size_t bodyLength = encryptedLength - 8;
    if (!validateEncryptionTrailer(
          peer,
          plaintext,
          bodyLength,
          plaintext + bodyLength)) {
      free(plaintext);
      return;
    }

    size_t offset = 0;
    if (plaintext[offset++] != BEDROCK_BATCH_NONE) {
      free(plaintext);
      return;
    }

    while (offset < bodyLength) {
      uint32_t packetLength = 0;
      if (!BedrockProtocol::readVarUInt(
            plaintext,
            bodyLength,
            offset,
            packetLength)) {
        break;
      }

      if (packetLength == 0 ||
          packetLength > 64 * 1024 ||
          offset + packetLength > bodyLength) {
        break;
      }

      handleBedrockPayload(
        peer,
        plaintext + offset,
        packetLength
      );
      offset += packetLength;
    }

    encryptedPacketsReceivedCount++;
    free(plaintext);
  }

  void handleConnectedDatagram(const uint8_t *data,
                               size_t length,
                               IPAddress ip,
                               uint16_t port) {
    if (length < 4) return;

    Peer *peer = findPeer(ip, port);

    if (!peer) {
      Serial.printf("[RAKNET] Datagram from unknown peer %s:%u\n",
                    ip.toString().c_str(), (unsigned)port);
      return;
    }

    const uint32_t sequence = readTriadLE(data + 1);
    peer->lastSeen = millis();
    peer->datagramsReceived++;
    connectedDatagramCount++;

    sendAck(ip, port, sequence);

    parseFrames(data + 4, length - 4, *peer);
  }

  void handleFramePayload(Peer &peer,
                          const uint8_t *payload,
                          size_t payloadBytes) {
    if (payloadBytes == 0) return;

    if (payload[0] == 0x09 &&
        payloadBytes >= 18) {
      peer.clientGuid = readU64BE(payload + 1);

      const uint64_t pingTime = readU64BE(payload + 9);
      const bool secure = payload[17] != 0;
      (void)secure;

      peer.openConnection = true;
      sendConnectionRequestAccepted(peer, pingTime);

      handshakeCount++;

      Serial.printf(
        "[RAKNET] ConnectionRequest guid=%s\n",
        u64ToDecimal(peer.clientGuid).c_str()
      );
      return;
    }

    if (payload[0] == 0x13) {
      peer.established = true;

      Serial.println(
        "[RAKNET] NewIncomingConnection received. "
        "RakNet session established."
      );
      return;
    }

    if (payload[0] == 0x00 &&
        payloadBytes >= 17) {
      const uint64_t pingTime = readU64BE(payload + 1);
      sendConnectedPong(peer, pingTime);
      return;
    }

    if (payload[0] == BEDROCK_GAME_PACKET_ID) {
      handleGameBatch(peer, payload, payloadBytes);
      return;
    }

    handleBedrockPayload(peer, payload, payloadBytes);
  }

  bool handleSplitFrame(Peer &peer,
                        const uint8_t *payload,
                        size_t payloadBytes,
                        uint32_t splitCount,
                        uint16_t splitId,
                        uint32_t splitIndex) {
    if (splitCount == 0 ||
        splitCount > RAKNET_MAX_SPLITS ||
        splitIndex >= splitCount) {
      return false;
    }

    const size_t requiredBytes =
      (size_t)splitCount * RAKNET_SPLIT_SLOT;

    if (requiredBytes > RAKNET_MAX_SPLIT_MEMORY) {
      Serial.printf(
        "[RAKNET] Split packet too large: %lu bytes\n",
        (unsigned long)requiredBytes
      );
      return false;
    }

    SplitAssembly &split = peer.split;

    if (!split.active ||
        split.splitId != splitId ||
        split.splitCount != splitCount) {
      clearSplitAssembly(split);

      split.buffer = (uint8_t *)ps_malloc(requiredBytes);

      if (!split.buffer) {
        split.buffer = (uint8_t *)malloc(requiredBytes);
      }

      if (!split.buffer) {
        Serial.printf(
          "[RAKNET] Split allocation failed: %lu bytes\n",
          (unsigned long)requiredBytes
        );
        return false;
      }

      memset(split.buffer, 0, requiredBytes);
      split.active = true;
      split.splitId = splitId;
      split.splitCount = splitCount;
      split.receivedCount = 0;
      split.totalBytes = 0;
    }

    const size_t writeOffset =
      (size_t)splitIndex * RAKNET_SPLIT_SLOT;

    if (writeOffset + payloadBytes > requiredBytes) {
      return false;
    }

    if (split.received[splitIndex] == 0) {
      memcpy(split.buffer + writeOffset, payload, payloadBytes);
      split.received[splitIndex] = 1;
      split.receivedCount++;

      const size_t endOffset = writeOffset + payloadBytes;

      if (endOffset > split.totalBytes) {
        split.totalBytes = endOffset;
      }
    }

    split.lastUpdate = millis();

    if (split.receivedCount != split.splitCount) {
      return true;
    }

    const size_t totalBytes = split.totalBytes;

    handleFramePayload(peer, split.buffer, totalBytes);

    clearSplitAssembly(split);
    return true;
  }

  void parseFrames(const uint8_t *data,
                   size_t length,
                   Peer &peer) {
    size_t offset = 0;

    while (offset + 3 <= length) {
      const uint8_t flags = data[offset++];
      const uint8_t reliability = (flags >> 5) & 0x07;
      const bool isSplit = (flags & 0x10) != 0;

      const uint16_t bitLength =
        ((uint16_t)data[offset] << 8) | data[offset + 1];
      offset += 2;

      // RakNet reliability modes commonly used by Bedrock:
      //   0 = unreliable
      //   1 = unreliable sequenced
      //   2 = reliable
      //   3 = reliable ordered
      //   4 = reliable sequenced
      // Modes 5-7 carry additional receipt metadata and are not needed
      // for the current Bedrock login/session path.
      if (reliability == 2 || reliability == 3 ||
          reliability == 4 || reliability == 6 ||
          reliability == 7) {
        if (offset + 3 > length) return;
        offset += 3; // reliable message index
      }

      if (reliability == 1 || reliability == 4) {
        if (offset + 3 > length) return;
        offset += 3; // sequence index
      }

      if (reliability == 1 || reliability == 3 ||
          reliability == 4 || reliability == 7) {
        if (offset + 4 > length) return;
        offset += 3; // order index
        offset += 1; // channel
      }

      uint32_t splitCount = 0;
      uint16_t splitId = 0;
      uint32_t splitIndex = 0;

      if (isSplit) {
        if (offset + 10 > length) return;

        splitCount = readU32LE(data + offset);
        offset += 4;

        splitId = readU16LE(data + offset);
        offset += 2;

        splitIndex = readU32LE(data + offset);
        offset += 4;
      }

      const size_t payloadBytes =
        (bitLength + 7U) / 8U;

      if (payloadBytes == 0 ||
          offset + payloadBytes > length) {
        return;
      }

      const uint8_t *payload = data + offset;

      if (isSplit) {
        handleSplitFrame(
          peer,
          payload,
          payloadBytes,
          splitCount,
          splitId,
          splitIndex
        );
      } else {
        handleFramePayload(peer, payload, payloadBytes);
      }

      offset += payloadBytes;
    }
  }

  void sendConnectedPong(Peer &peer, uint64_t pingTime) {
    uint8_t payload[17];
    size_t payloadLength = 0;

    payload[payloadLength++] = 0x03;
    writeU64BE(payload + payloadLength, pingTime);
    payloadLength += 8;
    writeU64BE(payload + payloadLength, millis());
    payloadLength += 8;

    sendFrameSet(
      peer,
      0,
      payload,
      payloadLength,
      false
    );
  }
};

class SerialTerminal {
public:
  void begin(World &w, EspBedrockNetworkServer &n) {
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
  EspBedrockNetworkServer *network = nullptr;

  void execute(String command) {
    command.trim();

    if (command == "help") printHelp();
    else if (command == "status") cmdStatus();
    else if (command == "players")
      Serial.printf(
        "Active peers: %u/%u\n",
        (unsigned)network->activePeers(),
        (unsigned)ESPBEDROCK_MAX_PLAYERS
      );
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
    else if (command == "raknet") {
      cmdRakNet();
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
    Serial.println("  raknet");
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

  void cmdRakNet() {
    Serial.println("[RAKNET] Direct-server transport");
    Serial.printf("  UDP port: %u\n", ESPBEDROCK_UDP_PORT);
    Serial.printf("  RakNet protocol: %u\n", RAKNET_PROTOCOL_VERSION);
    Serial.printf("  Bedrock protocol: %u (%s)\n",
                  BEDROCK_PROTOCOL_VERSION,
                  BEDROCK_VERSION_NAME);
    Serial.printf("  Active peers: %u/%u\n",
                  (unsigned)network->activePeers(),
                  (unsigned)ESPBEDROCK_MAX_PLAYERS);
    Serial.printf("  Server GUID: %s\n",
                  u64ToDecimal(network->guid()).c_str());
    Serial.printf("  Pongs answered: %lu\n",
                  (unsigned long)network->pingsAnswered());
    Serial.printf("  Handshake steps: %lu\n",
                  (unsigned long)network->handshakeSteps());
    Serial.printf("  Encrypted packets sent: %lu\n",
                  (unsigned long)network->encryptedPacketsSent());
    Serial.printf("  Encrypted packets received: %lu\n",
                  (unsigned long)network->encryptedPacketsReceived());
    Serial.printf("  Resource-pack responses: %lu\n",
                  (unsigned long)network->resourcePackResponses());
    Serial.printf("  StartGame packets sent: %lu\n",
                  (unsigned long)network->startGamePacketsSent());
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
EspBedrockNetworkServer network;
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
  Serial.println("          ESP-BEDROCK 0.6.0");
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

  network.attachWorld(world);
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
