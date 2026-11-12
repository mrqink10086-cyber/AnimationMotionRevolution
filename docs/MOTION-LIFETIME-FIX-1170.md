# AMR 2.0.3 动画数据生命周期修复

适用：Steam Skyrim 1.6.1170，SKSE 2.2.6 或支持该游戏版本的后续 SKSE。

## 修复

2.0.2 的解析结果包含 `hkRefPtr<const hkaAnimation>`，并进入按动画地址索引的 32 项 LRU。日志中的释放链为 `MotionCache::Remember` 淘汰解析结果，随后 `ParsedAnimation` 析构调用旧动画的 `RemoveReference()`，最终进入无效虚表。

2.0.3 删除这层 LRU，解析结果只包含 AMR 自有的平移、旋转、warping 数值及派生标志。每次激活都从当前绑定动画复制并解析注释，解析函数返回后不保留源动画或注释文本。活动 binding 中的动画地址只用于匹配激活与停用，代码不解引用该标记。

停用、重绑、清空和最后一个采样快照释放时，只销毁这些自有曲线，不再对动画调用 `AddReference()` 或 `RemoveReference()`。同一地址装入新动画时重新解析当前注释，避免旧地址命中旧曲线。

代价是每次激活重新解析注释；逐帧采样仍使用已解析曲线。插值公式、搜索方式、motion warping 和 2.0.2 的有效物理事件修复均保留。

## 验证

- 修改前，将生产解析类型与实际 `MotionCache` 组合；第 33 个条目触发 LRU 淘汰，已卸载源动画的引用操作计数非零，测试以 `check failed: RE::expiredReferenceCalls == 0 at line 81` 失败。
- 修改后，`amr_snapshot` 释放 64 个源动画的实际堆内存，随后检查采样、停用、清空及持有快照的释放；动画引用操作计数为零。
- 在同一内存地址析构并重建源动画，确认新激活采到新曲线，已有独立快照仍保存原曲线。
- 新增生命周期测试的 MSVC AddressSanitizer 构建通过。
- MSVC x64 RelWithDebInfo DLL 构建通过；CTest 10/10 通过，包含解析、采样、warping、缓存并发、分配失败、物理事件、调用栈和 trampoline 检查。

生命周期测试使用生产解析器、快照类型和缓存，测试内的 Havok 类型替身记录源动画引用操作。以上为离线验证；游戏内验证尚未执行。

## 安装

替换 `SKSE/Plugins/AnimationMotionRevolution.dll`，保留现有 INI。压缩包同时提供匹配 PDB 和文档。
