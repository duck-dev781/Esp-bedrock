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

Designed around:

- ESP32-WROVER-E
- PSRAM enabled
- MicroSD card
- Wi-Fi

The SD card is used for persistent data so the RAM footprint stays small.

## Repository layout

```
assets/       Runtime data and optional original assets
config/       Server configuration
include/      Public headers
src/          Firmware/server implementation
world/        Example world data format
```

## First prototype

The first build provides:

- SD-card detection and directory creation.
- World metadata load/save.
- A compact procedural voxel world.
- Player state.
- Serial terminal commands.
- UDP listener.
- Runtime statistics.

The Bedrock/RakNet implementation is intentionally separated from the world engine so protocol work can be expanded without rewriting the storage layer.

## Serial terminal

After boot, open the ESP32 serial port at 115200 baud.

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
stop
```

## Legal/asset policy

Only original code and original/simple placeholder data belong in this repository. The server does not bundle Microsoft's Minecraft client, Bedrock Dedicated Server, or copyrighted Minecraft resource files.

## Build

This project uses PlatformIO with the Arduino framework.

```
pio run
pio run -t upload
pio device monitor -b 115200
```
