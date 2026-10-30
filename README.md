# 从源码生成并行任务的 Cholesky 运行时

完整的讲课式说明见 [架构设计文档](docs/architecture.md)，按源码、Metadata、任务图和并行执行的顺序展开。

这个项目做两件事：分析算法源码，生成描述计算过程的 **Metadata**；读取 Metadata 和输入矩阵，把可以同时执行的算子交给多个线程。

目前主要处理分块 Cholesky 分解：输入正定矩阵 A，输出下三角矩阵 L，使 `A = L × Lᵀ`。矩阵大小 n 和分块大小 b 由输入文件给出，程序调用提供的 `cholesky`、`trsm`、`madd` 算子完成计算。

优化的重点是少做任务展开、依赖分析和调度中的重复工作。Metadata 保存循环、条件、调用和读写区域等信息；运行时再代入 n、b，因此同一份 Metadata 可以处理不同大小的矩阵。

## 先跑起来

需要 Linux、支持 C++17 的编译器、Clang、CMake 3.16+、Python 3.9+ 和 zlib 开发库。Ubuntu 上可以这样安装：

```bash
sudo apt install build-essential clang cmake python3 zlib1g-dev
```

在仓库根目录执行：

```bash
python3 tools/benchmark.py accept-public20
```

这条命令会构建程序、生成公开的 20 个测试矩阵、运行分解并校验结果，最后验证另外三种算法示例。成功时会看到：

```text
PUBLIC20 ACCEPTANCE: PASS (20/20; supplied verifier exit=0)
```

本次输入、输出、日志和 `acceptance.json` 会保存在新建的 `artifacts/acceptance/public20-*/` 目录里。默认使用仓库自带的 demo 算子库；要使用比赛正式库，见后面的构建说明。

## 小矩阵可视化演示

```bash
python3 tools/benchmark.py visual-demo
```

命令启动本地网页服务。浏览器打开 **http://127.0.0.1:8765**，填写 n、b、线程数和种子，点击 **“生成并运行”**。默认 `n=6、b=2、2个线程`；页面依次显示生成进度、输入矩阵 A、Metadata、输出 L、重建误差与下载链接。计算和两种校验由真实程序执行，不是在浏览器里模拟结果。终端保持运行，Ctrl+C 停止服务；无需 npm 或前端依赖。

页面可查看 A、L、重建误差、Metadata，以及离线导出的真实任务依赖图；可点击节点查看读写块、逐步播放依赖放行。播放是依赖示意，不是真实线程时间线。本次显式使用 graph 路径并关闭循环合并，便于观察；演示耗时不作为性能调优结论。

每次运行在 `artifacts/visual-demo/n*-b*-*/` 独立保留 `input.bin`、`block_cholesky.meta`、`result.out`、`result.csv`、`metadata.json`、结果网页和日志。页面限制 n≤16、块行数≤4；`--port 8766` 更改服务端口，`--clang /path/to/clang++` 指定离线分析工具。只生成离线结果页可加 `--export --n 9 --b 3`。可视化导出器仅在此命令显式构建，不链接到计算程序，不增加默认计时开销或计分 Metadata 内容。

## 运行自己的输入

如果只想构建程序：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
```

构建会生成 `build/dag_run` 和 `build/block_cholesky.meta`。其中 Clang 用于分析算子源码的读写行为，Python 工具把算法和这些信息编成 Metadata。

把下面的 `input.bin` 换成你的输入文件：

```bash
python3 tools/benchmark.py submit build/dag_run input.bin output.bin \
  --metadata build/block_cholesky.meta
```

`output.bin` 是分解结果。这个启动入口会清理之前实验留下的环境变量，默认自动选择线程数和内存预算。需要限制资源时，在命令后加上 `--threads 8` 或 `--memory-mb 4096` 等参数即可。

默认输入文件是二进制格式：

```text
uint32：矩阵个数
随后每个矩阵：uint32 n、uint32 b、n×n 个 double
```

矩阵按行存储，要求 `b > 0` 且 `n % b == 0`。输出保留相同的矩阵个数、n 和 b，每个结果仍保存完整的 n×n 数组。文件读写遵循配套公开包的约定。

旧题面中不含 b 的格式也有兼容入口，需要明确指定格式和分块大小：

```bash
python3 tools/benchmark.py submit build/dag_run input-original.bin output-original.bin \
  --metadata build/block_cholesky.meta --io-format original --block-size 100
```

## 怎么测性能

### 和 demo 串行程序比较

```bash
python3 tools/benchmark.py compare-demo
```

默认测试公开 20 例，以及 `(n, b)` 为 `(4096, 64)`、`(8192, 32)`、`(8192, 256)`、`(4000, 1)` 的四组输入。双方使用相同输入和算子库，每组各运行一次、不预热，并分别校验结果。

只跑其中一组可以写：

```bash
python3 tools/benchmark.py compare-demo --datasets n4000-b1
```

汇总报告在 `artifacts/benchmark/comparison.json`。其中两种时间要分开看：demo 的原始计时只包住分解函数；当前程序的计时还包含 Metadata 加载、内存分配、任务准备和释放等工作。**它们的原始时间比值不能直接当作同口径加速比。** 报告也记录双方包含文件 I/O 的进程总时间。

### 重复测量当前程序

准备好输入文件后运行：

```bash
python3 tools/benchmark.py build/dag_run input.bin \
  --metadata build/block_cholesky.meta --checker build/dag_check \
  --warmups 1 --repeats 5 --report artifacts/performance.json
