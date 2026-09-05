# Asset namespacing

How a region's model, texture and collision names should be kept from colliding — with San
Andreas' own assets, with another region's, and with themselves.

Everything here was measured against San Andreas 1.0 US (HOODLUM) and a retail Vice City install.
Numbers are from those files, not from documentation.

## The short version

Namespace **model names, TXD dictionary names and COL model names**. Do **not** namespace texture
names or the DFF material references that point at them. Prefer a readable prefix; use a hash only
when the prefix would overflow the field. Do not namespace GXT keys — import selectively instead.

That is a much smaller job than "namespace everything", because two thirds of the names in an
archive live in a scope that cannot collide in the first place.

## Why renaming is cheap: San Andreas does not keep the names

`CKeyGen::GetUppercaseKey` (0x53CF30) is

```
crc = 0xFFFFFFFF
for each c: crc = table[(toupper(c) ^ crc) & 0xFF] ^ (crc >> 8)
```

with no final complement. The table at 0x8CD068 is the standard reflected CRC-32 table
(polynomial 0xEDB88320), so `key(s) == zlib.crc32(s.upper()) ^ 0xFFFFFFFF`.

Every global asset lookup goes through it, and **no literal is retained**:

| Function | Address | What it stores or compares |
|---|---|---|
| `CBaseModelInfo::SetModelName` | 0x406E30 | writes only `GetUppercaseKey(name)` to `m_nKey` at +4; there is no name field |
| `CModelInfo::GetModelInfo(name, &id)` | 0x4C5940 | hashes, then scans `ms_modelInfoPtrs` for `[eax+4] == key` |
| `CTxdStore::AddTxdSlot` | 0x731C80 | builds `TxdDef { RwTexDictionary*; u16 refs; u16 parent = 0xFFFF; u32 hash }` |
| `CTxdStore::FindTxdSlot` | 0x731850 | hashes and scans |
| `CFileLoader::LoadObject` | 0x5B3DB0 | `m_nKey = GetUppercaseKey(name)`, then `SetTexDictionary(txd)` |
| `CFileLoader::LoadCollisionFileFirstTime` | 0x5B50BC | compares `GetUppercaseKey(colName)` against `m_nKey` |
| `CDirectory::FindItem` | 0x5324EC | hash compare |

So a renamed model costs nothing at run time. Readability is a property of *our* files and *our*
logs, not of anything the game does.

## Why textures are exempt

`CTxdStore::Initialise` installs `TxdStoreFindCB` (0x731720) as RenderWare's texture-find callback.
At DFF read time it calls `RwTexDictionaryFindNamedTexture` (0x7F39F0) on the current dictionary,
then walks the parent chain. 0x7F39F0 is a hand-rolled case-insensitive comparison over
`RwTexture->name` at +0x10, and `AddTxdSlot` creates our slots with `parent = 0xFFFF`.

**A texture name therefore only has to be unique within its own dictionary.** Measured across Vice
City: 12,023 texture instances, 6,077 distinct names, and **zero duplicates inside any single
dictionary**. The 1,026 texture names shared between Vice City and San Andreas are harmless,
because those two never appear in the same dictionary.

This is why the white-leaves bug was a *dictionary* problem (two dictionaries sharing one name, one
silently replacing the other) and never a texture-name problem, and why the fix was to merge
dictionaries and copy in what a dictionary was missing rather than to rename anything.

Leaving textures alone also avoids rewriting 4,617 DFFs and 1,368 TXDs, and avoids having to keep a
material's texture reference in step with the dictionary entry it points at — the exact pairing that
produces silent white geometry rather than an error when it goes wrong.

## Field budgets

The binding constraint is **19 characters of stem**, enforced independently in three places.

