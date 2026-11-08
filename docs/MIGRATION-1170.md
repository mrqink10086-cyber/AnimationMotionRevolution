# AMR → CommonLibSSE-NG / Skyrim 1.6.1170

核验日期：2026-11-08。

## 目标与输入

- AMR：本地 `AnimationMotionRevolution-main`，插件版本 1.5.3。
- CommonLib：本地 `CommonLibSSE-NG-ng`，版本 15.7.3。与上游 `ng` 的 `07262cc2ae75777a422ed0c9621310fadef7c382` 逐文件比较，忽略行尾后无差异，因此保留本地库源码。
- 运行时：Steam Skyrim SE 1.6.1170.0；本地 `../skse64/skse64_common/skse_version.h` 确认对应 SKSE 2.2.6。
- EXE：`E:/source/repos/valhallaCombat/build/abi/SkyrimSE-1.6.1170.exe.unpacked.exe`。
- Address Library：`E:/source/repos/CommonLibSSE-NG-ng/tests/REL/versionlib-1-6-1170-0.bin`。
- EXE SHA-256：`80c1ea737d33c6bfac09b101b8d77ab0f9f6630128c3ed052f9d945bed54e7e4`。
- 地址库 SHA-256：`c4093c569a3c83b26587f4b9ea4c55de9ae6e73b84a2af9fb3fbd30e2fe0d452`。

完整机器结果见 [abi-1170.json](abi-1170.json)，可用 `tools/verify_1170.py` 重跑。以下地址均为 RVA，实际 VA 为模块基址加 RVA；本机静态镜像基址为 `0x140000000`。

## 调用点

| 用途 | AE Address ID | 函数 RVA | 函数内偏移 | 调用点 RVA | 原调用目的 RVA |
|---|---:|---:|---:|---:|---:|
| Clip Activate | 59252 | ACD750 | 66E | ACDDBE | AD1050 |
| Clip Deactivate | 59254 | ACE1E0 | 1A | ACE1FA | AD11E0 |
| Translation 1 | 32703 | 540990 | 298 | 540C28 | 538240 |
| Translation 2 | 32703 | 540990 | 2AA | 540C3A | 538240 |
| Rotation 1 | 32703 | 540990 | 35C | 540CEC | 538290 |
| Rotation 2 | 32703 | 540990 | 36D | 540CFD | 538290 |
| Movement/GetState | **78275** | E9F170 | **7C2** | E9F932 | B7AE60 |

7 处均为完整的 5 字节 `E8 rel32` 调用。

- 旧代码的 `UpdateStateAndMovement` AE ID 误写为 `32703`，与动画运动处理函数重复。已改为 **78275 + 0x7C2**。调用前 `mov rcx,r13`，调用后 `cmp eax,5`，后续分支决定是否清零垂直速度，符合该 hook 的用途。
- Activate 的 `hkbContext*` 保存在 **R15**，维持此寄存器来源。
- Deactivate 的序言执行 `mov rsi,rdx`；上下文来源已由旧版 **R14 改为 RSI**。
- 动画运动处理的 Character 指针保存在 **R13**；clip 名称位于 **RBX - 0x14**，作为第 4 参数传入；Character 作为第 5 参数放在调用栈上。
- `538240` / `538290` 是容器包装函数（ID 32578 / 32579），内部跳转至底层 decoder ID **32582 / 32583**（`538390` / `538630`）。插件的 vanilla fallback 直接传入 `translationDataPtr` / `rotationDataPtr` 对应的段头，维持底层 decoder 调用。
- Activate 所替换调用的返回值在调用点未使用，hook 按 `void` 声明。Deactivate 的原函数清零 clip 的 `+0x105` 字节。

## 虚函数与结构布局

| 类/基类视图 | VTABLE AE ID | 槽（十六进制） | 目的 RVA |
|---|---:|---:|---:|
| bhkCharacterStateOnGround | 240833 | 08 | F02550 |
| bhkCharProxyController 的 bhkCharacterController 子对象 | 240560 | 02 / 08 / 0F | ED1DC0 / ED2020 / ED1C60 |
| bhkCharRigidBodyController | 240580 | 02 / 08 / 0F | ED42F0 / ED4710 / ED3F70 |
| bhkWorld / PickObject | 238918 | 33 | E86560 |
| INISettingCollection / ReadAllSettings | 187074 | 09 | 18CED0 |

Controller 的 02 / 08 / 0F 分别对应 GetPosition、GetCollisionFilterInfo 和 GetHavokWorld。ProxyController 的首张表是另一基类的表；这里使用其 **第二张表**，避免混淆多继承子对象。

当前 CommonLib 将状态类槽 08 命名为 `Unk_08(void)`。反汇编显示此处 ABI 为 `void(state*, controller*)`。插件在 `include/GameABI.h` 内通过类型明确的函数指针调用槽 08，保留共享库头文件。

移除了 AMR 的 29 个 `include/RE` 影子头，统一使用当前 CommonLib。重要布局已用编译期断言检查：

