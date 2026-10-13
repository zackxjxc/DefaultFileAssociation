# 默认文件关联研究检查点

记录日期：2026-10-13；实验时间使用 Asia/Shanghai。本文是本次研究的归档，区分已验证结果、静态分析推断和未执行方案。

## 范围与环境

- 从用户提供的三个 git-format patch 恢复项目，恢复文件与补丁中的 Git 对象指纹核对；当前目录原本没有 Git 仓库，本次归档初始化本地仓库，不推送。
- 本机 Windows 显示版本 26H2，构建 26300.9550；UCPD 运行，研究未关闭其保护，也未更改 Defender 防护设置。
- 目标为 `.pdf`、ProgID `docboxtest.pdf`、本地测试阅读器 `test_case/DocBox.exe`。阅读器和 PDF 样本是用户本地文件，不随源码提交。
- CMake 4.3.2、Visual Studio 2026、v143 工具集；x86 为原项目发布目标，x64 用于新格式原生对照实验。
- 已使用本机 IDA Pro 9.0、Hex-Rays、idalib 和 Windows CDB。未安装 REA；后续不需要在虚拟机安装 AI 才能采集证据。
- 用户要求只在后台工作；Defender 告警后停止物理机上的新宿主执行测试。虚拟机内存采集和内核调试仍未执行。

## 当前实验结果

| 路线 | 实际观察 | 结论边界 |
| --- | --- | --- |
| 原项目旧 UserChoice 路线 | 本机 PDF 设置被拒绝，错误 5 | 普通 `--check` 不验证新格式，不能作为 UserChoiceLatest 成功依据 |
| Python 新格式直接写入 | 系统调用返回 `STATUS_ACCESS_DENIED`，零次目标写入 | 已确认系统调用失败，未通过内核断点证明具体拒绝者和分支 |
| 自有 x64 EXE 新格式直接写入 | 只读 Hash/Shell 校验通过；实际写 ProgId 错误 5、零次写入 | 原生计算逻辑与对照一致，但没有获得独立设置能力 |
| Visual Studio MSBuild | 从 Edge 切换到 DocBox；Hash、Shell、实际 PDF 打开验证成功 | 单机结果，依赖精确系统 DLL 内部函数，不是稳定公开接口 |
| 系统 Framework64 MSBuild | 两次真实设置调用成功，独立原生校验通过 | 测试前已是 DocBox；尚未证明干净 VM 上完整首次切换 |
| InstallUtil、Regsvr32、ODBCConf | 各完成两次设置调用，独立 Hash/Shell 校验通过 | 测试前已是目标；近期查询未发现对应 Defender 告警，不代表跨环境不会告警 |
| RegAsm | 回调前权限检查退出 100 | 没有执行目标写入，不计为成功路线 |
| Microsoft.Workflow.Compiler | 写入和关联校验通过，但触发 Defender 行为检测 | 排除；退出码与关联有效不足以判定可用 |
| HTA | 用户报告较新 Win10 告警；本机默认 PDF 拒绝数组包含 mshta.exe | 按用户要求排除；Win10 告警不是本次自行测得的结论 |

新格式使用 `UserChoiceLatest/Hash` 和 `UserChoiceLatest/ProgId/ProgId`，验证涉及 ProgId 子键时间、HashVersion、机器标识及当前用户等输入。当前实现调用精确匹配版本的 `windows.storage.dll` 内部函数，尚未独立重实现算法。文件指纹不同不等于算法一定改变，但现有地址不能直接复用。

原生入口为 `--latest-native-probe`，只读时加 `--check`。Framework 脚本为 `test_case/set-pdf-default-latest.ps1`；普通旧格式设置/检查与实验入口分开。当前任务只归档，不执行关联设置。

## Framework MSBuild 与镜像检查

本机 Framework64 MSBuild 版本 4.8.9221.0，实验加载的构建组件来自 Windows Framework/GAC，未依赖 Visual Studio 组件。项目任务兼容 `Microsoft.Build.Tasks.v4.0.dll`，使用 ToolsVersion 4.0。

用户提供的 26H2 商业版 ISO 中，install.wim 元数据为 26300.9457，五个版本均含 Framework/Framework64 MSBuild 及相关构建组件。只读抽取的 Framework 文件与本机对应文件 SHA-256 相同。镜像检查后已卸载本轮挂载。

| 文件 | 本机分析指纹 SHA-256 | ISO 文件 SHA-256 |
| --- | --- | --- |
| windows.storage.dll | `8d6ac2b451af71e913665c8459935ac862e219458f0962c99f3a3d48ab9c251b` | `cf4793b9e7570f1c6621515d634e2fee115b51f6009a19e0d1a5d24cefb55703` |
| UCPD.sys | `1b9b4998342d51e9f350e5f87c52183ea096f8e6d226bada879493a67cf4ddc7` | `61bf7cedf139bafee613507f01117d388c78d7de02e43e43fe7ed118bb569d3c` |

该 ISO 携带 MSBuild 不代表其中安装出的系统已验证设置成功；系统 DLL 和驱动不同，必须重新定位并测试。

## 宿主候选如何产生

从“同一新格式逻辑在不同写入进程中成败不同”提出执行来源影响结果的假设，再寻找 Windows 提供的正常扩展机制：安装器回调、DLL 注册入口、构建任务、编译验证回调。官方文档用于确认扩展能力，本机文件清单用于确认存在性，实际调用和独立校验用于确认实验结果。

InstallUtil 来自安装器扩展；Regsvr32、ODBCConf 来自 DLL 注册机制；工作流编译器来自编译验证回调。候选并非从已读取的运行时白名单中直接得到，也不能因具有微软签名就推断必然通过。

