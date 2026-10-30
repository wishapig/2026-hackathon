# 数据与接口契约

> **非规范文档**：项目唯一需求与验收依据见 [REQUIREMENTS.md](../REQUIREMENTS.md)。本页仅说明实现、使用方式、设计候选或历史验证；其中的约束性表述不能新增、覆盖或缩小规范要求。

> **实现对照（2026-10-30）**：本文保留设计级接口与候选能力，实际公共 API 是 `include/dag/core.hpp` 中的 Program/Expr/Node/Task/Region/Graph、Limits、RuntimeOptions 和 Executor。当前为参数化程序 JSON、绑定时通用区域 CSR、连续计数与 atomic/owner 后端；没有 SymbolicGraph、隐式后继、rank/unrank 或运行中模式切换。详见 [实现与验证](implementation.md)。

以下共享接口为设计级契约，不是已实现的 API。demo 已提供的官方 ABI 与本地 I/O 见算子目录及 demo 审查；自定义 Metadata 二进制格式独立版本化，不与官方矩阵协议混用。所有标识、字段和未来代码使用英文。

各接口的所有权、性能边界与独立验证见 [模块设计索引](modules/README.md)。本文件定义共享语义；模块文档中的函数名为设计接口，不代表现有实现。

## 参数与缓冲区

| 类型 | 必要字段 | 约束 |
| --- | --- | --- |
| ParameterSchema | name, type, bounds, constraints | 参数来自源码与契约，不仅限于 n、b。 |
| ParameterSet | values | 本题绑定 n、b；检查 n > 0、0 < b <= n、n % b == 0，再派生 q = n / b。 |
| BufferDescriptor | id, base, bytes, element_type, extents, strides, alignment | 地址计算检查溢出与边界，所有权属于执行调用者。 |
| AliasConstraint | buffer_ids, relation | 只有经源码或调用契约声明并验证的区域才可视为不重叠。 |
| ExecutionContext | execution_id, parameters, buffers, cpu_set, memory_budget | 一次矩阵执行独占一份逻辑上下文。 |

n、b 的实际输入位置由官方执行接口决定；不能向题面二进制文件擅自增加 b 字段。

public20 文档和 demo 的 matrix_case_io.hpp 均将 b 写在每条记录的 n 之后，适配器遵循该现有官方样例协议。demo 直接读取宿主整数/double 字节，无字节序转换；不能据此宣称跨字节序兼容。

b 与 q 均无二次幂前置条件。公开集包含 b = 24、48、96，以及 q = 6、12、24、48；不能用位移、位掩码替代一般除法、取模，除非绑定时证明该次参数满足优化条件并保留通用路径。输入行主序不等于块连续布局；块起点、行步长与算子要求由 BufferDescriptor 和 ABI 契约共同确定，不能由 b 推断块地址对齐。

public20 的数值验收是文件元数据匹配、独立有限值/正对角线检查与残差检查的联合条件，详见 [需求规范](../REQUIREMENTS.md)。这些属于 Cholesky 验证配置，不成为通用图 runtime 的算法判断规则。

## 跨模块产物与所有权

| 类型 | 生产者 | 消费者与生命周期 |
| --- | --- | --- |
| TaskProgram | SourceFrontend | DependenceCompiler；独立于 AST 存储，包含任务族、顺序及访问声明 |
| BindingDescriptor | OperatorCatalog | RuntimeBinder；仅描述调用 ABI，不含算法依赖 |
| DecodedMetadata | MetadataCodec | RuntimeBinder；不可变，保持到所有绑定引用退出 |
| BoundGraph | RuntimeBinder | GraphEvaluator；只读参数域、关系和预计算常量 |
| InvocationBindings | RuntimeBinder | KernelDispatcher；已解析的符号、类型和调用目标 |
| BoundExecution | RuntimeBinder | ExecutionDriver；组合图、调用表、资源计划与缓冲区视图 |
| ExecutionPlan | RuntimeBinder | ExecutionDriver、DependencyTracker、ReadyScheduler；每次执行固定的表示、计数后端、索引能力、线程与容量计划 |
| TaskInvocation | GraphEvaluator | KernelDispatcher；仅携带一次算子调用需要的参数 |
| TaskLease | ReadyScheduler | ExecutionDriver；一个任务的唯一执行权，不转移依赖状态所有权 |
| ExecutionResult | ExecutionDriver | DatasetAdapter、ValidationBench；正常完成或安全失败的状态与统计 |

