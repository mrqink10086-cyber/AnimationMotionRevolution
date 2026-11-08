# AMR 执行与缓存优化

日期：2026-11-08。目标：Skyrim 1.6.1170 / SKSE 2.2.6；基于已完成 ABI 迁移的 AMR 1.5.3。

本文的基准和三组测试记录对应优化基线。后续的锁、游戏指针、异常回滚与汇编展开修订，以及当前五组测试结果，见 [安全审核报告](SAFETY-AUDIT-1170.md)。

## 实现结果

动画注释现在按动画对象解析并共享，采样通过短锁取得不可变快照，随后在锁外查找关键帧、插值和调用引擎。优化集中在注释解析、缓存查找、内存分配和跨角色锁竞争。

| 路径 | 原实现 | 当前实现 |
|---|---|---|
| 激活 | 各角色复制和排序曲线 | 按动画共享已解析曲线，缓存无自定义运动的结果 |
| 排序 | 各角色激活时重新解析和排序 | 每份共享动画首次解析后排序，沿用原 std::sort 比较器 |
| 缓存读取 | 临时 string、嵌套 map、多次查找 | string_view 异构哈希查找，复制 shared_ptr 快照 |
| 同步 | 所有角色共用递归自旋锁，锁内执行解析和引擎调用 | 16 个按角色分片、按 64 字节对齐的 mutex；引擎调用和解析在锁外 |
| 关键帧定位 | 每次从头扫描 | 64 帧以内连续扫描，长曲线 lower_bound |
| 平移插值 | NiPoint3 运算符调用 | SSE2 并行处理三个坐标，保持原乘法、加法顺序和区间除法 |
| 旋转插值 | 原引擎四元数插值 | 保留原引擎函数、参数和区间进度公式 |
| yaw 注释 | 通用 Euler 三角函数计算 | 复用 yaw 的 sin/cos，保留原角度换算顺序和零 roll/pitch 的表达式 |
| 失败处理 | 可能清空整个容器并使现有指针失效 | 独立构建，再发布共享快照；已有条目保持有效 |

`MotionCache.h` 仅依赖 STL，便于直接验证生产缓存逻辑。缓存读取后持有的快照允许其他线程同时停用 clip 或清空缓存。

## 时序和所有权

激活顺序维持为“读取注释、发布绑定、原 ComputeStartTime”；停用维持为“移除绑定、原 ResetIgnoreStartTime”。曲线时间允许循环、倒放、seek，因此每次独立定位区间，不保存可失效的上次采样游标。

解析缓存保留最近 32 个动画。每份曲线持有 `hkRefPtr<const hkaAnimation>`，防止地址复用命中旧内容。当前播放的绑定另外持有曲线快照；32 是解析 LRU 的条目上限，活动曲线按实际播放数量占用内存。激活同一动画的多个角色共享关键帧向量。

活动绑定以 `hkbCharacter* + behaviorGraph 身份 + clip 名称` 查找，并持有 behaviorGraph 引用。采样期间另外持有活动 BShkbAnimationGraph 的强引用。替换 graph 身份时退休旧绑定；对象释放发生在插件锁外。每次激活追加一条包含 generator 身份的记录，停用匹配对应 generator 和动画，避免同一动画的不同 generator 相互停用。

同一角色、同名 clip 同时绑定不同动画时，采样使用最近激活且仍活动的动画；它停用后恢复前一个。当前采样 hook 只有角色和 clip 名称，无法区分两个同名 generator 各自的曲线。独立选择需要扩展采样位置以取得 generator/animation 身份。

SKSE 的 PreLoadGame / NewGame 消息同步清理缓存。清理同时锁定全部分片和解析缓存、递增 generation，并交换出旧容器；解锁后释放对象。跨清理的在途构建不能再发布到新一代缓存。遗漏停用通知的旧条目会在 graph 替换或下次生命周期清理时释放。

注释解析检查 from_chars 的状态，跳过缺失数值、额外非空白内容和非有限数值。零长度区间保持端点行为。合法注释的负时间、首帧前、末帧后、相同时间戳和区间边界由回归测试与原采样公式比较。

## 与原版的数值一致性

角度换算维持 `yaw = yawDegrees * pi_v<float> / 180.0f`，之后计算 `yaw / 2`。不改写为 `yawDegrees * (pi / 360)`；两者的舍入位置以及大角度溢出行为可能不同。零 roll/pitch 的四元数分量保留原表达式，覆盖有符号零。

