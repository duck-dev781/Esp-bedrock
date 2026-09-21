# World storage

The live world is stored on the SD card at:

`/espbedrock/world/`

The firmware currently stores compact world metadata in `world.dat`.

The next storage layer will add:

- chunk files
- modified-block records
- player data
- entity data
- atomic save/rename recovery
