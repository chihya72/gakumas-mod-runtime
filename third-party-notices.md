# Third-party notices

本项目自身以 [MIT](LICENSE) 发布。Release zip 同时包含本文件、项目 `LICENSE`，以及编入
`xinput1_3.dll` 的第三方组件许可证副本。

| 组件 | 用途/位置 | 许可证 | Release 中的许可文件 |
|---|---|---|---|
| MinHook | 编入 Runtime；源码位于 `deps/minhook` | BSD-2-Clause | `licenses/MinHook-LICENSE.txt` |
| UnityResolve.hpp | 编入 Runtime；源码位于 `src/deps/UnityResolve` | MIT | `licenses/UnityResolve-LICENSE.txt` |
| JSON for Modern C++ 3.11.3 | 编入 Runtime；`src/deps/nlohmann/json.hpp` | MIT | `licenses/nlohmann-json-LICENSE.MIT` |
| Premake 5.0.0-beta1 | 仅构建时使用；`tools/premake5.exe`，不进入 Release zip | BSD-3-Clause | 仓库内 `tools/PREMAKE-LICENSE.txt` |

Premake 二进制 SHA-256：

`6810A9D0C39D6D8361158DA6BCE9CB146267BA3405CB9EAEA98DD0DF36E991E0`

上游项目：

- <https://github.com/TsudaKageyu/minhook>
- <https://github.com/issuimo/UnityResolve.hpp>
- <https://github.com/nlohmann/json>
- <https://github.com/premake/premake-core>
