# Source-driven DAG runtime

每次工作先读 [REQUIREMENTS.md](REQUIREMENTS.md)，这是唯一需求规范。本文仅说明当前程序用法。

**第二轮睡前推荐：round2/baseline（9690887），默认Metadata141字节。** 新实验没有稳定全程收益，保持P007数值实现并保留已提交R-27/R-28功能工具。当前生产源码与此完整基线一致；构建、正式库重链接、验收和干净启动见 [SUBMISSION.md](SUBMISSION.md) 顶部，旧候选为历史记录。

**比赛推荐与准确提交命令见 [SUBMISSION.md](SUBMISSION.md)。** 原交接版本为P005（257字节Metadata）；R-28后续实现P007改为141字节，变更和新证据见下节。默认资源自适应，不携带本机线程/内存覆盖。正式库和目标机未验证。

Python 编译器将支持的源码子集与算子契约转换为 Metadata；C++ runtime 按实际参数构图并调用公共 cholesky/trsm/madd。按后续回复，当前验收使用 public20 的 `count + [n, b, matrix]` 协议；原题面格式另有显式兼容模式。

## 一条命令完成本地验收

在仓库根目录运行：

```bash
python3 tools/benchmark.py accept-public20
```

命令依次配置/构建 Release、编译原包生成器和 verifier、生成20条输入、以两个位置参数运行 dag_run，然后执行原包 verifier。必须退出码为 0、case 0～19 全部 `status=PASS`，并与规格中的 n/b 匹配；随后执行保留的加强数值检查。任何一步失败，命令非零退出。

成功时显示 `PUBLIC20 ACCEPTANCE: PASS (20/20; supplied verifier exit=0)`。每次结果保存在新建的 `artifacts/acceptance/public20-*/`：包含输入、输出、各阶段日志、完整计算耗时、CPU 集和 `acceptance.json`，不会误用旧输出。

默认让 runtime 在实际允许的一个 NUMA 节点内自行选择线程；可用 `--threads 16` 限制线程数。`--build-dir build-judge --official-library /absolute/path/to/judge_kernels.a` 可验收指定库构建。默认库仍是 demo，公开20条通过代表本地接口和正确性验收，不代表全部隐藏用例或目标机成绩。此命令与下方重复性能测量共用同一个常驻端到端入口。

## 构建与运行

依赖 Linux、CMake 3.16+、C++17、Python 3.9+、pthreads 和 zlib >=1.2.9（Ubuntu 包 zlib1g-dev）。默认链接 demo 内核。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
mkdir -p artifacts
build/spd_generator artifacts/public20.bin \
  --spec vendor/public20/cases/preliminary_public_20.txt
DAG_METADATA="$PWD/build/block_cholesky.meta" DAG_THREADS=0 DAG_MODE=auto \
  build/dag_run artifacts/public20.bin artifacts/result.out
```

程序接收输入、输出两个路径。`DAG_THREADS=0` 使用实际允许的单 NUMA CPU 集；`DAG_MODE` 支持 `auto|parallel|serial`，`DAG_BACKEND` 支持 `atomic|owner`。

内存默认根据单 NUMA 可用估算、进程/cgroup 剩余和系统可用内存的最小值规划，并留 20% 余量；`DAG_MEMORY_MB` 可显式限制上限，校验器对应 `DAG_CHECK_MEMORY_MB`。这是资源快照，不是 OS 内存保留。无法发现单节点容量时要求显式设置预算。

`DAG_GRAPH_MEMORY_MB` 未设置或为0时，根据每个case扣除矩阵、Metadata、线程池和实际算子工作空间后的预算自动规划；正值仅作为额外图预算上限，不覆盖实际资源限制。任务展开、建图或执行器准备资源不足时，`auto`/`parallel` 都在复制和任何数值调用前改为流式执行，报告标为 `serial-resource`，并写入 `resource_limited`、原因、阶段和有效图预算；已经开始计算则绝不重放。该路径省内存，未承诺与并行同速。`DAG_MAX_TASKS`/`DAG_MAX_STEPS` 默认 0（不设总量上限），非零是用户主动限制，达到后明确失败，不静默绕过。

`DAG_STATS_FILE` 可指定统计 JSON；`COMPILER2026_TIMING_FILE` 可指定完整计算秒数文件。只有矩阵文件 I/O 从运行时间扣除，校验由独立程序完成。

## 原题面格式与朴素算法

`DAG_IO_FORMAT=original` 明确选择 `count + [n, matrix]`，输出同格式；默认值为 `public`，不自动猜测。分块应用必须额外提供实际指定的 b：`DAG_BLOCK_SIZE=100` 表示全部 case 使用该值，或 `DAG_BLOCK_SIZES=3,4,8` 按记录逐项指定；两者互斥。public 模式禁止覆盖文件中的 b。原题整数暂按同宿主 uint32 读取，正式字节序仍待确认。

```bash
DAG_IO_FORMAT=original DAG_BLOCK_SIZE=100 \
  DAG_METADATA="$PWD/build/block_cholesky.meta" \
  build/dag_run input-original.bin output-original.bin
