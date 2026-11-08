# AMR 线程、内存、栈与游戏指针审核

日期：2026-11-08。审核对象：本地优化版 AMR 1.5.3，Skyrim 1.6.1170 / SKSE 2.2.6。

已修复可确认的问题，通过 DLL 构建、5 组宿主回归测试和 3 组 AddressSanitizer 检查。游戏对象内部的并发约束仍有未验证项，列在本文末尾；这些检查不构成整个游戏进程的线程安全证明。

## 已修复问题

| 位置 | 问题与触发条件 | 修正 |
|---|---|---|
| `Motion.h` 注释入口 | 合法的空 `hkStringPtr` 返回 null，直接构造 C-string `string_view` 会执行空指针长度读取 | 空文本返回无自定义运动；新增空文本回归 |
| `Motion.h` yaw 解析 | 有限的极大角度在原乘法顺序中溢出，再进入 sin/cos | 检查换算后的角度，跳过溢出注释；正常数值保持原顺序 |
| `MotionCache.h::Activate` | 先发布 character/graph owner，再分配 clip，分配异常会留下无 clip 的 owner；旧的移动参数写法还可能在锁内释放最后一个快照 | 锁外完整构造待发布条目，成功后提交；保留参数的共享引用至解锁后；失败保留旧绑定 |
| `GameABI.h::ActiveGraph` | 未锁定地读取 graph 容器、活动下标和复制智能指针；仅在复制后保留引用不能保护复制过程 | 在管理器 `updateLock` 内检查下标并复制引用，解锁后使用图 |
| `Hooks.cpp::SimulateStatePhysics` | 未锁定地遍历控制器接收器；裸角色指针跨越回调 | 使用事件源锁，跳过 pending-unregister 项，通过角色 handle 取得强引用；解锁后获取图和发送事件；控制器及 world 使用局部强引用 |
| 地面状态及状态派发 | 未检查 stateManager、状态下标、注册项及实际状态类型就访问字段或虚函数 | 下标限制为 `[0, kTotal)`，空值检查，访问派生字段前做 Skyrim RTTI 转换 |
| 原始运动解码 | 无容器/数据时仍可能进入原引擎解码函数 | 检查容器、去除标志位后的数据地址和有效段数；缺失数据输出零平移/单位旋转 |
| Activate / Deactivate 汇编 | 合成调用帧无法作为正确的叶函数栈展开 | 改为 CALL 进入不修改 RSP 的寄存器适配器，再尾跳 C++ hook，保留真实调用返回地址 |
| 四个运动汇编入口 | 有栈帧但没有 Windows 展开信息 | 安装时注册 4 个 `RUNTIME_FUNCTION`，代码按 16 字节对齐，附加 DWORD 对齐的 `UNWIND_INFO` |
| `utils/Setting.h` | 长名称分支未初始化 name；字符串二次分配失败时泄漏名称 | 初始化 data/name；异常时释放名称；当前实际设置名是短常量 |
| `INISettingCollection.cpp` | 超长文件路径交给固定缓冲区安全 CRT 复制，可能触发 invalid-parameter 终止 | 先检查路径长度，再复制含终止符的有效范围 |

空指针检查只能处理空值。游戏传入的非空指针仍须满足对应 hook 的对象有效期约定；这里没有把地址可读性检查当作对象存活证明。

## 锁、引用和回调

插件缓存的普通操作只持一把分片锁或解析缓存锁；`Clear` 按固定顺序取得 16 把分片锁，再取得解析锁。没有发现反向锁顺序。generation 阻止跨清理的旧构建重新发布；读取者持有不可变共享快照，允许其他线程停用或清理。

退休的动画数据、graph owner 和缓存容器在锁外析构。异常注入覆盖首次激活、替换 graph、已有 clip 扩容，检查失败时旧状态完整、owner 不残留，并由析构回调重入 Lookup 验证解锁顺序。

物理路径的锁分为两段：事件源锁内只遍历接收器并取得角色强引用；随后释放它，再在管理器锁内复制图引用。采样与事件派发在这两把锁外。插件缓存锁不包围引擎插值、注释解析或事件回调。

`hkRefPtr` 包装器含普通指针，不能让同一个实例的并发读写自动安全。插件的缓存 owner 在分片锁下访问，解析结果发布后不再修改。动画引用防止缓存动画地址被复用；behaviorGraph owner 和采样期间的 BShkbAnimationGraph 引用分别保护缓存身份和内嵌 character 的存储。

## 1170 二进制证据

使用此前迁移报告中记录 SHA-256 的解包 EXE 和 Address Library，未改动 CommonLib 的类型头文件。

