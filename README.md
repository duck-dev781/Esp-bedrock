# ESP-bedrock

An original, lightweight Minecraft-inspired survival server/runtime for the ESP32-WROVER-E.

## Goals

- ESP32-WROVER-E target with PSRAM.
- SD-card-backed world storage and assets.
- Serial-port administration terminal.
- Lightweight voxel world and survival state.
- UDP networking foundation for a Bedrock-compatible protocol layer.
- No proprietary Minecraft binaries, textures, or copied game assets.

This project is **not** the official Minecraft Bedrock Dedicated Server. It is an independent implementation designed for constrained ESP32 hardware.

## Hardware

Designed specifically around the Freenove FNK0047 ESP32-WROVER board:

- ESP32-WROVER-E / ESP32 WROVER Module
- PSRAM
- Built-in microSD slot on the back of the WROVER board
- Normal router/LAN Wi-Fi in STA/client mode

### FNK0047 SD configuration

Freenove's FNK0047 documentation specifies the built-in SD slot as **SDMMC 1-bit** with fixed pins:

- CLK: GPIO14
- CMD: GPIO15
- D0: GPIO2

The single-file sketch uses:

`SD_MMC.setPins(14, 15, 2)`

and mounts the card at:

`/sdcard`

with 1-bit mode and no automatic formatting on failure. The ESP32 Arduino SD_MMC API defines the `begin` arguments as `mountpoint, mode1bit, format_if_mount_failed, frequency, maxOpenFiles`, so the safe server call is the equivalent of:

`SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5)`

World data and server assets are then stored at the SD card root under:

```
/espbedrock/
  world/
  config/
  assets/
```

Do not change the SD pins to SPI/CS=5 for the built-in FNK0047 slot.

### Wi-Fi/LAN

Use normal Wi-Fi **Station/client mode**. The ESP32 joins your router and receives a LAN IP, allowing other devices on the same LAN to connect to the server. No SoftAP is created.

The serial terminal can configure Wi-Fi without recompiling the sketch.

## Repository layout

```
ESP_Bedrock.ino  Main Arduino sketch
assets/          Runtime data and optional original assets
config/          Server configuration
include/         Modular development headers
src/             Modular development implementation
world/           Example world data format
```

## First prototype

The first build provides:

- SD-card detection and directory creation.
- World metadata load/save.
- A compact procedural voxel world.
- Player state foundation.
- Serial terminal commands.
- RakNet offline ping/pong server discovery.
- RakNet OpenConnectionRequest1/OpenConnectionReply1.
- RakNet OpenConnectionRequest2/OpenConnectionReply2.
- Basic RakNet connected-frame parsing and connected ping/pong.
- Runtime RakNet peer tracking and statistics.
- Initial protocol VarUInt utilities.

The Bedrock/RakNet implementation is intentionally separated from the world engine so protocol work can be expanded without rewriting the storage layer.

## Serial terminal

After boot, open the ESP32 serial port at **115200 baud**. Set the line ending to **Newline**.

Commands:

```
help
status
players
world
save
regen
say <message>
settime <0-23999>
wifi status
wifi scan
wifi set <SSID>|<PASSWORD>
wifi connect
wifi clear
lan
sd status
sd ls
stop
```

## Legal/asset policy

Only original code and original/simple placeholder data belong in this repository. The server does not bundle Microsoft's Minecraft client, Bedrock Dedicated Server, or copyrighted Minecraft resource files.

## Build

The easiest starting point is the single-file Arduino sketch:

`ESP_Bedrock.ino`

For the Freenove documentation's Arduino setup, select **ESP32 Wrover Module** and use the ESP32 Arduino board package. Freenove's current FNK0047 tutorial documents ESP32 package **3.0.x** and uses 115200 baud for the serial monitor.

Serial monitor: **115200 baud**.

The repository also keeps the modular `src/` and `include/` implementation as a development layout for future expansion.

## Network transport status

The sketch now has the classic RakNet direct-server path on UDP 19132 through the offline discovery and OpenConnection stages.

A modern Bedrock release can also use NetherNet/WebRTC for LAN games. That transport is separate from RakNet and is being added as its own layer; it should not be confused with the direct RakNet server path.

The current RakNet layer is therefore a real protocol foundation, not yet a complete playable Bedrock implementation. The next layers are:

1. RakNet reliable ordered frames, ACK/NACK handling, and connection acceptance.
2. Bedrock batch/compression and login packets.
3. Player/chunk/inventory/survival packets.
4. NetherNet LAN discovery and WebRTC transport where required by the client version.
