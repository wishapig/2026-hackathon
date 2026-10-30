# 赛事附加信息记录

> **来源与历史记录**：本页包含原文引述、历史转述和设计推导；三者不得混作要求。项目唯一维护的需求与验收规范见 [REQUIREMENTS.md](../REQUIREMENTS.md)。

> **阅读说明（2026-10-30）**：本文记录用户提供的约束与设计推导；不是已实现功能清单或官方完整评分原文。当前实现、设计差异和验证范围见 [实现与验证](runtime_strategy.md)。

> **新增原文**：后续提供的 [1.docx 已转录](problem_statement.md)。其中矩阵格式仅含 n、不含 b；评分明确两项最低均为 0，正确结果总分相加。本文中的早期“原文未提供”保留为来源历史；新发现的协议、真值判定与范围差异见 [题面对照](problem_alignment.md)。

## 来源与效力

本文件记录用户早期提供的八条附加信息及后续澄清，作为模块设计与后续实现的约束。最新澄清优先于早期规模描述。它们不是本项目从网页或二进制库中独立验证得到的结论。原 requirements.md 未包含在当前文档集中；现有评分规则见 [需求规范](../REQUIREMENTS.md)。

## 原意记录及设计落点

| 编号 | 用户提供的信息 | 必须落实的设计 | 验证证据 |
| --- | --- | --- | --- |
| 1 | 早期为单 NUMA、16–24 核；后续明确单节点全部核心允许使用。 | 以实际允许集和节点拓扑为准，16–24 不作上限，不把 socket 等同 NUMA 节点。 | 保存 CPU/NUMA 拓扑、affinity 与限制；测试单线程至允许节点的全部核心。 |
| 2 | 要求通用解决方案。最终仅测试分块算法，但也应适用于朴素算法或其他算法，评委据此判断合规性。 | 从源码和访问契约推导依赖；runtime 不识别算法名，不内置 Cholesky DAG。 | 同一编译器、Metadata 格式和 runtime 执行分块、朴素及其他算法；另检查 WAR/WAW。 |
| 3 | 所有队伍使用同一份子任务二进制库，静态链接到队伍源码构建的程序，不考虑 LTO。 | 官方计算实现保持原样；只做 ABI 适配；编译和链接明确禁用 LTO。 | 保存链接命令、链接映射和库指纹；核对算子调用目标。 |
| 4 | 静态链接方式不应产生跨任务自动向量化；runtime 技术不受限制。 | 优化依赖关系、计数、事件、队列、地址求值和派发；不依赖内核间数值融合。 | 分离内核时间与运行时成本；关系专门化如采用，必须由 Metadata 自动导出并计入准备成本。 |
| 5 | 只考虑单 NUMA 节点内优化。 | 在节点内控制线程亲和、首次触页、缓存局部性和工作窃取；不设计跨节点执行后端。 | 检查线程与内存放置；记录节点内实际资源，避免把整机资源用于提交结果。 |
| 6 | 核心是高效表达动态依赖，并在运行时高效解依赖、派发任务。 | 依赖编译、Metadata 编码、关系求值、依赖状态、就绪调度和调用分派各自独立。 | 分别报告关系大小、关系求值成本、每边通知成本、每任务派发成本和端到端时间。 |
| 7 | n、b 均由评委设置，且保证 n % b == 0，不必处理不能整除。 | n、b 是绑定时参数；可派生 q = n / b；不自动改 b；不构建尾块算法，不增加二次幂限制。 | 同一 Metadata 跨不同 n/b 运行；包含 b = n、q = 1、非二次幂 b/q 与较大 q；非法参数明确拒绝。 |
| 8 | 评测操作系统为 Ubuntu 22.04。 | 以 Ubuntu 22.04 为构建与运行目标，核实目标 CPU 架构、编译器、C++ ABI 和静态库要求。 | 在目标环境构建、链接并运行；仅在兼容环境测试不能代替目标机性能结论。 |

## 后续硬件与优化目标澄清

用户指定测试环境为 Kunpeng-920、2 Socket × 64 核、125 GB 内存；这是用户提供的信息，尚未在目标机独立探测。评测保持单 NUMA 范围，可使用该节点实际允许的全部核心。125 GB 是否整机容量、单节点可用量或进程额度仍未明确，不能直接拿来分配；按进程限制、节点可用量和配置预算计划资源，不假设两个 socket 对应两个 NUMA 节点。

用户明确主要目的是给定 case 跑得快，泛化性能纳入考虑但优先级较低，不能 hack case。因此允许使用公开负载调优通用阈值、目标机策略及从源码自动导出的结构；正确性、合法参数支持、统一官方算子与通用工具链仍是前提。内存够用时不强行压缩运行态，最终按完整计算时间选型；不对 case 编号、seed 或公开形状白名单分派硬编码图。详见 [运行时策略](runtime_strategy.md)。

