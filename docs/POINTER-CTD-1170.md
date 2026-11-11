# AMR 2.0.2：物理回调发送空动画事件导致 CTD

审核日期：2026-11-11。基线：`2d5c052b8bf065402aa934387363cd65ba5e25c0`，插件 2.0.1，Steam Skyrim 1.6.1170。

## 结论

本次日志与旧 DLL 机器码共同指向 `SimulateStatePhysics` 广播的空 `BSAnimationGraphEvent`。`event{}` 的 `tag` 底层指针为 null；原生 ActorMediator 接收器直接扫描该字符串，在 `SkyrimSE.exe+01EE5C5` 读 null。物理 hook 的函数槽位和参数签名匹配 1170 原生代码。

日志 `crash-2026-11-11-18-39-12.log` 的 probable 调用栈为：

```text
SkyrimSE.exe+01EE5C5  字符串扫描，RDI=RBX=0
SkyrimSE.exe+06CD3B8  ActorMediator::ProcessEvent 返回点
AMR.dll+002D74B       BSTEventSource<BSAnimationGraphEvent>::SendEvent
AMR.dll+002DA7B       hooks::SimulateStatePhysics
SkyrimSE.exe+0E9FD2F  原生控制器更新
```

旧 DLL/PDB 的符号和 `.pdata` 范围已核对。旧 DLL `+0x2DA69–+0x2DA76` 清零事件并调用 `SendEvent`；其 `+0x2D748` 调 sink 的 vtable slot 1。ActorMediator slot 1 指向 `0x6CD390`，在 `0x6CD3B3` 调用 `0x1EE580`。后者在 `0x1EE5B8` 从事件偏移 0 读取 tag，然后在 `0x1EE5C5` 扫描，没有 tag 判空。事件布局为 tag@0、holder@8、payload@0x10；native 稍后另有 holder 判空。该链不需要假设 Acheron 或 po3 Tweaks 冲突。

反汇编和映射见 `pointer-audit-evidence/pointer-audit-native.txt`；1170 地址核验结果见同目录 `pointer-audit-abi.json`。日志为 probable unwind，尚未进行游戏内复测。

## 修复

`include/PhysicsEvents.h` 提供生产代码与测试共用的事件构造函数：

```cpp
return { "AMR_PhysicsUpdate", a_actor, "" };
```

`source/PhysicsHooks.cpp` 在原发送位置使用该构造函数。事件名是有效的非空字符串，holder 来自已持有 `NiPointer<Actor>` 的 actor；actor 和 graph 的强引用均覆盖同步分发。payload 保持空值。此次未改变物理射线、重力、移动和状态分发算法。

这段广播可追溯至上游旧历史 `f1e23bf36116218b088391c7b5e1b392512881e3`（2022 年）。本地 NG 移植 `218b3d4` 保留了它，后续 2.0 合并继续沿用。此次合并的上游 2.0 已在 `d3103c7` 删除整段旧物理钩子，因此不能将此问题归为当前上游 2.0 仍有的缺陷。

## 回归验证

- 先将原有空事件构造抽取到生产 helper，运行新测试：退出码 1，`check failed: event.tag.value != nullptr`。
- 修复后完整 MSVC 19.44 x64 RelWithDebInfo DLL 构建成功，插件版本 2.0.2。
- CTest 9/9 通过：motion、warp、cache、cache_exception、physics_event、interpolation、stack、trampoline、trampoline_legacy。
- 新 physics_event 测试使用轻量 RE stub，调用生产构造函数并检查 raw tag、NUL 字符串扫描、holder 和空 payload。它验证构造回归，不替代 Skyrim 字符串池及原生事件分发的实机测试。
- 独立复审核对了实际 CommonLib `BSFixedString` 构造路径、事件字段顺序、Actor pin、graph 到事件源的多继承转换和同步回调寿命。

未运行或部署游戏。需要测试者用 2.0.2 复测创角及正常游戏。

## 独立指针审核的其他结果

- ground-state vtable slot 8 指向 RVA `0xF02550`，原生第二参数为 controller；controller `GetHavokWorld` slot `0xF` 与 world `PickObject` slot `0x33` 的参数和返回 ABI 均吻合。原生 gravity helper 是指针返回。
- 未发现可证实的缓存 map 数据竞争或悬空解引用；graph/animation 快照、generation 拒绝旧回写及锁外退休路径已核对。
- 撤回“COL cast 参数错误”的初判：实际 MSVC 多继承 RTTI 实验中，当前 COL offset 加动态 RTTI、显式静态 source RTTI 与 `dynamic_cast` 返回一致。
- 撤回“bhkPickData::ray 未初始化”的初判：实际 CommonLib 类型在填充 0xCD 的内存上默认构造后，ray 和 collector 均为零；`hkVector4` 自带零初始化构造。
- 两个独立的可选 warping 图变量一致性问题仅记录，未纳入本次 CTD 修复：AnyWarping 扫描与 setter 分离可能乱序发布；PreLoadGame/NewGame 的 Clear 未主动退休旧图 bool。它们不构成本次空 tag 崩溃的解释。
- 引擎所有写入者的 graph 锁协议、Havok 最后引用析构的线程约束，仍无完整证据；未将这些未知项当作已确认缺陷。
