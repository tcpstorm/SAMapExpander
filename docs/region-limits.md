# What limits how many regions fit

The question this answers: **how many imported cities can San Andreas hold, and what stops the next
one?** Two walls were named when this work started — the eight archive slots and the model id
ceiling. One of them is gone. The other turned out not to be where it was thought to be, and it is
not the model ids that bind first.

Every number below was read out of `gta_sa.exe` 1.0 US or counted out of the game's own `data/` and
`models/` trees. The addresses are given so it can be checked; nothing here is recalled.

---

## The short answer

| limit | vanilla | now | San Andreas takes | Vice City + Liberty City take | left |
|---|---|---|---|---|---|
| **archive slots** (`CStreaming::ms_files`) | 8 | **32** | 6 | 2 | **24** |
| **TXD ids** | 5000 | 5000 | 3687 | 1242 | **71** |
| **DFF ids** | 20000 (1369 free) | 26400 (**11568** free) | 14832 declared | 7238 | **4330** |
| **COL ids** | 255 | 255 | 251 | 0 | 4 |
| **IPL ids** | 256 | 256 | 190 | 0 | 66 |
| ped model infos | 278 | **1302** | 276 | 183 | 843 |
| time model infos (`tobj`) | 169 | 1193 | 169 | — | ~1000 |
| atomic model infos | 14000 | **one per model id** | ~13900 | 7000-odd | cannot run out |
| entity pools, col models, pointer nodes, TXD pool, object data | fixed | grown from the region files | — | — | sized to fit |

One thing found on the way that is not a limit at all: **`ModelIdLimit` was growing the savegame's
Streaming block by exactly the number of ids it added**, and San Andreas' savegame has no framing,
so every block after it was being read at the wrong offset by anything with a different limit — or
by vanilla. It is fixed, the fix is checked mechanically, and section 2 has the detail.

**How many regions fit now: three, and the third is tight.** The archive table is no longer the
constraint and neither is the model id range. **The texture dictionary range is**, with 71 ids left
after two cities, and the reason it binds is not the number of textures a region has — it is the
number of *dictionaries* the importer splits them into. That is the cheapest thing left to change,
and it is a data change rather than a patch. See "What to do next" at the end.

---

## The one fact everything rests on: it is a single budget

San Andreas does not have a model id space and a texture id space. It has **one flat numbering of
every streamable resource**, and each kind gets a fixed range of it:

```
DFF   0      (20000)      models
TXD   20000  (5000)       texture dictionaries
COL   25000  (255)        collision files
IPL   25255  (256)        streamed IPL sections
DAT   25511  (64)         path data
IFP   25575  (180)        animation files
RRR   25755  (475)        car recordings
SCM   26230  (82)         external scripts
                (4)       the four streaming-list heads
                26316 entries in all
```

Every one of those is an entry in `CStreaming::ms_aInfoForModel` (0x8E4CC0, 0x14 bytes each).
`ModelIdLimit` grows the DFF range and shifts everything above it up by the same amount — that is
what the 1407 sites in `src/limits/ModelLimitSites.h` are for — but it grows **nothing else**. TXD
stays 5000 wide, COL 255, IPL 256, whatever the limit is.

### And the budget has a hard ceiling of 32768 entries

Each entry carries three `int16` links into that same array — `m_nNextIndex` at +0, `m_nPrevIndex`
at +2, `m_nNextIndexOnCd` at +4 — and three instructions read one back **sign-extended** and use it
as an index:

| address | function | instruction |
|---|---|---|
| 0x408B22 | `CStreaming::RemoveFromList` | `movsx eax, word ptr [edi + 0x8E4CC0]` then `lea edx, [eax + eax*4]`, `[ecx + edx*4]` |
| 0x408B3E | `CStreaming::RemoveFromList` | `movsx eax, word ptr [edi + 0x8E4CC2]`, same shape |
| 0x40CEDB | `CStreaming::RequestModelStream` | `movsx esi, word ptr [ebx + 0x8E4CC4]` |

So the largest index the game can follow is **32767**, and it is the number of **entries** that has
to fit inside it, not the number of models. `CEntity::m_nModelIndex` at +0x22 is a sign-extended
`int16` too and gives the same 32767, but it is not the binding one: the streaming array runs out
first, because the ranges above the models are in it as well.

