# MapExpander

An ASI plugin for GTA San Andreas (1.0 US, HOODLUM exe) that makes the engine cooperate with custom
maps placed outside the vanilla ±3000 world: Vice City, Liberty City, your own islands. Works in
single-player and in SA-MP.

What it does today:

- **Radar and pause-map tiles for any number of regions.** Each region is a folder of PNG tiles in the
  region's *own* coordinate system (Vice City's original 64 `radar00..63` tiles work unchanged) plus one
  origin: where that map's (0,0) sits in SA. Tiles are drawn through the game's own radar pipeline, so
  they rotate, clip and mask exactly like the vanilla map.
- **Pause map that actually reaches the regions.** Scrolling, the cursor, waypoints and blips are freed
  from the ±3000 box, with a configurable margin.
- **Pickups and parked cars beyond ±4096.** SA stores those positions as `int16 / 8`; the plugin
  retargets every compression site (41 pickup, 23 car-generator) to a configurable scale, default
  ±8192 at 0.25-unit precision.
- **Melee and ped awareness beyond ±3000.** `CEntityScanner` clamped its sector search into the vanilla
  grid before wrapping it into the repeat-sector array, so outside the map a ped's ped and vehicle
  scanners came back empty: punches animated but never connected, and ped AI ignored everyone around
  it. Four two-byte patches drop the clamps.
- **Feature branches (pull requests open):** offset loading of original IPL sections (`feature/ipl-sections`),
  original water maps (`feature/water`), and original zone names (`feature/zones`). See *Region data*.

## Build

Only Docker is needed on the host (Linux containers). The image contains clang + MinGW, premake and a
compiled plugin-sdk; the plugin is cross-compiled to a 32-bit Windows DLL.

```powershell
.\build.ps1            # Release -> output\Release\MapExpander.SA.asi
.\build.ps1 debug
.\build.ps1 -Rebuild   # rebuild the toolchain image from scratch
```

`build.sh` does the same on Linux/macOS/WSL.

## Install