## 赛事最新回复与 demo 的效力

用户提供的新回复作为当前约束：

| 问题 | 回复内容 | 设计落点 |
| --- | --- | --- |
| 1、2 | 参见 demo.zip，问题原文未提供 | 读取 [demo 文件说明](../vendor/demo/src/base_kernels/README.md) 中已实际提供的源码/ABI/I/O，不补造原问题和额外规则 |
| 3 | 输入输出矩阵的文件 I/O、结果校验不计时；其余 Metadata 加载、解依赖、执行、写结果均计时 | Metadata 文件读取也计时；内存结果写入、复制/清零、必要准备和收束计时。demo 局部计时不能覆盖最新口径 |
| 4 | 多个输入必须依次处理，不考察多输入间并发 | 单 case 完成排空后再进入下一个；禁止跨 case 执行，本设计也不做跨 case 准备流水 |
| 5 | Metadata 内容由解题方案决定，本质为动态依赖的表达和存储 | 参数化格式是本方案选择；比较必要产物字节和实际执行成本 |
| 6 | 实测只针对分块 Cholesky，专家分析对其他算法的适用性 | 主优化聚焦 demo 分块负载；保留可扩展算子契约和通用依赖/运行机制 |
| 7 | 见群里其他回复 | 未收到具体文本，不据此推断规则 |

当前记录的 85/15 排名分值已获新题目原文确认，最低分均为 0、正确结果总分相加；并列及跨 case 排名聚合未提供，不新增图质量分数。最新执行计时边界已经明确，不再将“整个计时范围未知”作为当前状态。详细指标见 [需求规范](../REQUIREMENTS.md)。

## 与公开包的关系

2026-10-30 已读取工作区新增的 公开测试压缩包 `compiler2026_public20_cases.zip`（原件保存在 Git 历史中，解压内容见 [vendor/public20](../vendor/public20)），其中包含此前缺失的 `compiler2026_public20/docs/io_format.md`：每个文件以 uint32_t case_count 开始，每条输入、输出记录包含 n、b 和矩阵，类型为 uint32_t、uint32_t、double；矩阵行优先，上三角输出允许写零。公开校验器按 scaled residual < 100 判定。包清单、20 条规格与复核证据见 [需求规范](../REQUIREMENTS.md)。

这些是公开验证的具体约定。公开包使用“初赛”表述，而项目题面记录为决赛；正式评测是否完全沿用仍需确认。不得把公开包的残差规则自动等同于决赛对“完全一致”的解释，也不得因字节序未说明而擅自声称已经具备跨字节序兼容性。

原 public20 包单独缺少 matrix_case_io.hpp；新增 demo.zip 已补齐该头、分块 baseline、kernels.h 和可本地编译的三个内核源码。可以开展源码驱动和本地互操作验证，不再因这些资料缺失阻塞。最终评测会强制重链接官方库，其二进制版本与目标机实测仍未取得；本地调试源码不能替代最终重链接验证。

公开规格已确认包含非二次幂 b = 24、48、96；包内残差函数单独运行时 A=[4]、L=[NaN] 会返回 0，L=[-2] 也会通过残差。这是新增资料的审查结论，不是赛事额外授权或正式判定规则变更；本地验收增加独立有限值、正对角线及中间运算检查。新增头文件已确认按宿主对象字节直接读写，没有有限值检查；此前实验仍仅为函数级复现，不冒充已经运行完整文件级负例。

### 用户提供的补充原文
六水合氯化钴: 10-30 11:27:36
We have provided a ZIP package containing 20 public test cases, the input/output format specification, and the source code for the input generator and verifier.

Please use this package for your initial local validation before submission.

Basic workflow:

1. Compile the input generator:
c++ -std=c++17 -O2 -I. tools/local_generator/spd_generator.cpp -pthread -o build/spd_generator

2. Compile the verifier:
c++ -std=c++17 -O2 -I. tools/local_verifier/verifier.cpp -pthread -o build/verifier

3. Generate the test input:
./build/spd_generator public20.bin --spec cases/preliminary_public_20.txt

4. Run your program:
./your_program public20.bin public20.out

5. Verify the output:
./build/verifier public20.bin public20.out

Your program must accept two command-line arguments:

<input_file> <output_file>

The validation is successful when the verifier exits with status 0 and all 20 test cases report status=PASS.

These 20 cases are provided for initial debugging and interface validation. They do not represent the complete hidden test set used for final evaluation. Please ensure that your implementation supports multiple cases, dynamic matrix shapes, and the specified binary input/output format.

六水合氯化钴: 10-30 11:27:42
评判的时候我们会保证在一个NUMA节点内执行，一个NUMA里面的全部核心都允许使用，参与计算的核心数需要你们自己调度