**The DFF ceiling is therefore 32768 − 6316 = 26452, not 32767.** `ModelIdLimit` now derives that
rather than carrying a hand-picked number. At the 26400 this ships with, **there are 52 entries left
in the whole of San Andreas' resource id space.** There is no headroom between 26400 and 32767 to go
and get; the space between those two numbers is already full of texture dictionaries and collision
files.

That is why "give the TXD range more ids" is not an option, and why the model-id work below had to
come from somewhere other than the top of the range.

---

## 1. Archive slots: 8 → 32 (fixed)

**What it was.** `CStreaming::ms_files` is 8 × 0x30 at 0x8E48D8, ending at 0x8E4A58:

```
+0x00  char name[0x28]      the archive path, copied in without room for a terminator
+0x28  bool notPlayerImg
+0x2C  int  streamHandle    CdStreamOpen's return - the stream index, already << 24
```

San Andreas spends six before a region is asked for: `gta3` and `gta_int` in
`CStreaming::InitImageList` (0x4083C0), `carrec`, `script` and `cutscene` from `gta.dat`, and
`player.img` in `CGame::Init2`. Two cities took it to eight, and `CStreaming::AddImageToList`'s
answer to a full table is `xor eax, eax; ret` — **it returns slot 0**. A third region's archive
would not have failed; it would have been read as `gta3.img`.

**The HOODLUM thunk was not an obstacle.** 0x407610 is `jmp 0x1567B90`, and 0x1567B90 is
`AddImageToList` in full, in ordinary readable code:

```
xor edi, edi
mov eax, 0x8E48D8            ; ms_files
loop:  cmp byte [eax], 0     ; an empty name is a free slot
       je  found
       add eax, 0x30 / inc edi
       cmp eax, 0x8E4A58     ; one past the last entry
       jl  loop
xor eax, eax / ret           ; full: hand back slot 0
```

Three of the 27 sites that name the table are in that stub, and two more are in
`CStreamingInfo::GetCdPosnAndSize` (0x1560E50, from 0x4075A0) and `CStreaming::RequestFile`
(0x15663B0, from 0x40A080). Each stub was found from the thunk that reaches it, not from a byte
scan.

**Why it stops at 32.** `ms_files` is not the only fixed array in the path. `CdStreamOpen`
(0x1564A90, from 0x4067B0) scans `gStreamFileHandles` — 32 dwords at 0x8E4010, `cmp esi, 0x20; jl` —
for a free handle, copies the name into `gStreamNames`, 32 × 0x40 at 0x8E4098, and returns the index
shifted left 24. `CdStreamRead` (0x156C2C0) takes it back apart with `shr edx, 0x18`. So the
encoding would carry 256 archives, and the image id `CStreamingInfo` keeps (a byte at +7, read with
`movzx` at 0x407570) would too — but a 33rd archive writes its handle over `gNumChannels` at
0x8E4090 and its name over the global at 0x8E4898. **32 is the honest ceiling for this change and 32
is what it takes.** Behind it, 255 is structural; reaching it means moving those two CdStream arrays
as well, which is a small table by comparison.

**How it is guarded.** 27 operands, 22 naming the base and 5 a loop's end bound, each checked to
still hold the expected value before anything is written and put back if any refuses the new one.
Two neighbours had to be told apart from the array rather than moved with it: **0x8E4A58 is one past
the last entry and also a live byte** `CStreaming` reads and writes in six places, and **0x8E4A60 is
`ms_channels`**, whose index arithmetic (`add edi, 0x8E4A60`) is an immediate in exactly the window
an end bound falls in. An end bound is only ever an immediate a `cmp` closes a walk with, naming a
real field of the entry one past the last. `tools/verify_archive_slot_sites.py` checks the table and
the executable against each other in both directions and confirms none of the 20 neighbouring
references is in either table.

Two details that would otherwise bite: the block holds **one entry more than the count**, because
`CStreaming::LoadCdDirectory` reads the next entry's first byte (`mov al, [esi+0x30]`, 0x5B8318)
before it checks its cursor against the end; and the block is **refused above 2GB**, because
`gta_sa.exe` is `IMAGE_FILE_LARGE_ADDRESS_AWARE` and every walk over the table compares its cursor
with `jl` or `jge`.

