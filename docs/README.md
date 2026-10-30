# 文档导航

先运行程序，请看根目录 [README](../README.md)；准备交付，请看 [提交说明](../SUBMISSION.md)。

## 题目与要求

- [需求规范](../REQUIREMENTS.md)：接口、评分、计时和待确认问题。
- [题目原文](problem_statement.md)：转录及全部四张原图；原始 DOCX 可从 Git 恢复。
- [赛事补充说明](supplemental_constraints.md)：用户提供的补充信息及来源记录。
- [输入输出协议](problem_alignment.md)：题面格式与公开包格式的区别。

## 当前实现

- [架构设计：从算法源码到动态并行执行](architecture.md)：讲课式完整说明，含依赖示例、Metadata、CSR、循环压缩、窗口调度、设计亮点与边界。
- [运行流程与调度](runtime_strategy.md)：任务展开、依赖图、循环压缩、分窗口执行和资源管理。
- [编译器说明](../compiler/README.md)：支持的源码语法和 Metadata 格式。
- [算子源码分析](operator_source_inference.md)：Clang 分析流程及配置。

已完成的优化计划、旧模块方案和过期性能报告已移出当前文档树；历史提交和标签仍可通过 Git 查阅。

## 恢复原始附件

根目录的 `1.docx`、`demo.zip` 和 `compiler2026_public20_cases.zip` 已在确认 Git 中存在逐字节一致的备份后删除。题目原图保留在 `docs/assets/problem-original/`；公开包和 demo 的可用内容位于 `vendor/`。其中 demo 的部分源码已有后续修改，需要原包时应从 Git 恢复。

原件均保存在提交 `3e445f65026e8c2d26012f4124f1d0c87385487b` 中。例如，将原始 demo 包恢复到临时目录：

```bash
git show 3e445f65026e8c2d26012f4124f1d0c87385487b:demo.zip > /tmp/demo-original.zip
```

将命令中的 `demo.zip` 换成另外两个文件名即可恢复对应原件，并相应修改输出路径。
