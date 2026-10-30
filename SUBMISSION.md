# 比赛提交交接

## 第二轮推荐（2026-10-30睡前交接）

**推荐完整基线：96908872cab3e24e22096fbeac95c64a0f78862d，标签round2/baseline。** 数值实现与P007一致，默认Metadata141字节，包含当前R-27其他算法功能验证和R-28无审计负担规则。本轮fanout的CSE、计数批发、派发、缓冲、页布局均已检查并测量，没有稳定胜出的新增默认策略。不要从下方历史P005命令取旧版本。

    git worktree add --detach ../contest-round2-submit round2/baseline
    cd ../contest-round2-submit
    cmake -S . -B build-submit -DCMAKE_BUILD_TYPE=Release -DDAG_METADATA_ENCODING=auto -DDAG_REQUIRE_OFFICIAL=ON -DDAG_OFFICIAL_LIBRARY=/absolute/path/to/real_judge_kernels.a
    cmake --build build-submit --parallel 4
    python3 tools/benchmark.py accept-public20 --build-dir build-submit --output-dir artifacts/acceptance --official-library /absolute/path/to/real_judge_kernels.a --threads 0 --check-threads 4 --clean-env
    python3 tools/benchmark.py submit build-submit/dag_run input.bin output.bin --metadata build-submit/block_cholesky.meta --configuration-report submission-config.json

真实评委库路径必须替换；本地调试时去掉require-official和library两个CMake选项及验收official-library选项，但结果只属于demo内核。默认启动会清理继承的DAG实验变量并使用实际单NUMA资源，不硬设本机线程/内存。

本轮基线公开verifier20/20、加强检查20/20、三种其他算法串并行功能验证均通过；其他算法不进入性能成绩。独立五轮中位数0.284185243秒；最后交错基线0.289276792秒，目标自适应THP备选P008为0.287214250秒，范围重叠、未确认收益。推荐简单基线。

P008完整commit：a3b37ad7301fcc21475bc817983650eb30369acf，141字节，适合取得实际目标机后独立对照；它按系统页大小/策略建议L大页，失败时回退并退还预算，数值与计时边界不变。它不是默认提交版本。回退选round2/baseline或历史pareto/P007（后者缺少后续已提交的功能工具，优先完整基线）。

证据：[本轮基线](results/round2/baseline/manifest.json)、[同场对照](results/round2/final-page-comparison/common-scope.json)、[P008](results/pareto/P008/manifest.json)、[本轮目标与结论](PERFORMANCE_GOAL.md)。临时分支不自动进入提交；标签和证据不覆盖。正式目标环境尚未验证。第二轮最终本地验收映射见 [审计记录](results/round2/final-audit.json)。

## 以下为第一轮历史交接


> **R-28后续更新**：当前默认实现为P007（734db87d4101c8939e19ec3e98d381ac029445cf），执行Metadata为141字节，不含SHA256/FNV或原始局部变量名，重复审计默认关闭；旧格式仍可读。此文中的P005/P006及257/292字节是历史交接记录。最新证据见 [P007](results/pareto/P007/manifest.json)；默认验收命令不变。

本轮本地优化已收口，推荐 **P005**。正式评委静态库及 Ubuntu 22.04/Kunpeng-920 尚未取得，**本地通过不等于正式目标机通过，也不能推断比赛得分**。

## 推荐与备选

| 候选 | 被测实现 commit | Metadata | 最终交错中位数 | 状态 |
| --- | --- | ---: | ---: | --- |
| P005 | bbdc09e83747952d9f36948819668e115269d4d6 | 257 B | 0.292542022 s | 推荐，auto编码、自动资源 |
| P006 | 同上 | 292 B | 0.291897810 s | 备选，compact-zlib编码、自动资源 |

P005五轮范围0.282280～0.303149秒，P006为0.275742～0.293834秒，差异落在波动内；推荐P005争取Metadata排名，不声称P006稳定更快。两者都是同一已修复实现，仅编码构建配置不同。各自配套verifier和加强校验均20/20 PASS，重复测量无资源排除。

早期基线P001在另一组交错对照中为0.484026138秒/1839B，同组P005为0.295530587秒/257B，完整耗时约减少39%、Metadata约减少86%。这是本地x86-64/demo库结果；不要拼接不同批次的最快数据作为加速承诺。

- [目标与台账](GOAL.md)
- [P005证据](results/pareto/P005/manifest.json)、[公开验收](results/pareto/P005/acceptance.json)
- [P006证据](results/pareto/P006/manifest.json)、[公开验收](results/pareto/P006/acceptance.json)
- [最终同实现编码对照](results/pareto/final-encoding-comparison/comparison.json)

P001～P006的Git标签均保留。P001～P003已被后续版本取代；P004保留历史测量，但其通用源码零次循环边界问题已在P005/P006修复。较大编码的安全备选选P006。

## 从精确版本构建

依赖Linux、CMake≥3.16、C++17、Python≥3.9、pthreads、zlib≥1.2.9开发包（Ubuntu为zlib1g-dev）。在新的目录构建，避免旧缓存：

    git worktree add --detach ../contest-submit pareto/P005
    cd ../contest-submit
    cmake -S . -B build-submit -DCMAKE_BUILD_TYPE=Release -DDAG_METADATA_ENCODING=auto -DDAG_REQUIRE_OFFICIAL=ON -DDAG_OFFICIAL_LIBRARY=/absolute/path/to/real_judge_kernels.a
    cmake --build build-submit --parallel 4