**Savegame effect: none.** No block `CGenericGameStorage::GenericSave` writes mentions `ms_files`.
The Streaming block (0x5D29A0) is a byte per model slot out of `ms_aInfoForModel`, and the image id
lives in that array's +0x7, which is not part of what `Save` writes. Archive slots are assigned
fresh on every start from the ini's order, exactly as they were with eight.

---

## 2. Model ids: 7769 free → 11568 free (raised, but not from the top)

**Where the extra came from, since the top of the range is full.** San Andreas' own IDE files
declare 14832 models and the highest id any of them uses is **18630** — so the ids are not
contiguous. Counting them out of the game's `data/` tree:

```
free ids in 0..19999                          5168
  of those, at or above 18631                 1369   <- the window FreeModelIdStart already used
  of those, below 18631                       3799   <- new
longest free runs below the window
  1118 ids  11682..12799
   935 ids  15065..15999
   492 ids  13891..14382
   337 ids   6526..6862
   209 ids  16791..16999
```

Those 3799 cost nothing to take: no operand is patched and no array is moved, because **an id with
no `CBaseModelInfo` is an id the game has no way to name**. Free ids go from 7769 to 11568, a 49%
rise, for no patching at all.

They are a **fallback**, taken only when `FreeModelIdStart..FreeModelIdEnd` is exhausted, and that
is deliberate:

- a region that fitted in the window gets exactly the ids it got before, so adding a third city
  moves nothing the first two hold — the property `docs/save-compatibility.md` asks for;
- taking them last means taking as few as the regions need, which matters because the allocator's
  `!ModelInfoAt(id)` test only sees the models that exist when it looks. A mod that adds models at
  ids of its own *after* this plugin loads could still collide;
- `FreeModelIdGapStart` is the floor and defaults to **3000**, clear of everything San Andreas
  addresses by number rather than by name: peds 0..312, weapon models 321..373, vehicles 400..611,
  and the 2000 `CPopulation`'s ped groups use as an empty-slot sentinel. There are only 78 free ids
  below 3000 in any case. `-1` turns the fallback off.

### Savegame effect — and a bug that was already live

Looking at this turned up something that had nothing to do with the gaps. **`ModelIdLimit` was
growing the savegame's Streaming block**, and had been all along.

`CStreaming::Save` (0x5D29A0) and `CStreaming::Load` (0x5D29E0) walk the array writing and reading
one byte per slot:

```
mov esi, 0x8E4CC6            ; &ms_aInfoForModel[0].m_nFlags
...  write 1 byte  ...
add esi, 0x14
cmp esi, 0x9654B6            ; &ms_aInfoForModel[26316].m_nFlags
jl  loop                     ; 26316 bytes
```

Both of those operands are plain `ms_aInfoForModel` references, so they are in `kSites` as
`kInfoArray` entries (0x5D29A2 / 0x5D29D3 and 0x5D29E2 / 0x5D2A0A) — and the general rule shifts an
element at or above 20000 by the number of ids added. The end operand names element **26316**. So at
`ModelIdLimit = 26400` the loop was being repointed at element 32716 and **the block was 32716 bytes
where vanilla writes 26316** — 6400 bytes too many, with nine blocks behind it (PedType, TagManager,
IplStore, Shopping, GangWars, StuntJumps, EntryExits, Audio, 3dMarkers) and no framing of any kind
between them.

A save written and loaded under the same limit round-tripped, which is why it never showed. A
vanilla save loaded with the plugin on, or a save written with one limit and loaded with another,
read the whole tail of the file at the wrong offset.

**Fixed the way `PickupSlots.cpp` fixes it:** the two end operands get the new base and keep the
**vanilla count**, by address. The block is 26316 bytes again, in the same order, from the same
offsets, and every block after it lines up. `tools/verify_model_limit_sites.py` now proves it — it
recomputes the span of both loops from the table and fails if either is not exempted in
`ModelLimits.cpp`. The plugin logs the same fact on start-up.

What it costs: the block now carries the flags of the relocated array's first 26316 slots — DFF ids
0..26315 — rather than vanilla's mixture of 20000 models and 6316 dictionaries, collision files and
IPLs. Under one configuration Save and Load are symmetric so it round-trips exactly; across
configurations the bytes land on slots that mean something else, but that is a flags byte whose
worst effect is a model held in memory that need not have been, and it was already true of every id
above 20000. If that ever matters, hooking both functions to write 0..19999 and then the shifted
20000..26315 keeps the meaning as well as the size — 26316 bytes either way.