- `bhkCharacterController::context = 0x1E0`，`wantState = 0x21C`。
- `hkbClipGenerator::binding = 0xA0`，`localTime = 0xF0`。
- 本地 `MovementInput` 大小 `0x70`、16 字节对齐，gain 位于 `0x60`。

旧 clip 声明缺少 `cropStartAmountLocalTime`，导致中间字段偏移错误；当前库含完整字段。未在共享库中修改任何头文件。

## 其他 ABI 与接口修正

- Gravity：ID **77838**，RVA `E84230`。实际通过 RAX 返回 `const hkVector4*`，不是输出引用；返回 hkpWorld 的 `+0x20` 向量或静态 fallback。现在复制返回向量并累加 `gravity * deltaTime`。
- CalculateMovement：ID **81002**，RVA `F3D670`；RCX 指向输入，RDX 指向输出。Rotation interpolation：ID **70836**，RVA `D340E0`；参数为输出、float、左四元数、右四元数。
- 当前 `hkVector4` 使用 `quad.m128_f32`。旧影子类的 `operator+=` 未写回，现改为明确赋值累加 surface velocity 和 gravity；这恢复了原代码所表达的累加行为。
- GetCollisionFilterInfo 使用 `CFilter&`；ray input 使用 `CFilter`，对应字段保持 32 位过滤信息。
- `SetWantedState` 的旧内联逻辑保留为 currentState 赋值并将 wantState 重置为 `kTotal`。
- 事件 sink → Character 的转换保留完整 RTTI 描述符及子对象偏移，放在插件命名空间，避免依赖已删除的通用 `skyrim_cast` 扩展。
- 适配 `Setting::kInteger`、GetInteger、GetUnsignedInteger、ReadAllSettings 和 NiQuaternion 构造函数。
- 移除 Load 中的 `REL::Module::reset()`，避免已初始化静态 relocation 与模块缓存失配。
- 迁移阶段读过 `../SKSE-Threading-Loading-Notes.md`，在 SKSE Load 内安装 hook。后续优化增加了 PreLoadGame / NewGame 缓存清理，详见 [优化报告](OPTIMIZATION-1170.md)。

## 构建与产物

MSVC 19.44.35229.0 / VS Build Tools 2022，x64，C++23，RelWithDebInfo。CommonLib 以 AE-only、Xbyak ON、Patch Safety ON 从源码构建。导出包含 `SKSEPlugin_Load`、`SKSEPlugin_Query`、`SKSEPlugin_Version`。兼容信息限定 1.6.1170.0，最低 SKSE 2.2.6；Load 入口也检查这两个版本条件。

移除了旧 colorglass registry、过期 CommonLib vcpkg 包和未使用依赖。固定 vcpkg baseline 为 `ee12231b20c95013c6638d845d04c91559a1d1ff`。本次实际复用已安装的 directxtk 2025-10-27、directxmath 2025-04-03、rapidcsv 8.90、fmt 12.1.0、spdlog 1.16.0、xbyak 7.28，均为 `x64-windows-static-md`。

本机 vcpkg 下载 Ninja 时遇到 `WinHttpSetOption failed with exit code 87.`，因此使用已有同版本依赖完成构建。在 VS x64 开发者命令行中的复现命令：

```bat
cmake --preset ae1170 -B build/ae1170-local -DCMAKE_MAKE_PROGRAM=C:/Users/15219/AppData/Local/Programs/Python/Python310/Scripts/ninja.exe -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_INSTALLED_DIR=E:/source/repos/valhallaCombat/build/vcpkg_installed -DFETCHCONTENT_SOURCE_DIR_HDE64=E:/source/repos/valhallaCombat/build/_deps/hde64-src
cmake --build build/ae1170-local --parallel 8
cmake --install build/ae1170-local --component AMR --prefix dist/AnimationMotionRevolution-1.6.1170
```

本机 CMake 4.1.0-rc1 将中文 MSVC 的 include 前缀识别为乱码；在生成目录的 `CMakeFiles/4.1.0-rc1/CMakeCCompiler.cmake` 和 `CMakeCXXCompiler.cmake` 中将 `CMAKE_*_CL_SHOWINCLUDES_PREFIX` 修正为 `注意: 包含文件:  ` 后重新生成，解决 Ninja 头文件依赖追踪。该修正仅涉及本地生成文件。

源文件备份：`E:/source/repos/AnimationMotionRevolution-original-20261108.zip`。DLL 与符号位于 `build/ae1170-local`；安装树与 ZIP 位于 `dist`。没有向游戏目录自动部署，也没有启动游戏。

## 验证范围

编译链接、导出与运行时元数据检查通过。离线核验 7 个调用点、9 个虚表入口、5 个辅助函数 RVA 通过。反汇编支持上述上下文寄存器、函数签名及布局结论。

尚未验证游戏内动画激活/停用、自定义平移与旋转、垂直 root motion、斜坡/离地及与其他 hook 模组的交互；这些仍需在 1.6.1170 + SKSE 2.2.6 环境中实测。
