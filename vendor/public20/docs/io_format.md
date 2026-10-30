# 输入输出格式说明

## 输入文件 `input.bin`

二进制布局：

1. `uint32_t case_count`
2. 对每个测例依次写入：
   - `uint32_t n`
   - `uint32_t b`
   - `double A[n * n]`

矩阵按行主序存储。
其中 `n` 表示矩阵规模，`b` 表示该测例使用的分块大小；测试集应同时覆盖不同 `n` 与不同 `b`。

## 输出文件 `output.bin`

二进制布局：

1. `uint32_t case_count`
2. 对每个测例依次写入：
   - `uint32_t n`
   - `uint32_t b`
   - `double L[n * n]`

其中 `L` 为下三角结果矩阵；上三角区域允许写零。

## 判定要求

- 输入输出中的 `case_count` 必须一致
- 每个结果项的 `n` 与 `b` 必须和输入完全匹配
- 正确性由独立 `verifier` 根据 `scaled residual` 判定
