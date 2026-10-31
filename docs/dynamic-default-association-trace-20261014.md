# Windows PDF 默认关联的动态调试记录

2026-10-14，在用户授权的本机上跟踪正常系统界面。已经实际命中设置入口、后台任务和 OpenWith 激活辅助函数，并执行了单步指令。确认按钮后的写入链尚未捕获；目前没有发现可供自有 EXE 使用的原生静默授权入口。

## 已经运行到的代码

使用现有 Windows SDK 的 x64 CDB（WinDbg 调试引擎），以普通用户附加 `SystemSettings.exe`，PID 为 15408。点击系统设置中 PDF 的当前打开应用，依次命中以下断点：

| 模块与 RVA | 对应静态函数 | 运行时证据 |
| --- | --- | --- |
| Windows.UI.AppDefaults.dll + `0x2C350` | `AssociationSetting::Invoke` | 调试器线程索引 `0:002` 命中，调用栈来自 SystemSettingsViewModel 和 Windows.UI.Xaml |
| 同模块 + `0x292DC` | 负责关联操作的后台 lambda | 线程索引切到 `0:043`，调用栈来自 SHCORE 工作线程；对象字段读出 `samplepdfreader.pdf` 和 `.pdf` |
| 同模块 + `0x2CFE4` | `SHExtCoCreateLocalServerFromRegKey` | 在同一工作线程命中，调用者返回位置为模块 + `0x295C5`；恢复执行后出现 OpenWith 选择框 |

在第一个断点执行 `p` 后，RIP 从函数起点推进 5 字节，到下一条 `push rbp`。因此这是实际暂停和单步执行，不只是进程活动监控。CDB 缺少该模块的完整符号时显示 `GetSetting+偏移`；上述函数名来自同一 SHA-256 文件的 IDA 分析，不能照抄 CDB 最近导出符号作为真实函数名。

这段动态证据覆盖了设置入口和后台分派。注册表中的 OpenWithLauncher 配置、COM CLSID 和 WinUI3 内部调用关系，仍参考[正常流程静态分析](native-default-association-flow.md)。尚未通过正常确认抓到实际写入 PID、调用栈及 UCPD 的放行分支。

## 确认按钮处的限制

设置页面和文件资源管理器的“打开方式 → 选择其他应用”都能打开 WinUI3 选择框。该框由 `OpenWith.exe` 显示；本轮观察到其进程没有 MainWindowHandle 或窗口标题，Computer Use 的窗口列表也没有提供可单独操作的选择框对象。

父窗口截图能看到选择框，但向父窗口发送输入会激活父窗口并关闭选择框。设置入口和资源管理器入口均出现这一现象，未能完成“设为默认”或“始终”的正常确认。这是本轮自动界面操作的限制，不能解释为 Windows 拒绝了正常设置，也不能视为原生方案失败。

