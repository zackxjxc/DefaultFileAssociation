# Windows 11 默认关联机制与下一阶段研究

记录日期：2026-10-13。

## 目标与当前结论

研究目标是在 Windows 11 23H2 及后续版本上实现设置默认打开方式。截至 2026-10-13，已完成本机 26H2（26300.9550）的实验和用户提供的 26H2 镜像（26300.9457）的只读文件检查；镜像内系统的实际关联切换尚未测试。下文早期资料与后续实验按阶段保留，不以单机结果代表全部系统版本兼容。

当前结论、默认名单、动态策略及改名问题汇总见 [研究检查点](research-checkpoint-20261013.md)。读取已加载名单的后续候选方案见 [虚拟机内核内存采集计划](ucpd-vm-memory-capture-plan.md)，该方案尚未执行。

继续沿用普通用户权限运行的项目要求。若某条方案需要管理员、SYSTEM 或修改系统保护配置，应单独记录其前提与限制，不能据此宣称普通用户模式已经支持。

用户实测：从 Windows 11 23H2 开始，现有 HTA 方案不生效。目前尚未取得这些失败环境的完整构建号、补丁、驱动版本和日志，因此不能认定所有 23H2 环境均失败，也不能直接归因于某一种保护机制。

## UCPD 与 AppDefaultHashRotation 的区别

两者不是同一个东西，但可以共同参与默认关联保护。

| 机制 | 作用 | 需要关注的失败现象 |
| --- | --- | --- |
| UCPD，即 UserChoice Protection Driver | 内核驱动，限制对受保护默认关联注册表的修改；具体规则随驱动版本和启用状态变化 | 删除、写入或其他注册表操作被拒绝，可能出现 Win32 错误 5 |
| AppDefaultHashRotation | Windows 内部功能名称，涉及默认关联哈希及存储机制迁移 | 旧 UserChoice 写入成功，甚至旧哈希正确，但系统实际采用另一套关联记录 |
| AppDefaultHashRotationUpdateHashes | 与上述迁移相关的另一内部功能名称 | 需要结合实际迁移状态判断，不能仅凭名称存在就认定已经启用 |

这些内部机制并非稳定的公开 API。下列细节主要来自研究者的逆向分析，应以目标系统实测为准。

### UserChoiceLatest 与哈希迁移

SetUserFTA 作者 Christoph Kolbicz 在 2025-04-20 的研究中记录了以下行为：

- 系统检查已有的旧 UserChoice 关联，并迁移到 UserChoiceLatest。
- 新结构包含 UserChoiceLatest 下的 Hash，以及 UserChoiceLatest\ProgId 下名为 ProgId 的值。
- 迁移完成后，系统保护的用户数据中出现 HashVersion=1；在其观察到的环境中，系统开始采用新结构，忽略旧 UserChoice。
- 旧 UserChoice 可以继续存在，所以“旧注册表项存在且哈希正确”不足以证明实际默认关联设置成功。

以 PDF 为例，需要同时观察这些位置：

```text
HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoice
HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoiceLatest
HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoiceLatest\ProgId
HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\SystemProtectedUserData\<当前用户 SID>\AnyoneRead\AppDefaults
```

最后一个路径中的 SID 必须取当前用户实际 SID；HashVersion 位于该 AppDefaults 项下。读取失败要记录错误，不能当作值为 0。仅存在 UserChoiceLatest 也不足以证明迁移已经完成。

该研究识别出的内部功能 ID 为 AppDefaultHashRotation：43229420、AppDefaultHashRotationUpdateHashes：27623730。记录 ID 用于定位研究对象，不把启用、禁用这些功能作为项目默认实现。

