#include "net.h"
#include "espbedrock_config.h"
#include "bedrock_protocol.h"

bool NetworkServer::begin() {
  return udp.begin(ESPBEDROCK_UDP_PORT) == 1;
}

void NetworkServer::handlePacket(const uint8_t *data, size_t len, IPAddress from, uint16_t port) {
  rxPackets++;

  Serial.printf("[UDP] %u bytes from %s:%u\n",
                (unsigned)len,
                from.toString().c_str(),
                (unsigned)port);

  // This is intentionally only the protocol entry point for now.
  // A future layer will recognize RakNet datagrams, perform the connection
  // handshake, then hand Bedrock payloads to the session manager.
  if (len > 0) {
    size_t offset = 0;
    uint32_t firstVarUInt = 0;

    if (BedrockProtocol::readVarUInt(data, len, offset, firstVarUInt)) {
      Serial.printf("[PROTO] first varuint=0x%08lX\n",
                    (unsigned long)firstVarUInt);
    }
  }
}

void NetworkServer::update() {
  const int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  uint8_t buffer[512];
  const size_t n = udp.read(buffer, sizeof(buffer));
  handlePacket(buffer, n, udp.remoteIP(), udp.remotePort());
}
