# Save compatibility across map configurations

The question this answers: **what has to be true for a savegame to survive a city being added or
removed?** It matters now rather than later, because the answer is a policy about how offsets and
ids are assigned, and that policy is nearly free to adopt today and expensive to retrofit once
missions and garages keep state of their own.

Everything below about San Andreas' own format was read out of `gta_sa.exe`, not recalled; the
addresses are given so it can be checked.

---

## Scope: which compatibility

Three different promises get confused with each other. Only the second is worth making:

| promise | verdict |
|---|---|
| a mod save loads in vanilla San Andreas | not offered, and not worth engineering for |
| a save survives adding, removing or reordering regions | **this is the goal** |
| a save survives a deliberate repack | offered as a migration, not as a guarantee |

The first is nonetheless nearly free as a side effect of the rule in "Keep San Andreas' blocks
vanilla-shaped" below, and that is a good reason to keep following it.

---

## What a San Andreas save actually contains

`CGenericGameStorage::GenericSave` (0x5D13E0) writes a header, then these blocks in this order.
Every one was confirmed by the call sequence at 0x5D14D5-0x5D15A6.

| block | address | carries coordinates | keyed by position/index |
|---|---|---|---|
| SimpleVars | 0x5D1B80 | player and camera | - |
| Scripts | 0x5D4C40 | mission state | **globals blob, sized externally** |
| Pools | 0x5D0880 | vehicles, peds, objects | **model ids inside entities** |
| Garages | 0x5D3160 | garage boxes and stored cars | **fixed array by index** |
| GameLogic | 0x5D33C0 | - | - |
| PathFind | 0x5D34C0 | - | node/area indices |
| Pickups | 0x5D3540 | 620 x 0x20, positions | **array index** |
| Restart | 0x5D3620 | hospital and police points | count at 0xA4326C |
| Radar | 0x5D5860 | blip positions | pool handles |
| (unnamed) | 0x5D2E60 | - | - |
| Gangs | 0x5D3A60 | - | - |
| CarGenerators | 0x5D38C0 | 500 x 0x20, positions | **array index** |
| PlayerInfo | 0x5D3AC0 | - | - |
| Stats | 0x5D3B40 | - | - |
| SetPieces | 0x5D3C70 | trigger boxes | - |
| Streaming | 0x5D29A0 | - | **one byte per model slot** |
| PedType | 0x5D3CD0 | - | - |
| TagManager | 0x5D3D60 | tag positions | - |
| IplStore | 0x5D5420 | - | **one byte per IPL slot** |
| Shopping | 0x5D3DE0 | - | - |
| GangWars | 0x5D5530 | territory boxes | - |
| StuntJumps | 0x5D5570 | jump boxes | - |
| EntryExits | 0x5D5970 | - | **pool indices** |
| Audio | 0x5D3EE0 | - | - |
| 3dMarkers | 0x5D4300 | marker positions | - |

### There is no framing between blocks

This is the fact everything else hangs on. `CGenericGameStorage::LoadDataFromWorkBuffer` (0x5D1300)
is a **plain sequential byte reader**: it copies the number of bytes it is asked for out of a rolling
0xC800 work buffer, advancing a cursor at 0xC16EEC and refilling from the file when it runs dry.
There is no block tag, no length prefix, no offset table. Each `Load` function simply asks for what
it expects, in a fixed order.

So **a block that changes size does not fail - it silently shifts every block after it.** A save
written with a different `main.scm` does not report a mismatched Scripts block; it reads garbage into
the pools, the garages and everything downstream. That is why "never widen a block San Andreas saves"
is a hard rule rather than a preference.

### The integrity check is a plain byte sum, and that is good news

`CGenericGameStorage::CheckDataNotCorrupt` (0x5D1170) walks the whole file adding up every byte,
subtracts the final four, and compares the total against those four bytes read as a dword. That is
the entire check: no signature, no per-block hash, nothing keyed to the game build.