What *does* tie a save to a configuration is unchanged and already documented: the ids a region gets
depend on which regions loaded before it, so removing a region renumbers the ones after it. That is
the ledger's job (`docs/save-compatibility.md`, section A), not this one's.

---

## 3. Texture dictionary ids: the wall that actually binds

**5000 ids, 3687 of them San Andreas' own.** Counted two ways and they agree:

```
named by San Andreas' own IDE files (txd column, plus txdp)   3011
.txd entries in gta3.img, gta_int.img and player.img          3661
union - the slots the game creates                            3687   of 5000   (74%)
```

Both routes create a slot. `CFileLoader::LoadObject` creates one for the txd column of every IDE
line through `CBaseModelInfo::SetTexDictionary`, and `CStreaming::LoadCdDirectory` creates one for
every `.txd` in every registered archive that has no slot yet (0x5B62A5: `FindTxdSlot`, and
`AddTxdSlot` when that answers −1).

Vice City brings **820** dictionaries and Liberty City **422**. 3687 + 1242 = 4929, so **two cities
leave 71 of the 5000**. A third city of any size does not fit.

**And nothing refuses the 5001st.** `CTxdStore::AddTxdSlot` hands back whatever `CPool::New` gives
it, and `LoadCdDirectory` turns the slot number into a streaming id by adding 20000 — `add eax,
0x4E20` at 0x5B62CE — **with no comparison against the collision base**. So dictionary 5000 is
numbered as collision slot 0, 5001 as collision slot 1, and so on into collision San Andreas does
use. `CColStore::AddColSlot` has exactly the same shape with 25000 at 0x5B6313.

This is why `CityLoader` raising the CTxdStore **pool** past 5000 — which it must, or `AddTxdSlot`
writes through a null pointer — makes the overrun reachable without making it visible. `IdSpace`
now reads the three pools' byte maps after `CStreaming::Init2` and says, in the log, how full each
range is and which range an overrun is taking the entries of.

**It cannot be fixed by giving TXD more ids.** There are 52 spare entries in the whole space.

---

## 4. COL and IPL ids

**COL: 255 ids, 251 of them San Andreas' own** (`.col` entries in its three archives) — 98% full,
four spare. A region's loose `.col` files cost none of them: `CityLoader` loads them into slot 0,
the one slot the streamer never touches, so the region's collision is permanently in memory. A
`.col` placed *inside* a region's IMG archive would cost one each, and there are four.

**IPL: 256 ids, and `CIplStore::Initialise` builds a pool of exactly 0x100** (`push 0x100` at
0x405F25, pool pointer at 0x8E3FB0). San Andreas has 190 streamed IPL sections in its archives and
52 `IPL` lines in `gta.dat`. A region's IPLs are loaded through `CFileLoader::LoadScene` rather than
streamed, so they cost none of these either — what they *do* cost is one entity-index array each,
and `CityLoader` already moves that 40-pointer table (`ms_pEntityIndexArrays`, 0x8E3F08) to the heap
with room for them.

**The IPL pool is deliberately left at 256**, and that is the interesting part. Growing it is a
one-operand change, but the slot number is turned into a streaming id by adding the IPL base, so a
257th slot takes the first path-data id. Growing the IPL *range* to match would need entries the
space does not have. The 256 stands until the space is bigger.

**Savegame effect of leaving it alone: the `IplStore` block is untouched.** `CIplStore::Save`
(0x5D5420) writes a 4-byte count of 0x100 and then walks `esi` from 0x34 to 0x3400 in 0x34 steps —
**255 bytes, from a hard-coded immediate, not from the pool's size**. Even if the pool were grown
the block would stay 4 + 255 bytes; and because San Andreas' own IPLs are allocated before any
region's, entry *n* of the block goes on meaning the same IPL it meant in vanilla. Nothing here
changes that.

---

## 5. The stores that are not in the id space

These are fixed arrays of model-info objects. They cost no ids, they are pure heap, and each has an
adder that never checks its own count — so a bigger array is the whole fix.

