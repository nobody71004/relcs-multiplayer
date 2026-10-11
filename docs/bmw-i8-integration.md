# BMW i8 — vehicle model & door chime integration guide

## 1. Door chime (DONE — live in the game)

The vehicle door chime plays whenever the local player opens a car door
(ped state entering/carjack/exit). Sound file resolution order:

1. `mods/doorchime.wav`  — **your** recording (drop-in override)
2. `SFX/doorchime.wav`   — generated original fallback (2.1s bell chime)

Regenerate the fallback anytime:

    node tools/gen_doorchime.js SFX/doorchime.wav

Console commands (F7 console):

    doorchime          — test-play the chime
    doorchime off      — disable auto-play on door open
    doorchime on       — re-enable

Implementation: `src/net/nethooks.cpp` (`NetGame_DoorChimeTick`, `PlayDoorChime`).
Uses winmm `PlaySoundA` (SND_ASYNC — never blocks the game loop).

### On the audio itself

The viral "i8 chime" is BMW's seatbelt/door warning sound. The literal
recording is BMW's property, and the XXXTentacion "SAD!" remixes built on it
are copyrighted music — neither can be ripped/redistributed here. What the
game ships is an **original** bell chime in the same style (descending
ding-dong figure), synthesized by `tools/gen_doorchime.js`. If you own a
licensed recording, just drop it in as `mods/doorchime.wav` and it takes
precedence.

## 2. Vehicle model conversion pipeline

reLCS (RenderWare / librw) consumes **`.dff` (model) + `.txd` (textures) +
`.col` (collision)** plus data-file entries. FiveM/GTA V mods ship as
**`.yft` / `.ytd`** (RSC7) — a completely different container. The bridge is
desktop 3D tooling; there is no headless converter.

### Recommended route (best quality)

1. **Get the model**: a FiveM-ready i8 (e.g. gta5-mods.com "2015 BMW i8
   (I12)" or similar). Check the mod page's license — most forbid
   redistribution, fine for personal use.
2. **Extract to FBX**: open the `.yft` in **CodeWalker** (RPF Explorer →
   Export) or **OpenIV** (Export to openFormats/FBX).
3. **Clean in Blender/3ds Max**: fix materials, delete GTA V-only
   extras (dirt maps, damaged LODs you don't need), scale to LCS units
   (1 unit = 1 meter), origin at model center, Z-up per RW convention.
4. **Export to RenderWare DFF**: 3ds Max + Kam's/GIMS scripts (RW 3.5
   target), or Blender + **DragonFF** (SA-era DFF export works with librw).
   Split into `hi`/`lo` LODs like other LCS vehicles if needed.
5. **Textures**: pack into a `.txd` with **Magic.TXD** or **txdgen**.
   DXT1/DXT3 compressed, power-of-two sizes.
6. **Collision**: build `.col` in **Collision File Editor** (or COLModel
   exporter) — box + convex hulls is enough for a car.
7. **Install**:
   - `DATA/DEFAULT.IDE` — vehicle line: id, `i8`, `i8`, car, handlingId, ...
   - `DATA/HANDLING.CFG` — handling line (start from a sporty coupe's line,
     e.g. the Banshee/Banshee equivalent, tune mass 1485 kg, 369 hp feel)
   - `DATA/CARCOLS.DAT` — color palette for the i8
   - put `i8.dff`/`i8.txd`/`i8.col` where the streaming IMG/CD layout
     expects them (reLCS reads the game's `models/`-style layout — follow
     what `CStreaming` loads; check `src/core/Streaming.cpp` name lookups)
8. **Network play**: vehicle model IDs must match across clients — add the
   IDE entry on every client or the net vehicle spawn will fail to resolve
   the model.

### Quick-and-dirty route (lower quality)

Download a **GTA San Andreas** i8 (e.g. gtainside.com "BMW i8 2018" —
already `.dff`/`.txd`!) and skip steps 2–5. SA DFFs are RW 3.x and load
with minor or no fixes. This is the fastest path to driving *something*
i8-shaped.

### Door chime in the model itself (optional)

To have the chime tied to the actual i8 model rather than any car, replace
the generic `NetGame_DoorChimeTick` trigger with a model check
(`me->m_pMyVehicle->GetModelId()` against your i8's IDE id) — a one-line
condition where the hook lives.