Two consequences. It confirms the sidecar must be a **separate file** - appending to the `.b` would
break the sum. And, more usefully, **an existing save can be rewritten outside the game and
re-checksummed**, which turns a repack from a warning into a migration. See "Migrating a save written
before a move" below.

### Where saves live

`\GTA San Andreas User Files\GTASAsf<N>.b` under the user's documents folder; the name is built in
`CGenericGameStorage::MakeValidSaveName` (0x5D0E90). The sidecar belongs beside it as
`GTASAsf<N>.mapx`.

---

Two blocks deserve their own note, because they are the ones nobody expects.

**Streaming (0x5D29A0) is a byte per model slot, for every slot.** It walks
`CStreaming::ms_aInfoForModel` from 0x8E4CC6 to 0x9654B6 at stride 0x14 - **26316 entries** - and
writes one byte each. That array is not just models: its ranges also cover TXDs, collision files
and IPLs. Anything that shifts a slot assignment shifts the meaning of this block.

**IplStore (0x5D5420) is a byte per IPL slot, and there are exactly 255 of them.** The loop runs
`esi` from 0x34 to 0x3400 at stride 0x34 over `CIplStore::ms_pPool` (0x8E3FB0), writing 1 where the
entry exists and both its flag bytes (+0x2D, +0x2F) are set. `CIplStore::Load` (0x5D54A0) reads them
back **by the same index**. A region's IPLs take slots in this pool, so which slot a given IPL
occupies depends on what else was loaded.

---

## The three ways a save breaks

### 1. Coordinate drift - a region moves

If a region's world offset changes, every absolute coordinate in a save that fell inside it now
names empty air, or worse, a point inside a different city. That reaches the player's own position,
pickups, car generators, restart points, garage boxes and their stored cars, radar blips, tags, 3d
markers, set pieces and gang territories.

`PackRegions` re-solves the placement from the region list, so **adding a fifth city today moves the
other four**. That is the single largest hazard, and it is entirely self-inflicted - see the fix.

### 2. Index drift - a region's slots move

Our own allocator hands out model ids in load order:

```cpp
// src/region/CityLoader.cpp
int AllocateModelId() {
    int end = FreeIdEnd();
    while (gNextFreeId <= end) {
        int id = gNextFreeId++;
        if (id >= 0 && !ModelInfoAt(id)) return id;
    }
    return -1;
}
```

So a region's ids depend on **which regions loaded before it**. Remove Vice City and Liberty City's
buildings all shift down. A save then resolves a stored garage car, a pickup model or a streaming
flag to the wrong model. The same applies to IPL slots, collision slots and entry-exit pool indices,
each of which is positional in a save block above.

This is already understood for one case - imported weapon types are documented as tying a save to
the mod - but weapons are the mild version, because the type numbers are fixed by `weapons.dat`
rather than by load order.

### 3. Script space - the Scripts block is main.scm's global area

`Scripts::Save` reads a size from 0xA49963 and writes the global variable space in 0xC800-byte
chunks. The block's size is therefore whatever the loaded `main.scm` declares. **Any mission support
that grows SA's global space makes every existing save unreadable, and every save it writes
unreadable without the mod.** Mission state must live somewhere else.

---

## What to do about it

### A. A region ledger, so nothing ever moves

The fix for hazards 1 and 2 is one idea: **assign a region its ground and its id ranges once, and
never reassign them while it is known.** A small file in the game folder -
`MapExpander\regions.layout` - records for each region ever seen: its name, its world offset, and
the id ranges it was given (model, TXD, collision, IPL, zone, pickup, and later garage and mission).

The rules that make it work:

- a region's assignment is written on first load and is then read, not recomputed;
- adding a region allocates only from ground and ids nothing holds - packing chooses a corner for
  the new one instead of re-solving all of them;
- **removing a region leaves a tombstone**, so its ground and ids stay reserved and nothing else
  shifts;
- a deliberate `RepackLayout` action re-solves from scratch, and says plainly that saves written
  before it will need rebasing.

The cost is that packing degrades as tombstones accumulate - a slot freed by a removed city is not
reused until a repack. Given corner packing already fits five maps in the ground one map needs, there
is a lot of room to spend before that matters.