来源：[UserChoiceLatest – Microsoft’s new protection for file type associations](https://kolbi.cz/blog/2025/04/20/userchoicelatest-microsofts-new-protection-for-file-type-associations/)。

### UCPD 对 HTA 路线的影响

Xusheng Li 于 2026-08-22 发布的历史二进制研究比较了多个 UCPD.sys 版本，报告如下变化：

- 驱动二进制至少在 2023 年的 Windows 更新中已经出现；公开发现或广泛启用的时间不能直接当作首次包含驱动的时间。
- 较早版本保护 HTTP/HTTPS，随后增加 PDF；后续版本又扩大了关联保护范围。
- 保护规则逐步增加对调用进程、原始文件名及调用栈等信息的检查。
- 其分析的动态拒绝规则包含 mshta.exe，较新的 4.7 版本还将它加入硬编码拒绝列表。

因此，mshta.exe 是微软签名程序这一点不能保证 HTA 写入始终被允许。但上述新版本分析不证明用户的 23H2 虚拟机已经装载同样的规则。

来源：[How Windows UCPD Grew from a 29 KB Registry Filter into a 196 KB Policy Engine](https://xusheng.dev/posts/reversing/ucpd_evolution/main/)。

## 当前方案的局限

当前工具的哈希设置路线处理旧 UserChoice，并在直接设置失败时尝试 HTA 回退。HTA 改变执行写入的进程，不会自动实现 UserChoiceLatest 所需的新机制。

需要分别验证：

1. HTA 是否启动并完成脚本。
2. 注册表删除和写入是否被允许。
3. 写入后的关联记录是否与目标相符。
4. Windows Shell 查询的有效关联是否与目标相符。
5. 用户实际双击或打开 URL 是否使用目标程序。

不能以进程退出码、图标变化或旧哈希校验成功代替最终行为验证。错误 5 也可能来自普通注册表权限，不能单独作为 UCPD 拦截的证明。

## 建议的研究与测试顺序

### 第一阶段：补齐失败环境证据

每个虚拟机记录完整构建号与 UBR、系统版本、架构、补丁、当前用户 SID、进程是否 elevated、UCPD 服务运行状态和驱动文件版本。

新增只读诊断：旧 UserChoice、新 UserChoiceLatest、HashVersion、HTA 路径、启动与退出结果、每一步注册表错误，以及 Shell 实际返回的有效 ProgID/打开命令。UCPD 服务存在、正在运行与某次操作确实被拦截是不同结论。

可先手动运行以下只读命令，辅助收集证据：

```bat
sc query UCPD
reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoiceLatest" /s
whoami /user
```

### 第二阶段：控制变量复现

保留同一虚拟机快照，比较新用户从未设置默认关联、用户通过系统界面正式设置过默认关联这两种状态。对照系统正常设置流程与原生直接写入，记录失败发生在执行、写入还是有效关联检查阶段。HTA 已按用户反馈排除，保留历史结果，不纳入后续执行计划。

以 PDF 为第一目标，再独立测试 HTTP、HTTPS、HTM、HTML。不同关联可能受到不同规则保护，不以其中一个成功代表所有关联成功。

比较相同系统版本的不同补丁状态；不要只按 23H2、24H2 等名称分支。后续版本镜像取得后也按相同标准记录。

### 第三阶段：必要的逆向分析

在能够复现的虚拟机中，观察系统设置界面完成合法默认切换时修改的键值和调用路径，与当前工具对照。可使用 Process Monitor、调试器、公开符号和静态二进制分析；涉及的模块以实际调用路径为准，重点候选为 Windows Shell/存储组件及 UCPD.sys。

优先确认新关联存储、哈希输入和验证流程，再分析驱动如何判断写入来源。区分用户态关联校验与内核写入限制，分别记录证据、推断和待验证项。

若需要临时改变保护状态作为虚拟机对照实验，应记录改变了什么，并恢复快照；这种实验结果不能当作原配置下兼容成功。

### 验收标准

- 普通用户权限下完成预期设置，Shell 查询与真实打开行为均符合目标。
- 新用户首次设置和已有默认关联的切换均验证通过。
- 重启 Explorer、注销登录、重启系统后，仍按目标程序打开。
- 设置失败明确报告失败阶段，不把部分注册或图标变化当成成功。
- 每个支持结论注明具体构建号、补丁和保护状态。

## 后续参考入口

- [Firefox WindowsUserChoice.h](https://github.com/mozilla-firefox/firefox/blob/main/browser/components/shell/WindowsUserChoice.h)：默认关联代码及 UCPD 检查的参考入口，不能据此推断 Firefox 已解决全部新保护机制。
- [Microsoft ApplicationDefaults Policy CSP](https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-applicationdefaults)：官方管理部署方式的参考资料，其策略部署前提与本项目普通用户工具不同。

以上是本机实验前的资料与研究目标；后续已增加 UserChoiceLatest 实验诊断和系统内部哈希调用验证，详见下面各节，尚未实现跨版本独立哈希算法。

## 2026-10-13：本机自动逆向研究结果

本节补充实际二进制分析和运行时实验。本机已验证一种普通用户权限下设置 `docboxtest.pdf` 的实验方案：使用现有的微软签名 amd64 MSBuild 进程执行新格式写入，调用精确匹配本机构建的系统 DLL 生成哈希。新哈希、Shell 有效关联和真实 PDF 打开均已验证成功。该结果尚不代表跨版本通用支持。

### 环境与分析方式

- 注册表版本标识为 Windows 11 26H2，构建号 `26300.9550`。
- UCPD 服务正在运行；磁盘上的 `UCPD.sys` 文件版本为 `4.8.0.702351`。未通过内核调试确认正在执行的驱动内存与磁盘文件逐字节一致。
- 使用已经安装的 IDA Professional 9.0、Hex-Rays 反编译器与 Windows SDK CDB；无需安装 REA 即可完成本轮自动分析。
- 项目内建立隔离的 Python 环境，通过 IDA 9.0 自带的 `idalib` 导出函数、导入、相关字符串、交叉引用、汇编与伪代码。
- 只分析 `research/binaries` 中的系统文件副本，不修改 Windows 二进制。隔离环境的 Lumina 连接选项指向本机回环端口；未更改已安装 IDA 的配置文件。
- UCPD 副本识别出 383 个函数；`windows.storage.dll` 副本识别出 43,747 个函数并导出 148 个相关函数的档案。自动函数边界和反编译类型仍可能有误。

### 本机使用的新关联记录

读取到 `.pdf\UserChoiceLatest\ProgId` 的 `ProgId=MSEdgePDF`，`UserChoiceLatest` 的 `Hash=nwTrccHD32s=`；当前用户的系统保护数据中 `HashVersion=1`。

在本机 `windows.storage.dll` 中定位到以下入口。表中的数值均为 RVA，相对于该模块的加载基址；不能直接用于其他构建。

| RVA | 分析得到的作用 | 证据 |
| --- | --- | --- |
| `0x57A3A4` | 读取系统保护数据中的 HashVersion | `evidence/windows-storage/18057a3a4_sub_18057A3A4.c` |
| `0x6289FC` | 打开 HKLM SQMClient 并获取机器标识 | `evidence/windows-storage/1806289fc_sub_1806289FC.c` |
| `0x628AC4` | 读取 MachineID，处理外层花括号 | `evidence/windows-storage/180628ac4_sub_180628AC4.c` |
| `0x628D98` | 根据版本、机器标识、用户、关联、ProgID 和时间生成新哈希 | `evidence/windows-storage/180628d98_sub_180628D98.c` |
| `0x57AE14` | 对照新记录中的 Hash 校验 UserChoiceLatest | `evidence/windows-storage/18057ae14_sub_18057AE14.c` |
| `0x6381AC` | 写新 ProgID，读取 ProgID 子键时间，生成并写 Hash | `evidence/windows-storage/1806381ac_sub_1806381AC.c` |

上述证据路径相对于 `research`。新流程取 `UserChoiceLatest\ProgId` 子键的最后写入时间；不能直接沿用旧 UserChoice 的时间处理规则。

`research/scripts/hash_probe.py` 在独立的普通用户进程内调用本机新哈希入口，并先检查系统 DLL 的 SHA-256 必须等于分析副本。只读模式成功重算出 `nwTrccHD32s=`，与当前实际保存的 PDF 哈希完全相同。

同样输入时间下，换成 `docboxtest.pdf` 会得到另一个哈希；它只是输入变化对照，不是已经提交或已经生效的默认关联。该验证仅覆盖本机当前 PDF 样本，尚未独立重实现算法，也不能视为跨版本支持。

### Python 进程直接写新结构被拒绝

使用该脚本的显式 `--set-pdf` 模式，尝试把已注册的 `docboxtest.pdf` 写入新结构。没有删除或改 ACL，也没有关闭 UCPD 或修改系统保护功能。

实验结果：

1. 可以用 `KEY_READ | KEY_SET_VALUE` 打开新结构及其 ProgId 子键。
2. `RegSetValueExW` 写 `ProgId=docboxtest.pdf` 返回 Win32 错误 5。
3. 在 CDB 中命中真实的 `ntdll!NtSetValueKey`，确认值名为 `ProgId`、待写数据为 `docboxtest.pdf`、键路径为 `.pdf\UserChoiceLatest\ProgId`。
4. 键句柄的 GrantedAccess 为 `0x2001B`，包含 SetValue；系统调用返回后 `EAX=0xC0000022`，即 STATUS_ACCESS_DENIED。
5. 此轮新结构实验完成的注册表写入为 0；原新结构的 ProgID、Hash 和 ProgId 子键时间均未变化。
6. Shell 复查仍返回 `MSEdgePDF`，打开命令仍指向 Edge。

这证明拒绝发生在实际注册表系统调用中，而不是 Python 参数处理或原工具的旧格式校验。不能仅凭用户态调用栈证明是哪一个内核过滤器返回拒绝；还没有在内核调试器中命中 UCPD 的具体回调分支。

### UCPD 静态分析得到的拒绝路径

- RVA `0x114E0` 调用 CmRegisterCallbackEx，注册 RVA `0x11380` 的回调。
- 回调对注册表操作类别分派；设置值路径进入 RVA `0xDA58`，再根据保护规则类型选择处理函数。
- 用户关联规则入口 RVA `0xA30C` 根据功能状态选择处理分支；其中一个分支 RVA `0xAA0C` 会查询当前进程的信任条件，并可能返回 `0xC0000022`。
- 该分支调用的信任检查入口为 RVA `0x1667C`，涉及签名/发布者、允许与拒绝规则，以及受功能位控制的原始文件名检查。进一步的栈检查受单独条件控制。
- 初始化函数 RVA `0x2A360` 包含 PDF 的旧结构、新结构与新 ProgId 子键的保护规则。

这些是磁盘副本的静态调用关系；注册表功能值可帮助提出运行分支假设，但不能代替对驱动实际内存状态及回调执行的验证。

### 产物与复现入口

- `research/evidence/hash-probe-edge-validation.json`：最初对 Edge 关联的只读新哈希验证。
- `research/evidence/hash-probe.json`：当前关联的只读新哈希验证。
- `research/evidence/latest-set-probe.json`：实际新结构写入结果和前后状态。
- `research/evidence/registry-write-trace-automated.log`：自动 CDB 系统调用跟踪；包含当前用户 SID，分享前应脱敏。
- `research/evidence/final-shell-diagnosis.log`：Shell 实际选择。
- `research/binaries/UCPD.sys.i64`、`research/binaries/windows.storage.dll.i64`：可在 IDA 中打开的数据库。
- `research/scripts/ida_export.py`、`research/scripts/hash_probe.py`、`research/scripts/trace-write.dbg`：分析、验证与调试脚本。

只读哈希验证命令：`python .\research\scripts\hash_probe.py`。带 `--set-pdf` 会尝试改动真实 PDF 默认记录，只在明确需要设置测试时执行。

CDB 使用本地符号路径及 `-cf` 读取命令文件；不要用 `$$><` 把包含 `.sympath` 和运行控制命令的文件折叠成一行，本轮已实测该方式导致命令解析错误。使用 `-cf` 的完整自动流程已执行，捕获到上述 `0xC0000022` 返回值。

### 微软签名 MSBuild 进程的对照实验成功

验证现有 `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe` 的 Authenticode 签名有效，签名主体为 `.NET / Microsoft Corporation`。通过 CodeTaskFactory 在该进程中执行项目内的 C# 测试任务，不启动提升进程、不修改签名程序文件、不更改 UCPD 状态。

任务首先验证未提升权限、64 位进程、系统 DLL SHA-256 精确匹配、HashVersion 为 1，并成功重算现有哈希。随后写入 `UserChoiceLatest\ProgId\ProgId=docboxtest.pdf`，读取该子键的新时间戳，生成新哈希，写入 `UserChoiceLatest\Hash`，通知 Shell。

结果：

- 完成 2 次目标注册表写入，任务返回 0。
- 独立 Python 只读复查成功重算新哈希。
- Shell 返回有效 ProgID `docboxtest.pdf`，打开命令为 `"E:\Work\DefaultFileAssociation\test_case\DocBox.exe" "%1" %*`。
- 通过系统 Shell 打开 `test_case/012.pdf`，实际启动 `test_case/DocBox.exe`；进程命令行包含测试 PDF 路径，窗口显示该 PDF 第 1 页、共 10 页。
- 封装脚本再次执行设置并验证成功，产生独立运行报告。

这个成败对照说明写入来源会影响结果，但没有通过内核断点证明 MSBuild 被允许的具体原因；不能仅凭签名有效就宣称所有微软签名程序都会通过。

可重复执行的入口为 `test_case/set-pdf-default-latest.ps1`，支持 `-CheckOnly` 和 `-OpenPdf`。其依赖的任务位于 `research/scripts/msbuild-latest-probe.proj`。任务对系统 DLL 版本实施 SHA-256 拒绝保护，防止在更新后的系统上误调用内部函数地址。

原 C++ EXE 的普通设置路径和旧 `test-pdf-association.ps1` 仍用于旧结构测试。新增的 `--latest-native-probe` 是独立的原生实验入口，见下一节；普通 `--check` 仍可能报告旧哈希缺失。新脚本以新哈希验证、有效 Shell ProgID 与打开命令判断成功。

后续在可恢复快照的虚拟机中验证其他构建、注销/重启持久性、首次设置和 UCPD 实际放行分支；当前交付为本机实验脚本方案。

### 原生 C++ EXE 的对照实验：写入被拒绝

新增 `src/latest_native_probe.cpp`，在当前 `DefaultFileAssociation.exe` 内实现同一套新格式读取、系统 DLL 指纹检查、HashVersion 检查、内部哈希函数调用、直接注册表写入及 Shell 查询。调用入口不启动 MSBuild 或 HTA，不删除键、不改 ACL、不跳过已经是目标 ProgID 的实际写入。

实验要求 x64 EXE、当前普通用户、已有 `.pdf` 新结构及已注册 ProgID；不同 DLL 指纹或 HashVersion 会被拒绝。x86 构建仍可编译，调用本实验会返回不支持。`--check` 仅读取并验证；显式写入允许修复已有无效 Hash，并如实记录前后状态。

复现原生写入：

```powershell
& .\out\build\windows-x64-v143\Release\DefaultFileAssociation.exe --latest-native-probe --ext=.pdf --prog-id=docboxtest.pdf --log-file native-probe.log
```

加 `--check` 可只读复查。这个入口只复用已注册处理器，不接受 `--exe-path`，也不走普通设置路径。

本轮开始时，注册表中的 Hash 与 ProgId 子键时间不匹配。Python、MSBuild 和原生 EXE 三者计算出了相同的预期 Hash，排除了原生移植计算差异。首次原生实际写入返回错误 5，零次写入，前后值与时间均未变化。

随后单独运行 MSBuild 对照任务，使用显式 `/p:PermitHashRepair=true` 修复已有 `docboxtest.pdf` 的无效 Hash。该参数默认关闭，只有同时显式设置且原 ProgID 已经是目标时才允许修复；普通任务行为保留原校验要求。MSBuild 对照成功完成两次设置调用。该操作不是原生 EXE 的一部分。

在有效记录上重测：

| 操作 | 结果 |
| --- | --- |
| 原生 EXE 只读重算 Hash 和 Shell 查询 | 返回 0，Hash 匹配，Shell 为 `docboxtest.pdf`，命令指向 DocBox |
| 原生 EXE 实际 `RegSetValueExW` 写 ProgId | 返回错误 5 / HRESULT `0x80070005`，EXE 退出 5 |
| 实际完成的原生注册表写入 | 0；值和子键时间均未变化 |
| 原生 EXE 写后只读复查 | 返回 0，Hash 和 Shell 仍有效 |
| UCPD | 仍在运行，没有修改保护配置 |

因此，单纯把新哈希函数调用和注册表操作搬入自有 EXE，未能获得独立设置能力。执行来源会影响结果；MSBuild 的具体放行分支仍未通过内核断点确认，不能把观察结果直接表述成已证实的白名单成员关系，也不能推断普通商业代码签名可以通过。

证据：`research/evidence/native-experiment-summary.json`、`native-valid-write-probe.log`、`native-valid-check-before.log`、`native-valid-check-after.log`、`native-port-msbuild-control.json`。本轮没有使用 computer use，也没有打开 PDF；成功性通过后台 Hash 重算与 Shell 接口查询检查。

### 允许列表、拒绝列表与 MSBuild 放行原因的进一步分析

本轮仅使用后台命令和 IDA 的 idalib，对同一驱动副本作静态分析，并读取现有签名缓存与注册表功能值。没有调用设置签名缓存的接口，没有修改驱动、保护功能或 PDF 关联。

已确认允许列表和拒绝列表机制确实存在，但“只有名字在白名单中的程序才可以写入”不是该实现的准确描述：

- RVA `0x16458` 读取每类规则的允许列表。列表非空时必须命中；列表计数为 0 时直接返回允许列表检查通过。这不代表整个保护检查通过。
- RVA `0x16510` 读取拒绝列表。命中时返回拒绝标记，计数为 0 时返回未命中。
- RVA `0x1667C` 组合签名检查、允许列表、拒绝列表及受功能位控制的 OriginalFilename 检查。还存在 System 进程等特殊路径。
- RVA `0x12460` 和 `0x125F0` 包含明确的 `AllowListV1`、`DenyListV1` 名称，支持动态规则更新。

PDF 对应规则类别 1。默认配置在 RVA `0x116C4` 分配并清零；初始化函数 RVA `0x2A360` 设置该类拒绝列表，未设置其允许列表。默认允许列表的备份指针 `qword_1400259A0` 和计数 `dword_1400259A8` 在磁盘映像中均为 0，交叉引用也未发现静态写入。因此默认 PDF 配置允许列表为空。加载后的动态策略可能改变它；尚未读取实际内核配置表。

静态拒绝数组 `off_140025450` 包含 14 项，包括 `mshta.exe`、`powershell.exe`、`pwsh.exe`、`reg.exe` 等，不含 `MSBuild.exe`。扫描该驱动的 ASCII/UTF-16 字符串未发现 MSBuild 名称。完整内容位于 `research/evidence/ucpd-policy-detail/policy-data.json`；这不能排除动态策略中的名称。

签名判断进一步定位到 RVA `0x19098`：它调用 ZwGetCachedSigningLevel，并使用 NtCompareSigningLevels 与签名等级 8 比较；还可进入 CiValidateFileObject 的验证路径。Windows SDK 的 `winnt.h` 将等级 8 定义为 Microsoft，等级 4 定义为 Authenticode，等级 12 定义为 Windows。

本机只读 GetCachedSigningLevel 的结果：

| 文件 | 缓存签名等级 | 其他证据 |
| --- | --- | --- |
| MSBuild.exe | 8 / Microsoft | Authenticode 有效，OriginalFilename 为 MSBuild.exe |
| Python.exe | 4 / Authenticode | 直接写入实验拒绝 |
| mshta.exe | 12 / Windows | PDF 默认拒绝列表明确包含该程序 |
| 自有原生 EXE | 查询返回错误 50 | Authenticode 检查为 NotSigned；不能把缓存查询失败等同于某个签名等级 |

当前读取到 FeatureV3=126，其位 0 未设置；RVA `0x15934` 在该条件下选择 `0x19098` 路径。FeatureV3 的位 1 已设置，对默认拒绝数组选择 14 项。FeatureV2=3010323438（0xb36de3ee），OriginalFilename 检查位 0x200000 已设置；条件性栈检查入口的位 0x400 未设置。这些是根据当前注册表值和磁盘代码作出的运行路径推断，并非已经观察到的驱动内存状态或断点执行。

因此目前最有力的解释是：MSBuild 满足本机可见签名门槛，未命中默认拒绝列表，且没有被当时生效的额外规则拒绝。不是已经证实它被特意添加到一个 MSBuild 专属白名单。仍需内核运行时验证具体放行分支。

本机 UCPD/DR/0000 存在动态规则数据，说明不能只凭默认初始化还原全部生效策略。后续候选包括先采集虚拟机完整实时内核转储、离线读取名单，再按需通过 WinDbg/KD 实时内核调试确认写入分支。RVA `0x1667C`、`0x19098` 和 `Context+64` 仅适用于已分析的精确驱动版本；目标版本不同必须重新定位。只有取得实际配置和返回路径，才能把目前的解释提升为确定的运行时原因。主机不改内核调试配置，也不重启。

新增复现脚本为 `research/scripts/ucpd_policy_dump.py` 与 `research/scripts/signing_level_probe.py`。签名缓存报告位于 `research/evidence/signing-level-probe.json`；签名、默认初始化与动态更新入口的反编译档案分别位于 `ucpd-signature-detail`、`ucpd-policy-detail`、`ucpd-policy-update-detail`。

### 系统自带 Framework MSBuild：本机设置模式成功，ISO 五个版本均携带

已测试 `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\MSBuild.exe`，文件版本为 4.8.9221.0，Authenticode 签名有效。任务已兼容 `Microsoft.Build.Tasks.v4.0.dll`；Framework MSBuild 需要显式 `/ToolsVersion:4.0`，否则没有 ToolsVersion 声明的项目会选择旧工具集路径。本轮没有安装 Visual Studio 或修改系统组件。

测试顺序为 Framework 宿主只读重算 Hash、实际执行设置模式、封装脚本只读复查、独立原生 EXE 再次验证。结果：

- 设置模式完成 2 次注册表设置调用，MSBuild 退出 0。当前原本已经是 `docboxtest.pdf`，任务仍实际执行写入，没有以“已是默认”为由跳过；前后值与时间未变化。
- 有效 Hash、Shell ProgID 和打开命令均正确；Shell 命令指向 `test_case/DocBox.exe`。
- 宿主和任务加载的 Microsoft.Build 组件均来自 Windows 的 .NET Framework 目录/GAC，未依赖 Visual Studio 构建组件。
- UCPD 仍在运行；没有修改保护配置，没有使用 computer use，没有打开 PDF 或其他窗口。

`test_case/set-pdf-default-latest.ps1` 已改用系统 Framework64 MSBuild，仍支持 `-CheckOnly` 和 `-OpenPdf`；本轮未使用 `-OpenPdf`。设置报告为 `test_case/results/latest-pdf-20261013-223927-4e7417b6/report.json`，只读报告为 `latest-pdf-20261013-224202-d79bfab3/report.json`。独立验证日志为 `research/evidence/framework-independent-native-check.log`。

同时检查用户提供的 `E:\Download\zh-cn_windows_11_business_editions_version_26h2_x64_dvd_74d48a06.iso`。ISO 只读挂载且不分配盘符；读取 `sources/install.wim`，未安装或应用 Windows 映像。WIMGAPI 可读元数据，但提取文件要求特权，因此使用从 wimlib 官方下载的项目内便携工具，在普通用户下列出每个版本并抽取少量指定文件，不运行抽取的程序。

映像元数据为 10.0.26300.9457，包含以下 5 个版本：Education、Enterprise、Pro、Pro Education、Pro for Workstations。所有版本均找到 Framework/Framework64 的 MSBuild.exe、Microsoft.Build.Tasks.v4.0.dll，以及 Framework64 的 Utilities、配置文件、csc.exe 和编译器配置文件。抽取的 Framework 文件 SHA-256 全部与本机对应文件相同；该镜像确实携带所需宿主与组件。报告为 `research/evidence/iso-framework-inspection.json`。

兼容性边界仍然存在：

- ISO 的 `windows.storage.dll` SHA-256 为 `cf4793b9e7570f1c6621515d634e2fee115b51f6009a19e0d1a5d24cefb55703`，与当前已验证的 `8d6ac2b451af71e913665c8459935ac862e219458f0962c99f3a3d48ab9c251b` 不同。
- ISO 的 UCPD.sys SHA-256 为 `61bf7cedf139bafee613507f01117d388c78d7de02e43e43fe7ed118bb569d3c`，也与本机分析副本不同。
- 因此当前脚本会拒绝在该 ISO 原始系统 DLL 上调用内部地址；不能以存在同一个 MSBuild 文件就推断该虚拟机已验证可设置。下一步需针对镜像中的系统 DLL 定位与验证新哈希入口，再在干净虚拟机中测试。

本次新增 ISO 检查脚本为 `research/scripts/inspect_framework_in_wim.py` 和 `inspect_framework_iso_wimlib.py`；前者保留系统 API 特权错误证据，后者完成实际文件检查。临时 ISO 挂载在读取完成后卸载；只卸载本轮自己创建的挂载，状态记录为 `research/evidence/iso-mount-state.json`。

### 其他宿主实验与 Defender 告警

后台筛查了本机 12 个候选路径。InstallUtil、Regsvr32、ODBCConf 的回调均成功完成固定 `docboxtest.pdf` 的两次设置调用，随后独立原生检查 Hash 与 Shell 返回 0。当前记录原本即为目标，任务没有跳过真实写入；没有安装服务、注册实际 COM 组件或创建 ODBC 数据源。RegAsm 在回调前因权限要求退出 100，没有执行目标写入。结果与完整逐项日志在 `research/evidence/alternate-host-results.json`、`research/evidence/host-probes`。

另测试了 Microsoft.Workflow.Compiler 的普通工作流编译/验证回调。虽然测试进程退出 0、回调报告成功且关联验证通过，但随后 Defender 行为监控命中，因此不能将它计为可用方案，也不能仅以退出码或关联生效判断宿主方案可部署。

2026-10-13 23:13:57 至 23:13:58（Asia/Shanghai），Defender 事件 1116/1117 记录 `Behavior:Win32/CICompiler.B!attk`，ThreatID=2147730425，关联工作流测试进程 PID 27328、28848 及相关进程。对应只读/写入回调报告分别生成于 23:13:56.802 与 23:13:57.898。时间、执行路径与过程一致，确认由本次工作流宿主实验触发。

告警资源为行为/进程对象；系统 Microsoft.Workflow.Compiler.exe 文件仍存在，签名检查有效。Get-MpThreatDetection 报告处理成功，事件显示无需额外操作；没有据此宣称全机无风险。Defender 的实时保护和行为监控保持启用。没有关闭防护、添加排除项、恢复或重建被处理内容。

已停止主机上的新宿主执行测试。`run_alternate_host_tests.py` 移除了 workflow 执行计划与选项，并要求显式选择其余宿主，默认不再执行任何宿主。工作流相关源码仅保留为静态分析证据。后续此类行为风险较高的研究应转到隔离虚拟机，保持防护启用并记录拦截结果。

告警证据为 `research/evidence/defender-host-experiment-events.json` 与 `defender-workflow-threat-detection.json`。新增原生宿主 DLL 通过默认关闭的 CMake 选项 `DFA_BUILD_HOST_PROBES` 构建；所有实验仍受当前系统 DLL 精确指纹与固定 PDF 目标限制。

## 2026-10-14：无需研究宿主的正常流程分析

后台静态分析定位了 Windows.UI.AppDefaults.dll 中的扩展名设置项和 PDF 快捷设置实现。单扩展名入口通过 OpenWithLauncher 本地 COM 服务器启动系统选择界面，本机注册服务器是 OpenWith.exe；PDF 快捷动作另有同源 UserAssocSet/新 Hash 写入链。UCPD 一个受功能条件选择的分支检查实际当前写入进程，没有找到向自有 EXE 转移直接写入信任的授权步骤。

本轮未操作界面、启动上述系统选择组件或修改 PDF 默认关联，未把私有函数在自有 EXE 中执行。正常确认后的实际 writer 和内核放行分支仍需 VM 运行时证据。完整路径、公开接口限制和可能的原生能力边界见 [原生 EXE 与正常默认关联流程](native-default-association-flow.md)。
