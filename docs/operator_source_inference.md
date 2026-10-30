# 从算子源码生成 Metadata

实现记录，需求依据为 REQUIREMENTS.md 的 R-01、R-03、R-16、R-28。显式 catalog 保留为兼容输入。

## 当前默认流程

```text
算子及 helper 的 C++ 源文件
  → LLVM/Clang AST
  → 参数类型、循环边界、访问下标、读写区域
算法驱动源码 + 上述分析结果
  → 代入调用参数、消除重复区域
  → 自包含 Metadata
  → 原有 runtime 和公共算子库
```

CMake 默认不再读取 `examples/catalog.json`。`compiler/infer_catalog.py` 使用 Clang 的真实类型、引用关系和函数定义，跨输入文件解析 wrapper/helper，按循环边界与 `row*stride+col` 下标推导参数化访问范围。没有算子名称表，也不根据参数名称或末两个参数的位置猜测块宽、步长。不要求 pragma。

生成的 `build/catalog.generated.json` 是离线分析报告。运行所需的调用参数和区域表达式全部写入 `.meta`；runtime 不读取该 JSON，不运行 Clang，不加载算子源码。只分发 Metadata 和已链接的程序即可运行。

## 用法

需要安装 Clang，例如 Ubuntu 上 `apt-get install clang`。C++ runtime 仍可用 GCC 编译。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
```

指定自己的算子实现及头文件目录：

```bash
cmake -S . -B build \
  '-DDAG_OPERATOR_SOURCES=/path/impl.cpp;/path/wrappers.cpp' \
  '-DDAG_OPERATOR_INCLUDES=/path/include' \
  -DDAG_LLVM_COMPILER=/usr/bin/clang++
```

`DAG_OPERATOR_CLANG_ARGS` 可以传额外编译选项。修改算子源文件或上述 include 目录下的头文件会重新生成 Metadata。

也可以直接编译：

```bash
python3 compiler/compile.py vendor/demo/src/baseline/block_cholesky.cpp \
  --operator-source vendor/demo/src/base_kernels/kernels_impl.cpp \
  --operator-source vendor/demo/src/base_kernels/kernels_public.cpp \
  --include vendor/demo/include \
  --include vendor/demo/src/base_kernels \
  --catalog-output build/catalog.generated.json \
  -o build/block_cholesky.meta
```

`--operator-source` 可重复；`--operator name` 可选定公开入口；`--clang-arg=-DMODE=1` 可传预处理参数。标量源码不需要外部算子，也不需要空 catalog 文件。显式 `--catalog` 仅保留旧调用的兼容入口，默认构建和 benchmark 不使用它。

## 分析范围及大小

- 支持 double 指针和整数公开参数、正向整数 for 循环、矩形/三角循环范围、直接数组下标、指针偏移与别名、简单返回地址/引用的 helper、跨文件包装调用和参数重排。
- 从访问下标推导矩形冲突范围；三角区域可用包围矩形表示。这是依赖冲突覆盖，不表示算子完整覆盖写入了每个元素。
- 同模式的被覆盖区域在离线分析阶段消除。实际参数代入后，仅当 read 与 write 区域完全相同，才省略该 read；write 本身已产生与此前读写访问的依赖。同一指针读大区域、写小区域时保留较大的 read。
- 含数据相关地址、无法解析的外部内存访问或循环携带指针递推的源码需要继续扩展分析器。目前会在离线阶段报告不支持，不以猜测范围生成 Metadata。
- 算子仍遵循当前项目已有的同步、可重入、内部单线程执行约定；这些调度属性并非由 AST 自动证明。现有算法驱动解析器的语法范围不变。
- 新算子的参数和访问契约可以自动生成；运行新的符号仍需链接其实现并接入调用 ABI。本次不改变正式 cholesky/trsm/madd 调用约束。

Metadata 大小以本次生成文件为准。分析报告不是运行必需文件；默认执行格式不包含来源摘要或审计字段。

## 验证入口

使用 `tools/benchmark.py accept-public20` 验证构建、公开结果和其他算法功能。更换算子实现后，应重新生成 Metadata 并核对访问语义。历史性能数据已清理，当前结果以本次报告为准。
