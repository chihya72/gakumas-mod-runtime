# Third-party notices

本仓库自有代码以 [GPL-3.0](LICENSE) 发布，与同级 `gakumas-mod-runtime` 保持一致——
本项目复用它的公开头文件、JSON 依赖和已编译的 MinHook。

本仓库**不携带任何第三方源码副本**，全部通过相对路径引用同级仓库：

| 组件 | 引用方式 | 许可证 |
|---|---|---|
| MinHook | `../gakumas-mod-runtime/deps/minhook`（头文件 + 已编译 `minhook.lib`） | 见该仓库 `deps/minhook/LICENSE.txt` |
| JSON for Modern C++ 3.11.3 | `src/RuntimeModSnapshot.cpp` 直接 `#include ../../gakumas-mod-runtime/src/deps/nlohmann/json.hpp` | MIT；见该仓库 `src/deps/nlohmann/LICENSE.MIT` |
| Premake 5.0.0-beta1 | `../gakumas-mod-runtime/tools/premake5.exe` | BSD-3-Clause；见该仓库 `tools/PREMAKE-LICENSE.txt` |

`include/gkmm/gmr_runtime_api.h` 是 `gakumas-mod-runtime` 的 `src/runtime/ModRuntimeApi.h`
的逐字节副本（同为 GPL-3.0）。改动 Runtime API 时两份必须同步。

上游项目：

- <https://github.com/TsudaKageyu/minhook>
- <https://github.com/nlohmann/json>
- <https://github.com/premake/premake-core>
