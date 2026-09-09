# Migration roadmap

What is still to bring across from the donor games, in the order worth doing it, and why each item
sits where it does. This is a planning document: it says what has not been built and what it depends
on. What *has* been built and its limits belong in
[known-limitations.md](known-limitations.md) and [traffic-and-population.md](traffic-and-population.md).

---

## The rule that decides most of these

Nearly every item below could be implemented either by filling San Andreas' own arrays or by keeping
our own state and driving the game from outside. [save-compatibility.md](save-compatibility.md)
settles which:

> **Implement donor services as plugin logic reading mined data. Do not inject into San Andreas'
> vanilla arrays or its script.**

The reasons are specific rather than stylistic. `CGarages` writes its garage count into the save, so
a save carrying imported garages, loaded without them, restores that count into San Andreas' own
array and writes past the end of it. `CTheCarGenerators` and `CRestart` are saved by array index and
by absolute coordinate, so both drift when a region moves or is removed. And the Scripts block is
sized by `main.scm`'s global area, so anything that grows it makes every existing save unreadable.

Since the plugin already places and drives pedestrians, vehicles and pickups itself, the outside
route is usually the easier one as well as the safer one.

## What is already mined out of `main.scm`

Two things, and both prove the technique:

- **World pickups** — mined once and kept beside the importer as `tools/pickups_<region>.dat`,
  because walking another game's bytecode is not worth redoing on every import.
- **Zone to ped group links** — `SET_ZONE_PED_GROUP` (opcode `0324`), scanned live at import, with
  each candidate validated against the zone file rather than trusted.

Everything below is unmined.

---

## Tier 1 — no dependencies, do first

### Restart points: hospitals and police

Dying or being arrested in Vice City currently wakes you in All Saints or a Los Santos cell, because
`CRestart::FindClosestRestartPoint` only knows San Andreas' points.

The analysis is already done and written down: the arrays are at counts `0xA4326C`/`0xA43268`,
positions `0xA43318`/`0xA43390`, headings `0xA432E8`/`0xA43298`, town ids `0xA432C0`/`0xA43270`, with
**ten slots each**, of which San Andreas' own script uses 8 hospital and 7 police.

**Do not fill those slots.** `CRestart::Save` (0x5D3620) copies the counts and arrays into a vanilla
save block. Hook `CRestart::FindClosestHospitalRestartPoint` (0x460850) and
`FindClosestPoliceRestartPoint` (0x460A50) instead — both are plain
`__cdecl (CVector where, CVector* outPos, float* outHeading)` — and answer from the region's own
points when the player is inside a region. Nothing enters the save.

Highest value for the effort on this list, and the groundwork exists.

### Parked cars

III and Vice City create them from `main.scm` with literal coordinates, exactly as they do pickups,
so the same mine-once-and-keep-it approach applies. Empty kerbs are most of why an imported city
still reads as a model rather than a place.

Note `CTheCarGenerators` is a vanilla save block of 500 entries at 0x20 each, keyed by index — so
these should be plugin-owned spawns rather than entries in that array. The vehicle spawning built for
traffic already does most of the work.

### Police scanner zone dialogue

`CAEPoliceScannerAudioEntity::AddAudioEvent` matches the player's zone against a hard-coded 194-entry
table of San Andreas' own zones and says nothing when it finds no match, so the dispatcher never
names where you are. A zone dependency rather than a path one, and small.

---

## Tier 2 — services

### Pay 'n' Sprays and bomb shops

These look like `CGarages` work and should not be. A respray is: car inside the box, player has the
money, deduct it, repair the car, close the doors. Implemented as our own logic it avoids the garage
save block entirely, and the garage block is the one with the memory-overwrite failure.

### Safehouse garages that store cars

These genuinely need `CGarages` semantics, and a stored car carries a model id — which is exactly the
index-drift hazard. **Gated on the region ledger** (Tier 3).

### Ammunation and weapon shops

Vice City's are interiors containing pickups, so this is partly reachable with what already exists.

---

## Tier 3 — the region ledger

Not a feature, but it blocks the rest, and it is the only item that gets strictly harder the longer
it waits: every save written before it exists was written against a layout nothing recorded.

Assign each region its ground and its id ranges once, never reassign while it is known, tombstone a
removed region rather than reclaiming its slots, and carry the layout manifest in a sidecar save so a
region that did move can be rebased rather than lost. Full design in
[save-compatibility.md](save-compatibility.md).

---

## Tier 4 — collectibles and side content

- **Hidden packages.** Check first whether these are already placed: the pickup mining took
  always-running threads and excluded 25 mission-thread placements, and hidden packages are usually
  created by an always-running thread. This may be a counting job rather than a build.
- **Rampages.** Needs `CDarkel`, which already will not hand an imported weapon back after one.
- **Unique stunt jumps.** `CStuntJumpManager` is a save block, so same treatment as the rest.
- **Vigilante, taxi, paramedic and firefighter missions.**

---

## Tier 5 — missions

Story missions, phone-booth missions, Vice City's property purchases. All blocked on the same
decision, and it is not negotiable: **mission state must not live in `main.scm`'s global space.** The
Scripts block is sized by that area, and there is no framing between save blocks, so growing it
silently shifts every block after it. Keep mission state in the sidecar, keyed by a mission id
*string* so adding a mission never renumbers an existing one.

---

## Tier 6 — world systems

III's elevated railway (a separate `CTrainNode` path system), the Callahan drawbridge, per-region
radio and audio zones, per-region weather and timecyc.

---

## Order

1. Restart points — small, self-contained, immediately felt.
2. Parked cars — reuses the mining and the vehicle spawning that already exist.
3. The region ledger — because Tier 2's garages and all of Tier 5 wait on it.
4. Pay 'n' Spray as plugin logic, then safehouse garages once the ledger exists.
