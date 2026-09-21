#pragma once

#include <Arduino.h>

namespace BedrockProtocol {

// Unsigned LEB128/VarInt helpers used by many Minecraft protocol fields.
// These helpers are deliberately isolated from networking so packet parsing
// can be tested without a live client.
size_t writeVarUInt(uint32_t value, uint8_t *out, size_t capacity);
bool readVarUInt(const uint8_t *data, size_t length, size_t &offset, uint32_t &value);

// Bedrock packets travel inside RakNet frames. This structure only describes
// the small amount of state needed by the future session layer.
struct Session {
  bool connected = false;
  uint32_t protocolVersion = 0;
  uint64_t clientGuid = 0;
  IPAddress address;
  uint16_t port = 0;
};

}