This one change turns "adding a city moves the others" into "adding a city moves nothing", which is
the property save compatibility actually needs.

### B. A sidecar save, so our state is ours

SA's blocks are the wrong home for anything we add. Write `gta_sa03.mapx` beside `gta_sa03.b`, on
`GenericSave` (0x5D13E0) and read on `GenericLoad` (0x5D17B0), holding:

- **the layout manifest as it stood at save time** - region name to offset and id ranges. This is
  what makes recovery possible: a load can compare it against the current ledger, spot a region that
  moved, and **rebase** every coordinate that fell in its old box by the difference, rather than
  giving up;
- everything SA's fixed blocks cannot carry: our pickups above the vanilla 620, our garages, mission
  progress, ped and police state.

The manifest is the important half. Without it a moved region is undetectable; with it, even a
deliberate repack is a migration rather than a loss.

### C. Keep San Andreas' blocks vanilla-shaped

The pickup array is the precedent and it should be the general rule. `CPickups::Save` still writes
exactly the first 620 entries; ours live above that window and are carried separately, so the block
stays byte-identical. Do the same for garages when they arrive: `CGarages` is a fixed array saved by
index, so imported garages must sit above the vanilla count and persist in the sidecar.

Never widen a block San Andreas saves. It costs vanilla readability, it costs cross-configuration
readability, and there is always room above the window instead.

### D. Decide what a missing region does

If a save names a region that is not installed, refusing to load is the wrong answer. Quarantine
instead: drop pickups and generated cars inside its box, keep the rest, log what was dropped. The one
case needing a real decision is **the player saved inside a region that is now gone** - the position
in the save is in the void. The obvious answer is the nearest surviving safehouse or restart point,
with a log line saying so.

### E. Missions and garages, specifically

- **Do not extend main.scm's globals.** Keep mission state in the sidecar, keyed by a mission id
  *string* rather than an index, so adding a mission never renumbers an existing one. A CLEO-based
  implementation gets this for free, since CLEO keeps its own store.
- **Garages** get the pickup treatment: allocate above the vanilla array, persist ours, leave the
  vanilla block untouched. Note `CGarages::Save` also stores the cars inside a garage, which carry
  model ids - so garages depend on the ledger in A being in place first.
- **Version the sidecar** with the repo `VERSION` and a hash of the layout, so a future format change
  can migrate instead of breaking.

---

## Migrating a save written before a move

Because the checksum is a byte sum and nothing else, a save is rewritable outside the game. That
makes an offline migration possible: read a `.b`, add the old-to-new offset to every coordinate that
fell inside a region that moved, write it back, recompute the final dword. The same pass can renumber
model ids that shifted.

What it costs is a parser. Since there is no framing, reaching the coordinates in a late block means
knowing the exact size of every block before it, and two of them are not fixed:

- **Scripts** is sized by the loaded `main.scm` global area, read from 0xA49963 and written in
  0xC800-byte chunks;
- **Pools** is sized by how many vehicles, peds and objects the save holds, so it has to be walked
  rather than skipped.

Everything else is a fixed size or is self-describing from a count written immediately before it
(`CRestart` at 0xA4326C, `CTheCarGenerators` at 500 x 0x20, `CPickups` at 620 x 0x20).

So this is real work, but it is bounded, it is entirely offline, and it can be validated without the
game by round-tripping a save through the parser and checking the bytes come back identical. **Write
that round-trip test before writing the rebase.** It is also the only way to rescue saves that people
have already made, which the ledger by itself cannot do.

---

## Order of work

1. **The ledger (A)** before any more regions are added, and before `PackRegions` is ever turned on
   by default. It is the only item that gets harder the longer it waits, because every save written
   in the meantime is written against a layout nothing recorded.
2. **The manifest half of the sidecar (B)** next - small, and it is what makes every later mistake
   recoverable.
3. The rest of the sidecar when there is state to put in it, which is the moment garages or missions
   start.
