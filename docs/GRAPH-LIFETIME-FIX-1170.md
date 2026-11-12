# AMR 2.0.4 图实例生命周期

适用：Steam Skyrim 1.6.1170，SKSE 2.2.6 或支持该游戏版本的后续 SKSE。

## 播放数据与引擎对象分离

2.0.3 已删除按动画地址复用的解析 LRU；每次激活复制注释为 AMR 自有数值曲线。2.0.4 继续移除活动表中的 `hkRefPtr<const hkbBehaviorGraph>`。活动表只保存自有曲线、自有播放状态以及用于匹配的地址标记，清理这些数据不会对动画或 behavior graph 调用 Havok 引用计数函数。

采样期间仍在 manager 的 `updateLock` 内复制当前 `BShkbAnimationGraphPtr`，并持有到本次同步采样结束。这个局部引用覆盖其内嵌 `hkbCharacter` 的使用期，不进入长期播放表。

## 实例登记与失效

- `BShkbAnimationGraph` 原生构造函数返回后，为内嵌 character 登记新实例编号。
- 原生析构函数开始前，移除登记和该 character 的全部播放记录。移出的自有对象在表锁外释放。
- 每次激活先获取包含 character、behavior graph、实例编号和清理代数的 ticket；提交时在同一 shard 锁内核对它。
- 活着的 character 换用另一个 behavior graph 地址时，`Begin` 更新实例编号并退休旧播放记录。
- 同一地址销毁后重建，会获得不同实例编号。旧 ticket 的激活、停用和异常恢复均不能修改新实例。
- `PreLoadGame` / `NewGame` 清空播放记录并更新清理代数，保留仍存活图的登记。清理前取得的 ticket 失效；存活图的后续激活可取得新 ticket。
- 激活不会补建缺失的生命周期登记，因此析构期间的回调不能重新创建已退休记录。

逐帧采样使用已解析曲线和各次播放的 warping 历史。V1 查询 API、曲线计算和已有物理事件修复保留。

## 1.6.1170 原生入口

构造函数：Address Library ID `63555`，RVA `0xBAD070`；参数为 RCX=this、RDX=holder、R8B=flag，RAX 返回 this。内嵌 character 位于 `this+0xC0`。

| 构造调用点 | RVA |
| --- | --- |
| ID 20108 + 0x64 | 0x2F6FD4 |
| ID 32973 + 0x5A | 0x54E94A |
| ID 32989 + 0x5A | 0x54EEFA |
| ID 37979 + 0x7A | 0x69D59A |
| ID 40753 + 0x64 | 0x74E254 |
| ID 63547 + 0x1F | 0xBACCAF |

析构函数：ID `63556`，RVA `0xBAD2C0`；RCX=this。两个直接调用为 ID `63547+0x38`（RVA `0xBACCC8`）及 ID `63758+0x19`（RVA `0xBBCF49`），异常清理路径另有 RVA `0x166E737` 的尾跳转。主虚表 ID `232870` 的 slot 0 指向删除包装函数 ID `63758`，其内部调用同一析构函数。

`tools/verify_1170.py` 校验这些入口的目标，并扫描本地完整 EXE 中指向上述构造/析构函数的 E8/E9 相对引用，结果与安装集合一致。证据见 `graph-lifecycle-evidence/abi.json`。这些偏移专用于已限定的 1.6.1170。

## 验证

- 修改前的实际活动表保留 graph owner；源图到期后的清理触发引用释放计数，回归以 `check failed: expiredGraphReleases == 0` 失败。
- `amr_graph_lifetime` 使用生产 `MotionCache`，检查销毁、地址复用、behavior graph 替换、Clear 后继续激活，以及通过 barrier 控制的晚到提交/停用。
- 缓存测试检查各次播放的恢复顺序、采样快照寿命、锁外析构、并发读写与 Clear。分配失败测试覆盖首次激活、追加、重绑恢复和登记失败；析构退休无需分配。
- 生产安装器与实际 CommonLib trampoline 测试安装 16 个位置，验证 380 字节预算、单次池申请、原目标保存、E8/E9 路由、边界哨兵和 4 份 unwind 表。
- MSVC x64 RelWithDebInfo DLL 构建通过，CTest 11/11 通过。生产数值快照及图实例测试的 MSVC AddressSanitizer 构建通过。
- 1170 离线 ABI 校验通过：16 个入口、10 个虚表条目、8 个 helper 地址。

以上覆盖 AMR 表的所有权、失效顺序和离线安装。游戏中的构造/析构转发、OAR 换动画及读档流程尚未实测；引擎仍负责传入回调参数在该次调用中的有效期。

## 安装

替换 `SKSE/Plugins/AnimationMotionRevolution.dll`，保留现有 INI。包内提供匹配 PDB、ABI 证据及构建清单。