DAG_IO_FORMAT=original build/dag_check input-original.bin output-original.bin
```

原生矩阵标量表达式示例可直接编译并执行：

```bash
python3 compiler/compile.py examples/naive_cholesky.cpp \
  --entry naive_cholesky --catalog examples/naive_catalog.json -o build/naive.meta
DAG_APPLICATION=scalar DAG_IO_FORMAT=original \
  DAG_METADATA="$PWD/build/naive.meta" \
  build/dag_run input-original.bin output-original.bin
```

`DAG_APPLICATION=blocked` 为默认正式分块应用，只接受公共算子；`scalar` 是显式的标量扩展示例，支持矩阵元素赋值、算术及 sqrt，不替代正式分块内核。scalar 原题模式无分块参数，绑定器内部占位 b=1，不用于算法分块。尚不支持局部 `double sum` 等任意 C++，未取得的官方朴素源码仍待核对。

## 唯一常驻测试：端到端性能

```bash
python3 tools/benchmark.py build/dag_run artifacts/public20.bin \
  --metadata build/block_cholesky.meta --threads 0 --mode auto \
  --repeats 5 --checker build/dag_check --report artifacts/performance.json
```

每轮从启动程序、加载 Metadata、读取输入、计算到生成输出，保留完整计算时间及程序墙钟时间；随后独立校验，不把校验耗时混入性能。失败记录保留并令整体失败，只有校验成功的样本参与成功耗时统计。报告记录输入/程序/Metadata/校验器指纹与 Metadata 大小。

原题面协议使用 `--io-format original`；标量应用使用 `--application scalar`，其报告只用于功能验证，不产生性能中位数或参与性能比较。benchmark 默认清除继承的 DAG 环境设置，因此原格式分块参数必须使用显式 CLI `--block-size 100` 或 `--block-sizes 3,4,8`（按输入记录对应，二者互斥）；不能仅设置父环境的 `DAG_BLOCK_SIZE`。基准向程序和校验器传递同一协议配置。例如：

```bash
python3 tools/benchmark.py build/dag_run input-original.bin \
  --metadata build/block_cholesky.meta --io-format original --block-size 100 \
  --mode auto --repeats 5 --checker build/dag_check \
  --report artifacts/performance-original.json
```

当前加强校验器使用残差、有限值、正对角线检查；指定的公开验收命令始终额外执行原包 verifier。demo/宿主结果不是正式库或 Kunpeng 成绩。多个配置依次运行，避免构建、生成器与测量并发。

临时验证放临时目录，用后删除脚本、数据和日志。项目不保留单元/回归测试套件、CTest 注册或历史测试报告文档。

### 其他算法：仅功能验证

```bash
python3 tools/benchmark.py verify-algorithms
```

### DAG 调度时间估算（显式诊断命令）

如果需要单独观察算子耗时、运行时任务图规模和调度策略的理想化影响，可以使用新增的 `estimate-schedule` 子命令。它不会被普通 benchmark、`summarize()`、`compare-reports` 或正式性能排名调用，也不会修改默认 runtime 调度配置。

先构建估算工具和当前 Metadata：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target dag_estimate metadata
python3 tools/benchmark.py estimate-schedule \
  --metadata build/block_cholesky.meta \
  --n 16 --b 4 \
  --workers 1 2 4 8 \
  --samples 5 --warmups 0 \
  --policy current fifo \
  --report artifacts/estimate-schedule.json \
  --dag-estimate build/dag_estimate
```

默认 `--warmups 0`，每个算子样本在计时区间外重新初始化缓冲区，适合原始应用数据没有预热机会的场景。校准使用当前构建中链接的 `cholesky`、`trsm` 和 `madd` C ABI；如果库或 ABI 无法使用，命令会报告错误，不会生成伪造耗时。

估算工具复用 `src/metadata.cpp` 和 `src/graph.cpp` 的 Metadata 解码、参数化实例化和完整 CSR 构图路径，报告任务数、边数、图内存、copy/zero-upper 阶段，以及 `current` 和 `fifo` 的估计 DAG 时间、总建模工作量、关键路径、理论下界、利用率、窃取次数、continuation 次数和各 worker busy time。`--graph-memory-mb`、`--max-tasks`、`--max-steps` 超限会明确标记 `resource_limited`。

报告还包含 demo baseline 的同一 `n,b` kernel 流程实测，并生成同名 `.svg` 对照图。该图只用于诊断，不是端到端性能结果。估算结果不包含 Metadata 解码、任务展开、建图、矩阵分配、线程池启动、copy/zero-upper（这些阶段会单独标记）、cache contention、内存带宽、NUMA、同步和 CPU 频率变化；报告固定包含 `performance_eligible: false` 和 `correctness_validated: false`。macOS 上可以运行这条独立估算路径，但现有正式 worker runtime 仍依赖 Linux CPU affinity/NUMA 接口，因此本机结果不能代表 Ubuntu/Kunpeng 正式性能。