| Field | Where | Width | Usable | How it is enforced |
|---|---|---|---|---|
| IMG VER2 directory entry | record + 8 | 24 B | **19-char stem + extension** | `LoadCdDirectory` 0x5B6204 forces `name[23] = 0`; 0x5B6222 `cmp edx, 0x14 / jg` silently skips the entry when the `.` sits past index 20 |
| IDE `objs/tobj` model column | `LoadObject` stack | 24 B | 23 | unbounded `sscanf %s` into a 24-byte frame slot |
| IDE txd column | same frame | **20 B to the return address** | **19** | txd buffer at `E+0x34`, frame ends at `E+0x48` |
| IPL `inst` name column | `LoadObjectInstance` 0x538690 | 24 B | 23, and **unused** | 0x538090 indexes `ms_modelInfoPtrs[inst.modelId]`; the parsed name is never passed on |
| COL chunk name | header + 8 | 22 B | 21, not NUL-forced | 0x5B507C: `rep movsd (5) + movsw` |
| TXD `TextureNative` name | struct + 8 | 32 B | 31 | — |
| TXD `TextureNative` mask | struct + 40 | 32 B | 31 | — |
| DFF `Texture` chunk strings | RW String chunk | variable, padded to 4 | ≤ 31 | bounded by `RwTexture::name[32]` |
| GXT key (III/VC) | TKEY entry + 4 | 8 B | 8, not terminated | `Gxt.cpp` entry size 12 |
| GXT key (SA) | TKEY entry | u32 hash | n/a | `CKeyArray::Search` 0x6A0008 |
| IFP anim block | `CAnimBlock` | 16 B | 15 | `RegisterAnimBlock` 0x4D3E50, `strncpy(dst, src, 0x10)` |

San Andreas' own longest archive entry is 23 characters — a 19-character stem plus `.dff`.

## Hash collisions: measured, not estimated

The birthday bound over a 32-bit space is not negligible at this scale — 18,000 names gives 3.70%,
26,000 gives 7.57%, 30,000 gives 9.95% — so it has to be checked rather than waved away.

Checked, using the real name sets and the exact `CKeyGen` algorithm:

| Set | Names | Distinct keys | Collisions |
|---|---|---|---|
| SA model-name space (IMG stems ∪ IDE model columns) | 15,366 | 15,366 | 0 |
| SA TXD-name space | 3,988 | 3,988 | 0 |
| SA collision names | 251 | 251 | 0 |
| SA + Vice City renamed, `.dff` | 19,979 | 19,979 | 0 |
| SA + Vice City renamed, `.txd` | 5,349 | 5,349 | 0 |
| SA + Vice City + a simulated third map, `.dff` | 24,592 | 24,592 | 0 |
| SA + two maps, `.txd` | 6,710 | 6,710 | 0 |

**Zero collisions everywhere.** And the important negative result:

> Hashing cannot fix within-archive duplicates. The same source name hashes to the same value, so
> two entries that already share a name still share it afterwards. Only a discriminator — an
> occurrence index — separates them.

Hashing therefore costs readability and buys nothing that a prefix does not already buy. It belongs
only as an overflow fallback.

## Recommended scheme

1. **Tag: two characters, `v_` / `l_`.** Share of Vice City's 6,015 entries that keep a fully
   readable name within the 19-character budget: `v` 99.2%, **`v_` 98.2%**, `vc_` 96.4%,
   `vice_` 87%. A bare `v` collides with San Andreas' own ped naming (`vbmycr`, `vhfyst`,
   `vwmycr`); `v_` collides with nothing.

2. **`v_` + stem when it fits and is unused.** Otherwise
   `v_ + stem_head + '~' + 4 hex + preserved _nt/_dy suffix`, within 19 characters.

   Hash the stem **without** its `_nt`/`_dy` suffix. `CBaseModelInfo::FindOtherTimeModel`
   (0x4C47E0) does `strstr(name, "_nt")` then `strncpy(p, "_dy", 4)` — which truncates everything
   after the marker — and looks the result up by key, so a day/night pair must share a body and keep
   its suffix in place. A prefix is safe; displacing the suffix is not. Vice City has 262 such
   models, of which 4 need the fallback.

3. **Rename unconditionally, not on clash.** Still rebuild the "taken" set — for the *key* check
   rather than the name check — from `gta3.img`, `gta_int.img`, `player.img`, `cutscene.img`, every
   SA IDE model and txd column, and the loose `models/*.txd|dff|col`: 15,386 model, 4,057 txd and
   254 col names.

