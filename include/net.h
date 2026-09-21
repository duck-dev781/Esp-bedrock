#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

class NetworkServer {
public:
  bool begin();
  void update();

  uint32_t packetsReceived() const { return rxPackets; }
  uint32_t packetsSent() const { return txPackets; }

private:
  WiFiUDP udp;
  uint32_t rxPackets = 0;
  uint32_t txPackets = 0;

  void handlePacket(const uint8_t *data, size_t len, IPAddress from, uint16_t port);
};
