# Compiler 2026：20 条公开初次验证用例

本压缩包用于考生提交前的快速验证，包含 20 条确定性的 `(n, b, seed)` 测例规格。
它检查输入输出格式、多组测例处理、Cholesky 结果正确性和基本运行性能，不代表正式评测的完整测试集。

## 压缩包内容

```text
cases/preliminary_public_20.txt   20 条测例规格
docs/io_format.md                 输入输出二进制格式
docs/preliminary_public_cases.md  公开用例说明
tools/local_generator/            SPD 输入生成器源码
tools/local_verifier/             结果校验器源码
```

## 编译本地工具

在压缩包根目录执行：

```bash
mkdir -p build
c++ -std=c++17 -O2 -I. \
  tools/local_generator/spd_generator.cpp -pthread \
  -o build/spd_generator
c++ -std=c++17 -O2 -I. \
  tools/local_verifier/verifier.cpp -pthread \
  -o build/verifier
```

要求编译器支持 C++17。也可以直接使用 SDK 中与本机架构匹配的 `bin/spd_generator` 和 `bin/verifier`。

## 运行 20 条测例

考生程序必须接受两个参数：输入文件和输出文件。

```bash
./build/spd_generator public20.bin \
  --spec cases/preliminary_public_20.txt

./your_program public20.bin public20.out

./build/verifier public20.bin public20.out
```

校验器返回码为 `0` 且 20 条记录均显示 `status=PASS`，表示通过这组公开验证用例。

生成的 `public20.bin` 和结果文件可能较大，请确保工作目录有足够磁盘空间。
