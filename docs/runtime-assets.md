# Runtime assets and the SD card

The GitHub repository is the source/build side of ESP-Bedrock. The physical SD card is the runtime side.

The firmware creates:

```
/espbedrock/
  world/
  config/
  assets/
```

Small original data files such as `assets/blocks.tsv` can be copied into:

```
/espbedrock/assets/
```

Large world state should stay on SD and should not be compiled into the firmware.

The eventual Bedrock resource-pack path will be kept separate from server world data. The project will not bundle Microsoft's proprietary Minecraft assets.
