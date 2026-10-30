# reLCS / reStories — Deep Bug Scan Report

**Target:** `github.com/knackers4/res`, branch `relcs` (commit `83ae2fe`) — the live, public successor to reLCS.
**Scope:** 523 engine source files, 283,484 lines of C++ (`relcs/src`), vendor libs excluded.
**Date:** 2026-10-29
**Status:** P0, P1 and the safe P2 fixes have been **applied** — see [§9](#9-fixes-applied-2026-10-29) for the change list, verification and what was deliberately left alone.

---

## 1. About the "OpenLCS build"

There is **no publicly verified leak of LCS source code**. What circulates under the "OpenLCS" name
(`github.com/ZeptoBST/reLCS`) is an install note pointing at `Lcs_dist.7z`, which is *game data*, not
engine source — and redistributing that is copyright infringement. It is not a bug-fixable codebase,
and using it will not help you fix reLCS.

The real, fixable source is the one this report is based on: **reStories**, which is the actively
maintained continuation of the abandoned reLCS work. It ships **no game assets** and requires a
legally owned PS2 copy run through its asset converter. That is the correct base for bug work.

## 2. Method (and what this report is not)

| Step | What was done |
|---|---|
| Static analysis | `cppcheck 2.14` over all 20 engine directories — `--enable=warning,performance,portability --inconclusive`, 1,550 findings |
| Manual verification | Every P0/P1 below was read in source and traced to a caller or a compile-time flag before being ranked |
| Not done | **No compilation and no runtime testing.** This project builds only with MSVC + Visual Studio 2022 (premake) against the DirectX 8/9 SDK, which is not available here. All findings are static + hand-traced; none are confirmed by a live crash. |

Raw output: [`_scratch/lcs-scan/all.txt`](_scratch/lcs-scan/all.txt) (1,550 rows), per-directory files alongside it.

**Finding counts by class:** 51 error-level, 369 uninitialised-member warnings, 702 pointer-cast
portability warnings (normal for this codebase), 33 unsafe `sscanf`, 30 possible null derefs,
29 conditional out-of-bounds, 14 un-terminated `strncpy`.

Several findings are **latent only** because they sit behind flags that are *off* by default
(`MORE_LANGUAGES`, `FIX_INCOMPATIBLE_SAVES`) or *on* by default and thereby mask the bug
(`FIX_BUGS`, defined at `src/core/config.h:290`). Those are marked **[latent]** — do not "fix" them
blindly, because the `FIX_BUGS` path is the intended one and undefining it is documented as
release-build-only.

---

## 3. P0 — live memory corruption / crashes

### P0-1 `MAX_STREAMS` is 1, but the game uses stream index 1
**`src/audio/sampman.h:130`** · affects `sampman_miles.cpp:2604-2612`, `sampman_oal.cpp:2100-2103`

```c
#define MAX_STREAMS 1
```

`nStreamVolume[MAX_STREAMS]`, `nStreamPan[MAX_STREAMS]`, `mp3Stream[MAX_STREAMS]` and `aStream[]`
are all sized from this. But `PolRadio.cpp:209-211` — the police-radio / mission-audio path — does:

```c
SampleManager.PreloadStreamedFile(g_nMissionAudioSfx, 1);
SampleManager.SetStreamedVolumeAndPan(MAX_VOLUME, 63, TRUE, 1);   // stream 1
SampleManager.StartPreloadedStreamedFile(1);
```

and the setter then writes `nStreamVolume[1]` and `nStreamPan[1]` — **one element past the end of two
file-scope globals**. The dead branch at `sampman_miles.cpp:2611` (`if (nStream == 1 || nStream == 2)`)
shows the author expected at least three streams; this is a re3 leftover (re3 only ever had one stream).

**Fix:** raise the count to cover every index the code actually uses.

```c
// 0 = user MP3 track, 1 = police radio / mission audio, 2 = reserved by SetStreamedVolumeAndPan
#define MAX_STREAMS 3
```

Re-test mission audio and police radio afterwards: `MAX_DIGITAL_MIXER_CHANNELS` (same header) is
derived from this, so the Miles mixer gets two more digital channels. That is the only behavioural
side effect, but it is worth an audio smoke test.

### P0-2 `CPhoneInfo::GrabPhone` indexes `m_aPhones[-1]`
**`src/control/Phones.cpp:267-289`** · caller: `src/control/Script3.cpp:816` (script opcode `024A`)

```c
int nearestPhoneId = -1;
...
for (int phoneId = m_nScriptPhonesMax; phoneId < m_nMax; phoneId++) {
    if (phoneDistance < nearestPhoneDist) { ... nearestPhoneId = phoneId; }
}
m_aPhones[nearestPhoneId].m_nState = PHONE_STATE_MESSAGE_REMOVED;   // -1 if nothing matched
```

If no booth is within 100 units, **or** if the loop body never runs because
`m_nScriptPhonesMax == m_nMax` (every phone already handed out), `nearestPhoneId` stays `-1`
and the function reads **and writes** `m_aPhones[-1]`, then performs the swap at the end, which
propagates the corruption. A mission calling `024A` twice in the same area is enough.

**Fix:**

```c
int CPhoneInfo::GrabPhone(float xPos, float yPos)
{
    if (m_nScriptPhonesMax >= m_nMax)
        return -1;                       // pool exhausted
    int nearestPhoneId = -1;
    ...
    if (nearestPhoneId == -1)
        return -1;                       // no booth in range
    ...
}
```

`Script3.cpp:816` already stores the result with `SET_INTEGER_PARAM`, so a `-1` return is a valid
script-visible sentinel and needs no caller change.

### P0-3 `CToggleTargetMarker` writes `ms_RadarTrace[-1]`
**`src/core/Radar.cpp:1565-1583`** · inside `#ifdef MAP_ENHANCEMENTS`, which **is** defined (`config.h:374`)

```c
for (nextBlip = NUMRADARBLIPS-1; nextBlip >= 0; nextBlip--)
    if (!ms_RadarTrace[nextBlip].m_bInUse) break;

if (nextBlip == 0)
    return;

ms_RadarTrace[nextBlip].m_eBlipType = BLIP_COORD;   // nextBlip == -1 if all 75 blips are used
...
```

When the blip pool is full the loop ends with `nextBlip == -1`, the guard (`== 0`) does not fire, and
**twelve consecutive writes** land before the array.

**Fix:**

```c
if (nextBlip <= 0)
    return;
```

### P0-4 Cutscene animation lookup is dereferenced without a null check
**`src/animation/CutsceneMgr_overlay.cpp:230`**

```c
CStreaming::ImGonnaUseStreamingMemory();
pNewAnim = ms_cutsceneAssociations.CopyAnimation(animName);
CStreaming::IHaveUsedStreamingMemory();

pNewAnim->SetCurrentTime(0.0f);        // nil when the anim name is not in the group
```

`CAnimBlendAssocGroup::CopyAnimation(const char*)` explicitly `return nil` when `GetAnimation`
misses (`AnimBlendAssocGroup.cpp:76-84`). The original re3 code has exactly this null check, and it is
sitting **commented out directly above** (lines 217-228) along with the `Can't find the fucking
animation` diagnostic. Any `.CUT` referencing an animation absent from the `.IFP` crashes the game.

**Fix** — insert after `IHaveUsedStreamingMemory()`:

```c
if (pNewAnim == nil) {
    debug("Give cutscene anim %s: not in assoc group\n", animName);
    return;
}
```

The streaming-memory token is already balanced by then, so an early return is safe.

---

## 4. P1 — live logic bugs and out-of-bounds reads

### P1-1 Path reconstruction typo silently breaks ped/vehicle pathing
**`src/control/PathFind.cpp:131` and `:135`**

```c
else if (nodeIdX > 39 && pathNodes[nodeIdX + 1][nodeIdY].id + 5 == pPathNode->id)
...
else if (nodeIdY > 39 && pathNodes[nodeIdX][nodeIdY + 1].id + 5 == pPathNode->id)
```

Both should be `< 39` — every sibling branch eight lines below uses `nodeIdX < 39` / `nodeIdY < 39`
for exactly these `+1` neighbour accesses, and `nodeIdX`/`nodeIdY` are `uint8` in a `pathNodes[40][40]`
grid, so `> 39` is **never true**. Consequence: the +X and +Y neighbours are never matched during
path reconstruction, so `pointsFound` under-runs and actors follow truncated routes. It is also one
signed/unsigned slip away from a wild read.

**Fix:** `>` → `<` on both lines.

### P1-2 `NewGameRadioTimers` is one element too small
**`src/audio/MusicManager.cpp:37` and `:165-166`**

```c
uint32 NewGameRadioTimers[10] = ...;
...
for (int i = 0; i < TOTAL_STREAMED_SOUNDS; i++)
    if (i < STREAMED_SOUND_CITY_AMBIENT && isNewGameTimer)
        m_aTracks[i].m_nPosition = NewGameRadioTimers[i];
```

`eStreamedSounds` (`src/audio/audio_enums.h:39-52`) has **11** entries before `STREAMED_SOUND_CITY_AMBIENT`
(11 stations incl. `STREAMED_SOUND_RADIO_MP3_PLAYER`), so `i == 10` reads past the array end and seeds
the MP3 player's start position with whatever global follows.

**Fix:** size it from the enum.

```c
uint32 NewGameRadioTimers[STREAMED_SOUND_CITY_AMBIENT] = ...
```

### P1-3 `vfs_open_callback` underflows on short filenames and rewrites a const buffer
**`src/audio/sampman_miles.cpp:274-286`**

```c
if ((strcmp(Filename + strlen(Filename) - 4, ".adf") == 0) || ...)
...
strcpy((char*)Filename + strlen(Filename) - 4, ".mp3");
```

Two problems: any filename shorter than 4 characters makes `Filename + strlen - 4` point **before**
the buffer; and the `strcpy` writes through a `const char*` into memory owned by the Miles provider,
which is not guaranteed to be writable or large enough. Both fire on the radio path.

**Fix:** guard the length, and never write through the caller's pointer — build the replacement name
in a local buffer and hand *that* to `fopen`.

### P1-4 Uninitialised `pVehicle` used on a malformed save
**`src/core/Pools.cpp:157-190`**

```c
CVehicle* pVehicle;
if (type == VEHICLE_TYPE_BOAT) ...
else if (type == VEHICLE_TYPE_CAR) ...
else if (type == VEHICLE_TYPE_BIKE) ...
else assert(0);            // compiled out in release
...
pVehicle->Load(buf);
```

`COMPATIBLE_SAVES` is on (`config.h:292`), so the release build has no `else` at all — a save whose
vehicle-type byte is corrupt falls straight through to dereferencing the uninitialised pointer.

**Fix:** initialise to `nil` and `continue` on an unknown type instead of relying on `assert`.

### P1-5 `CHud::SetBigMessage` writes one past the row
**`src/renderer/Hud.cpp:2086-2095`**

```c
for (i = 0; i < 128; i++) {
    if (message[i] == 0) break;
    m_BigMessage[style][i] = message[i];
}
LastBigMessage[style][i] = 0;
m_BigMessage[style][i] = 0;
```

A message with no terminator in the first 128 wide chars exits with `i == 128` and writes
`[style][128]` — into the next style's row, or past the array for `style == 7`.

**Fix:** `const int end = Min(i, 127);` and terminate with `end`.

### P1-6 `CColModel::operator=` has no self-assignment guard
**`src/collision/ColModel.cpp:127`** · plus uninit `ownsCollisionVolumes` at `:9`

`a = a` runs `delete[] triBBoxes` (and the same for spheres/vertices) on the buffer it is about to
read from — a double-free. The operator also never copies `level` or `ownsCollisionVolumes`, so a
copied model's destructor may free volumes it does not own.

**Fix:** `if (this == &other) return *this;` at the top, copy the two missing members, and initialise
`ownsCollisionVolumes` in the constructor.

---

## 5. P2 — correct but worth fixing

| Location | Issue |
|---|---|
| `src/audio/oal/stream.cpp:86` | `PcmBuf = realloc(PcmBuf, size)` — old pointer lost on failure. Use a temp. |
| `src/core/CdStreamPosix.cpp:66,81`, `src/core/re3.cpp:1300`, `src/renderer/Console.cpp:96` | `va_start` without `va_end` → UB on some ABIs, leaks. |
| `src/core/ControllerConfig.cpp:141` (`buff`), `src/core/Frontend.cpp:3112` (`headerText`), `src/core/Camera.cpp:3605` (`&c`), `src/core/Streaming.cpp:756` (`success`), `src/core/re3.cpp:426` (`contKey`), `src/rw/TexRead.cpp:60,64` (`tex`) | Read before initialisation on some paths. |
| `src/audio/MusicManager.cpp:165` et al. (14 sites) | `strncpy` results used without forcing a terminator → over-read on `%s`. |
| `src/audio/AudioManager.cpp:745` | `AddDetailsToRequestedOrderList(uint8 sample)` never validates `sample` against `NUM_CHANNELS_GENERIC` before `m_aRequestedQueue[...][sample]`. Currently safe only because every caller's loop bound happens to be correct. Add an assert/clamp. |
| 30 `nullPointerRedundantCheck` sites | `CarCtrl.cpp:482`, `Garages.cpp:2079`, `Phones.cpp:397`, `Script.cpp:1788`, `Script3.cpp:173/191/209`, `Fire.cpp:286`, `Frontend.cpp:3795/3857`, `CopPed.cpp:707`, `Ped.cpp:6886`, `PedAI.cpp:3207/3576/6519`, `PedFight.cpp:3134`, `Population.cpp:1378`, `Weapon.cpp:458/2489` — each needs a 30-second read to decide check-vs-deref order. |

## 6. Latent — only if you change the build flags

| Location | Bug | Guard |
|---|---|---|
| `src/control/OnscreenTimer.cpp:230` | `sprintf(suffix, "/%d", ...)` into `char suffix[2]` where `/99` needs 4 bytes | only with `FIX_BUGS` **off** |
| `src/control/Script4.cpp:2247-2250` | `ScriptSphereArray[16]` at index 16 when the pool is full | only with `FIX_BUGS` **off** |
| `src/audio/AudioLogic.cpp:256` | `m_asActiveSamples[20]` at index 20 | only with `FIX_BUGS` **off** |
| `src/peds/PedAttractor.cpp:119` | returns a pointer into a local `CVehicleToEffect` | only with `FIX_BUGS` **off** |
| `src/save/GenericGameStorage.cpp:709,1082` | file-supplied `size` read into the 102,400-byte `work_buff` with no bound check | `FIX_INCOMPATIBLE_SAVES` **off** by default (`config.h:293`) — **enable-at-your-peril** |
| `src/renderer/Font.cpp:242,265,293` | `Sprite[3]` on a 3-element array | `MORE_LANGUAGES` **off** (`config.h:291`) |
| `src/core/timebars.cpp:69` | `TimerBar.Timers[50]` at index 1500 — almost certainly a macro artefact, worth 2 minutes' look | unverified |

## 7. Recreating the scan

The Strawberry Perl `cppcheck` on this machine has a broken baked-in `FILESDIR`, so it must be run
from its cfg directory:

```bash
cd /c/Strawberry/c/share/Cppcheck/cfg
cppcheck --enable=warning,performance,portability --inconclusive \
  --std=c++17 --language=c++ -j 8 \
  --suppress=missingInclude --suppress=missingIncludeSystem \
  --suppress=unusedFunction --suppress=useInitializationList \
  --template='{file}:{line}:{severity}:{id}:{message}' \
  --output-file=<out>.txt <relcs>/src/<dir>
```

Run it **per directory and synchronously** — a whole-tree `-j8` run gets reaped at roughly 6% and
leaves orphaned `cppcheck.exe` processes behind.

## 8. Suggested order of work

1. P0-1 (`MAX_STREAMS`) and P0-2 (`GrabPhone`) — live corruption, small, self-contained diffs.
2. P0-3 and P0-4 — one-line guards against a full blip pool and a bad `.CUT`.
3. P1-1 (PathFind) — the highest gameplay impact for the smallest change; alpha-test pathing after.
4. P1-2 … P1-6.
5. Then work the P2 table, and only then the `nullPointerRedundantCheck` list.

---

## 9. Fixes applied (2026-10-29)

18 files changed, 237 insertions, 97 deletions. Every file below was re-analysed afterwards.

| Fix | File |
|---|---|
| `MAX_STREAMS` 1 → 3 | `src/audio/sampman.h` |
| `GrabPhone` exhaustion + no-booth guards, returns `-1` | `src/control/Phones.cpp` |
| radar `nextBlip <= 0` | `src/core/Radar.cpp` |
| cutscene null-anim guard | `src/animation/CutsceneMgr_overlay.cpp` |
| path-find `> 39` → `< 39` (×2) | `src/control/PathFind.cpp` |
| radio timer array sized from the enum | `src/audio/MusicManager.cpp` |
| `vfs_open_callback` length guard | `src/audio/sampman_miles.cpp` |
| corrupt-save guards for vehicle and ped pools (both `#ifdef` branches) | `src/core/Pools.cpp` |
| big-message terminator clamp | `src/renderer/Hud.cpp` |
| `CColModel` ctor ownership flag, self-assignment guard, missing member copies, stale `trianglePlanes` | `src/collision/ColModel.cpp` |
| `realloc` failure handling (×2) | `src/audio/oal/stream.cpp`, `src/core/re3.cpp` |
| missing `va_end`, unbounded `vsprintf` (×4) | `src/core/CdStreamPosix.cpp`, `src/core/re3.cpp`, `src/renderer/Console.cpp` |
| uninitialised reads | `src/core/Camera.cpp`, `src/core/ControllerConfig.cpp`, `src/core/Streaming.cpp` (×3), `src/core/re3.cpp` |
| audio channel bound | `src/audio/sampman_oal.cpp` |

### Cutscene `.CUT` parser rewrite

`LoadCutsceneData_preload` was rebuilt around bounded input instead of trusting the file:

- `GetNextLine` ignored its `lineSize` argument completely (the `// size check never happened, eh?` comment) — any line over 1024 bytes smashed the stack. It now copies at most `lineSize-1` bytes, always terminates, and skips the rest of an over-long line so the next record starts on a real boundary.
- The `CUTS.IMG` block was `new char[size]` with no terminator, so the `*buf != '\0'` scan could run off the heap block. It is now `size + 1`, value-initialised, with a short-read check that refuses to parse a truncated entry.
- A failed `RwStreamOpen` used to fall through and feed the parser uninitialised heap. It now returns through `LoadCutsceneData_postload()`.
- Every `sscanf` return value is checked, every `strtok` result is NULL-checked, `"%s"` into fixed buffers became `"%1023s"` plus `snprintf` truncation, and all five tables (`model`, `text`, `attach`, `remove`, `peffect`) are bounds-checked against `NUMCUTSCENEOBJECTS` / `NUM_CUTS_MAX_TEXTS` / `NUM_CUTS_PARTICLE_EFFECTS`.

**One thing deliberately left as-is:** the `peffect` branch restarted `strtok()` mid-line upstream, so `x`/`y`/`z` re-read columns 2-4 and `name2` was never populated. That mapping is preserved exactly, with a comment. Guessing the intended layout from code alone would change every cutscene particle effect, and there is no sample `.CUT` in the tree to check against. If you have a real one, that branch is worth revisiting.

**Verification:** `cppcheck` re-run over all 18 files → **0 syntax errors, 0 error-level findings**, and none of the fixed diagnostic classes remain. A full MSVC build/run was not possible here (no DirectX SDK on this machine), so this is a static + parse verification, not a runtime one.

### Deliberately not changed

| Left alone | Why |
|---|---|
| 369 `uninitMemberVar` | Most are loader-filled struct members. Machine-initialising all of them is large, noisy churn that would also hide genuinely unset fields. Triage per subsystem. |
| 702 `invalidPointerCast` | Expected for this codebase's compressed-vector math; not bugs. |
| 33 unsafe `sscanf` in `CutsceneMgr_overlay.cpp` | Real, but the `.CUT` parser needs a structured rewrite rather than a field-width patch; a half-fix risks cutscene regressions. |
| 30 `nullPointerRedundantCheck` | Each needs a per-site read to decide check-vs-deref order. |
| 14 `terminateStrncpy` | Behavioural (truncation) rather than memory-safety; needs a policy decision. |
| Everything in [§6](#6-latent--only-if-you-change-the-build-flags) | Those paths are not compiled under the default flags. |
| `vfs_open_callback`'s `strcpy` through `char const*` | That write *is* the `.adf` → `.mp3` rename mechanism; only the underflow read was fixed. Changing it needs Miles call-site knowledge. |
| `CColModel::operator=` still `delete[]`s without consulting `ownsCollisionVolumes` | No live call sites found, and ownership is arranged externally for chunk-loaded models. Needs call-site confirmation before touching. |

---

## 10. Second pass — the §9 leftovers, closed (2026-10-29, later)

Supersedes the "Deliberately not changed" table at the end of §9. 80 files differ from `HEAD`.
Verified by MSVC Release builds of **both** backends (`win-amd64-librw_d3d9-oal`,
`win-amd64-librw_gl3_glfw-oal`) and by running the binary against the real assets in `relcs-run/`.

### 10.1 Uninitialised constructor members — 369 → 0

Every flagged member now has a defined value before the constructor returns: pointers `nil`,
integers/enums `0`, floats `0.0f`, bools `false`, CVector/CVector2D zeroed, arrays zeroed in the
body (never in an initialiser list). Two deliberate deviations, both to avoid introducing a *new*
bug: `CPhysical::m_fMass`/`m_fTurnMass` start at `1.0f` (they are divisors), and
`CObject::m_fCollisionDamageMultiplier` starts at `1.0f` (zero would silently disable collision
damage). Members that cppcheck reported but which are provably assigned before use (e.g.
`CMemoryCard`, `CBoat::m_aBoatNodes` via `SetupModelNodes`) were left alone.

### 10.2 The startup crash — root cause found

Not a renderer problem and not caused by the first pass. `CText::Load` never checked that
`AMERICAN.GXT` opened, and the chunk loop had no end-of-file exit:

- every `sscanf`-style read hit `myfread` with a zero length, which is an invalid argument to
  `fread` — the CRT fast-fails with `0xC0000409` (`FAST_FAIL_INVALID_ARG`, faulting module
  `ucrtbase.dll`);
- with a handler installed, the real shape showed up: `fread_s` → `myfread` → `CText::Load` in an
  infinite loop, because the chunk header keeps its previous contents at EOF and the `size != 0`
  test therefore never stops.

Fixes: `myfread`/`myfwrite` reject zero-length and invalid-handle calls; `ReadChunkHeader` returns
the byte count and both chunk loops stop on a short read; `CText::Load` and `LoadMissionText` bail
out cleanly when the file will not open. The LCS asset set ships the English table as
`ENGLISH.GXT`, not `AMERICAN.GXT`, so the loader now falls back to that name for
`LANGUAGE_AMERICAN`.

### 10.3 Null-pointer check order — all 30 fixed

Every `nullPointerRedundantCheck` was a guard placed *after* a dereference. Each site now tests
before it uses: `CarCtrl`, `Garages`, `Phones`, `SceneEdit`, `Script`, `Script3` (three script
opcodes that indexed `CWorld::Players[]`/ped-pool handles without validating them), `Fire`,
`Frontend`, `CopPed`, `Ped`, `PedAI` (three), `PedFight`, `Population`, `Weapon` (three). Where the
pointer is provably non-null the dead condition was removed instead; where it can be null the
existing null path (early return / `continue` / no shot fired) is preserved.

### 10.4 Cutscene `.CUT` parser

The §9 rewrite holds: `%1023s` field widths, every `sscanf`/`strtok` result checked, all five
tables bounds-checked, `CUTS.IMG` reads sized and terminated. Three real holes remained and are
closed:

- `strcpy(ms_cutsceneName, szCutsceneName)` into an **8-byte** static — now a bounded copy with a
  terminator;
- `sscanf(line+7, ...)` after `strncmp(line, "offset", 6)` — a bare `offset` line read one byte
  past the buffer; the arguments pointer now skips the keyword and spacing;
- `toupper()` called with a possibly negative `char`.

### 10.5 Flag-gated latent bugs (§6) — all seven fixed

| Site | Fix |
|---|---|
| `OnscreenTimer.cpp` `suffix[2]` | one 4-byte buffer in every build (`"/99"` + NUL) |
| `Script4.cpp` sphere array | the overflow case now returns `-1` regardless of `FIX_BUGS` |
| `AudioLogic.cpp` `ProcessReverb` | iterates `NUM_CHANNELS_GENERIC`, never one past the array |
| `PedAttractor.cpp:119` | returns the vector element, not a pointer into a destroyed local |
| `GenericGameStorage.cpp:709,1082` | file-supplied length validated against `sizeof(work_buff)` before the read |
| `Font.cpp` `Sprite[3]` | `MAX_FONTS` is 3 in every build; the array is now `MAX_FONTS + 1` under `MORE_LANGUAGES` |
| `timebars.cpp:69` | releases no longer write `Timers[1500]` for an unknown timer; `tbStartTimer` is bounded too |

### 10.6 `invalidPointerCast` — triaged, benign (all 702)

| Pattern | Count | Verdict |
|---|---|---|
| `int *` ↔ `float *` | 685 | benign |
| `unsigned char *` ↔ `float *` | 12 | benign |
| `float *` → `signed char *` | 4 | benign |
| `signed char *` ↔ `float *` | 1 | benign |

~683 of them are the script VM: `#define GET_FLOAT_PARAM(i) (*(float*)&ScriptParams[i])`, with
`int32 ScriptParams[32]`. The console VM stores every parameter as a 32-bit word and the opcode
handler reinterprets it as a float — same size, same alignment, deliberate bit reinterpretation,
exactly as the original engine did. The remainder are the same idiom in loaders and
`WaterLevel.cpp` (reading float bytes). They are not pointer-arithmetic errors and there is no
type confusion: no case reinterprets a *pointer* as another pointer. They would become real if the
port ever targeted a big-endian host or a platform with a different `float` representation; a
portable rewrite would use `memcpy`, which is a refactor, not a bug fix.

One genuine overflow was found while chasing the streaming loop and fixed: `TxdDef::name` was
`char[20]` filled with a bare `strcpy` while IMG directory names are 24 bytes. It is now
`char[24]` with a bounded, terminated copy, and `GetTxdName` no longer dereferences a null slot.

### 10.7 Verification, and what is still open

Both backends build clean (`0` errors). Run against `relcs-run/`: the GL3 build creates its
window — `Title='GTA: Liberty City Stories'`, ~72 MB working set, no exception, no fast-fail,
`LoadCutsceneData`/text/font stages all pass. The D3D9 build reaches `CreateDevice()` and fails
because this session exposes no Direct3D device; that is environmental, and the code now takes the
null-deref path it used to crash on.

**Still open, and the only thing between this build and a playable frame loop:** the game requests
a texture dictionary called `sceelee.txd` and never stops retrying (~50,000 attempts in 12 s). That
name is neither a file in `relcs-run/` nor an entry in `models/gta3.img`/`gta3.dir`, so it is not
in this asset dump at all. The port has no "this stream is permanently unavailable" state, so the
request is retried every frame instead of being dropped. Two candidate follow-ups: source the
missing LCS dictionary (or the separate txd archive it normally lives in), or add a bounded-failure
state to the streaming request so an absent asset cannot stall startup.

---

## 11. Third pass — actually running the game (2026-10-29, later still)

§10.7 closed the streaming question by naming two follow-ups: stop retrying missing streams, and
find where `sceelee.txd` comes from. Both are done, and the run turned up four more real crashes
that only appear once the game gets past startup. The build now reaches **gameplay**.

### 11.1 `sceelee.txd` — where it came from

`sceelee` is not a streaming asset and never was: it is the hard-coded splash dictionary in
`CMenuManager::DrawQuitGameScreen` (`Frontend.cpp`). The data set simply had no `TXD/` directory,
and `LoadSplash` retried it on every frame because nothing remembered the failure.

- `LoadSplash` now records a name that failed to load and returns immediately for it, so a missing
  splash costs one attempt instead of ~50,000.
- The asset set it wanted (`TXD/SCEELEE.TXD` and friends) exists in the `reLCS/` tree under
  `Downloads`; it is now copied into `relcs-run/`, so the splash renders.

### 11.2 Crashes fixed this pass

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | modal error + `_exit(3)` during audio init | a missing music track (`AUDIO\MUSIC\HEAD.VB`) went through `USERERROR`, which is a modal box followed by `_exit(3)` | log and leave the track silent (`sampman_oal.cpp`); `nStreamLength` stays 0 |
| 2 | `0xC0000094` right after audio init | `cMusicManager::GetTrackStartPos` did `pos %= m_aTracks[i].m_nLength` with length 0 — every track is length 0 when the audio folder is absent | early return for zero-length tracks; `SetRadioChannelByScript` guards the same modulo |
| 3 | same class in the MP3 start-position code | `_CurMP3Pos = … % randmp3->nTrackLength` | guarded in **both** backends (`sampman_oal.cpp`, `sampman_miles.cpp`) |
| 4 | `_exit(3)` on Start Game, assert `fd > 0` in `CFileLoader::LoadLevel` | `InitialiseGame` hard-codes `DATA\GTA_LCS.DAT`; this data set ships the LCS level description as `gta_vc.DAT` (it lists `fortstaunton.ide`, `fortdestroyed.ipl`, …) | startup resolves the first of `GTA_LCS.DAT` / `GTA_VC.DAT` / `GTA3.DAT` that exists, and reports a clear error if none does; `LoadLevel` logs instead of asserting |
| 5 | `0xC0000005` a few seconds into gameplay | `CChannel::Start` converted 2D samples through `tempStereoBuffer[PED_BLOCKSIZE*2]` (158 kB) — mission audio is longer, so the mono→stereo loop wrote past a static buffer | the scratch buffer is heap-allocated and grown on demand |

### 11.3 Permanently-missing streams

`CStreaming::ReRequestModel` re-requested an asset forever when its data was not in the IMG, which
stalls anything waiting for it. It is now bounded to 5 attempts, logs the give-up, and the counter
is reset when the same stream later loads successfully. This is the "no permanently unavailable
state" item from §10.7.

### 11.4 Verification

- Both backends build clean: `win-amd64-librw_gl3_glfw-oal`, `win-amd64-librw_d3d9-oal`.
- All diagnostic instrumentation used to find these (the temporary vectored exception handler and
  stack walker) has been removed; `grep` for `TraceWrite`/`TraceInit`/`TEMPORARY` returns nothing.
- GL3 against `relcs-run/`, driven through the real UI: window `GTA: Liberty City Stories` → main
  menu → **Start Game** → legal screen → loaded world. The player ped, HUD (clock, health, money),
  radar with blips and world geometry all render; the process holds ~300 MB and a full core, no
  exception. Harnesses: `_scratch/start-game.ps1` (drives the menu and captures) and
  `_scratch/capture-window.ps1`.

### 11.5 Still open (data, not code)

- **No audio content at all.** The dump has no `AUDIO/` tree, so radio, news, cutscene and mission
  audio are silent. Every one of those paths now degrades instead of aborting, which is why the
  game runs; supplying the folder is what makes it audible.
- **First level load takes minutes** (~4 on this machine) before the world appears. Nothing fails,
  it is simply slow; profiling `CGame::Initialise` / `LoadScene` is the next lead if that matters.