```

每轮启动一个新进程，计算后独立校验。报告包含各轮耗时、Metadata 实际字节数、逐 case 结果和资源限制。测量时请让其他构建和计算任务结束，避免互相干扰。

当前运行时计时只扣除矩阵文件 I/O；加载 Metadata、分配和初始化、任务展开、建图、线程池、算子执行及释放都在计时内。**源码分析和生成 Metadata 是否计入比赛性能时间仍待确认**，目前的运行时计时不能代表包含生成阶段的总时间，详见 [需求中的 O-08](REQUIREMENTS.md#口径与未完成条件)。

如果确实因内存不足而回退或失败，报告会标记 `resource_limited`，保留原因和原始时间，并将其排除出算法速度统计。普通慢样本不会因此被排除；只完成部分 case 也不算整组通过。

## 程序内部怎么工作

```text
算法源码 + 算子源码
        ↓ 分析控制流程和读写行为
     Metadata
        ↓ 代入本次输入的 n、b
   任务及其访问区域
        ↓ 分析依赖，或证明一组任务互相独立
    多线程执行
        ↓
     输出矩阵 L
```

当前运行时有两条主要并行路径：

- **依赖图执行**：根据读写冲突建立“谁必须等谁”的关系。任务的前驱都完成后，它才进入就绪队列。
- **分窗口执行**：对能证明独立的循环工作生成一组任务，并行执行完这一组后再继续。它减少了完整图的存储，但窗口之间需要等待。

连续调用还可以压缩成“起始参数 + 重复次数 + 参数增量”，减少任务描述数量；实际执行时仍逐次调用公共算子。线程池和矩阵容量可以跨 case 复用，case 本身按输入顺序处理。

线程默认使用实际允许的一个 NUMA 节点。总内存预算考虑该节点可用内存及进程限制；图预算再扣除矩阵、线程池等开销，不固定为 256 MiB。保留的矩阵容量也计入预算，必要时在数值计算开始前回收。

想读代码，可以从这几个文件开始：

| 文件 | 负责什么 |
| --- | --- |
| [compiler/compile.py](compiler/compile.py) | 解析支持的算法源码，生成 Metadata |
| [compiler/infer_catalog.py](compiler/infer_catalog.py) | 借助 Clang 分析算子的访问行为 |
| [src/metadata.cpp](src/metadata.cpp) | 解码、参数绑定、任务展开和循环压缩 |
| [src/graph.cpp](src/graph.cpp) | 从任务的读写区域建立依赖图 |
| [src/runtime.cpp](src/runtime.cpp) | 线程池、就绪队列和任务执行 |
| [src/main.cpp](src/main.cpp) | 输入输出、资源预算、路径选择和算子调用 |
| [tools/benchmark.py](tools/benchmark.py) | 构建、验收、性能测量和报告 |

## 其他算法和诊断工具

项目支持的是一个结构化 C++ 子集，包括受支持的 `for`、整数 `if/else`、算子调用及部分标量表达式，尚不能编译任意 C++。语法范围见 [编译器说明](compiler/README.md)，算子源码分析见 [相关说明](docs/operator_source_inference.md)。

单独验证朴素 Cholesky、右看式 Cholesky 和二维波前递推示例：

```bash
python3 tools/benchmark.py verify-algorithms
```

这些用例用于检查同一套机制能否处理不同算法，不参与分块算法的性能统计。`accept-public20` 已包含这一步。

若要研究图的关键路径或调度策略，可以运行估算工具：

```bash
cmake --build build --target dag_estimate metadata
python3 tools/benchmark.py estimate-schedule \
  --metadata build/block_cholesky.meta --n 16 --b 4 \
  --workers 1 2 4 8 --samples 5 --warmups 0 --policy current fifo \
  --dag-estimate build/dag_estimate --report artifacts/estimate-schedule.json
```

它会生成 JSON 和 SVG，帮助观察任务数、依赖边和理想化调度时间。估算没有覆盖真实运行中的全部准备成本、缓存争用和同步开销，应与端到端测量分开使用。

## 换成比赛正式库

把路径替换为真实的评委静态库：

```bash
cmake -S . -B build-judge -DCMAKE_BUILD_TYPE=Release \
  -DDAG_REQUIRE_OFFICIAL=ON \
  -DDAG_OFFICIAL_LIBRARY=/absolute/path/to/judge_kernels.a
cmake --build build-judge --parallel 4
python3 tools/benchmark.py accept-public20 --build-dir build-judge \
  --official-library /absolute/path/to/judge_kernels.a
```

CMake 默认分析仓库中的 demo 算子源码。若评委另提供了算子实现源码或头文件，应同时配置 `DAG_OPERATOR_SOURCES`、`DAG_OPERATOR_INCLUDES`，让分析使用的访问语义与实际链接的库一致。

正式环境是 Ubuntu 22.04 / Kunpeng-920，需要在实际获准资源上重新验收和测量。本地 demo 通过不代表正式环境已通过。

## 进一步阅读

- [题目原文](docs/problem_statement.md)：比赛希望我们完成什么。
- [需求说明](REQUIREMENTS.md)：接口、计时、评分及尚待确认的问题。
- [运行策略](docs/runtime_strategy.md)：不同执行路径的说明。
- [提交说明](SUBMISSION.md)：构建、验收和部署步骤。

历史计划和实验记录可通过 Git 查阅；当前性能结果以本次 benchmark 报告为准。
