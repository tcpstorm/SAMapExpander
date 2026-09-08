# The WidescreenFix crash at +0xE550, and why PackRegions brought it out

Status: **cause fully established from disassembly; fix written but NOT COMPILED** (the Docker
toolchain engine was down on the machine this was worked out on, so nothing here has been built or
run). Everything below is from static analysis of `gta_sa.exe` (1.0 US HOODLUM) and
`GTASA.WidescreenFix.asi` (329216 bytes, 2024-07-15), plus the register/stack dump in
`MapExpander.log`.

## What the faulting code is

`GTASA.WidescreenFix.asi` is a normal PE32, ImageBase 0x10000000, 5 sections,
`.text` RVA 0x1000 at file offset 0x400. It was loaded at 0x67C00000, so the faulting address
0x67C0E550 is RVA 0xE550 = **file offset 0xD950**.

The function containing it starts at RVA 0xE4E0 (file 0xD8E0) and ends at RVA 0xEB9B with

```
push [ebp+18h] / push [ebp+14h] / push [ebp+10h] / push edi / push esi
call dword ptr [0x1004E114]      ; the saved original
add esp, 14h / pop edi / pop esi / mov esp, ebp / pop ebp / ret
```

Five stack arguments and a `__cdecl` return: it is a replacement for
**`CSprite2d::SetVertices(const CRect&, const CRGBA&, const CRGBA&, const CRGBA&, const CRGBA&)`**
(`0x00727420`). WidescreenFix installs it by rewriting the `call` at **`0x00728360`**, which is
inside `CSprite2d::Draw(const CRect&, const CRGBA&)` at `0x00728350`:

```
00728350  mov  eax, [esp+8]        ; the CRGBA
00728354  push esi
00728355  push eax  x4             ; the four corner colours
00728359  mov  eax, [esp+18h]
0072835D  push eax                 ; the CRect
0072835E  mov  esi, ecx            ; this  <-- ESI is the CSprite2d
00728360  call 00727420            ; <-- WidescreenFix redirects this
00728365  mov  eax, [esi]          ; this->m_pTexture
00728367  add  esp, 14h
0072836A  test eax, eax
0072836C  pop  esi
0072836D  jnz  0072837D            ; textured path
0072836F  nop
00728370  jmp  00409B78            ; NULL texture: untextured path. THE GAME CHECKS.
```

The hook's prologue is `push ebp / mov ebp,esp / sub esp,14h / push esi / push edi /
mov [ebp-4],0 / mov [ebp-4],esi` — it **captures the caller's ESI**, i.e. `this`, which is only
valid because of the `mov esi, ecx` two bytes before the call site.

Then, disassembled by hand from file 0xD8F2:

```
RVA E4F2  mov  esi, [ebp+8]                 ; the CRect
RVA E4F5  mov  edi, [ebp+0Ch]               ; the CRGBA
RVA E4F8  cvttss2si eax, [esi]              ; left    > 0  -> passthrough
RVA E504  cvttss2si eax, [esi+0Ch]          ; bottom  > 0  -> passthrough
RVA E511  cvttss2si eax, [esi+8]            ; right   < screen width  -> passthrough
          mov  ecx, [0x1004E058]            ; WidescreenFix's resolution record
RVA E525  cvttss2si eax, [esi+4]            ; top     < screen height -> passthrough
RVA E533  mov  ecx, [ebp-4]                 ; the captured CSprite2d
RVA E538  test ecx, ecx / je passthrough    ; checks the sprite ...
RVA E53E  mov  ecx, [ecx]                   ; ... but NOT m_pTexture
RVA E540  add  ecx, 10h                     ; &RwTexture::name  (RwTexture: raster+0, dict+4,
                                            ;   link+8, name[32] at +0x10)
RVA E550  mov  al, [ecx]  <-- FAULT         ; strlen loop
RVA E552  inc ecx / test al,al / jnz E550
RVA E55D  push &{ptr,len} / push 0x10035B90 / push 0x10035590
RVA E56A  call <find>                       ; 384 `const char*` in [0x10035590, 0x10035B90):
                                            ;   "10ls", "backgnd", "black", "butnA", ... the
                                            ;   HUD/menu/tag texture names
