# 作者分支同步记录

## 合并来源

- 本地分支：`main`，同步前 `dfc52bc`。
- 备份分支：`backup/pre-maxsu-sync-20261108`。
- 上游：[`max-su-2019/AnimationMotionRevolution`](https://github.com/max-su-2019/AnimationMotionRevolution)，固定提交 [`b5be808a212f2b124f19d5365a18d1158d24b77b`](https://github.com/max-su-2019/AnimationMotionRevolution/commit/b5be808a212f2b124f19d5365a18d1158d24b77b)。
- 合入作者新增的八个提交，采用保留双方父提交的 merge；源码版本更新为 2.0.0。

此次同步包含攻击位移设置、悬崖保护、分段 `animwarp`、战斗目标 warping、行为图状态、TrueHUD 调试及 V1 查询 API。
动画注释格式和配置说明见 [开发文档](developer-guide.zh-CN.md) 和 [animwarp 文档](animwarp.zh-CN.md)。

## 本地整合

本地仍以相邻 `../CommonLibSSE-NG-ng` 为依赖，限定 Skyrim 1.6.1170 / SKSE 2.2.6。
保留经二进制核验的钩子地址、版本化图管理器访问、游戏对象引用、动态汇编的 Windows 展开信息，
以及共享的不可变动画解析结果与 16 路缓存分片。

作者的可变 warping 状态按活动 clip 分离，避免不同角色或同时播放的 clip 共同修改共享解析数据。
缓存查询只在短临界区内取得快照；引擎调用和 TrueHUD 绘制不在共享缓存锁内执行。
游戏载入与新游戏会清理活动记录；解析缓存中的动画不应单独让 API 返回 true。
同一 generator 更换动画会替换旧绑定；切换到无 motion 动画、或更换行为图后遇到无 motion 动画，也会清理旧活动记录。
重复激活同一动画的计数与其他 generator 的绑定仍保留。退休的动画、图和运行时所有权在缓存锁释放后销毁。
解析或激活抛出异常时，通过无需分配的节点摘除撤销该 character 的活动记录，回退原生位移；过期 generation 不会清理新游戏记录。图状态刷新与所有权释放在分片锁外执行。
图变量只在 warping 状态、所属角色或活动记录发生变化时重新聚合；垂直位移标记在解析时计算，采样不再每帧扫描全部活动记录或全部位移关键帧。

保留作者的 SimpleIni 设置、BDI 配置和可选的 TrueHUD 集成。设置在安装钩子前初始化，未增加运行时重载。
本地默认 `uLogLevel=4`，避免逐动画调试日志。`AMR_ENABLE_TRUEHUD_DEBUG=OFF` 可在编译时去掉 TrueHUD 调试代码。

## API 合约

导出名为 `RequestPluginAPI`，V1 的枚举底层类型为 `std::uint8_t`、值为 0。
`IVAMR1` 没有虚析构函数，唯一虚方法为：

```cpp
virtual bool HasAnimMotionData(const RE::hkbCharacter* a_hkbCharacter) const noexcept = 0;
```

它查询该 character 是否有活动的 AMR 位移/旋转记录。空指针返回 false；未知接口版本返回 nullptr。
它不等同于当前帧已经应用位移，不应跨帧缓存结果。接口由 DLL 内的单例提供，消费者不负责释放。
这与作者提交中的 V1 ABI 及 AnimationMotionFix 的消费者声明一致。

## 验证

2026-11-08 在 MSVC 19.44 / x64 / RelWithDebInfo 下完成以下验证：

| 检查 | 结果 |
| --- | --- |
| TrueHUD 调试开启与关闭的完整 DLL 构建 | 均通过，无编译警告 |
| 最终普通 CTest | 6/6 通过：motion、warp、cache、cache_exception、interpolation、stack |
| AddressSanitizer 构建的 CTest | 6/6 通过；前四项启用 ASAN，interpolation/stack 使用普通构建 |
| 1170 离线 ABI 核验 | 7 个调用点、9 个虚表项、5 个辅助 RVA 全部通过 |
| DLL 导出与声明 | x64、版本 2.0.0、1170 / SKSE 2.2.6、STRUCT_DEPENDENT；存在 `RequestPluginAPI` 与 SKSE 三个导出 |

新增回归覆盖 warp 解析及分段、独立活动状态、motion A→B→reset 不复现 A、无 motion 切换和行为图替换清理。
缓存测试还通过析构回调确认退休所有权不在分片锁内释放。
故障注入覆盖重绑定解析分配失败后的清理、零分配摘除、访问与析构回调重新查询缓存，以及过期 generation 不影响新记录。
最终默认开启 TrueHUD 调试，DLL 大小为 756736 字节，SHA-256 为：

```text
f807d59a7bcd3584c5be5e2129cd06f8db6c0530ba848b2b8b60f1b64f43e888
```

本次证据保存于 [maxsu-sync-logs](maxsu-sync-logs/)，原有 safety 报告和日志保留为同步前基线。
复现普通构建与测试：

```powershell
cmake -S . -B build/ae1170-local -DAMR_BUILD_TESTS=ON -DAMR_ENABLE_ASAN_TESTS=OFF -DAMR_ENABLE_TRUEHUD_DEBUG=ON
cmake --build build/ae1170-local -j6
ctest --test-dir build/ae1170-local --output-on-failure --no-tests=error
```

以上命令需在 VS x64 开发者环境中运行，并沿用 README 中的本地依赖配置。启用 ASAN 时须保留该环境的运行库 PATH。

## 证据范围

宿主测试覆盖解析、缓存、运行时状态隔离与汇编调用约定；离线反汇编核验已知 1170 钩子。
这些检查不覆盖游戏内 Havok 工作线程调度、目标消失、悬崖碰撞和第三方动画/行为图的所有组合。
活动表沿用作者的非拥有 character 地址键，API 不解引用该键。采样期间保留外层动画图，缓存保留内层行为图与动画；后者不拥有嵌入在外层图中的 character。
正确失效仍依赖 reset、图替换激活与读档清理。如果引擎遗漏 deactivation 后重用了 character 地址，API 可能暂时命中旧记录；此次没有把任意地址查询扩展为游戏指针有效性检测。
此次未运行 Skyrim；游戏内验证应包含攻击切换、多个 clip 混合、目标死亡/卸载、读档、NPC 同时战斗及 TrueHUD 开关。
原有迁移与性能报告中的微基准描述同步前版本，不能直接作为新增 warping 的帧率测量。