Copy `MapExpander.SA.asi` and `MapExpander.ini` into the game folder (an ASI loader is required, as for
any `.asi`). Put tiles and data under `MapExpander\` in the game folder as configured. The plugin writes
`MapExpander.log` next to the `.asi`; read it first when something does not show up. The log names
the version, what was configured and patched, and any error; a crash is logged with its exception
address, registers and stack even in a normal build.

With `[World] Debug=1` the log becomes verbose and `F8` in game prints your SA coordinates and logs
them together with the equivalent region-local coordinates and the zone the game resolves there —
handy for placing an origin against a known landmark. Leave it off for normal play.

## Configuration

`MapExpander.ini` has one `[World]` section and one section per region. Any section name other than
`World` is a region.

### Region

| Key | Meaning |
|---|---|
| `TileFolder` | folder with the tile PNGs, relative to the game directory |
| `FilePattern` | two `%d` = column,row (zero based, west→east, north→south); one `%d` = row-major index like `radar00..radar143` |
| `TileSize` | world units per tile (III/VC/SA radar tiles are 500) |
| `TilesX`, `TilesY` | grid size |
| `GridMinX` | local X of the west edge of column 0 (default `-TilesX*TileSize/2`) |
| `GridMaxY` | local Y of the north edge of row 0 (default `+TilesY*TileSize/2`) |
| `WorldOriginX/Y` | SA world position of the region's local (0,0) |
| `WorldMinX/MaxX/MinY/MaxY` | alternative to the origin: extent in SA space; the origin becomes its centre |

Vice City as shipped in the example: 8×8 tiles of 500 covering local −2000..2000, origin
`5998.5, 1052.5` (VC is placed by `SA = VC + offset` in the VC2SAMP conversion, so its landmass spans
SA x 4171..6963, y −778..2626).

### World

| Key | Default | Meaning |
|---|---|---|
| `FixPickupPositions` | 1 | retarget the pickup position-compression sites |
| `FixCarGeneratorPositions` | 1 | same for car generators |
| `CompressedPositionScale` | 4 | int16 units per world unit: 8 = vanilla (±4096), 4 = ±8192 at 0.25, 2 = ±16384 at 0.5. Saves store positions with the scale active when saved. |
| `FixMeleeBeyondMap` | 1 | let `CEntityScanner` find peds and vehicles outside ±3000, so melee lands and ped AI sees its neighbours |
| `ExtendPauseMap` | 1 | free the pause map's scroll/cursor/blip limits |
| `PauseMapMargin` | 1500 | extra scrollable world units past the map and all regions |
| `PauseMapMinX/MaxX/MinY/MaxY` | – | fix the scrollable area explicitly instead (all four) |
| `ExtendWaterRendering` | 1 | draw region water beyond ±3000 instead of SA's flat ocean (see *Region data*); 2/3 are bisect modes for diagnosing rendering problems |
| `Gxt` | – | comma list of GXT files (III/VC/SA format) merged into the game's text table; ours win on a shared key |
| `GxtLogMisses` | 0 | diagnostic: hook `CText::Get` to log keys nobody provides (conflicts with SA-MP's own hook; keep off) |
| `Debug` | 0 | verbose log, F8 diagnostics, water render statistics |

### Region data (feature branches)

These load *original* data files in the region's own coordinates and add the origin, so nothing has to
be hand-translated. Copy the files straight from the source game's `data\` folder.

| Key | Branch | Source file (Vice City) | Notes |
|---|---|---|---|
| `Ipl=` (comma list) | `feature/ipl-sections` | `data\maps\*.ipl` | sections `cull enex tcyc jump pick cars grge auzo`; `inst`/`zone`/`occl` ignored. VC's own IPLs only carry `cull` and `pick`; the SA-only sections must be authored in SA format. The entry/exit quad tree is widened beyond ±3000. Pool limits are checked per line and headroom is logged; `tcyc` (20) and `auzo` spheres (3) are nearly full in vanilla. |
| `Water=`, `WaterXOffset=` | `feature/water` | `data\waterpro.dat` (`WaterXOffset=400` for VC, 0 for III) or an SA `water.dat` | physics (swimming, boats, camera) use real levels beyond ±3000 via a hook on `CWaterLevel::GetWaterLevelNoWaves`, and the same quads are rendered in place of SA's endless ocean. `tools/waterpro_to_dat.py` converts offline. |
| `Zones=`, `ZoneNames=` | `feature/zones` | `data\navig.zon`, `data\info.zon`, names from `text\american.gxt` | area names on VC through the vanilla zone system; their display names are merged into the game's own GXT key table. |

IPL in one paragraph: "item placement" is the map's placement data — `inst` are the building/prop
objects themselves, `cull` invisible behaviour boxes (interior lighting, no-rain, mirrors, tunnels),
`pick` pickups, `enex` interior doorway markers, `tcyc` local weather/lighting boxes, `jump` stunt jumps,
`cars` parked-car generators, `grge` garages, `auzo` ambient-sound areas, `zone` area names.

## Tools

All run in throwaway containers; nothing is installed on the host.

- `tools/sftp.ps1` — serve the project folder over SFTP (port 2222) to test on another machine.
- `tools/scan.ps1 <gamedir>` / `tools/scan_exe.py` — find every code site using the int16/8 compression constants and the pickup accessors.
- `tools/xref.py`, `tools/fieldref.py`, `tools/check_fn.py`, `tools/readfloat.py`, `tools/dumpb.py` — capstone helpers for cross-referencing addresses, struct-field accesses, disassembly, constants and raw bytes/branch targets in `gta_sa.exe`.
- `tools/gen_functions.py` → `tools/sa_functions.txt` — 9.6k named addresses merged from gta-reversed and plugin-sdk, used to label scanner output.
- `tools/montage.py`, `tools/montage_orders.py`, `tools/tile_order_score.py` — stitch tiles and score the eight possible grid orderings to verify a tile set's numbering.

## Engine facts worth knowing

- SA's static world is 120×120 sectors of 50 units = ±3000; beyond that, sector lookups clamp into the edge sector (works, degrades). Fully enlarging the sector grid is fastman92 Limit Adjuster territory (~700 patch sites) and is not attempted here.
- Moving entities (peds, vehicles, objects) live in `ms_aRepeatSectors`, a 16×16 grid the game indexes by
  masking the 0..119 static sector index with 15 — so it wraps every 800 units and keeps working outside
  ±3000, as long as a caller masks the *raw* index instead of clamping it first. `CPhysical::Add`, `CWorld::FindObjectsInRange`
  and `CWorld::ProcessLineOfSight` mask the raw index; `CEntityScanner::ScanForEntitiesInRange` did not.
- The pause map is drawn by `CMenuManager::PrintMap` with `tile = zoom/6` px per 500 units and raw `width/640` scaling — never `CMenuManager::StretchX`, which other mods hook for aspect correction. The overlay replicates the raw arithmetic for that reason.
- `CWaterLevel::RenderWater` (0x6EF650) draws water outside the 12×12 block grid as one
  `RenderWaterRectangle` call per 500-unit block (0x6F004E), over the ≤70 blocks
  `CWaterLevel::BlockHit` (0x6E6CA0) collected from the camera frustum this frame. Retargeting that
  one call is enough to render arbitrary water out there without touching the block grid.
- Path nodes (`nodes*.dat`) are `int16 / 8` at ~490 inline sites: traffic and ped paths cannot be extended past ±4096 without a rewrite. Irrelevant under SA-MP.
- Audio zone boxes and occluders are also `int16 / 8` and stay limited to ±4096.

## Roadmap

- **Self-contained city loading.** Today the objects themselves come from elsewhere (for Vice City, the
  VC2SAMP server places them). Loading a city from its original files inside the plugin means:
  registering models from the source `.ide` (`objs`/`tobj` with a free model-id range), streaming
  `.dff`/`.txd`/`.col` from the source `.img` through `CStreaming`, placing `inst` lines with the origin,
  and lighting/`timecyc` differences. It is the largest remaining piece and would make a region fully
  standalone in single-player.
- Zone/water array relocation for regions that exceed the vanilla pool sizes.
