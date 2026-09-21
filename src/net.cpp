#include "net.h"
#include "espbedrock_config.h"

bool NetworkServer::begin() {
  return udp.begin(ESPBEDROCK_UDP_PORT) == 1;
}

void NetworkServer::handlePacket(const uint8_t *data, size_t len, IPAddress from, uint16_t port) {
  rxPackets++;

  Serial.printf("[UDP] %u bytes from %s:%u\n",
                (unsigned)len,
                from.toString().c_str(),
                (unsigned)port);

  // Protocol work starts here. We deliberately do not pretend that a
  // raw UDP socket is already a Bedrock/RakNet implementation.
  // The next protocol layer will parse RakNet framing and Bedrock packets.
  (void)data;
}

void NetworkServer::update() {
  const int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  uint8_t buffer[512];
  const size_t n = udp.read(buffer, sizeof(buffer));
  handlePacket(buffer, n, udp.remoteIP(), udp.remotePort());
}
