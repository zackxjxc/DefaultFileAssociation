# DefaultFileAssociation

[English](README.md) · 中文

`DefaultFileAssociation.exe` 用当前用户权限注册 Windows 文件关联，并在系统允许旧版 `UserChoice` 兼容路径时设置当前用户的默认程序。工具不会请求管理员权限；如果进程已经提升，会直接退出，避免把关联写入错误的管理员用户配置。

针对 Windows 11 26H2 的部分已验证构建，项目还找到了一些可以用**普通用户权限设置默认浏览器关联**的实验性方案。相关方案仍处于研究阶段，不保证稳定性或跨版本兼容性。

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

针对 Windows 11 26H2 的部分已验证构建，本项目已经找到几种可以在**普通用户权限**下尝试设置默认打开方式（包括默认浏览器相关关联）的实验性方案。这些方案仍处于研究阶段，不保证稳定性、跨版本兼容性，或在所有系统安全策略下都能使用。

默认浏览器演示使用 `--browser`，名称必须是浏览器在 `RegisteredApplications` 中的注册名称，例如 `Google Chrome`：

```powershell
DefaultFileAssociation.exe --browser="Google Chrome" --dry-run
DefaultFileAssociation.exe --browser="Google Chrome" --log-file=browser.log
DefaultFileAssociation.exe --browser="Google Chrome" --check --log-file=browser-check.log
```

浏览器模式会复用已安装浏览器的 Capabilities，依次处理 `http`、`https`、`.htm` 和 `.html` 四项关联，不要求传入 EXE、ProgID 或图标参数。MSBuild 和四种宿主后端属于 PDF 专用研究路线，不能与 `--browser` 组合；运行时会根据当前机器的 UserChoice 结构选择旧哈希或新哈希路径。

PDF 后端使用同样的参数和三个操作阶段，只需把 `--msbuild` 换成 `--backend=<name>`：

```powershell
DefaultFileAssociation-x64.exe --backend=installutil --preflight --ext=.pdf --prog-id=samplepdfreader.pdf
DefaultFileAssociation-x64.exe --backend=regsvr32 --ext=.pdf --prog-id=samplepdfreader.pdf --exe-path="C:\Apps\SamplePdfReader.exe"
DefaultFileAssociation-x64.exe --backend=odbcconf --check --ext=.pdf --prog-id=samplepdfreader.pdf
DefaultFileAssociation-x64.exe --backend=msiexec --ext=.pdf --prog-id=samplepdfreader.pdf --exe-path="C:\Apps\SamplePdfReader.exe"
```

可用名称为 `msbuild`、`installutil`、`regsvr32`、`odbcconf` 和 `msiexec`；`--msbuild` 是 `--backend=msbuild` 的兼容别名。四种新增路线分别使用托管宿主 DLL 或原生宿主 DLL，DLL 会随构建输出放在 EXE 旁边。它们只支持 `.pdf`、`DefaultFileAssociation-x64.exe` 和普通用户权限；x86 EXE 会拒绝这些模式。

新结构后端要求 `HashVersion=1`；已知 `windows.storage.dll` 指纹优先调用系统内部实现，未知指纹则改用项目内置的 26H2 独立实现，并在写入后重新计算和验证 live `UserChoiceLatest` 记录。独立实现属于实验性兼容路径，不能保证适用于所有 Windows 服务版本。MSBuild 任务嵌入 EXE，其他宿主使用随 EXE 构建的 `DfaAssociationHost-*.dll` 和 `DfaAssociationManagedHost-*.dll`，运行时不需要 Python 或 Visual Studio。[虚拟机测试包说明](docs/msbuild-vm-test-package.md)。

独立的 `--latest-native-probe` 仍只用于特定构建的研究验证；已测试主机上的直接写入被拒绝。旧设置路径不实现 `UserChoiceLatest`。HTA 和工作流编译器路线已经从当前研究计划中排除。

### 模式与 UCPD 边界

程序提供一个用户级默认关联接口，支持文件扩展名、协议和已安装浏览器；PDF 新格式还提供五种显式后端：

1. Framework64 MSBuild；
2. InstallUtil；
3. Regsvr32；
4. ODBCConf；
5. msiexec。

这些后端不是 UCPD 绕过选择器。项目不修改 UCPD 驱动、不向受信任进程注入、不伪造进程身份、不关闭 Defender，也不改变系统保护设置。HTA 和工作流编译器路线因安全检测已排除。`--latest-native-probe` 是独立的版本受限探针，只读验证不能等同于受支持的设置接口。

`--diagnose` 会在不写入的情况下报告两层 Hash：`legacy_hash_profile` 表示根据系统版本选择的 Legacy、Windows 10 Threshold 1 或 Threshold 2 及以上算法；`latest_hash_version` 和 `hash_profile` 表示是否检测到经过验证的 `UserChoiceLatest` 记录。只有受保护的 `HashVersion` 值和记录结构同时存在时，才认为新结构已启用；不能只根据 OS build 判断。

“Windows 11 26H2 UCPD 破解”不是本项目支持的产品声明。当前已对 `26300.9711` 和本机 `26H2` 的普通用户写入流程完成实验验证；未知 DLL 不会调用未经确认的内部地址，而是尝试独立算法并以 live 记录校验结果。该实验仍不能描述为最新 Windows 11 的通用方案。

`UserChoiceLatest` 独立实现的计算顺序是：读取 `HKLM\SOFTWARE\Microsoft\SQMClient\MachineId` 并去除外层花括号；按机器 ID 的最后一个 UTF-16 单元选择 3 条版本表之一；按表内排列拼接扩展名、SID、ProgID、UTC FILETIME 高低 DWORD 的 16 位小写十六进制文本、Microsoft 固定种子和 Machine ID，整体转小写；用 26H2 的变宽码表和回溯压缩器生成 UTF-16 字符串；把压缩字符串连同结尾 NUL 作为 UTF-16LE 输入做 MD5；取 MD5 前 8 字节作为两条 CS64 混合链的种子，逐 32 位小端字处理后将两条 64 位结果异或；最后对 8 字节结果做标准 Base64，得到写入 `Hash` 的值。Machine ID 是 Windows SQM 遥测标识，不是密码学唯一标识，可能因克隆、重装或策略变化而改变，也不建议作为其他应用的长期唯一身份。

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
- `src/latest_hash_self.*`：读取 Machine ID，并实现独立的 26H2 UserChoiceLatest 压缩和 Hash。
- `src/crypto/`：Windows CNG/crypt32 封装和 UserChoice Hash 实现。
- `tests/`：旧 Hash 向量、独立 UserChoiceLatest 向量、命令行、注册表、关联和浏览器回归测试。

EXE 和测试程序链接同一个 `dfa_core` 静态库。注册表集成测试使用私有键；UserChoice 测试在进程内重定向 HKCU，不启动 HTA。真实 Windows 7/8/10/11 验证仍然必要，尤其是 UserChoice 保护发生变化时。

依赖许可和固定版本见 [NOTICE](NOTICE)；开源实现对比见[open-source review](docs/open-source-review.md)。
