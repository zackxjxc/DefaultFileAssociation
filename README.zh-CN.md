# DefaultFileAssociation

[English](README.md) · 中文

`DefaultFileAssociation.exe` 用当前用户权限注册 Windows 文件关联，并在系统允许旧版 `UserChoice` 兼容路径时设置当前用户的默认程序。工具不会请求管理员权限；如果进程已经提升，会直接退出，避免把关联写入错误的管理员用户配置。

## 构建

项目使用 C++23、CMake 和 Visual Studio 2026 的 **v143（VS 2022）** 工具集，目标为 Windows 7 SP1 及以上版本。请在 Visual Studio Installer 的单个组件中安装 v143 x64/x86 构建工具。默认的 VS 2026 v145 运行库需要 Windows 10，并会导入 Windows 7 不具备的 API；仅定义 `_WIN32_WINNT` 不能改变预编译运行库的要求。

主发布版本是 x86，同一个 EXE 可以运行在 32 位 Windows 和带 WOW64 的 64 位 Windows 上：

```powershell
cmake --preset windows-x86
cmake --build --preset windows-x86-release
```

配置、构建并运行测试：

```powershell
cmake --preset windows-x86-tests
cmake --build --preset windows-x86-tests-release
ctest --test-dir out/build/windows-x86-tests-v143 -C Release --output-on-failure
```

可选的 `windows-x64` 预设用于原生 x64 对照。Release 输出在 `out/build/windows-x86-v143/Release`。构建会检查最终 EXE 的导入表，但导入检查不能替代每个目标 Windows 版本上的实际测试。

配置阶段通过 CMake `FetchContent` 下载固定版本的 CLI11 和 Microsoft WIL。若依赖由包管理器或已有工具链提供，可使用 `-DDFA_FETCH_DEPENDENCIES=OFF`。

## 架构与 WOW64

