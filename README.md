# Source-driven DAG runtime

每次工作先读 [REQUIREMENTS.md](REQUIREMENTS.md)，这是唯一需求规范。本文仅说明当前程序用法。

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

依赖 Linux、CMake 3.16+、C++17、Python 3.9+ 和 pthreads。默认链接 demo 内核。

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

`DAG_GRAPH_MEMORY_MB` 默认 256，仅限制尝试完整构图的存储。任务展开、建图或执行器准备资源不足时，`auto`/`parallel` 都在复制和任何数值调用前改为流式执行，报告标为 `serial-resource`；已经开始计算则绝不重放。该路径省内存，未承诺与并行同速。`DAG_MAX_TASKS`/`DAG_MAX_STEPS` 默认 0（不设总量上限），非零是用户主动限制，达到后明确失败，不静默绕过。

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

原题面或标量应用的最终测量使用 `--io-format original`、`--application scalar`（按需），分块参数仍显式通过上述环境变量给出；基准会向程序和校验器传递同一协议配置。

当前加强校验器使用残差、有限值、正对角线检查；指定的公开验收命令始终额外执行原包 verifier。demo/宿主结果不是正式库或 Kunpeng 成绩。多个配置依次运行，避免构建、生成器与测量并发。

临时验证放临时目录，用后删除脚本、数据和日志。项目不保留单元/回归测试套件、CTest 注册或历史测试报告文档。

## 正式库

```bash
cmake -S . -B build-judge -DCMAKE_BUILD_TYPE=Release \
  -DDAG_REQUIRE_OFFICIAL=ON \
  -DDAG_OFFICIAL_LIBRARY=/absolute/path/to/judge_kernels.a
cmake --build build-judge -j 8
```

正式环境使用真实评委归档，重新运行上述端到端性能入口并选择 `build-judge` 下的程序、Metadata 和校验器。外部链接配置只说明链接路径，不能认证库来源。目标是 Ubuntu 22.04 / Kunpeng-920 的实际获准单 NUMA 资源。

源码子集和 Metadata 格式见 [编译器说明](compiler/README.md)，实际模块见 [模块输入输出](docs/problem_alignment.md)。
