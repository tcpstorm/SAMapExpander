# Testing status and procedure

State as of `a87dbc9` (5 September 2026). Everything below was run on GTA:SA 1.0 US (HOODLUM)
with the Vice City region from the shipped `MapExpander.ini`, in single player and on an SA-MP
0.3.DL server (VC2SAMP) that places the city itself.

## What is verified

| Feature | Single player | SA-MP | How it was checked |
|---|---|---|---|
| Radar + pause-map tiles | ok | ok | 64 VC tiles drawn in place, rotate/clip like vanilla; pause map scrolls to them |
| Pause map scroll/cursor/blips beyond ±3000 | ok | ok | free movement over the whole extent, waypoints placeable on VC |
| Pickups / car generators beyond ±4096 | ok | n/a | 40 + 23 sites patched, pickups on VC spawn |
| Melee / ped awareness beyond ±3000 | ok | ok | punches connect, peds react, on VC |
| Water physics (waterpro.dat) | ok | ok | swimming/boats at VC's levels, `WorldOriginZ=-6` applied |
| Water rendering (region quads replace SA's ocean) | ok | ok | no seams, no divide-by-zero after even-aligned rectangles |
| GXT merge (VC american.gxt) | ok | ok | zone/area names show in the HUD and on the pause map under SA-MP since the table merge replaced the `CText::Get` hook |
| Zones (navig.zon, RegionName) | ok | ok | F8 (`Debug=1`) shows the resolved zone; HUD names change between VC areas |
| IPL sections (cull/pick/…) with origin | loads | n/a | VC IPLs' `cull`/`pick` lines load; z now offset too (not visually re-checked yet) |
| `ModelIdLimit` 25000 | loads | skipped | 1407 sites verified against the exe; game boots and plays with it active |
| City loader: IDE / COL | loads | skipped | 3798 VC models registered, 333 `tobj` dropped (time store full), VC `COLL` files load (306 + 8 + 110 + 4 models), placeholder box for models in no col file |
| City loader: IMG / inst | **not yet** | skipped | see *Open* |

## Open

- **IMG not yet tested.** Every single-player run so far had no `Img=` in the INI, so all 3798 models
  reported `no .dff in any registered archive` and nothing was visible. Next run: build the archive
  with `tools\import-city.ps1` (or any VER2 archive) and confirm `img: … registered as archive N`
  plus buildings on the VC landmass.
- `tobj` (night-lit) models are dropped: SA's 169-entry time-model store is full in vanilla.
- `tools/import_city.py` has passed a synthetic fixture (IMG v1→v2 round trip, clash renames through
  IMG/IDE/IPL/COL, DXT1 and raw raster decode, `--lite`) but has not yet been run on a real VC install.
- Interiors keep VC's interior numbers; not checked.

## Crashes fixed on the way (so they are recognised if they come back)

| Symptom in `MapExpander.log` | Cause | Fix |
|---|---|---|
| `exception … gta_sa.exe+0x13CF36` (Core::GetUppercaseKey) under SA-MP / other mods | three hooks stacked on `CText::Get` | strings are merged into the game's own key table; no `CText::Get` hook unless `GxtLogMisses=1` |
| `EXCEPTION_INT_DIVIDE_BY_ZERO … +0x2E9210` | 1-unit water slivers handed to the renderer | all water rectangles even-aligned and ≥ 32 units |
| crash approaching VC in single player | `CTheZones::Update` writing outside its 10×10 visited grid | original skipped outside ±3000 |
| `exception … +0x11700B5` at first load with `ModelIdLimit` | `lea esi,[edi+25000]` in `CColStore::LoadAllCollision` missed by the site scanner (displacement, not immediate) | scanner classifies displacement forms; 27 sites added |
| `exception … +0x1375A6` (LoadCollisionModel) writing `0x62` | `CFileLoader::LoadCollisionFile(path, slot)` passes a stale register when it has to allocate a CColModel | collision loaded through `LoadCollisionFileFirstTime(data, size, slot)` |
| `exception … +0x134134` (CEntity::GetBoundRect) reading 0 | a placed model with no CColModel (in no col file) | placeholder `ms_colModelBBox` for such models before placement |
| model limit / city loader active under SA-MP | `samp.dll` is injected after ASI init | detection from the launcher's command line |

## How to test

### Setup

1. Copy `output\Release\MapExpander.SA.asi` and `MapExpander.ini` next to `gta_sa.exe`.
2. Region data in `MapExpander\vicecity\` (see README *Install*). From a VC install:
   `.\tools\import-city.ps1 "<VC dir>" "<SA dir>"` (or `-Lite` for tiles/text/water/zones only).
3. `Debug=1` in `[World]` for a verbose log and F8.

### Single player

1. New game (saves are not compatible across a `ModelIdLimit` change).
2. Read `MapExpander.log`, in this order:
   - `MapExpander 0.1.0 (release)` and no `SA-MP detected` line.
   - `model id limit 20000 -> 25000` and `1407 site(s) patched` (or `ERROR: … another mod patched it`).
   - `city: loading own assets`, `img: … registered as archive N`, `models: N registered`,
     `col: … loaded into col slot 0`, `col: N model(s) … placeholder box`, `inst: N entity/entities placed`.
   - `GXT: game table rebuilt`, `zones: … added N`, `water: … served outside`, `pause map scroll extent`.
3. Teleport / drive to VC (`WorldOriginX/Y` = 5998.5, 1052.5 in SA space). Check tiles, water,
   zone names (bottom right), collision (walk on a building), pickups.
4. F8 logs position, region-local coordinates and the zone the game resolves there.
5. Any `CRASH:` block: note the `gta_sa.exe+offset`, registers and the plugin return addresses.

### SA-MP

1. Connect through the SA-MP launcher (so the command line carries `-h/-p/-n`).
2. The log must show `SA-MP detected: …`, `SA-MP: the model id limit stays at 20000` and
   `SA-MP: region Img/Ide/Col are left alone`.
3. Check tiles, pause map, water, zone names on the server-placed VC. Zone names and GXT text go
   through the game's table, so SA-MP's own `CText::Get` hook does not interfere.
4. A crash also produces SA-MP's `crashinfo`-style log with the same exception address; both
   together locate it.

### The importer

- `.\tools\import-city.ps1 <VC dir> <SA dir>` prints the archive entry count, how many were
  renamed for clashes, IDE/IPL/COL counts, tiles decoded, and writes `MapExpander\vicecity\region.ini`.
- Sanity: `vicecity.img` starts with `VER2`; `col\` holds the area `.col` files plus `generic.col`;
  `tiles\` has 64 PNGs; `ide\generic.ide` is first in `region.ini`'s `Ide=` list.