已在 OpenWith 进程设置内部 `OpenWithModel::MakeDefault`（WinUI3 模块 + `0x5AA5C`）和 `ntdll!NtSetValueKey` 断点；未观察到用户确认触发这两个断点。这里的内部 MakeDefault 与[公共 IAssocHandler::MakeDefault 的空实现](livedump-and-alternatives-20261014.md#makedefault-的实测结果)是两个不同函数。

后续只需在调试器准备好后，由可操作该选择框的界面能力或用户手动完成一次确认，即可继续观察，无需重做已完成的入口分析。

## 系统更新对旧测试包的影响

本机 OS 构建号已从此前的 `26300.9550` 更新到 `26300.9711`。`windows.storage.dll` 也已变化：

| 项目 | 此前本机文件 | 当前本机文件 |
| --- | --- | --- |
| SHA-256 | `8d6ac2b451af71e913665c8459935ac862e219458f0962c99f3a3d48ab9c251b` | `a0d41fcfd66f4fbba8dbb84776c7d344f66969ce8b6edac19b3ac1dd737a628a` |
| Machine ID 辅助函数 RVA | `0x6289FC` | `0x62902C` |
| Hash 函数 RVA | `0x628D98` | `0x6293C8` |

通过 PE 异常表确认候选函数边界，函数长度分别仍为 193 和 641 字节。Capstone 对照显示，在掩去 RIP 相对地址和指向函数外部的分支目标后，两组函数的机器码一致；此比较不保证被引用函数及数据也完全相同。IDA 对当前副本的反编译也对应 Machine ID 查询和原有七参数 Hash 计算结构。

在独立 Python 进程中，只读调用当前指纹下的两个函数：固定向量仍得到 `9rTXfFYM9CQ=`；本机 HashVersion 为 1，现有 `.pdf` 记录计算值与存储值均为 `9tHM7P3w54o=`。这些结果支持“本次至少发生了函数位置移动”，没有证据说明这次更新更换了 Hash 算法。

旧测试包不包含当前指纹，`--latest-native-probe --check` 在 `dll_fingerprint` 阶段拒绝继续，错误 1306，完成写入数为 0。该错误表示版本不匹配，不表示当前默认关联或 Hash 已损坏。**本轮没有把新地址加入产品的 MSBuild 后端或重新发布测试包。**

Windows.UI.AppDefaults.dll、OpenWith.exe、WinUI3 选择组件和 UCPD.sys 的文件指纹与此前分析一致。驱动文件相同不能代替当前运行时策略快照，也不能据此宣称运行中的名单未变化。

## 继续捕获写入的方法

观察用命令文件为 [openwith-ui-trace.dbg](../research/scripts/openwith-ui-trace.dbg)。附加前必须核对本机 WinUI3 文件 SHA-256 等于脚本注释中的值；`bp` 成功解析只说明断点已安装，不能代替命中证据。

```powershell
# 将 1234 换为当次正常系统选择框的 OpenWith PID。
& 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe' `
  -pd -G -p 1234 `
  -y 'E:\Work\DefaultFileAssociation\research\symbols' `
  -logo 'E:\Work\DefaultFileAssociation\out\openwith-trace.log' `
  -cf 'E:\Work\DefaultFileAssociation\research\scripts\openwith-ui-trace.dbg'
```

脚本先停在内部 MakeDefault，继续后筛选值名为 `ProgId` 或 `Hash` 的 NtSetValueKey 调用，打印数据、键句柄信息和调用栈。这些名称也可能出现在其他键，必须用句柄路径确认目标确实属于 `.pdf\UserChoiceLatest`。脚本不修改参数、返回值或注册表内容。

在目标 NtSetValueKey 入口记录调用栈后，用 `gu` 执行到函数返回，再检查 `eax` 的 NTSTATUS，才知道该次写入结果。继续观察 ProgId 和 Hash 两次写入，最后对照独立 Hash 与 Shell 查询；不能只凭到达 API 入口宣称成功。

用户态调试可确认写入进程、参数、调用栈和系统调用返回状态。若要确定 UCPD 内部实际走过哪条信任判断分支，仍需有匹配符号/地址的内核调试或适用的内核状态证据，不能从用户态栈直接读出完整内核授权过程。

结束调试时可用 `bc *`、`.detach`、`q` 解除断点并恢复目标运行；本轮所有 CDB 会话已结束，SystemSettings 已脱离调试器，没有留下暂停的系统设置进程。

## 复现文件与最终状态

当前原始证据位于本地忽略目录 `research/evidence/dynamic-flow-20261014-205049/`：`baseline.json`、`settings-live.log`、各次 OpenWith 日志、`storage-9711/`、`storage-compare/comparison.json`、`hash-check-9711.json` 和 `final-shell-diagnostics.log`。原始日志可能含本机 SID、句柄及路径，不纳入 Git。

新增 [compare_storage_functions.py](../research/scripts/compare_storage_functions.py) 用于静态机器码对照，[check_latest_9711.py](../research/scripts/check_latest_9711.py) 仅对上述精确指纹做只读 Hash 检查。Capstone 5.0.9 安装在项目已有的 `research/tools/ida-env` 环境内；未安装新驱动、服务或全局调试配置。

```powershell
& .\research\tools\ida-env\Scripts\python.exe .\research\scripts\compare_storage_functions.py `
  .\research\binaries\windows.storage.dll .\research\binaries\windows.storage-26300.9711.dll `
  .\research\evidence\dynamic-flow-20261014-205049\storage-compare `
  --function 0x6289fc:0x62902c --function 0x628d98:0x6293c8

& .\research\tools\ida-env\Scripts\python.exe .\research\scripts\check_latest_9711.py
```

IDA 新文件分析期间获取公共 PDB 的 HTTP 连接被重置；IDA 仍完成了函数识别与反编译，43,751 个函数、2 个目标函数导出。没有把 PDB 获取失败视为不存在这些函数。

结束时只读 Hash 检查退出码为 0，写入数为 0；产品 `--diagnose` 退出码为 0，Shell 有效 ProgID 仍为 `samplepdfreader.pdf`，打开命令仍指向 `test_case\SamplePdfReader.exe`。初始和结束时 PDF 新格式关联值相同；本轮没有完成新的系统默认应用确认。
