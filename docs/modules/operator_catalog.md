# OperatorCatalog：算子声明目录

> **非规范文档**：项目唯一需求与验收依据见 [REQUIREMENTS.md](../../REQUIREMENTS.md)。本页仅说明实现、使用方式、设计候选或历史验证；其中的约束性表述不能新增、覆盖或缩小规范要求。

> **实现对照（2026-10-30）**：当前访问/ABI 契约位于 `examples/catalog.json`，由 `compiler/compile.py` 代入调用并写入 Metadata；运行程序不另读 catalog。算子名字只用于契约查找与 ABI 分派，不用于生成算法依赖。契约仍需与最终评测库复核。 当前实现见 [实现说明](../implementation.md)。

以下保留模块职责与候选接口；函数名不等同于当前公共 API，实际接口见 `include/dag/core.hpp`。

## 单一职责

为编译器与绑定器提供唯一、可核对的算子语义声明及 ABI 描述。它不执行内核，不推导后继，也不记录某个算法的调用顺序。

输入来自官方声明或人工核对的契约；输出 OperatorContract 与 BindingDescriptor。设计接口为 lookup_contract(symbol)、resolve_binding(symbol, abi_version)、verify_fingerprint(expected)。运行期以只读表访问。

## 契约内容

每个算子必须声明参数类型、读写区域、原地更新、别名要求、对齐、同步完成语义、重入性、内部线程行为及语义指纹。访问描述保留元素或区域覆盖，部分写入不能伪装成整个块定义；read-modify-write 同时声明读和写。

编译器使用可参数化的逻辑访问关系；绑定器使用具体缓冲区约束；分派器使用 ABI 描述。调用包装函数只做必要类型和参数适配，不能重排数学操作。官方库必须提供足够的重入保证或串行化要求；未知时不能直接允许同一符号并发执行。

算法相关契约的完整内容或可解析引用写入 Metadata。若必需的契约存于外部文件，该文件计入产物总量；仅有一个指纹但没有可解析内容不构成自包含描述。

## 当前 demo 的算子配置与可延拓性

[demo 审查](../demo_review.md) 已确认公开 C ABI：`cholesky(double*,double*,int,int)`、`trsm(double*,double*,double*,int,int)`、`madd(double*,double*,double*,int,int)`，末两个参数分别为 b、lda。三者返回 void，推荐源码传 lda=n。第一版目录使用这三个真实符号，不把假设的 potrf/syrk/gemm 接口作为正式主路径。

基础保守契约按完整 b×b、行跨度 lda 的块描述：cholesky R(A)/W(L)，trsm R(A)/R(L)/W(X)，madd R(A)/R(B)/R(C)/W(C)。推荐源码中 A=L、A=X 和 madd 的 A=B 分别是已观察的合法原地/输入别名；同一物理区域先归一化，避免重复边和任务自环。cholesky 读取 A 下三角但写整个 L 块（含上三角零），trsm 读取 L 下三角；精确区域优化另需证明。不得从已支持的完全相等推断任意部分重叠安全。

算子目录只声明单次调用的效果。改变源码循环或换算法后，依赖由源码重新推导，不在目录内保存 Cholesky 专用后继公式。新算子以契约、能力标记和调用适配扩展；无法证明访问、别名或 ABI 的结构明确拒绝。三个强制接口约束当前评测计算，不把通用工具链锁死为三个符号。

## 成本与兼容性

符号查找与指纹核对发生在编译或绑定，热路径使用已解析的稠密 OperatorId 与调用目标。统一库静态链接，编译与链接禁用 LTO。保留目标架构、ABI 版本和官方库身份供报告，但文件哈希本身不能证明访问契约正确。

