#include "bedrock_protocol.h"

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

bool readVarUInt(const uint8_t *data, size_t length, size_t &offset, uint32_t &value) {
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