Windows 7 及以上版本中，本项目使用的 HKCU Software 键和普通 Classes 文件关联在 x86/x64 进程间共享。ProgID、图标、UserChoice、Capabilities 和 toast 路径相同；工具不会追加 `Wow6432Node`，也不会为两个视图重复写入关联。COM CLSID/Interface 注册有不同规则，不属于本工具范围。[微软共享注册表键说明](https://learn.microsoft.com/en-us/windows/win32/winprog64/shared-registry-keys)。

HTA 回退使用系统的 `mshta.exe`：32 位 Windows 或 x64 进程使用 `System32`，x86 进程运行在 64 位 Windows 时使用 `Sysnative`。这条内部宿主路径与阅读器注册的打开命令分开。请传入阅读器的真实安装路径；系统目录受 WOW64 重定向规则影响。[微软文件系统重定向说明](https://learn.microsoft.com/en-us/windows/win32/winprog64/file-system-redirector)。

x86 工具可以在 64 位 Windows 上注册 x86 或 x64 阅读器；在 32 位 Windows 上会拒绝 64 位阅读器。UserChoice Hash 使用固定的 32 位运算和 UTF-16 输入，进程位数不会改变格式。64 位主机上的测试不能替代真实 32 位 Windows 虚拟机测试。

## 使用方式

程序接收 UTF-16 Windows 命令行参数，并保留带下划线的旧参数别名：

```text
DefaultFileAssociation.exe --ext=.pdf --exe-path="C:\Apps\SamplePdfReader\SamplePdfReader.exe" --icon-index=1 --prog-id=samplepdfreader.pdf
```

常用选项：

- `--ext`：文件扩展名，例如 `.pdf`。
- `--exe-path`：设置操作需要的绝对 EXE 路径。
- `--icon-index`：EXE 内的图标资源序号；未提供时使用默认图标。也可以使用 `--icon-path` 指定 `.ico`、`.exe` 或 `.dll`。
- `--prog-id`：稳定的 ProgID；省略时根据 EXE 文件名和扩展名生成。
- `--description`：Windows 中显示的友好名称。
- `--no-hta`：禁用 Windows 10/11 HTA 兼容回退。
- `--dry-run`：只校验参数，不修改注册表。
- `--check`：检查当前有效关联。
- `--diagnose`：采集只读注册表和 Shell 诊断；返回 0 只表示采集完成。
- `--log-file`：追加 UTF-8 日志和前后状态快照。
- `--experiment=default|none|toasts|notify|toasts-notify`：虚拟机首次打开对照实验，详见[虚拟机测试说明](docs/first-use-vm-tests.md)。

## 协议与默认浏览器

`--protocol` 和 `--browser` 与 `--ext` 互斥，并复用系统已安装的注册信息：

```text
DefaultFileAssociation.exe --protocol=https --prog-id=ChromeHTML --dry-run
DefaultFileAssociation.exe --protocol=https --prog-id=ChromeHTML
DefaultFileAssociation.exe --protocol=https --prog-id=ChromeHTML --check
DefaultFileAssociation.exe --browser="Google Chrome" --dry-run
DefaultFileAssociation.exe --browser="Google Chrome" --check
```

浏览器模式从所选应用的 Capabilities 读取 `http`、`https`、`.htm`、`.html` 四组映射，不覆盖浏览器注册信息、图标、Capabilities 或已安装命令。设置是顺序执行的，不是注册表事务；某一项失败时，前面的项可能已经改变。每一项都会通过 UserChoice 和有效 Shell 处理器独立验证。

协议模式使用 `HKCU\Software\Microsoft\Windows\Shell\Associations\UrlAssociations\<protocol>\UserChoice`，而不是 `FileExts`。Windows 8 及以上会验证协议 Hash；Windows 7 使用旧结构。

## Windows 11 研究状态

EXE 内嵌了 Framework64 MSBuild PDF 实验后端。使用 `--msbuild` 时，程序会先进行版本兼容性检查，再注册候选程序、写入新格式记录，并验证 Hash 与有效 Shell 处理器。该模式的默认图标序号是 **1**：

```powershell
DefaultFileAssociation.exe --msbuild --preflight --ext=.pdf --prog-id=samplepdfreader.pdf
DefaultFileAssociation.exe --msbuild --ext=.pdf --prog-id=samplepdfreader.pdf --exe-path="C:\Apps\SamplePdfReader.exe" --icon-index=1 --log-file=set.log
DefaultFileAssociation.exe --msbuild --check --ext=.pdf --prog-id=samplepdfreader.pdf
```

该模式要求 64 位 Windows、Framework64 MSBuild、HashVersion 1 和精确匹配的 `windows.storage.dll` 指纹。未知版本会在注册前拒绝继续。任务已嵌入 EXE，运行时不需要 Python、Visual Studio 或外部 `.proj` 文件。[虚拟机测试包说明](docs/msbuild-vm-test-package.md)。

独立的 `--latest-native-probe` 只用于特定构建的研究验证；已测试主机上的直接写入被拒绝。旧设置路径不实现 `UserChoiceLatest`。HTA 和工作流编译器路线已经从当前研究计划中排除；InstallUtil、Regsvr32、ODBCConf、msiexec 只在研究脚本中用于对照，不是产品后端。

### 模式与 UCPD 边界

本仓库包含多个关联和研究模式，但**没有实现 UCPD 绕过**。测试时保持 UCPD 和 Defender 开启；项目不修改驱动、不向受信任进程注入、不伪造进程身份，也不改变系统保护设置。

| 模式 | 入口 | 状态 |
| --- | --- | --- |
| 文件扩展名 | `--ext` | 用户级注册和验证；新版 Windows 可能拒绝旧 UserChoice 路径。 |
| 协议 | `--protocol` | 复用已安装处理器并验证有效 Shell 关联。 |
| 浏览器 | `--browser` | 解析所选应用注册的 HTTP、HTTPS、HTM、HTML 处理器。 |
| Framework64 MSBuild | `--msbuild` | 版本受限的 PDF 实验；要求 Framework64 MSBuild、HashVersion 1 和已知 DLL 指纹。 |
| Native 最新记录探针 | `--latest-native-probe` | 特定构建研究探针；支持只读验证，已测试主机上的直接写入被拒绝。 |
| 其他宿主 | `research/scripts/run_host_switch_tests.py` | InstallUtil、Regsvr32、ODBCConf、msiexec 的研究对照，不属于产品后端。 |
| HTA/工作流编译器 | 历史回退和实验 | 因近期 Windows/Defender 测试被拒绝，已排除。 |

“Windows 11 26H2 UCPD 破解”不是本项目支持的产品声明。当前主机 `26300.9711` 只有只读 Hash 校验脚本；产品会拒绝调用尚未完成验证的新写入指纹。Framework64 MSBuild 实验也不能描述为最新 Windows 11 的通用方案。未知构建会在调用内部地址前失败。

## GitHub 版本打包

GitHub Actions 会在推送 `v主版本.次版本.修订版本` 格式的 tag 后运行。流程使用 v143 工具集构建 x86 和 x64 Release，运行 x86 CTest，生成可复现的 Windows ZIP 包，并发布 GitHub Release。压缩包包含中英文 README、许可文件、虚拟机说明、校验清单和该版本的更新说明。

创建新版本前，请先把用户可见变化写入 `CHANGELOG.md`；如果需要独立的中英文发布说明，再添加 `docs/releases/vX.Y.Z.md`。打包脚本优先读取对应版本文件，并将其写入压缩包中的 `RELEASE_NOTES.md`。本地测试阅读器样本是可选的，GitHub CI 不依赖这些样本。

相关研究资料：

- [研究检查点](docs/research-checkpoint-20261013.md)
- [默认关联研究路线](docs/default-association-research-roadmap.md)
- [正常默认关联流程](docs/native-default-association-flow.md)
- [动态调试记录](docs/dynamic-default-association-trace-20261014.md)
- [UCPD 虚拟机内存采集计划](docs/ucpd-vm-memory-capture-plan.md)

原始证据、系统二进制、调试工具、内存转储和阅读器样本均保留在本地并被 Git 忽略。不要在物理工作机上继续运行新的宿主切换实验；虚拟机测试应保留 Defender，并同时记录关联结果和安全检测结果。

## 源码结构

- `src/cli/`：命令行解析和校验。
- `src/platform.*`：Windows 版本、令牌、SID 和字符串辅助函数。
- `src/registry.*`：HKCU 注册表封装。
- `src/browser.*`：读取已注册浏览器 Capabilities 并生成四项计划。
- `src/association_target.h`：共享文件/协议 UserChoice 路径和 ProgID 校验。
- `src/association.*`：注册 ProgID、图标、打开命令、Capabilities 和 Explorer 记录。
- `src/user_choice.*`：验证并写入版本化 UserChoice Hash，并保留 HTA 回退接口。
- `src/crypto/`：Windows CNG/crypt32 封装和 UserChoice Hash 实现。
- `tests/`：Hash 向量、命令行、注册表、关联和浏览器回归测试。

EXE 和测试程序链接同一个 `dfa_core` 静态库。注册表集成测试使用私有键；UserChoice 测试在进程内重定向 HKCU，不启动 HTA。真实 Windows 7/8/10/11 验证仍然必要，尤其是 UserChoice 保护发生变化时。

依赖许可和固定版本见 [NOTICE](NOTICE)；开源实现对比见[open-source review](docs/open-source-review.md)。
