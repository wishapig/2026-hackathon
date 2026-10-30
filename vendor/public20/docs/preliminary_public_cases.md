# 初赛公开用例说明

为帮助参赛者完成本地开发与性能基线验证，仓库提供一份公开的初赛用例规格文件：

- `cases/preliminary_public_20.txt`：20 条初次验证用例
- `cases/preliminary_public_150.txt`

该文件包含 150 组 `(n, b, seed)` 组合，设计目标如下：

- 覆盖更多矩阵规模 `n` 与分块大小 `b` 的联合分布
- 同时包含同一 `n` 下多个 `b`、同一 `b` 下多个 `n` 的场景
- 在当前本地脚手架约束下满足 `n % b == 0`
- 兼顾小规模、中规模和较大规模输入
- 避免对同一 `(n, b)` 仅通过切换 seed 重复采样

## 20 条初次验证用例

`preliminary_public_20.txt` 从公开规格分布中抽取 20 条确定性用例，覆盖
`n=128,256,384,768,1536`，并覆盖小 block、中等 block 和整块
分解。它用于检查程序的输入输出协议、动态读取多组 case、结果正确性和基本性能，
不代表正式评测的完整规模。

完整本地验证命令：

```bash
./bin/spd_generator public_preliminary_20.bin \
  --spec cases/preliminary_public_20.txt
./your_program public_preliminary_20.bin public_preliminary_20.out
./bin/verifier public_preliminary_20.bin public_preliminary_20.out
```

最后一条命令返回码为 `0` 且每条 case 显示 `status=PASS` 时，表示 20 条用例
通过当前独立校验器。`your_program` 必须接受两个参数：输入二进制文件和输出二进制文件。

若要测试官方提交重链接流程，可使用：

```bash
./scripts/judge_submission.sh template build/public20_workspace public20 \
  --spec-file cases/preliminary_public_20.txt
```

该命令会生成 `build/public20_workspace/public20_audit.csv` 和
`build/public20_workspace/public20_perf.csv`。

当前公开分布按 150 个唯一 `(n, b)` 组合组织，每组保留 1 个代表性 seed：

- `n` 覆盖范围为 `128` 到 `2048`
- `b` 覆盖范围为 `8` 到 `384`
- 组合按可整除关系构造，确保每个规格都满足 `n % b == 0`
- 分布同时保留规则分块、细粒度分块和较大 block 的场景

## 本地生成

可直接使用本地 SPD 生成器：

```bash
./bin/spd_generator public_preliminary.bin --spec cases/preliminary_public_150.txt
```

当公开集包含较大矩阵时，可通过 `SPD_GENERATOR_THREADS` 指定并行线程数：

```bash
SPD_GENERATOR_THREADS=8 ./bin/spd_generator public_preliminary.bin --spec cases/preliminary_public_150.txt
```

再配合你自己编译得到的程序运行并校验，例如：

```bash
./your_program public_preliminary.bin public_preliminary.out
./bin/verifier public_preliminary.bin public_preliminary.out
```

`contestant_sdk` 发布包只附带 `src/baseline/` 源码，不附带预编译的
`baseline_serial` 可执行文件；如需串行参考实现，可自行编译
`src/baseline/` 下源码。

## 与线上评测的关系

这份公开用例的主要作用是：

- 作为公开的本地验证集
- 提供稳定可复现的联合分布参考
- 引导参赛者关注不同 `(n, b)` 组合下的通用优化能力

线上评测可以沿用这份公开用例的 `(n, b)` 分布，但不要求复用完全相同的矩阵数值。