来源：[InstallUtil](https://learn.microsoft.com/en-us/dotnet/framework/tools/installutil-exe-installer-tool)、[Regsvr32](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/regsvr32)、[ODBCConf](https://learn.microsoft.com/en-us/sql/odbc/odbcconf-exe?view=sql-server-ver17)。ODBCConf 已弃用，并计划在未来移除。

## UCPD 名单及其他检查

本机 UCPD 文件版本 4.8.0.702351。IDA 静态分析确认存在允许列表和拒绝列表，以及名为 `AllowListV1`、`DenyListV1` 的动态更新入口。

PDF 默认拒绝数组的 14 项实际模式均为 `*\程序名.exe`：

```text
dllhost.exe
reg.exe
rundll32.exe
powershell.exe
regedit.exe
wscript.exe
cscript.exe
cmd.exe
InfDefaultInstall.exe
pwsh.exe
wmiprvse.exe
regini.exe
bssafe.exe
mshta.exe
```

静态初始化的 PDF 允许列表为空。允许列表非空时必须匹配，为空时仅表示这一关通过；整体还要组合签名/信任、拒绝名单、受条件控制的 OriginalFilename 和调用栈等检查。System 进程等有特殊路径，名单也按受保护规则类别区分。

静态默认名单不包含 MSBuild，驱动字符串扫描未发现该名称。签名检查可比较 Microsoft 等级，并进入代码完整性验证路径。本机缓存查询中 MSBuild 为 Microsoft 等级 8、Python 为 Authenticode 等级 4。自有 EXE 缓存查询错误 50 不能当作某个签名等级。

“MSBuild 满足可见信任门槛且未命中默认拒绝列表”是目前有依据的解释；“MSBuild 被专门列入白名单”没有得到证明。当前功能值与磁盘代码对应的路径只是推断，尚未读取实际驱动内存配置或命中内核分支。

## 动态更新与改名问题

已看到动态名单加载入口：加载成功使用动态配置，失败恢复默认配置。本机存在 UCPD/DR/0000 数据，但尚未完整解析，也未确定是谁写入或经什么渠道分发。因此不能说当前名单一定更长、默认名单已经过时，或 Defender 情报更新一定负责更新 UCPD 名单。

Defender 自身可以更新安全情报与检测逻辑；这是独立事实，不证明其更新 UCPD 的 AllowListV1/DenyListV1。来源：[微软安全情报更新说明](https://www.microsoft.com/en-us/wdsi/definitions)。

改外部 EXE 文件名可能改变某个名称匹配结果，但内部 OriginalFilename 通常不变，签名与信任等级也不因改名提升。修改已签名文件资源通常破坏原签名。非空允许名单还可能因改名失去匹配。因此没有验证“仅改名即可绕过整体检查”，不作为解决方案。

UCPD 是否允许写入、Shell 是否接受关联 Hash、Defender 是否阻止执行是不同层次，均应分别记录。

## Defender 事件与处理

2026-10-13 23:13:57 至 23:13:58，工作流编译器实验触发 `Behavior:Win32/CICompiler.B!attk`，Defender 事件 1116/1117 和测试进程时间对应，处理报告成功。系统工作流编译器文件仍存在、签名有效；“已删除”行为告警不等于系统 EXE 已删除。

已从执行计划移除 workflow；其他宿主测试要求显式选择，默认不执行。工作流源码只保留作静态研究记录。原生宿主 DLL 的 CMake 开关 `DFA_BUILD_HOST_PROBES` 默认关闭。没有添加排除、关闭防护或恢复被处理内容；后续执行测试转入隔离 VM。

## 后续候选与未完成事项

1. [完整实时内核转储方案](ucpd-vm-memory-capture-plan.md)：先取得正常运行时的策略快照，在本机离线分析，尚未执行。
2. 必要时实时内核调试：确认实际拒绝/放行分支、策略变化及更新来源，尚未建立连接。
3. 研究无需宿主的方向：观察用户通过 Windows 设置完成默认切换的正常流程，定位注册、确认、Hash 生成与受信任写入的边界；当前没有已验证的原生独立设置解法。
4. 新系统 DLL 适配、干净用户首次切换、从其他阅读器切换、注销/重启持久性，以及 Defender 开启时的 VM 验证均待完成。

快照中的名单只能证明采集时刻状态；更新来源和操作为何通过仍需时间对照或执行路径证据。

## 本次归档验证

重新构建 x86 Release、x64 Release 和 x86 Debug 测试目标均成功，Windows API 导入检查通过，构建日志无编译警告或错误。x86 Debug 的 6 个 CTest 回归测试全部通过；它们使用私有键或进程内 HKCU 重定向，不替换真实默认关联。8 个 Python 脚本通过 AST 语法检查，2 个 PowerShell 脚本通过解析器检查，未执行宿主测试或关联写入。文档链接、UTF-8 无 BOM/LF 和 Git 暂存字节一致性已检查；这些检查不替代后续 VM 验证。

## 证据保存与代码范围

源码、研究脚本、实验宿主源码和文档纳入 Git。系统二进制、IDA 数据库、便携工具、符号、测试阅读器/PDF、原始日志和内存转储留在本地并忽略，避免把大型文件和含用户信息的证据混入代码。

本地关键记录包括 `research/evidence/native-experiment-summary.json`、`framework-msbuild-final-summary.json`、`iso-framework-inspection.json`、`alternate-host-results.json`、`ucpd-policy-detail/policy-data.json` 和 Defender 事件报告。`registry-write-trace-automated.log` 含用户 SID，分享前须脱敏。完整过程见 [研究主记录](windows11-default-association-research.md)。