RVA E572  cmp eax, 0x10035B90 / je passthrough
```

So: **the special path is taken only for a rectangle that covers the entire screen**, and on that
path WidescreenFix reads `this->m_pTexture->name` with no null check on `m_pTexture`. Ninety bytes
later, at RVA 0xE5EE, the *same* function does have the check
(`mov eax,[ebp-4] / test / je ; mov ecx,[eax] / test ecx,ecx / je ; mov ecx,[ecx]` then
`[ecx+0x28]`, `[ecx+0x2C]` = `RwRaster::originalWidth/Height`) — it is only the name lookup that
is missing it.

That is an exact match for the crash record:

```
exception 0xC0000005 reading 0x00000010, ecx 00000010, edx 00000011 (= lea edx,[ecx+1])
eax 0000043D = 1085 = (int)rect.top, left over from the cvttss2si at RVA E525
[ebp-4] = 0x00BAB378 (esp+18h in the dump), and the saved esi at esp+4 is the same
rect at 0177FC68 = { -5.0, 1085.0, 1925.25, -5.0 }, colour at 0177FC60 = 0xFFFFFFFF
```

## Why `m_pTexture` was null

`this` = **0x00BAB378**. Working out where that came from:

* The immediate caller is the `call CSprite2d::Draw` at **`0x0059001B`**, which does
  `mov ecx,[0x008D093C] / lea ecx,[ecx*4 + 0x00BAB35C]`. So `this = &gSplash[gSplashIndex]` and
  `gSplashIndex == 7`.
* `gSplash` is built by the vector constructor call at `0x0084F2DA`:
  `push 0x007281E0 (dtor) / push 0x00727230 (ctor) / push 7 / push 4 / push 0x00BAB35C`.
  **Seven entries.** The destructor loop at `0x00590CB4`..`0x00590CD4` agrees:
  `mov esi, 0x00BAB35C ... add esi,4 / cmp esi, 0x00BAB378 / jl`.
* So **0x00BAB378 is one element past the end of a seven-element array**, and the four bytes there
  read zero.

`gSplashIndex` (`0x008D093C`) is advanced in two places:

* `0x005904D0` — `cmp dword ptr [0x008D093C], 6 / je out` in front of its `inc`. Bounded.
* `0x00590ADE` — a bare `inc dword ptr [0x008D093C]`. **Not bounded.** It sits in the routine at
  `0x00590AC0`, which takes one argument: non-zero sets the fade flag at 0x00BAB31F, zero
  increments the index; either way it then runs 20 frames of the splash draw at `0x0058FF60`.
  Its only two callers are `0x00590D7C` (arg 1) and `0x00590DB3` (arg 0).

The caller of those, at `0x00590D40`, is the loading screen's tick. It reads the millisecond clock
(`call 0x00619410`), scales it by `[0x00858CDC] = 0.001` to seconds, subtracts the last advance time
at `[0x00BAB340]`, and advances when the gap passes `[0x00859B4C] = 5.5` for splash 0 or
`[0x00858C80] = 5.0` for every one after.

**Seven screens at 5.5 + 6 x 5.0 seconds is a 35.5-second budget. The next advance, 40.5 seconds
into a load, writes 7 and the draw indexes past the array.**

Vanilla San Andreas never loads that long, and when it does it survives, because
`CSprite2d::Draw` checks `m_pTexture` at `0x00728365` and takes the untextured path. WidescreenFix,
hooked one instruction earlier, does not.

The stack in the crash log confirms the whole chain:
`CFileLoader::LoadLevel (ret 0x005B92C2, arg = "DATA\MAPS\interior\gen_int2.IPL") ->
f@0x005B36B5 -> the tick f@0x00590DB3 -> the advance f@0x00590B1D -> the draw f@0x0059001B ->
CSprite2d::Draw (ret 0x00728365) -> WidescreenFix+0xE550`.

## Where PackRegions comes in

Nowhere directly. Packing only rewrites `MapRegion::originX/originY` in `Settings::PackRegions`;
it never touches the loading screen, `gSplash`, or `0x008D093C`. What it changes is **how long the
load takes**, and 40.5 seconds is a cliff:

* This plugin's load is already long - the log has the region models finishing at 48.7 s and the
  crash at 84.1 s, and the loading pass that crashed had been running for roughly 40 seconds.
* Packing takes the sector from 121 units to 85 (`world 14488 -> 10236`), so every one of the
  17803 region placements plus San Andreas' own 45884 is filed into more sectors - the pointer-node
  pool is already grown to 213448 - which makes the load slower, not faster.

So packing is what pushed this particular run over the cliff, but it is not the bug and turning it
off is not a fix: a bigger region list, a slower disk or a fourth city would cross 40.5 seconds on
its own.

Hypotheses that were considered and are **ruled out** by the above: that WidescreenFix caches
anything derived from map extents; that it iterates a structure our sector constants reshape; that
it reads an empty sector or quadtree entry; that our packing runs at a moment that leaves something
half-initialised. None of them are involved - the only input to the fault is `gSplashIndex == 7`.
`0x43D` in eax is not an index into anything, it is `(int)rect.top` = 1085 = screen height + 5.

## The fix

`src/patches/LoadingScreenFix.cpp` replaces the six bytes of the unbounded
`inc dword ptr [0x008D093C]` at `0x00590ADE` with `call rel32` + `nop` to a stub that does
`if (index < 6) index++`, i.e. bounds it exactly the way `0x005904D0` already bounds the other
advance. The last loading screen then stays up for the rest of the load.

eax, ecx and edx are dead across the site (the game reloads them at `0x00590AF0`, `0x00590B0D` and
`0x00590B3D`), the instruction returned into is `mov esi, 14h` which reads no flags, and esi/edi/ebx
are callee-saved, so a plain `__cdecl` C function is safe there. `MemPatch::ReplaceBytes` verifies
the original bytes first, so if another mod has already patched the site we log and stand down.

Deliberately *not* done: a general null-texture guard on `CSprite2d::Draw`. That would suppress
legitimate untextured full-screen quads (fades), and there is no evidence of a second site.

## What still needs doing

1. **Build it.** Nothing here has been compiled - the Docker Linux engine was returning
   `Internal Server Error` and `com.docker.service` was stopped.
2. **Test with WidescreenFix installed and `[World] PackRegions = 1`**, which is the configuration
   that fails. Expect: no crash, the last of the seven loading screens held for the remainder of
   the load, and the new `loading screen:` line in `MapExpander.log`.
3. Worth confirming the same build still starts with `PackRegions = 0`.

Unrelated bug fixed in passing while here: `src/core/Main.cpp` had
`if (...extendWorldSectors) WorldSectors::Install(); MapWindow::Install();` with no braces, so
`MapWindow::Install()` ran unconditionally. Braced. It is **not** implicated in this crash.