区间进度维持 `(sampleTime - previousTime) / duration`，以及原来的 epsilon 判断。时间差仍由两个关键帧相减得到：这个标量减法无需额外缓存数组和内存访问。平移保持 `current * progress + previous * (1 - progress)`。SSE2 分别执行乘法和加法，不做 FMA 收缩。只读取和写入三个有效 float，不跨越 NiPoint3 的 12 字节边界。

旋转的区间定位、进度和传给回调的四元数由宿主回归测试对照；生产回调继续调用 1.6.1170 的引擎插值函数。宿主测试使用替身数值类型，不能执行游戏内的该函数。

## 调度、物理和指令集

采样结果参与当前帧的运动计算，需要同步返回。SKSE AddTask 在之后执行，无法满足这个时序，因此继续在引擎调用线程执行。解析移出采样热路径并缩小临界区即可降低调度等待。

物理替换中的动画事件、射线检测、运动计算、重力和状态派发保持原来的次序。空动画事件仍会同步访问订阅者；目前没有经过验证的条件可以跳过这些步骤。

平移插值使用 x64 基线已有的 SSE2，现有 hkVector4 SIMD 保持不变。AVX2 的宽向量需要成批数据才能利用更多通道；当前 hook 每次只提供三个坐标，增加打包和分派没有已测收益，因此没有提升最低 CPU 要求。

不启用 `/fp:fast` 或 FMA；MSVC 目标显式使用 `/fp:precise`。FMA 将乘加合并为一次舍入，会改变原版两步运算的结果（[MSVC 浮点模式说明](https://learn.microsoft.com/en-us/cpp/build/reference/fp-specify-floating-point-behavior?view=msvc-170)）。SSE/AVX2 没有直接计算 sin/cos 的向量硬件指令；相关向量函数属于数学库实现，不能据此假定与原运行库逐位相同（[Intel SVML 说明](https://www.intel.com/content/dam/develop/external/us/en/documents/cpp_compiler_classic.pdf)）。yaw 在动画首次解析后随曲线缓存，继续使用原运行库 sin/cos。

## 复现验证

在现有 VS x64 构建环境中：

```bat
cmake -S . -B build/ae1170-local -DAMR_BUILD_TESTS=ON
cmake --build build/ae1170-local --parallel 8
ctest --test-dir build/ae1170-local --output-on-failure
build\ae1170-local\amr_benchmark_motion.exe
build\ae1170-local\amr_benchmark_interpolation.exe
build\ae1170-local\amr_benchmark_cache.exe
```

`amr_motion` 使用与生产相同的曲线和解析头文件，并为 RE 数值类型提供最小宿主替身。`amr_cache` 直接实例化生产缓存，检查 LRU、空结果缓存、重复激活、同名动画恢复、graph 身份、generation、锁外析构和并发清理。`amr_interpolation` 对照标量公式和生产 SSE2 函数，覆盖随机值、零、非正规数、极值和无穷大；有限结果逐位比较，NaN 按分类比较。检查在 RelWithDebInfo 中保持启用。

基准使用本机宿主程序和合成数据，衡量对应函数的耗时；游戏内的总帧时间仍需结合实际动画数量与线程负载测量。

## 本机测量

Intel Core i5-13600KF；MSVC 19.44，x64 RelWithDebInfo、`/O2 /MD /fp:precise`。以下记录最终实现的一轮合成基准；相同输入的校验和一致。

| 合成负载 | 旧公式或旧结构模型 | 当前实现 |
|---|---:|---:|
| 单独平移插值，262144 点 × 12 轮 | 标量 13.550 ms | SSE2 5.225 ms |
| 完整采样，4 个关键帧，30 万次 | 2.444 ms | 2.559 ms |
| 完整采样，16 个关键帧，30 万次 | 3.281 ms | 3.503 ms |
| 完整采样，128 个关键帧，30 万次 | 8.585 ms | 6.910 ms |
| 缓存查找，1 线程 × 20 万次 | 11.488 ms | 10.967 ms |
| 缓存查找，4 线程 × 各 20 万次 | 43.051 ms | 28.197 ms |

短曲线的完整采样结果有涨有跌；独立 SIMD 算术的收益不能直接当作完整 hook 或 FPS 的收益。保留了短曲线的连续扫描，避免统一二分查找增加其成本。移除了额外的时间差缓存数组，减少内存和采样时的间接访问。

缓存基准用 256 个角色身份、64 字节 clip 名和互不重叠的线程角色范围；旧结构模型使用嵌套 map、临时 string、一次 find 和全局 std::mutex。它没有执行原版的引擎自旋锁、解析或插值，因此只比较相应的数据结构和竞争形态。完整原始输出见同目录 `benchmarks/`。

构建和三组宿主测试通过。游戏内的实际 hook 线程分布、动画行为和帧时间尚未实测。