4. **Never emit a reserved name.** `generic particle vehicle hud fonts ps2btns script grass_pc
   frontend1 frontend2 frontend3 frontend_pc player loadscs gen_int1..5 gen_intb savehous stadint
   int_la int_sf int_veg int_cont levelmap procobj proc_int props`.

5. **Occurrence index for duplicates.** Count `(extension, stem)` occurrences in directory order;
   occurrence 0 keeps the readable name and later ones take the fallback with the index folded into
   the hash. This is the only thing that fixes within-archive duplicates, and it matches what the
   source game does — first entry wins — while keeping the second addressable.

6. **Assert before writing.** Compute `CKeyGen` keys for every final name, union with San Andreas',
   and assert no duplicate name and no duplicate key per namespace; on a hit, re-hash with an
   incrementing salt. A validation run over Vice City gives: 5,904 readable, 100 fallback for
   length, 11 fallback for in-archive duplication; 0 duplicate names, 0 key collisions, 0 clashes
   with San Andreas, longest name 19.

7. **Leave textures alone**, per the section above.

8. **Keep a name map for diagnostics.** Write `MapExpander\<region>\names.txt` as
   `newname<TAB>original<TAB>reason`, and have the loader use it so a missing asset reports
   `model 19342 (v_veg_fern_balcny~AD50 = veg_fern_balcny_kb2) has no .dff` rather than a bare id.
   This is what makes any hashing tolerable.

9. **Filter the archive to what is referenced.** 1,059 entries and 63.6 MB are not referenced by any
   `objs`/`tobj`/`anim` line. Dropping them halves the clash surface at no cost.

10. **GXT: import selectively rather than namespacing.** Prefixing keys is pointless, because
    nothing would then ask for the prefixed key — the only consumer under our control is
    `Zones.cpp`. Replace the wholesale merge with an allow-list of the keys a region actually needs,
    plus an opt-in override for anyone who wants the donor's wording. The 8-character III/VC key
    field is not a constraint, since `Gxt.cpp` computes the hash itself.

## Bugs this study found, independent of which scheme is adopted

- **San Andreas is adopting Vice City's models.** `CFileLoader::MatchAllModelStrings` (0x5B57C0)
  resolves 157 hardcoded names. 96 of them exist in Vice City, and **16 exist in no San Andreas
  archive or IDE at all** — so `GetModelInfo` finds *ours* and stores it in San Andreas' global:
  `mall_fans`, `newsstand1`, `boatcranelg0`, `blimp_day`, `blimp_night`, `hotroomfan`,
  `od_lightbeam`, `littleha_police`, `glassfx_composh`, `doc_crane_cab0..03`, `yt_main_body`,
  `yt_main_body2`, `lodnetopa0`. Universal renaming fixes this; renaming only on clash does not.

- **`generic.txd` takes San Andreas' `generic` TXD slot**, which `CGame::Init1` creates, `Create`s
  and `AddRef`s, and which eight built-in model infos point at.

- **647 San Andreas strings are overwritten by Vice City's GXT**: `UZI` becomes "Uz-1", `WIN_TTL`
  names Vice City, `FEH_MPH` replaces the PC pause-map help text, plus roughly 600 credits keys.

- **`[:19]` truncation collides silently.** `veg_fern_balcny_kb1` and `veg_fern_balcny_kb2` both
  become `vc_veg_fern_balcny_`, which is the existing log line about a model with no `.dff` in any
  registered archive.

- **IDE `peds`/`cars`/`weap`/`hier` columns are never renamed** although their assets are packed.
  248 Vice City TXD names are referenced only by sections the loader skips, and compete for global
  slots for nothing.

- The five duplicate `.dff` names in Vice City's `gta3.dir` (`chef`, `Pga`, `Pgb`, `camera`,
  `veg_fern_balcny_kb2`) have **no visible effect today** — all are in `peds`/`weap` sections the
  loader skips, or are placed zero times. The risk is structural rather than live.

## What this does not address

Model-info store capacity (333 models are still dropped), the `cull` array overflow, and the water
quad limit. None of those are name-collision problems.
