# 构建与交付

按当前源码重新构建，程序、Metadata 和被测算子库应来自同一次构建。旧版本的通过记录不代表当前工作区已验收。

## 本地检查

```bash
python3 tools/benchmark.py accept-public20 --build-dir build --jobs 4
```

成功后检查命令末尾给出的 `acceptance.json`：公开 20 例、额外数值检查和其他算法功能检查都应通过。日常运行和性能测量见 [README](README.md)。

## 链接正式库

准备真实评委静态库，在目标环境建立独立构建目录：

```bash
cmake -S . -B build-submit -DCMAKE_BUILD_TYPE=Release \
  -DDAG_REQUIRE_OFFICIAL=ON \
  -DDAG_OFFICIAL_LIBRARY=/absolute/path/to/judge_kernels.a
cmake --build build-submit --parallel 4
python3 tools/benchmark.py accept-public20 --build-dir build-submit \
  --official-library /absolute/path/to/judge_kernels.a --jobs 4
```

替换库路径。若提供的算子源码或头文件与 demo 不同，还要配置 `DAG_OPERATOR_SOURCES`、`DAG_OPERATOR_INCLUDES` 及必要的 Clang 参数，详见 [算子分析](docs/operator_source_inference.md)。分析出的访问语义必须与实际库一致。

运行需要 `dag_run`、与它对应的 `.meta` 文件及系统运行库（包括 zlib）。算子库静态链接；运行时不读取算子源码或 `catalog.generated.json`。使用 Python 启动入口时，还需保留 `tools/benchmark.py`。

## 启动

```bash
python3 tools/benchmark.py submit build-submit/dag_run input.bin output.bin \
  --metadata build-submit/block_cholesky.meta \
  --configuration-report artifacts/submission-config.json
```

启动工具清理继承的 DAG 实验配置，使用自动线程、自动预算和默认执行策略。目标机有明确限制时，用 `--threads`、`--memory-mb`、`--graph-memory-mb` 指定；操作系统亲和集和 cgroup 限制仍然有效。

若评测器直接启动 `dag_run input.bin output.bin`，需在受控环境中设置 `DAG_METADATA`，或将对应文件以 `block_cholesky.meta` 放在工作目录。不要把本机调优的线程数和内存上限带到另一台机器。

## 交付前核对

- 保存所用源码 commit、构建配置、算子库及 Metadata；确认没有混用旧构建。
- 原 verifier 和加强校验都通过，其他算法功能验证通过。
- 性能报告保留原始样本、完整计时范围、资源限制和有效 case 集合；资源不足的子集结果不作完整成绩。
- 正式库和目标机的验证单独记录。本地 demo 或服务器 demo 的通过不等于正式评测通过。
- 源码分析及 Metadata 生成的计时归属仍见 [O-08](REQUIREMENTS.md#口径与未完成条件)，不能仅凭运行时耗时宣称总成绩。

历史优化过程保留在 Git 中。可用 `git tag --list 'pareto/*'` 查看旧候选；恢复历史版本时应新建 worktree 并重新验收。已清理的旧报告不作为当前交付证据。