可变任务计数属于 DependencyTracker，控制队列属于 EventTransport，就绪队列属于 ReadyScheduler；线程池、待传播游标和生命周期计数属于 ExecutionDriver。缓冲区仍由调用者持有。这样的所有权避免多个模块分别维护同一可变状态。

## 算子契约

OperatorContract 包含 symbol、abi_version、argument_types、read_regions、write_regions、completion_mode、reentrant、internal_threading、alignment_requirements 和 semantic_fingerprint。

- 访问声明描述逻辑元素或块区域，并保留覆盖范围；不能把所有调用统一猜为“写一个块”。
- 不透明库调用的语义来自人工核对的契约或官方声明，不能通过读取静态库的机器码推断出完整别名与读写保证。
- 本设计首选同步返回代表计算完成。若库实际异步，必须使用显式完成事件适配，不能直接沿用同步协议。
- ABI 表仅做符号与参数适配，不保存算法的依赖关系或执行顺序。

demo 的正式计算接口是 cholesky(double*,double*,int,int)、trsm(double*,double*,double*,int,int)、madd(double*,double*,double*,int,int)。正式分块路径不得重新定义、替换或绕过它们；其他算子可扩展 OperatorContract/BindingDescriptor，不修改依赖核心。具体区域与别名见 [算子目录](modules/operator_catalog.md)。源码中的复制、清零同样是语义操作，可用显式任务或已描述的前后阶段表达，不可忽略或重复执行。

## 编译中间表示

| 类型 | 描述 |
| --- | --- |
| TaskFamily | family_id、迭代坐标、域条件、算子引用、参数表达式、原始顺序、读写区域。 |
| TaskDomain | 参数化整数点集合，可含三角界与分段约束。 |
| AccessRegion | 缓冲区引用、逻辑坐标/区间、读写模式与条件。 |
| DependencyRelation | 不同源/目标任务间的有向关系，并保留 RAW/WAR/WAW 来源供诊断。 |
| SymbolicGraph | 所有任务族、关系、源任务域、任务数表达式与必要前置条件。 |
| IndexProgram | 有效任务到 [需求规范](../REQUIREMENTS.md) 记录不重叠的 T_common、T_case、T_finalize，合计 T_score。仅矩阵文件 I/O 与独立结果校验排除，Metadata 文件读取、解码、资源准备、绑定/建图、复制/清零、执行及结果组织都计入。ExecutionResult/基准报告同时保留错误状态，失败 case 不能以不完整输出或缺失样本改善评分。

## 模块边界与依赖方向

- 前端输出 TaskFamily IR，不能修改调度策略。
- 依赖编译器生成关系，不能直接调用内核。
- Metadata 编解码器不推断或修复依赖。
- 绑定器负责资源、ABI 和 ExecutionPlan；可按预算通过求值器构建只读索引/关系缓存，不维护可变计数。
- 图求值器不持有可变任务状态，不发送就绪事件。
- 依赖跟踪器决定何时就绪，不决定何时占用某个 CPU 执行。
- 调度器安排已就绪任务，不制造依赖或跳过未就绪任务。
- 分派器只调用指定算子，不推导后继。
- 数据适配器只处理文件与矩阵缓冲，不控制算法调度。
- 验证工具在外部观察和比对，不成为生产热路径的必要组件。

运行时的双向信息流通过显式事件连接；模块之间不相互拥有对方的具体实现，以免依赖跟踪和调度策略形成不可替换的循环耦合。