| store | vanilla | San Andreas uses | now | what spends it |
|---|---|---|---|---|
| `ms_atomicModelInfoStore` | 14000 | ~13900 | **one per model id** | every `objs` line |
| `ms_timeModelInfoStore` | 169 | 169 (exactly) | 1193 | every `tobj` line |
| `ms_pedModelInfoStore` | 278 | 276 | **1302** | every `peds` line |
| damage atomic store | 70 | most | 70 | `objs` lines with the damage flag |
| clump store | 92 | most | 92 | every `anim` line |
| weapon store | — | — | grown by `RegionWeapons` | every `weap` line |

Two were changed here. The **atomic store** used to be `14000 + (ids added above 20000) + 1000`,
which was right while region ids only came from above 20000 and is wrong now that they also come
from the gaps below it — 11568 region models against a store grown by 7400 would have run out before
the ids did. It is now **one entry per model id**, which no set of IDE files can exhaust, because
every id has at most one model info and peds, vehicles, `tobj` and `anim` lines have stores of their
own. It costs 0x20 bytes an id: 845KB at the ceiling. The **ped store** went from 278 + 512 to
278 + 1024, which is Vice City's 105 and Liberty City's 78 with room for three more cities of that
size, at 0x44 bytes a slot.

The **damage** (70) and **clump** (92) stores are still vanilla and still nearly full. Neither has
been the thing that ran out, but both are the same shape as the time store and could be moved the
same way if a region ever needs many animated or damageable models. `CityLoader`'s per-region log
line names whichever store refused a line.

The entity pools, the col model pool, the pointer-node pools, the TXD pool and the object-data table
are all already sized from what the region files actually hold, per region, before
`CPools::Initialise` runs. They scale with the regions and are not counted here.

---

## 6. Five regions: the arithmetic

Vice City is the largest of the five the project wants. Carcer City, Bullworth and a Warriors level
are all smaller. Taking Vice City's 4131 models as the worst case and something like
2500 / 2000 / 1500 for the other three:

| limit | after VC + LC | 3 more regions need | verdict |
|---|---|---|---|
| archive slots | 24 free | 3 | **fits, 21 spare** |
| DFF ids | 4330 free | ~6000 | **binds at four regions** |
| TXD ids | 71 free | ~1000 at today's dictionary counts | **binds at three** |
| COL ids | 4 free | 0 (slot 0) | fits |
| IPL ids | 66 free | 0 (LoadScene) | fits |
| ped model infos | 843 free | ~300 | fits |
| atomic model infos | cannot run out | — | fits |
| entity pools, col models, nodes | grown per region | — | fits |
| world grid | five maps in 9936 units | — | fits (already solved) |

**So map count is still the thing that limits the project, and the id space is why.** The archive
wall is genuinely gone. The next two walls are both inside the 32768-entry budget, they are 52
entries from full between them, and there is nothing left to reallocate.

---

## What to do next, in the order the payoff justifies

### A. Fewer texture dictionaries per region — a data change, no patching

Vice City splits 4131 models across **820** dictionaries: about five models each. San Andreas does
the opposite — one dictionary per area, shared by hundreds of models. Merging a region's textures by
area in `tools/import_city.py` would take Vice City and Liberty City from 1242 dictionaries to
something like 130, which removes the TXD wall for five regions outright and costs nothing in the id
space.

The trade is streaming granularity: a merged dictionary is loaded whole when any of its models is
requested, so merging must follow the area grouping the model names already carry rather than being
done wholesale. This is the cheapest large win available and it needs no site table.

### B. Make the streaming index unsigned — the only way to make the budget bigger

Flipping the three `movsx` at 0x408B22, 0x408B3E and 0x40CEDB to `movzx` is one byte each
(`0F BF` → `0F B7`, same length) and would take the space to 65535 entries. That is not the whole
job, and this is the honest statement of what is left:

- **−1 is a live sentinel in those fields.** `CStreaming::RemoveFromList` writes it into both links
  when it unlinks an entry, and `m_nNextIndexOnCd` carries it to end a walk. The tests that compare
  a *word* against −1 (`cmp word ptr [eax + 0x8E4CC0], -1` at 0x4098C8, 0x409978, 0x409A39) keep
  working unchanged, because −1 and 0xFFFF are the same sixteen bits. The tests that compare the
  *sign-extended 32-bit register* against −1 do not, and each of those would have to become a 16-bit
  test — which is an instruction rewrite, not an operand rewrite.