- 管理器：RVA `0x54C1F0` 调用 `0xBA3C50`；后者在 `0xBA3CCA` 获取 manager `+0xA0` 的锁，`0xBA3D23–0xBA3D73` 遍历 graph 数组，`0xBA3DC5` 读取 `+0xB0` 的 activeGraph，`0xBA3DE5–0xBA3E18` 释放锁。对应当前库的 `updateLock`。这证实引擎在该路径使用这把锁，并未覆盖所有写入者。
- `BSTEventSource` 的 Add/Remove/SendEvent 均使用自身锁；SendEvent 在锁内同步调用订阅者，因此插件先释放控制器事件源锁，再发送动画图事件。
- Havok 引用 ID `57010/57011`，RVA `0xA882E0/0xA883A0`：存在标志判断、TLS/临界区分支及直接计数分支。不能把所有 Add/RemoveReference 笼统视为无条件原子操作，也不能据此推断最终析构可在任意线程执行。

## 栈与 SIMD

运动入口仍传递五个参数：RCX 为容器、XMM1 为时间、R8 为输出、R9 为 clip 名，R13 中的角色写入第五参数槽。CALL 入口 RSP 为 8 模 16，减 `0x28` 后保持调用对齐，`[rsp+0x20]` 是第五参数，前 32 字节保留给被调函数。展开描述为 `UWOP_ALLOC_SMALL`，大小 `0x28`；表项的函数范围不包含目标地址常量和展开数据。

宿主测试直接执行生产 Xbyak 类生成的入口，检查五个参数；使用 `RtlLookupFunctionEntry` 和 `RtlVirtualUnwind` 检查入口与序言之后的 RIP/RSP 恢复。表和 SKSE trampoline 按插件进程期安装使用；没有实现 DLL 热卸载。

动态代码需要显式提供展开表，展开数据须 DWORD 对齐，依据 [Microsoft x64 展开规则](https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64?view=msvc-170)、[RtlAddFunctionTable](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtladdfunctiontable)；寄存器与影子空间依据 [Microsoft x64 调用约定](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170)。

SSE2 插值通过布局断言限制输入为 12 字节点，按三个标量构造向量，写回 8 字节 XY 和 4 字节 Z。没有对 NiPoint3 做 16 字节读写，也未启用 AVX2/FMA 或修改最低 CPU 要求。

## 验证

MSVC 19.44，x64 RelWithDebInfo 构建通过。CTest **5/5**：

- `amr_motion`：解析、空文本、溢出角度、边界、重复时间戳、与原数值顺序的比较。
- `amr_cache`：引用生命周期、LRU、graph/generator 身份、并发读取/写入/清理、锁外析构。
- `amr_cache_exception`：逐个分配点异常注入、旧状态保留、析构重入。
- `amr_interpolation`：有限输出逐位比较和特殊浮点分类比较。
- `amr_stack`：实际五参数入口执行与 Windows 展开。

另将 motion、cache、interpolation 三个宿主程序分别用 `/O1 /Zi /fsanitize=address /std:c++latest /EHsc /MD` 构建并执行，均通过，未报告访问越界或 use-after-free。异常分配测试保留为独立普通程序，避免它的全局 new 替换干扰 ASan。ASan 未用于游戏 DLL，也不检测数据竞争。

在 VS x64 环境复现普通测试：

```bat
cmake -S . -B build/ae1170-local -DAMR_BUILD_TESTS=ON
cmake --build build/ae1170-local --parallel 8
ctest --test-dir build/ae1170-local --output-on-failure
```

完整输出保存于 `docs/safety-logs/`；当前 DLL 身份见 `safety-build.json`。此前 `optimization-build.json` 保留为优化基线的历史记录。

## 尚未验证的引擎约束

1. **地面状态的共享和调度。** 仍沿用 AMR 在 `bhkCharacterStateOnGround::unk10` 记录垂直运动标志的逻辑。已检查真实类型，但没有证明该状态对象对每个控制器独占，或读取/写入在所有引擎任务间串行。如果对象共享，跨角色覆盖与数据竞争仍可能存在。这是当前最主要的待验证项；需要实机记录控制器、stateManager、ground state 地址和 hook 线程/时序后再决定改为哪个生命周期明确的侧存储。
2. **引擎对象的最终析构线程。** 插件锁外释放避免插件锁重入；这不证明 Havok graph/animation 最后一个引用允许在所有 hook 线程释放。需要实际引擎调度及析构路径验证。
3. **完整引擎锁协议。** 已跟随观察到的 manager 锁读取图，尚未枚举所有 graph/activeGraph 写入者。handle 解析的内部锁顺序也没有完全建立；当前未发现具体反向路径。
4. **外部输入与其他插件。** hook 的非空角色、动画、容器及 RTTI 指针依赖引擎调用契约；解码入口只验证已知标志/段数，不能从裸指针推出整个压缩块分配长度。其他插件异步销毁、修改注释或不遵守引擎锁的行为未覆盖。

本次没有启动或部署游戏。读档、新游戏、动画图重建、NPC 卸载、多角色并发和其他动画模组组合仍需实机验证。
