# Animation Motion Revolution

本地分支已同步作者 `max-su-2019/AnimationMotionRevolution` 的
[`b5be808`](https://github.com/max-su-2019/AnimationMotionRevolution/commit/b5be808a212f2b124f19d5365a18d1158d24b77b)，
版本为 **2.0.0**。保留本地 CommonLibSSE-NG 源码构建、共享动画缓存、1170 ABI 修正和生命周期保护，
并合入作者的攻击位移控制、分段 motion warping、行为图变量、TrueHUD 调试与 V1 查询 API。

## Runtime support

- Steam Skyrim **1.6.1170.0 / SKSE 2.2.6**。
- AE Address Library（包含 `versionlib-1-6-1170-0.bin`）与 Microsoft Visual C++ x64 运行库。
- 依赖使用相邻的 `../CommonLibSSE-NG-ng`；运行时由加载入口严格检查。

The core root-motion features do not require behavior generation. The packaged
behavior-graph variable integration requires Behavior Data Injector. On Skyrim
AE 1.6.1170, install BDI v0.13 plus a compatible Behavior Data Injector Universal
Support release, allowing Universal Support to overwrite the original BDI DLL.
No Nemesis/Pandora patch is needed for the injected variable.

## New features

### Downward ground probe

For each attacking-actor frame driven by an animmotion curve, AMR evaluates the next
horizontal root-motion step after warping. It casts a ray straight down on the
world Z axis at that predicted destination. If no support is hit, the horizontal
step is suppressed without accumulating a later catch-up jump. Vertical root
motion is left intact.

### Combat-target motion warping

While an actor is attacking and has a current combat target, authored
horizontal root motion is uniformly scaled so its travel distance matches the
target distance. Its authored direction is never rotated or steered. Motion may
be reduced to zero; extension beyond the authored distance is disabled by
default. An angle gate limits scaling to segments already directed roughly
toward the target.

Warping is segment based. A segment's authored motion is the cumulative
`animmotion` displacement at its end minus the cumulative displacement at its
start. Segment endpoints are sampled and cached once when the clip activates;
runtime scale calculation is constant time. If one game update crosses a
segment boundary, AMR applies the old scale up to the exact boundary and the new
scale after it, preserving the whole frame's motion. Normal activation uses the
complete segment displacement. If a target is acquired or a distance gate is
re-entered after the segment has begun, recalculation uses only the authored
motion still remaining to that same segment end.

Animations can override the attack-only default with timestamped annotations:

```text
animwarp <lowerScale> <upperScale> [maximumAngleDegrees] [maximumDistance]
animwarpend
```

An `animwarp` starts a segment at its timestamp. That segment ends at the next
`animwarp`, the next `animwarpend`, or the end of the `animmotion` curve.
`animwarpend` disables warping from its timestamp onward until a later
`animwarp`; it is synonymous with `animwarp 1 1`. Later controls at the same
timestamp replace earlier ones.

Any valid or malformed reserved warp-control annotation makes the clip
explicitly controlled and suppresses default attack warping for the entire
clip. Consequently, the interval before its first valid `animwarp` is
unwarped. This fail-closed behavior prevents a typo from unexpectedly enabling
the implicit attack rule. An explicit `animwarp` enables its segment even while
the actor is not attacking.

The optional angle defaults to 60 degrees and replaces the direction-angle limit
for that rule. The optional distance is a maximum current horizontal
actor-to-target distance; beyond it, the rule is inactive and authored motion
plays unchanged. Distance defaults to unlimited. Because arguments are
positional, an angle must be supplied before a distance.

When a clip contains no warp controls, INI-enabled attack warping starts with an
implicit `animwarp 0 1` (using the configured default limits). Every exact,
case-insensitive `HitFrame` event or event whose name begins with
`Collision_Add` ends that segment and starts a new default segment immediately.
Payloads such as `HitFrame.$payload` and `Collision_Add.Node(WEAPON)` are
supported. This recalculates the scale for each
successive hit instead of aiming the entire animation only once. The INI can
disable this default without disabling explicitly annotated segments. Default
minimum/maximum scale and angle are independently configurable.

### Behavior graph integration

The package includes
`SKSE/Plugins/BehaviorDataInjector/AnimationMotionRevolution_BDI.json`. Behavior
Data Injector uses it to define the boolean graph variable
`AMR_IsAnimationWarpingEnabled`, initially `false`, throughout behavior projects
under `Data/Meshes/Actors`.

AMR publishes `true` while an actor has at least one active animation clip whose
motion warp is actually applicable. This includes a valid evaluation whose
clamped scale is exactly 1. It publishes `false` after all of that actor's warp
claims stop—for example at `animwarpend`, when the attack/target/gate conditions
fail, or when the last warped clip deactivates. Claims are aggregated across
blended clips so an unwarped clip cannot clear another active warped clip.

### TrueHUD debug visualization

Configure with the single CMake option `AMR_ENABLE_TRUEHUD_DEBUG`, enabled by
default. When disabled, the TrueHUD API and integration sources are omitted from
the plugin target and all initialization, input, and draw calls are compiled out.
When enabled, both debug views start hidden. Page Up toggles motion-warp
visualization, while Page Down independently toggles ledge-prevention
visualization. Neither hotkey changes gameplay behavior. Motion-warp debug draws
a one-second snapshot when an explicit `animwarp` segment or generated default
combat segment activates. It also draws when an eligible segment first begins
applying after a late attack-state, target, angle, or distance-gate change.
Yellow is the segment's authored motion, and elevated cyan is its predicted
scaled motion when warping applies. Inactive segments, including `animwarpend`,
do not draw a snapshot. The target point and connector are intentionally not
drawn.

Settings live in
Data/SKSE/Plugins/AnimationMotionRevolution.ini.

Developer documentation for `animmotion`, `animrotation`, `animwarp`,
`animwarpend`, and the behavior graph variable is available in
[English](docs/developer-guide.md),
[Simplified Chinese](docs/developer-guide.zh-CN.md), and
[Korean](docs/developer-guide.ko-KR.md). The focused motion-warp reference remains
available in [English](docs/animwarp.md),
[Simplified Chinese](docs/animwarp.zh-CN.md), and
[Korean](docs/animwarp.ko-KR.md).

## Build

在 VS x64 开发者命令行中，设置 `VCPKG_ROOT` 后运行：

```bat
cmake --preset ae1170 -DAMR_BUILD_TESTS=ON
cmake --build --preset ae1170 --parallel 6
ctest --test-dir build/ae1170 --output-on-failure
cmake --install build/ae1170 --component AMR --prefix dist/AnimationMotionRevolution-1.6.1170
```

依赖由 `vcpkg.json` 提供，使用 `x64-windows-static-md`。可用
`-DCOMMONLIBSSE_SOURCE_DIR=绝对路径` 指定依赖源码，
`-DAMR_ENABLE_TRUEHUD_DEBUG=OFF` 编译掉可选的 TrueHUD 调试集成。
构建不会自动复制到游戏；需要时设置 `AMR_DEPLOY_DIR`。

## References and licensing

The original AMR source is MIT licensed. Targeting and orientation patterns were
compared with True Directional Movement, while raycast usage was checked against
SmoothCam and the current CommonLibSSE-NG bhkPickData API. No source was copied
from those projects.

本地依赖的许可证以 `CommonLibSSE-NG-ng` 中的许可证文件为准。

Original Nexus page:
https://www.nexusmods.com/skyrimspecialedition/mods/50258

安装包交给模组管理器，或将包内 `SKSE` 目录放入游戏 `Data` 目录，覆盖旧 DLL。
安装新配置前请合并自己的设置。此次同步说明、验证和限制见 [同步报告](docs/MAXSU-SYNC.md)。
2026-11-11 修正了逐 hook 申请 trampoline 导致启动失败的问题；预算和回归证据见 [修复记录](docs/TRAMPOLINE-FIX-1170.md)。
原有 [迁移报告](docs/MIGRATION-1170.md)、[优化报告](docs/OPTIMIZATION-1170.md) 和
[安全审核报告](docs/SAFETY-AUDIT-1170.md) 记录同步前版本的证据；新增 warping 以同步报告为准。

## 重跑地址核验

```bat
python -m pip install capstone
python tools/verify_1170.py "解包后的SkyrimSE.exe" "versionlib-1-6-1170-0.bin" --output abi-1170.json
```

脚本检查 7 个调用点的 E8 指令及目的地址、9 个虚表入口和 5 个辅助函数 RVA，并输出带输入文件 SHA-256 的反汇编证据。它用于核验 1.6.1170；其他运行时需要单独迁移。