- **28 `lea` sites compute a pointer into the array and read the links through it**, so a sweep for
  instructions that name the array by displacement does not find every read. The dataflow has to be
  followed, or the three functions involved replaced outright.
- `CEntity::m_nModelIndex` at +0x22 is sign-extended too, so **model ids would still stop at 32767**
  even with the streaming index unsigned. That is a real ceiling and widening the field is a
  different and much larger job — it changes `CEntity`'s layout, and every pool in the game is sized
  from it.

What it would buy, precisely: DFF to 32767 (+6367 ids over today's 26400) and the TXD range to
whatever is wanted out of the remaining ~32000 entries. That is enough for five regions with room
over, and it is the only change that makes the budget bigger rather than moving it about. It is a
bounded job — three reads, their tests, and three functions' worth of dataflow — but it is a
separate one, and it should be done with the same scan-and-verify machinery the archive table got,
not by hand.

### C. If a region ever needs streamed collision or its own IPL sections

The COL range has four ids and the IPL range 66. Both are one-operand pool changes and both need
range room the space has not got, so both wait on B.

---

## What to look for in the log

On a good start, with two regions:

```
archive slots: CStreaming::ms_files 8 -> 32 entries at 0x... (27 site(s) patched); San Andreas
  spends six on gta3, gta_int, carrec, script, cutscene and player.img, so 26 are left for the regions
model id limit 20000 -> 26400: txd 26400, col 31400, ipl 31655, ... 32716 streaming entries
models: 7769 free id(s) in 18631..26399, and 3799 more in the gaps San Andreas' own IDEs leave in
  3000..18630 - 11568 in all; store headroom atomic ..., ped ...
[Vice City] img: MapExpander\... registered as archive 7 (7/32 slots used)
id space: 32716 entries in all, of a hard ceiling of 32768 - DFF 26400, TXD 5000, COL 255, IPL 256,
  ... and there are 52 entries spare
id space: DFF uses ... of 26400 id(s) ...
id space: TXD uses 4929 of 5000 id(s) (98%), highest slot 4928, pool 6498 entries
id space: TXD has 71 id(s) left of 5000 - the next region will not fit in it
id space: COL uses 252 of 255 ...
id space: IPL uses 191 of 256 ...
```

### What each failure looks like

| line | what happened | what to do |
|---|---|---|
| `ERROR: the archive table was not enlarged: 0x... holds 0x..., expected 0x...` | another mod patched `ms_files`, or this is not the 1.0 US HOODLUM executable | the game is left completely vanilla at 8 slots; run `tools/verify_archive_slot_sites.py` against your exe to see which site moved |
| `ERROR: the archive table was not enlarged: archive 0 is already registered` | the plugin loaded after `CStreaming::InitImageList` | another ASI is loading first and initialising streaming; nothing is patched |
| `ERROR: the archive table was not enlarged: the heap gave back 0x8...` | the block landed above 2GB, where the signed loop bounds invert | restart; if it persists, set `ArchiveSlotLimit = 8` |
| `... NOT registered - CStreaming::ms_files is full` | more than 31 archives asked for | fewer archives, or a region has too many `Img` entries |
| `ERROR: id space: TXD has N slot(s) past the end of its 5000-id range` | **the range overran; texture dictionaries are taking collision files' streaming entries** | drop a region, or merge that region's dictionaries — see A above. Expect missing or wrong collision until it is fixed |
| `ERROR: id space: COL has N slot(s) past the end of its 255-id range` | a region put `.col` files inside its IMG archive | move them out and list them under `Col`, which loads into slot 0 |
| `models: the free id range ... ran out, and so did the ... gaps behind it` | out of model ids entirely | nothing to raise — `ModelIdLimit` is at the ceiling. Drop a region |
| `[region] models: ... dropped (model-info store full)` | the damage (70) or clump (92) store ran out | those two are still vanilla-sized; see section 5 |
| `ModelIdLimit N clamped to 26452` | asked for more ids than the id space holds | expected; the message says why |

`tools/verify_archive_slot_sites.py <gta_sa.exe>` must pass 100% before the archive table ships, and
`tools/scan_archive_slot_sites.py` regenerates it. Both run in a throwaway container like every other
tool here.
