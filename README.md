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
- **A region's own map assets.** Point a region at its original `.img`, `.ide`, `.ipl` and `.col` and the
  plugin registers the archive, remaps the model ids into a free range and places every `inst` line at
  the region's origin — through the game's own loaders. See *Loading a city's own assets*; `ModelIdLimit`
  raises the DFF id ceiling so a whole city's models fit.
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
any `.asi`). Everything a region owns lives in one folder, `MapExpander\<region>\`, under the source
game's own file names:

```
MapExpander\vicecity\tiles\radar00.png ..   radar tiles
MapExpander\vicecity\american.gxt           text
MapExpander\vicecity\waterpro.dat           water
MapExpander\vicecity\navig.zon, info.zon    zones
MapExpander\vicecity\vicecity.img           models + textures      (city loader)
MapExpander\vicecity\ide\, ipl\, col\       definitions, placements, collision
```

`tools\import-city.ps1 "<Vice City dir>" "<San Andreas dir>"` fills that folder from a III/VC
install in one go (see *Importing a city*); `-Lite` produces just the tiles, text, water and zones for
when a server or map mod already places the buildings. The plugin writes
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
| `WorldOriginZ` | height added to everything the region loads (water levels, zone boxes, IPL and inst coordinates) when the map was placed higher or lower than in its own game; default 0 |
| `WorldMinX/MaxX/MinY/MaxY` | alternative to the origin: extent in SA space; the origin becomes its centre |
| `Img`, `Ide`, `Ipl`, `Col` | the region's own map assets — see *Loading a city's own assets* |
| `Interior<N>` | `area, minX, minY, minZ, maxX, maxY, maxZ [, name]` — one box, in the region's own coordinates, that puts the player in that interior; see *Interiors* |
| `Weapons` | the source game's guns, added as weapon types of their own alongside San Andreas' (or written over them, under `RegionWeaponMode = Replace`). Ignored under SA-MP. |
| `Pickups` | the world pickups the source game's main script leaves around its map, in the region's own coordinates. Ignored under SA-MP. |

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
| `ModelIdLimit` | 25000 | number of DFF model ids; 20000 turns it off, 26400 is the ceiling. Skipped under SA-MP. Savegames are not compatible across a change. |
| `FreeModelIdStart`, `FreeModelIdEnd` | 18631, `ModelIdLimit`−1 | model ids region IDEs may claim |
| `AtomicModelInfoLimit` | grows with `ModelIdLimit` (14000 stock) | capacity of SA's atomic model-info store; set only if another limit adjuster changed it |
| `RegionWeapons` | 1 | master switch for every region's `Weapons` file. Skipped under SA-MP. |
| `RegionWeaponMode` | `Add` | `Add` gives the region's guns weapon types of their own and leaves San Andreas' forty-seven exactly as they were (ten new types is the hard limit); `Replace` writes them over the nearest San Andreas weapon, which changes that weapon for the whole game |
| `RegionPickups` | 1 | master switch for every region's `Pickups` file. Skipped under SA-MP. |
| `PickupReserve` | 200 | how many of SA's 620 pickup slots to keep back for its own script, which asks for them after the regions are placed |
| `Debug` | 0 | verbose log, F8 diagnostics, water render statistics |

### Region data (feature branches)

These load *original* data files in the region's own coordinates and add the origin, so nothing has to
be hand-translated. Copy the files straight from the source game's `data\` folder.

| Key | Branch | Source file (Vice City) | Notes |
|---|---|---|---|
| `Ipl=` (comma list) | `feature/ipl-sections` | `data\maps\*.ipl` | sections `cull enex tcyc jump pick cars grge auzo`; `inst`/`zone`/`occl` ignored. VC's own IPLs only carry `cull` and `pick`; the SA-only sections must be authored in SA format. The entry/exit quad tree is widened beyond ±3000. Pool limits are checked per line and headroom is logged; `tcyc` (20) and `auzo` spheres (3) are nearly full in vanilla. |
| `Water=`, `WaterXOffset=` | `feature/water` | `data\waterpro.dat` (`WaterXOffset=400` for VC, 0 for III) or an SA `water.dat` | physics (swimming, boats, camera) use real levels beyond ±3000 via a hook on `CWaterLevel::GetWaterLevelNoWaves`, and the same quads are rendered in place of SA's endless ocean. `tools/waterpro_to_dat.py` converts offline. |
| `Img=`, `Ide=`, `Ipl=`, `Col=` | `feature/city-loader` | `models\gta3.img`, `data\maps\*.ide`, `data\maps\*.ipl` | the region's own buildings; the `inst` section of an IPL is used here, the other sections by the IPL loader above. See *Loading a city's own assets*. |
| `Zones=`, `ZoneNames=` | `feature/zones` | `data\navig.zon`, `data\info.zon`, names from `text\american.gxt` | area names on VC through the vanilla zone system; their display names are merged into the game's own GXT key table. |

## Importing a city

```powershell
.\tools\import-city.ps1 "D:\Games\GTA Vice City" "D:\Games\GTA San Andreas"          # everything
.\tools\import-city.ps1 "D:\Games\GTA Vice City" "D:\Games\GTA San Andreas" -Lite    # tiles, text, water, zones
```

Runs in a throwaway container (`tools/import_city.py`; `import-city.sh` on Linux/macOS). It reads the
source game's `gta_vc.dat`/`gta3.dat` for the file order and writes `MapExpander\<region>\`:

- `<region>.img` — every `.dff`/`.txd` from the source `gta3.img`, repacked as an SA *VER2* archive
  (SA cannot read the III/VC *v1* `.img`+`.dir` pair, so never point `Img=` at the original).
- `col\` — every `.col` from the archive plus the `.dat`'s `COLFILE` entries, as loose files.
- `ide\`, `ipl\` — the files the `.dat` lists, copied in its order.
- `tiles\radarNN.png` — decoded from the radar TXDs (DXT1/3, paletted and raw rasters).
- `american.gxt`, `waterpro.dat`, `navig.zon`, `info.zon`.
- `region.ini` — the `[region]` keys to paste, with `Ide`/`Ipl` in `.dat` order.

**Names.** San Andreas keeps no asset names - only a CRC32 of each (`CKeyGen`) - so a region's
model or dictionary whose name matches one of SA's would silently lose to SA's copy, and two of a
region's own that meet after the game's 19-character cut would leave one with no data behind it,
which freezes the streamer. The importer therefore renames every model and texture dictionary under
a two-character tag (`v_` for Vice City, `l_` for III), consistently across the archive, the IDE
model/txd columns, the IPL `inst` lines and the collision entry names. A name stays readable when
it fits and takes a short hashed tail when it does not; every name is checked to be unique and to
share no key with anything SA has before the archive is written. Texture names inside a dictionary
are never touched - they resolve within that dictionary alone. `names.txt` beside the archive maps
each new name back to the original, and the loader reads it (the `Names` key) so the log can say
`v_veg_fern_balcn~D50 (veg_fern_balcny_kb2)` rather than an id. `-NoRename` turns all of this off.
The measurements behind the scheme are in `docs/asset-namespacing.md`.

## Loading a city's own assets

With `Img`, `Ide`, `Ipl` and `Col` a region brings its own buildings instead of relying on a server to
place them. Everything stays in the region's coordinates; the plugin adds the origin.

```ini
[ViceCity]
Img = MapExpander\vicecity\vicecity.img
Ide = MapExpander\vicecity\ide\generic.ide, MapExpander\vicecity\ide\*.ide
Ipl = MapExpander\vicecity\ipl\*.ipl
Col = MapExpander\vicecity\col\*.col
```

The models themselves must be RenderWare clumps San Andreas can read; Vice City geometry generally
loads, and anything the game refuses shows up as a missing model, not as a crash. There are 8 archive
slots in total and SA uses 3, so up to 5 region archives can be added.

**What the plugin does with them.** Region archives are registered just before SA scans archive
directories; the IDE files are then fed through the game's own parser with every model id rewritten
into the free range, and each `inst` line is placed through the game's own scene loader with the
origin added to x and y. All three `inst` dialects are accepted — GTA III's 12 fields, Vice City's 13
and San Andreas' 11 — and LOD parents keep working because the whole file goes through in order. The
rotation quaternion is copied verbatim (all three games store the same conjugate convention) and the
III/VC scale columns are dropped, which loses nothing: those games ignore them too. The rewritten files land in `MapExpander\cache\` — read them when a
region looks wrong.

**Collision** comes from loose `.col` files (not from inside the IMG). They go into collision slot 0,
the one slot San Andreas never streams out, so a region's collision is in memory for the whole
session and does not depend on the collision quad tree, which cannot address anything past ±3000.

**The ID limit.** Vanilla SA leaves model ids 18631–19999 free — 1369 for all regions together — and
its 14000-entry *atomic model-info store* is almost full, so on its own only a few dozen models fit.
`ModelIdLimit` (default 25000) adds 5000 ids and grows that store to match, which is enough for a
Vice City. `tobj` (time-of-day) and `anim` models still have no room: SA fills those stores exactly,
so a city's night-lit variants are dropped and logged. The plugin never overruns a store: it stops
and writes to `MapExpander.log` how many models were dropped and which limit ran out.

**Under SA-MP** the region's `Img`/`Ide`/`Col` are ignored (the server places the city and owns the
id space), so one INI serves single-player and multiplayer.

**Listing files.** `Ide`, `Ipl`, `Col` (and `Zones`, `Gxt`) take comma lists; an entry like
`MapExpander\vicecity\ide\*.ide` expands to every match in name order. Put files whose order matters
first — `generic.ide, *.ide` loads `generic.ide` once, first.

IPL in one paragraph: "item placement" is the map's placement data — `inst` are the building/prop
objects themselves, `cull` invisible behaviour boxes (interior lighting, no-rain, mirrors, tunnels),
`pick` pickups, `enex` interior doorway markers, `tcyc` local weather/lighting boxes, `jump` stunt jumps,
`cars` parked-car generators, `grge` garages, `auzo` ambient-sound areas, `zone` area names.

## Interiors

Vice City and San Andreas hide interiors with the same mechanism, because San Andreas inherited the
code. Every entity carries a one-byte *area code* — the third column of an IPL `inst` line, which
`CFileLoader::LoadObjectInstance` stores at `CEntity + 0x2F` (0x538353 takes only the low byte; the
higher bits of that column are unrelated flags, which is why the loader can keep setting bit 512
there). The renderer then draws an entity only when the code matches, in
`CRenderer::SetupEntityVisibility` at 0x554476:

```
mov al,  [esi+0x2F]     ; entity->m_nAreaCode
mov ecx, [0xB72914]     ; CGame::currArea
cmp edx, ecx  / je  visible
cmp al,  0xD  / jne hidden      ; 13 means "everywhere", in both games
```

`CRenderer::SetupBigBuildingVisibility` (0x55466A) and `CStreaming::RemoveBuildingsNotInArea`
(0x4094EC) test the same two values and the same 13.

Vice City writes real area codes into its own IPLs — 1 Ocean View hotel, 2 Vercetti estate, 3 the
bank, 4 North Point Mall, 5 Pole Position, 6 Ken Rosenberg's office, 7 Café Robina, 8 the concert
hall, 9 the studio, 10 the rifle range, 11 the biker bar, 12 the police station, 13 *everywhere*,
14/15/16 the three stadium events, 17 the Malibu Club, 18 the print works. So the column carries
across untouched and the interiors are already loaded, already placed and already correctly hidden.
That is what black rectangles in doorways are: the room behind the opening is switched off.

Unlike San Andreas, which parks its interiors in an unused corner of the world and teleports you
there, Vice City builds them **inside the real buildings at real world coordinates**. Nothing has to
be moved, and nothing leaks out: the building shell is in the way, and the area code covers the
doorway. Collision is not area-gated anywhere in San Andreas — only rendering and streaming read
`currArea` — so an interior is solid whether or not it is drawn; that is true in Vice City too, and
harmless for the same reason, because the only way to reach the collision is to be inside.

The one missing piece is the switch. Vice City changed the area from its mission script when the
player crossed a trigger at a door, and that script is not here; VC's data records nothing about
where its doorways are. `Interior<N>` is that switch, one box per room, and `src/Interiors.cpp`
does what script command 04BB does — store `CGame::currArea` and call
`CStreaming::RemoveBuildingsNotInArea` — from a hook on `CEntryExitManager::Update`, so it runs in
the same frame slot as SA's own "has the player entered an interior" check and backs off entirely
whenever SA owns the area. It also stamps the new code on the player and on whatever they are
driving, as `CEntryExit::TransitionFinished` does at 0x4406BB and 0x4406CE: the player's own model
is gated on it like any other entity, and `CColStore::EnsureCollisionIsInMemory` (0x410AF5) and
`CIplStore::SetIplsRequired` read the *player's* code and give up when it does not match
`currArea`, so without it collision would stop streaming and the player would drop through the
world. Boxes for the same area may be repeated for an interior that is not one
room. Entry uses the box as written, exit the box plus two units, so a doorway cannot flicker.

Entry/exit markers were the alternative and are the wrong tool here: `enex` exists to *teleport* the
player to an interior that lives somewhere else, and these interiors are already where the player is
standing, so the marker would be a fade to black that puts them back where they were — and every
door would need a hand-written position and heading, which is how a player ends up inside a wall.
The `enex` section is still loaded from region IPLs by `IplLoader` for anyone who wants that
behaviour. Vice City's `cull.ipl` is a red herring: cull zones control fading, wanted-level drops
and camera behaviour, not interiors, and San Andreas' dialect of the section differs anyway
(III/VC write centre + min + max, SA writes centre + length/bottom/width).

## Tools

All run in throwaway containers; nothing is installed on the host.

- `tools/import-city.ps1` / `tools/import_city.py` — build a region folder from a III/VC install (see *Importing a city*).
- `tools/sftp.ps1` — serve the project folder over SFTP (port 2222) to test on another machine.
- `tools/scan.ps1 <gamedir>` / `tools/scan_exe.py` — find every code site using the int16/8 compression constants and the pickup accessors.
- `tools/xref.py`, `tools/fieldref.py`, `tools/check_fn.py`, `tools/readfloat.py`, `tools/dumpb.py` — capstone helpers for cross-referencing addresses, struct-field accesses, disassembly, constants and raw bytes/branch targets in `gta_sa.exe`.
- `tools/gen_functions.py` → `tools/sa_functions.txt` — 9.6k named addresses merged from gta-reversed and plugin-sdk, used to label scanner output.
- `tools/montage.py`, `tools/montage_orders.py`, `tools/tile_order_score.py` — stitch tiles and score the eight possible grid orderings to verify a tile set's numbering.
- `tools/scan_model_limit_sites.py` → `tools/model_limit_sites.txt` → `tools/gen_model_limit_sites.py` → `src/ModelLimitSites.h` — the 1407 instructions `ModelIdLimit` rewrites, with the candidates it deliberately leaves alone and why. `tools/verify_model_limit_sites.py` re-reads every one of them out of `gta_sa.exe` and must pass 100% before the table ships.

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
- Every streamable resource shares one id space: DFF 0 (20000), TXD 20000 (5000), COL 25000 (255), IPL 25255 (256), DAT 25511 (64), IFP 25575 (180), RRR 25755 (475), SCM 26230 (82), plus 4 list-head sentinels — 26316 `CStreamingInfo` entries. `ModelIdLimit` shifts every base above the DFF range and reallocates the three arrays that size with it (`ms_aInfoForModel`, `ms_modelInfoPtrs`, `ms_atomicModelInfoStore`); the streaming lists link entries with `int16` indices, hence the 26400 ceiling. The other model-info stores keep their vanilla capacities (damage 70, time 169, weapon 51, clump 92, vehicle 212, ped 278, 2dfx 100) and none of them is bounds-checked.

- **San Andreas takes ten new weapon types, and no more.** `src/WeaponTypes.cpp` widens the weapon system so a gun
  can be added rather than written over one of San Andreas' own; `RegionWeaponMode = Add` is the default and does it.
  - *Type numbers.* `aWeaponInfo` at 0xC8AAB8 is eighty records of `0x70` — `CWeaponInfo::Initialise` (0x5BF750) walks
    0xC8AABC to 0xC8CDBC, which is 80 × 0x70 exactly — and `GetWeaponInfo` (0x743C60) indexes it as
    `type + {POOR 25, STD 0, PRO 36, COP 47}` with no bounds check. San Andreas fills 0–46 (the real weapons), 47–57
    (POOR for types 22–32), 58–68 (PRO for the same) and 69 (COP for the pistol); **70–79 are never written**, and
    `CWeaponInfo::LoadWeaponData` (0x5BE9C2) computes exactly that index itself, so no relocation of the array and no
    repointing of the skill offsets is needed. Nothing lower is usable: `eWeaponType` does not stop at 46 — 47 is
    `LAST_WEAPON`, 48 `ARMOUR`, and 49–58 are the pseudo weapons the damage system uses for `RAMMEDBYCAR`,
    `EXPLOSION`, `DROWNING` and the rest.
  - *Names.* `CWeaponInfo::FindWeaponType` (0x743D10) walks a fixed table of 49 `char*` at 0x8D6150 and answers 0 —
    `UNARMED` — for anything else, so an unknown name silently overwrites unarmed combat. Exactly one instruction
    names that table (0x743D20), so it is copied to an 80-entry array on the heap and the loop bound `cmp esi, 0x30`
    (0x743D36) is raised.
  - *The switches.* The reported blocker was half right. `CWeapon::Fire` (bound 0x742461, types 16–43),
    its tail switch (0x742874, 16–40), `CWeapon::FireInstantHit` (0x740C75, 22–38), the gunshot audio (0x504F96,
    22–45) and the reload audio (0x5036F2, 22–34) really are keyed on the **weapon type**, not on the fire type from
    weapon.dat column B, and really do end in `ja default`. But each is a jump table, and a jump table can be made
    longer: both tables are copied to the heap with room for types up to 79, the `cmp` bound is raised, and the two
    `disp32` operands are repointed, so a new type is routed into its donor's case. Only `CWeapon::Fire` is fatal on
    its own — its default is the case `ROCKET` takes, which fires nothing. `CPickup::Update` (0x457A3B) also refuses a
    weapon pickup whose type is 47 or above, and `CWeapon::IsType2Handed` (0x73B1E0) is a hardcoded list of seven
    types, so it is hooked rather than patched. Every site is verified against the exact bytes 1.0 US ships before
    anything is written; one mismatch and nothing at all is changed.
  - *Still wrong.* `Events::ComputeDamageResponse` (0x4C0327) and `Events::ComputeDamageAnim` (0x4B44BB) branch on
    `type >= 47` into the arm for the pseudo weapons, and that bound cannot be raised without sending `RAMMEDBYCAR`
    with it — so a ped shot with a new gun takes the right damage but plays the generic reactions. `CHud::DrawWeaponIcon`
    (0x58D7D0) draws the HUD icon from a texture in the weapon model's own TXD named *model name* + `ICON` — SA's
    `desert_eagle.txd` holds `desert_eagleicon`, and an imported `v_python.txd` holds only `python` and `gun_python`,
    so the new guns show no icon in the wheel. Fixing it means the importer adding a `<model>icon` texture to each
    weapon TXD it copies. `CDarkel` (0x43D7B6) will not hand a new gun back after a rampage. A savegame carrying one
    expects the mod to still be installed.
- All 13 of `CPed`'s weapon slots (`m_aWeapons` at +0x5A0, `0x1C` each) are used, and `Ped::GiveWeapon` indexes them
  without a bounds check — but a slot *groups* weapons rather than naming one (slot 2 already holds the pistol, the
  silenced pistol and the Desert Eagle, one at a time), so a new gun joins its donor's slot and none of this needs
  widening. That is why `CPed`'s size never has to change.
- `CPickups::aPickUps` is 620 entries of `0x20` at 0x9788C0 (type at +0x1C, model at +0x18, positions `int16 / scale`).
  `GenerateNewOne` (0x456F20) returns −1 when full, and `CPickups::Save` writes the whole array into a savegame — so
  the array is deliberately *not* relocated, and regions are capped by `PickupReserve` instead.

## Roadmap

- **More than 26400 models.** `ModelIdLimit` frees DFF ids past 20000 and grows the atomic model-info
  store to match, but the streaming lists link entries with int16 indices, so 26400 is where that
  approach stops; a second city the size of Vice City needs those lists widened too.
- Lighting and `timecyc` differences between the source game and San Andreas.
- Zone/water array relocation for regions that exceed the vanilla pool sizes.
