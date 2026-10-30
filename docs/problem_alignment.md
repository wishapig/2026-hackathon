# 输入输出协议

当前默认采用后续公开包提供的 public 协议；旧题面的 original 协议作为显式兼容模式保留。依据见 [需求规范](../REQUIREMENTS.md)、[题面转录](problem_statement.md) 和 [公开包说明](../vendor/public20/docs/io_format.md)。

| 项目 | public（默认） | original（旧题面） |
| --- | --- | --- |
| 文件开头 | uint32 矩阵个数 | 4 字节矩阵个数 |
| 每条记录 | uint32 n、uint32 b、n² 个 double | 4 字节 n、n² 个 double |
| b 的来源 | 输入记录，不能覆盖 | 调用者显式指定 |
| 文件长度 | 4 + Σ(8 + 8n²) | 4 + Σ(4 + 8n²) |

两种格式都按行存储完整矩阵，输出的数量、顺序和参数与输入对应。L 也写满 n² 个元素，不压缩成下三角序列。程序按宿主整数和 double 字节表示读写；原题面没有明确字节序，不宣称跨字节序兼容。

默认入口：

```bash
python3 tools/benchmark.py submit build/dag_run input.bin output.bin \
  --metadata build/block_cholesky.meta
```

旧格式分块输入：

```bash
python3 tools/benchmark.py submit build/dag_run input-original.bin output-original.bin \
  --metadata build/block_cholesky.meta --io-format original --block-size 100
```

不同 case 使用不同 b 时，可将 `--block-size` 换成 `--block-sizes 3,4,8`，数量与记录数对应。两个选项互斥。原包生成器和 verifier 使用 public；original 的结果可交给同样选择 original 的 `dag_check`。

执行程序本身接收两个位置参数：输入路径、输出路径。Metadata 路径和运行选项由启动工具通过环境变量传入。统计 JSON 和计时文本是附加输出，不能替代矩阵结果文件。

## 公共算子

块指针指向矩阵内部，`lda` 是整矩阵行跨度，当前绑定为 n。

| 调用 | 功能 |
| --- | --- |
| `cholesky(A, L, b, lda)` | 分解 b×b 对角块 |
| `trsm(A, L, X, b, lda)` | 求解 `X × Lᵀ = A` |
| `madd(A, B, C, b, lda)` | 更新 `C ← C − A × Bᵀ` |

整矩阵复制及严格上三角清零也是源程序操作，由执行器保留。数值验收按配套 verifier 和需求中明确的有限值、正对角线等检查执行。
