# Trampoline 启动分配故障修复

日期：2026-11-11。目标：AMR 2.0.0、Steam Skyrim 1.6.1170、SKSE 2.2.6 及以上兼容版本。

## 根因

启动时报 `SKSE/Trampoline.cpp(164): Failed to handle allocation request`。
本地 CommonLibSSE-NG 的 `SKSE::Impl::API::InitTrampoline()` 使用 `std::call_once`，
因此 trampoline 仅在首次请求时初始化。旧插件却在每个 hook 前调用已弃用的
`SKSE::AllocTrampoline()`，将它当成可重复预留空间的接口。

安装顺序中的第一次请求只包含状态查询 hook 的 14 字节跳转桥。该 hook 立即用完这 14 字节。
激活 hook 随后的请求不再调用共享池、不扩大已初始化的 trampoline，复制 17 字节代码时触发容量检查。

这段调用方式在作者功能合并前的 `dfc52bc` 中已经存在；本次 2.0 合并保留了它。
根因是此前 NG 适配没有匹配当前依赖的一次初始化语义。
此前的栈测试只直接执行汇编代码，没有覆盖逐个安装时的分配链，因此没有发现这个故障。

本地 SKSE 源码 `skse64/skse64.cpp` 确实创建了 64 KiB branch pool 和 64 KiB local pool，
`skse64_whatsnew.txt` 也要求插件仅申请所需空间。
但这个错误检查的是插件获配子块的剩余容量；本例失败时为 14/14 字节，并非共享池已用满。

## 修复

`HookInstallation` 先生成将要安装的汇编，再根据真实代码长度计算完整预算。
加载入口通过 `SKSE::Init(..., { .trampoline = true, .trampolineSize = ... })` 一次预留。
安装函数只消费这块空间，不再逐 hook 调用 `AllocTrampoline`。

| 内容 | 数量与大小 | 合计 |
| --- | --- | ---: |
| 七个 call 的间接跳转桥 | 7 × 14 字节 | 98 字节 |
| 激活、停用的上下文转接代码 | 2 × 17 字节 | 34 字节 |
| motion 转接代码、目标地址与展开数据 | 4 × 40 字节 | 160 字节 |
| 各 motion 分配的最坏 16 字节对齐预留 | 4 × 15 字节 | 60 字节 |
| **总预算** | | **352 字节** |

所有 call site 偏移、R15/RSI 上下文来源、第五参数布局和展开元数据保持原实现。
安装前检查完整剩余容量；成功后日志记录实际使用量与容量，当前为 `352 / 352 bytes`。
352 字节约占 64 KiB 的 0.54%。

## 回归验证

新增的 `amr_trampoline_tests` 使用本地 CommonLibSSE-NG 的实际 API 初始化实现、实际 trampoline
分配及补丁实现，提供一个内存中的 SKSE trampoline 接口和七个可写 call site。
它调用与生产共用的 `HookInstallation::Install`，检查：

- 共享池只收到一次请求，大小等于计算预算。
- 七个调用点完整安装，原激活/停用目标正确保存，补丁最终指向获配代码。
- 四个 motion 入口按 16 字节对齐；Windows 能查到对应展开表。
- 分配恰好落在 352 字节子块内，紧随其后的哨兵字节保持不变。

`--legacy` 模式保留最小旧调用链，在独立进程内确认后续请求没有扩容，并重现相同错误。
测试单独编译依赖的 `Trampoline.cpp`，开启 `ENABLE_COMMONLIBSSE_TESTING`，将其致命错误转成可捕获异常；
生产 DLL 继续链接正常的 CommonLibSSE-NG，不受该测试开关影响。

首次修复前复现输出：

```text
[critical] [Trampoline.cpp:164] Failed to handle allocation request
FAIL: Failed to handle allocation request; pool requests=1, capacity=14, used=14
```

修复后输出：

```text
7 call sites installed; 352 / 352 bytes, 1 pool request, 4 unwind tables verified
Trampoline installation passed
```

MSVC 19.44、x64 RelWithDebInfo、TrueHUD 调试开启，完整 DLL 构建通过。
CTest 8/8 通过，包含原 motion、warp、cache、cache_exception、interpolation、stack，
以及新的完整安装与旧逻辑复现测试。日志见 `trampoline-fix-logs/`。

```powershell
cmake --build build/ae1170-local --parallel 8
ctest --test-dir build/ae1170-local --output-on-failure --no-tests=error
build/ae1170-local/amr_trampoline_tests.exe
build/ae1170-local/amr_trampoline_tests.exe --legacy
```

以上命令在 VS x64 开发者环境中运行。没有部署或启动 Skyrim；本次游戏启动结果仍需由实际加载确认。
