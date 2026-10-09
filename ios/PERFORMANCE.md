# Tsubomi performance & stability guide

## Stability pack (`perf/stability-pack`)

1. **Thermal / memory monitoring** during play (watchdog samples every ~2s).
2. **Frame pacing** with monotonic deadlines (less micro-stutter).
3. **Surface-sync readback coalescing** — intermediate GPU→CPU post-syncs are
   dropped while one is in flight. This is the main fix for **God Eater
   Resurrection** menu FPS (settings / quest UI). Rage Burst 2 was already fine.

## God Eater Resurrection menu FPS

**Symptom:** Lobby / combat smooth after first shader compile, but opening the
in-game settings or quest-accept UI tanks FPS until the menu closes.

**Cause:** Resurrection redraws **linear color surfaces** every menu frame.
Surface sync queued a blocking wait on the previous readback → serialized the
wait-queue. Not the Tsubomi virtual-pad overlay.

**Fix (in this branch):** `VKSurfaceCache::queue_post_surface_sync` skips
in-flight intermediates (`SurfaceReadback::ready()`).

**Recommended per-game settings** (Library → long-press → Settings):

| Knob | Value |
|------|--------|
| Resolution | 1.0× |
| High accuracy | On |
| Async pipelines | On |
| Anisotropic | 2 |
| Surface sync | leave enabled (renderer fix handles menu thrash) |

Optional XML profiles in-tree (copy to device `config/config_<TitleID>.xml`):
- `vita3k/config/config_PCSA00026.xml` (US)
- `vita3k/config/config_PCSB00874.xml` (EU)

Validation layer must stay **off** in those profiles.

## Other titles

| Title | Notes |
|-------|--------|
| God Eater 2 Rage Burst | Usually smooth in menus; no special surface-sync thrash. |
| Persona 4 Golden | Keep double-buffer memory mapping **off**. |
| VA-11 HALL-A | Light; aniso 2 is enough. |

## Global tips

- JIT required (StikDebug etc.) before boot.
- Keep the device cool; thermal *critical* forces 0.5× resolution via the watchdog.
- Attach `tsubomi.log` when reporting issues (`iOS runtime pressure:` lines help).

## iPhone 8 Plus (A11): startup and compatibility

The iPhone 8 Plus uses the **iOS 16** path. Building with the iOS 26 SDK does
not require iOS 26 on the phone: the app and dependencies target iOS 16.0,
using availability checks for newer UI APIs. StikDebug's iOS 17.4+ integration
is not an option on this phone. Use an iOS 16-compatible external JIT method,
or TrollStore on a supported iOS release; see [COMPATIBILITY.md](COMPATIBILITY.md).
The launch gate must confirm executable memory is available before starting a game.

### How a game runs

1. SwiftUI sends the selected game/settings through the Objective-C++ bridge.
2. Vita3K mounts the installed Vita filesystem and loads the executable/modules.
3. Dynarmic translates the Vita's ARM CPU code into native ARM64 instructions.
   The iOS frontend caps shared JIT execution at two slots, with 16 MiB code
   caches selected automatically; guest threads borrow slots rather than each
   owning a separate large cache.
4. GXM graphics commands go to the Vulkan renderer. MoltenVK translates those
   operations/shaders to Metal for the A11 GPU. CPU/GPU surface synchronization
   remains necessary for games that read rendered data back into guest memory.
5. SDL and the iOS frontend provide sound, touch/controller input and presentation.

### Startup changes

- Start the iOS render consumer **before** `run_app` executes synchronous library
  `module_start` functions. A module can now submit a GXM request and wait for
  the renderer without waiting for a thread that has not started yet. Failed
  startup still joins that renderer through the existing session cleanup.
- Give optional shader-cache warmup a two-second scheduling budget. Check
  process memory pressure before each entry and defer remaining entries at
  pressure 50 (approximately 180 MiB headroom with the current thresholds).
  Required shaders still load on demand; the disk cache is retained. The budget
  is checked between entries and cannot interrupt a stalled Metal driver call.
- Keep the existing `ios-upstream.yml` build. Its portable regression job must
  pass before the macOS IPA job starts. Packaging checks arm64, iPhone support
  and the iOS 16 minimum in both Info.plist and the linked Mach-O executable.

For an initial device comparison, use resolution 0.5×, anisotropic filtering 1,
High Accuracy on, surface sync on, shader/texture cache on and DoubleBuffer off.
These are a starting point for measurement, not forced overrides of saved settings.
Compare the same game/scene with a cool device, then after sustained play.

### Verification still needed on the phone

Host regressions simulate a GPU-dependent module startup, failed/throwing loads,
cleanup and shader warmup under time/memory limits. They also retain the desktop
startup behavior. They do not establish game compatibility or certify the A11 GPU.

On the iPhone, record the exact iOS version, JIT method, title ID and build commit;
try a cold launch, a warm-cache launch, quitting/relaunching, and a scene transition.
Keep `Documents/Tsubomi/tsubomi.log` from a failed launch and an iOS crash/jetsam
report if the app closes. A successful IPA build alone cannot establish that
all games load without hangs or run at full speed.