正式库路径必须替换成真实评委归档。去掉最后两个CMake参数仅得到本地demo调试构建，不能冒充正式库。P006使用相同commit，在独立构建目录改为 -DDAG_METADATA_ENCODING=compact-zlib。

构建得到build-submit/dag_run和build-submit/block_cholesky.meta。Metadata包含全部算法描述；执行时不另行加载源码/catalog。系统运行库仍需存在，包括zlib；官方算子按要求静态链接。没有把算法依赖移到未计费辅助文件。

## 干净部署

使用现有工具的submit子命令清理本地残留设置：

    python3 tools/benchmark.py submit build-submit/dag_run input.bin output.bin --metadata build-submit/block_cholesky.meta --configuration-report submission-config.json

子程序仍只收到输入、输出两个位置参数。默认自动线程与预算、auto模式、atomic后端、batch=1。清理只影响子进程环境，OS亲和集、NUMA/cgroup/rlimit约束始终有效。确有目标限制时用明确的 --threads、--memory-mb、--graph-memory-mb 参数，并保存配置报告；不要以 --inherit-env 导入未审计的本地设置。

若评测器必须直接启动二进制，可在受控环境中运行：

    env -u DAG_MEMORY_MB -u DAG_GRAPH_MEMORY_MB -u DAG_MAX_TASKS -u DAG_MAX_STEPS -u DAG_BLOCK_SIZE -u DAG_BLOCK_SIZES -u DAG_STATS_FILE DAG_THREADS=0 DAG_MODE=auto DAG_BACKEND=atomic DAG_BATCH=1 DAG_AUDIT=0 DAG_IO_FORMAT=public DAG_APPLICATION=blocked DAG_METADATA="$PWD/build-submit/block_cholesky.meta" build-submit/dag_run input.bin output.bin

由评测器指定的计时路径保持其设置，不覆盖成开发机路径。当前协议为count + 各组n/b/double矩阵；输出数量、顺序、n/b一致，保留完整n²个元素。

## 正式环境复验

在精确worktree内执行同一验收入口：

    python3 tools/benchmark.py accept-public20 --build-dir build-submit --output-dir artifacts/acceptance-final --official-library /absolute/path/to/real_judge_kernels.a --threads 0 --mode auto --backend atomic --batch 1 --clean-env

从该次acceptance.json取得实际生成的输入路径，替换下例task_input：

    task_input=/absolute/path/from/acceptance/public20.bin
    python3 tools/benchmark.py build-submit/dag_run "$task_input" --metadata build-submit/block_cholesky.meta --checker build-submit/dag_check --threads 0 --mode auto --backend atomic --batch 1 --clean-env --warmups 1 --repeats 5 --report artifacts/performance-final.json

验收和性能必须同配置。计分测量期间停止其他构建、生成器、校验与计算。只排除矩阵文件I/O和独立校验，加载Metadata、分配初始化、复制、展开、建图、首次建池、执行及必要收束全部计时。

内存不足导致的case保留原始结果/耗时/原因，排除算法性能统计；共同有效case诊断不冒充完整成绩。失败分类区分此前已执行未校验、当前资源失败、后续未运行。未知错误/超时不冒充OOM。默认图预算按case剩余资源自动计算，显式正值仅作为额外上限；真正不足时计算前选择流式路径，数值执行后绝不重放。

## 已完成的额外本地检查

- 规则分区与原构图器及独立冲突可达性对照；Release和ASan/UBSan。
- 槽位/opcode求值与原轨迹、作用域、短路、溢出和预算对照；零次循环增量求值问题已修复并核对原生C++行为。
- 七种Metadata编码的数值与轨迹一致性、整数宽度及31类损坏流/长度/校验/预算检查。
- 朴素标量及wavefront样例验证通用机制，不宣称支持任意C++。
- 最终P005二进制n=10000,b=100,A=4I并行输出与独立精确L=2I一致；n=512,b=1在显式1MiB图预算下完成22,500,866个语义任务，资源标记正确，该运行不计速度成绩。
- 大小交替、同尺寸缓冲复用、显式上限/自动0、父环境污染、fatal分类及不同b拒绝混比。
- 外部静态库链接分支使用明确标识的本地demo归档，公开及加强校验各20/20；main对象只引用公开算子。此项仅证明链接分支，不证明真实评委库。

这些都是有限覆盖，不能宣称合法域逐项实测。真实评委库的工作空间、重入/内部线程契约、Ubuntu22.04/Kunpeng性能仍需复验。临时验证脚本/数据/日志按规范清理，最终benchmark证据保留。

## 采纳和拒绝

采纳：通用二进制/zlib、词法槽位/opcode、规则分区ID与线性CSR、线性契约校验、同尺寸缓冲复用、就绪后继直接续执行、工作量辅助auto、自动资源预算、干净启动与资源感知统计。

未采纳：局部桶/全局堆关键路径优先级、owner作为默认、固定本地线程数、增大batch/调整空闲探测。对照没有稳定收益或更慢，实验commit保留，不进入推荐代码。

本轮已提前留下睡前可提交版本。换库或目标机后应另建证据和前沿，不覆盖原标签/样本。推荐实现与证据commit分离：后续仅文档记录变化，不冒充实现重新测量。
