# Animation Motion Revolution

https://www.nexusmods.com/skyrimspecialedition/mods/50258

本地迁移版：使用当前 `CommonLibSSE-NG` 的 `ng` 分支源码，目标为 Steam Skyrim SE **1.6.1170.0 / SKSE 2.2.6**。插件版本保留为 1.5.3。

安装需要 SKSE 2.2.6、AE Address Library（包含 `versionlib-1-6-1170-0.bin`）和 Microsoft Visual C++ x64 运行库。把安装包交给模组管理器，或将包内 `SKSE` 目录放入游戏的 `Data` 目录。新版 DLL 应覆盖旧版 AMR DLL。默认配置 `uLogLevel=4` 表示只记录错误。

已通过 MSVC x64 编译和针对本机 1.6.1170 解包 EXE 的离线反汇编核验；尚未进行游戏内运行测试。具体地址、虚表槽和 ABI 修正见 [迁移报告](docs/MIGRATION-1170.md)。

当前优化版包含动画共享缓存、16 路短锁分片、关键帧混合查找与 SSE2 插值；时序、所有权、测试和微基准见 [优化报告](docs/OPTIMIZATION-1170.md)。

安全审核修订版补充图访问锁、游戏对象引用、状态边界、空注释、分配失败回滚和动态汇编展开信息。5 组宿主测试及 3 组 ASan 检查通过；地面状态共享和引擎析构线程等待验证项见 [安全审核报告](docs/SAFETY-AUDIT-1170.md)。安装新 ZIP 时覆盖同名 DLL，保留之前的包便于对比。

## 构建

在 VS 2022 x64 开发者命令行中，设置 `VCPKG_ROOT` 为实际 vcpkg 目录，然后运行：

```bat
cmake --preset ae1170
cmake --build --preset ae1170 --parallel 8
cmake --install build/ae1170 --component AMR --prefix dist/AnimationMotionRevolution-1.6.1170
```

默认从相邻的 `../CommonLibSSE-NG-ng` 构建依赖。可用 `-DCOMMONLIBSSE_SOURCE_DIR=绝对路径` 指定其他源码目录。依赖由 `vcpkg.json` 的固定 baseline 提供。构建不会自动复制到游戏目录；需要时可显式设置 `-DAMR_DEPLOY_DIR=游戏Data或模组目录`。

本次实际构建复用了本机已有的同版本依赖，构建目录为 `build/ae1170-local`；复现命令见迁移报告。

## 重跑地址核验

```bat
python -m pip install capstone
python tools/verify_1170.py "解包后的SkyrimSE.exe" "versionlib-1-6-1170-0.bin" --output abi-1170.json
```

脚本检查 7 个调用点的 E8 指令及目的地址、9 个虚表入口和 5 个辅助函数 RVA，并输出带输入文件 SHA-256 的反汇编证据。它用于核验 1.6.1170；其他运行时需要单独迁移。