该 benchmark 子命令构建当前运行时，将朴素左看式 Cholesky、右看式标量 Cholesky、二维波前递推源码分别编译成 Metadata，并在 n=3/7/16/31 的多 case 输入上验证串行和并行执行。两个 Cholesky 示例使用已知正对角因子构造输入，另跑配套 verifier 和 dag_check；波前使用独立的精确分数递推参考。示例只证明当前源码子集适用性，不冒充未提供的题目源码。

`accept-public20` 默认在分块计时结束后执行同一功能门槛；普通 benchmark 命令加 `--validate-algorithms` 可附加运行。功能失败令命令失败，报告以 `functional_only` / `performance_eligible: false` 标记，保留每算法 Metadata 大小、指纹和诊断。只有分块 Cholesky 进入性能汇总与比较；功能用例的时间、Metadata 大小不会累加到分块成绩。默认功能报告为 `artifacts/functional/functional-algorithms.json`。

## 正式库

```bash
cmake -S . -B build-judge -DCMAKE_BUILD_TYPE=Release \
  -DDAG_REQUIRE_OFFICIAL=ON \
  -DDAG_OFFICIAL_LIBRARY=/absolute/path/to/judge_kernels.a
cmake --build build-judge -j 8
```

正式环境使用真实评委归档，重新运行上述端到端性能入口并选择 `build-judge` 下的程序、Metadata 和校验器。外部链接配置只说明链接路径，不能认证库来源。目标是 Ubuntu 22.04 / Kunpeng-920 的实际获准单 NUMA 资源。

源码子集和 Metadata 格式见 [编译器说明](compiler/README.md)，实际模块见 [模块输入输出](docs/problem_alignment.md)。

## 比赛候选与干净部署

候选commit、配置、Metadata大小、完整性能和验收证据见根目录 [GOAL.md](GOAL.md) 的Pareto台账。每个 `pareto/Pxxx` 标签指向实际被测实现，证据在 `results/pareto/Pxxx/`。本地宿主成绩不等于正式目标机成绩。

benchmark默认清除继承的DAG实验设置，显式CLI参数再覆盖；`--inherit-env` 才保留继承配置。部署也可使用同一工具的干净入口，子程序仍只接收两个文件参数：

```bash
python3 tools/benchmark.py submit build/dag_run input.bin output.bin \
  --metadata build/block_cholesky.meta \
  --configuration-report artifacts/submission-config.json
```

正式部署优先采用这个干净入口；前面的直接环境变量调用用于明确配置的本地运行。需要目标机限制时，使用 `--threads`、`--memory-mb`、`--graph-memory-mb` 等显式 CLI 参数，或在确有需要时选择 `--inherit-env`；操作系统亲和集、cgroup 和 rlimit 不被绕过。

内存受限运行保留原始耗时和失败记录，但不进入完整算法性能汇总。失败报告分别列出已校验有效、资源回退、已执行但未校验、资源失败、未运行和错误/未知的 case 数量；没有验证的先前结果不产生速度。`compare-reports` 仅比较相同输入及资源约束下的共同有效集合，输出覆盖率；资源指纹包含当前 cgroup 到可见挂载根的稳定内存/cpuset 限额，不含波动的 usage/free 计数。

该入口默认自动线程与预算，避免把本地16线程或低内存限制带到目标机。确有目标资源限制时通过 `--threads`、`--memory-mb`、`--graph-memory-mb` 明确指定，OS亲和集/cgroup限制始终有效。

资源回退的原始耗时保留，但不进入算法完整性能汇总；共同有效case诊断单独列出覆盖率和排除原因，不能冒充完整公开集成绩。可在相同输入、硬件资源和算子库条件下执行 `python3 tools/benchmark.py compare-reports report-a.json report-b.json --report comparison.json`，只比较共同有效case。

## R-28：默认执行文件精简

当前默认编译输出lean/lean-deflate，分块Metadata为141字节。删除默认SHA-256来源字段、额外FNV和zlib包装校验；源变量名、表达式运算符及重复调用引用改为编号。外部算子和缓冲区绑定身份仍各保存一次，控制流程与读写语义不变。

重复图扫描和契约审计默认关闭，原有DAG_AUDIT=1可显式开启。资源约束、数值正确性与配套verifier门槛仍保留。默认编译不计算来源SHA；仅显式旧格式或可选json-debug在离线编译时生成旧字段，历史Metadata仍可读。

实现commit与五轮交错结果见 [P007](results/pareto/P007/manifest.json)。此前SUBMISSION.md的P005为历史可回退交接，不代表这次更新后的默认产物。验收继续使用现有accept-public20入口，并包含R27功能验证。
