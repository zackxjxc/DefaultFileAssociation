# 原生 EXE 与 Windows 正常默认关联流程

研究日期：2026-10-14；延续 2026-10-13 的本机实验。此次只在后台读取文件、注册信息和已有证据，并使用 IDA/idalib 对项目内系统文件副本做静态分析；未操作界面、启动 OpenWith/设置页面或更改 PDF 默认关联。

## 当前结论

已定位 Windows 默认应用设置组件、单扩展名选择界面的激活路径、PDF 快捷设置的内部写入函数，以及 UCPD 一个受功能状态选择的来源检查分支。尚未完成正常用户确认过程的运行时跟踪，因此不能把静态调用关系表述为本机实际点击后的完整调用栈。

没有发现可让自有 EXE 取得通用直接写入许可的公开接口或可转移授权令牌。已分析的 UCPD 分支以实际执行写入的当前进程为信任检查对象；把系统 DLL 的写入函数放进自有 EXE 中调用，不会自动把系统进程的信任身份转移给该 EXE。这与先前原生写入失败、受信任宿主写入成功的对照一致，但不是证明任何原生方案都不可能。

原生 EXE 可以完成阅读器注册、引导用户进入系统默认应用页面，以及读取并验证结果。完全由自有 EXE 静默写入新记录的方案仍未找到；调用系统的正常用户确认流程与完全自身执行写入是不同的产品能力。

## 已分析的本机组件

| 文件 | 文件版本 | SHA-256 |
| --- | --- | --- |
| SystemSettings.exe | 10.0.26100.8875 | `c514b73f3b9cc38be1dc817c177d07ecf12ce4ce9e21df5d54128e7359198bba` |
| SystemSettings.dll | 10.0.26100.9549 | `129f124fe001c75855e7f3ed5038f98f0bc0662362496c6b2724469dd65f4a16` |
| Windows.UI.AppDefaults.dll | 10.0.26100.8875 | `50e5d587cb53a33fd1df578fe5d8e6b371e11fb139fe8e5fc5e7a3b6688e7314` |
| OpenWith.exe | 本轮以指纹区分 | `313f821ce8a0206d058015efb0dbaae7e8f616cd0f1c1a38401a2d8bbb9e8d19` |
| Windows.Internal.OpenWithHost_WinUI3.dll | 10.0.26100.9549 | `eafd3b32d36a0872ad55576dc13c2d43f7a707a63c6710170c8ee12ebd3b871a` |
| windows.storage.dll | 10.0.26100.9278 | `8d6ac2b451af71e913665c8459935ac862e219458f0962c99f3a3d48ab9c251b` |
| UCPD.sys | 4.8.0.702351 | `1b9b4998342d51e9f350e5f87c52183ea096f8e6d226bada879493a67cf4ddc7` |

设置 EXE、设置 DLL、Windows.UI.AppDefaults.dll、windows.storage.dll 的 Authenticode 检查有效，签名主体为 Microsoft Windows；WinUI3 选择组件签名检查也有效。文件版本不同于 OS 构建号是分别记录的信息，不据此推断组件损坏。

SettingsHandlers_nt.dll 的初步分析只找到一个 `\DefaultApps` 相关字符串，没有据此确认它是 PDF 设置核心。实际默认应用实现是在 Windows.UI.AppDefaults.dll 中找到的。SystemSettings.dll 的完整 IDA 初始分析受符号获取阻塞，本轮不以未完成导出作为证据；已完成的主要组件足以定位以下路径。

## 正常流程不是一条固定写入链

### 单个文件扩展名的选择入口

Windows.UI.AppDefaults.dll 的公共符号包含 `AssociationSetting::Invoke`。该函数把工作放入任务池，工作函数读取当前关联标识并调用 `SHExtCoCreateLocalServerFromRegKey`。

后者明确读取 `Software\Microsoft\Windows\CurrentVersion\OpenWith` 中的 `OpenWithLauncher`，按本地 COM 服务器激活。本机只读查询结果：

- HKCU 对应配置键不存在。
- HKLM 的 OpenWithLauncher 为 `{e44e9428-bdbc-4987-a099-40dc8fd255e7}`。
- 此 CLSID 的 LocalServer32 为 `C:\WINDOWS\system32\OpenWith.exe`。

调用方允许服务器取得前台窗口，然后调用启动选择界面的接口。OpenWith.exe 的 `COpenWithLauncher::Launch` 再调用 `_LaunchImmersiveOpenWith`；其界面实现还有 WinUI3/旧实现的功能分支。本机 WinUI3 类注册在 Windows.Internal.OpenWithHost_WinUI3.dll，旧类注册在 Windows.Internal.OpenWithHost.dll，均是加载进服务器的进程内组件。此处没有实际激活 COM 对象，也没有测试任何私有接口。

静态关系可以表示为：

```text
设置页面中的扩展名设置项
  -> Windows.UI.AppDefaults.dll / AssociationSetting::Invoke
  -> 后台工作函数
  -> OpenWithLauncher 本地 COM 服务器
  -> OpenWith.exe / COpenWithLauncher::Launch
  -> 系统选择界面（例如 WinUI3 组件）
  -> InvokeSelected / MakeDefault
  -> SetDefaultAssociationForAssocHandler
  -> UserAssocSet 等处理器相关分支
  -> 新格式 Hash 和注册表写入
```

WinUI3 组件的 MakeDefault 调用 SetDefaultAssociationForAssocHandler，后者根据处理器类型选择 SetDefaultAssociation 或 UserAssocSet；后一条路径经 SetUserAssoc、_SetUserChoiceAndHash 进入新格式 SetProgIdAndHash。已得到函数间静态调用证据，尚未观察实际点击时启用了哪个界面分支、目标处理器选择了哪个写入分支，以及实际 PID。

OpenWith 的 DetectCaller 会读取 COM 客户端进程和映像名称；在已分析的 Launch 中这些数据进入遥测，不能仅凭函数名宣称它实现了调用方签名授权。另一个 `_AllowSetDefault` 涉及 `NoOpenWith` 等关联设置，不是 UCPD 的写入信任检查。

### PDF 快捷设置的内部写入链

同一 DLL 中存在 `AppDefaultApplication::SetDefaultBrowserPDFExt`，取得处理器 ProgID 后调用内部 UserAssocSet。已确认以下静态链：

```text
AppDefaultApplication::SetDefaultBrowserPDFExt
  -> UserAssocSet
  -> SetUserAssoc
  -> HasProtectedUserChoice
  -> _SetUserChoiceAndHash
  -> SetProgIdAndHash（新格式功能分支）
  -> SHRegSetString / 注册表写入
```

新格式分支写 `UserChoiceLatest\ProgId` 中的 ProgId，读其时间，调用 HashAssociationRotation，再写 `UserChoiceLatest/Hash`。函数内部没有找到向外部 EXE 颁发 UCPD 通用许可的步骤。上述是 PDF 快捷动作，不能直接当作普通 PDF 设置项对话框的全部后续实现。

windows.storage.dll 中也有同源 UserAssoc 写入代码；本机 `Windows.Internal.ApplicationDefaults.AppDefaultHelpers` 注册的 DllPath 为 windows.storage.dll，ActivationType 为 0（进程内）。其 HasProtectedUserChoice 使用的工厂方法用于查询保护状态，不能视作“取得写入授权”。

## 写入授权目前如何解释

UCPD 本机副本的 RVA 0xA30C 根据功能条件分派，其中 RVA 0xAA0C 调用 `PsGetCurrentProcessId()`，把当前进程交给 RVA 0x1667C 的来源检查，规则类别为 PDF。检查失败可返回 STATUS_ACCESS_DENIED；通过后还有条件性进一步检查。

RVA 0x1667C 组合映像信任、允许名单、拒绝名单、OriginalFilename 等条件。这说明至少在该分支中，驱动不是只核对注册表句柄权限，也不是只看 Hash，或只看执行代码所在的 DLL 是否微软签名。

系统设置和系统选择界面可以在 Windows 受信任组件中执行正常操作；已有静态证据支持“实际写入进程的信任条件影响结果”这个解释。仍需运行时捕获单扩展名确认后的实际 writer、UCPD 配置和返回分支，才能确认每条正常流程的具体放行原因。

`SHSetProtectedValue` 和 `CShellProtectedRegLock` 的名字容易误导。已导出的实现操作旧 `UserChoice`，通过旧结构的锁定/解锁辅助设置值；没有证据表明它们提供可跨过新 UCPD 检查的授权票据。此次未调用这些写入/权限修改函数。

## 公共接口与自有 EXE 的能力边界

| 方式 | 判断 |
| --- | --- |
| 自有 EXE 重现 Hash 并直接写入 | 已在上一阶段拒绝；这轮来源检查支持进程信任仍是障碍 |
| 在自有 EXE 中调用系统内部同源写入函数 | 没有证明能通过；进程内调用本身不改变实际进程身份 |
| IApplicationAssociationRegistration::SetAppAsDefault | 微软说明 Windows 8 起该接口仅 QueryCurrentDefault 功能受支持，不能作为现代系统设置方案 |
| SHOpenWithDialog 的旧注册默认标志 | 微软说明 Windows 10 起相关标志被忽略，不能靠它改变扩展名默认程序 |
| IAssocHandler::MakeDefault | 官方有此方法；这轮没有在自有 EXE 中调用，也未证明其在当前保护策略下可设置新格式，保留为待验证候选 |
| 注册应用并打开 ms-settings:defaultapps 的应用页面 | 官方入口；由用户在系统流程中选择，避免研究宿主执行，但不是完全由自有 EXE 静默写入 |
| 购买普通商业签名、改 EXE 名称 | 没有证明能满足本机来源策略，不当作解法 |

来源：[IApplicationAssociationRegistration](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iapplicationassociationregistration)、[SHOpenWithDialog](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shopenwithdialog)、[IAssocHandler::MakeDefault](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iassochandler-makedefault)、[默认应用设置页面入口](https://learn.microsoft.com/en-us/windows/uwp/launch-resume/launch-default-apps-settings)。

## 后续研究方向

1. 在正常保护配置的 VM 中先采集 [内核内存快照](ucpd-vm-memory-capture-plan.md)，取得已加载策略，验证静态解释。
2. 在 VM 记录一次用户通过系统界面确认 PDF 默认程序的完整操作，关联注册表写入的 PID、用户态调用栈和组件版本。此步骤尚未执行，需要用户操作或后续允许的 VM 界面测试，不在当前物理机执行。
3. 如需要，再用内核调试确认对应写入的进程身份、信任结果与实际分支。只观察，保持独立的正常启动对照；不通过修改返回值制造成功。
4. 若要求完全静默且严格由自有进程执行，则继续审查是否存在受支持的授权入口，包括先静态审查 IAssocHandler::MakeDefault 的本机实现，再在 VM 单独验证实际写入、Hash/Shell 和是否出现 UI/告警。接口返回成功不代替有效关联检查；目前没有找到可直接使用的解法，也不承诺私有函数移植即可解决。

本轮不增加新宿主、不进行 DLL 注入或进程身份伪装、不关闭保护；未执行关联设置。研究目标是定位授权边界，而不是把系统界面调用包装成已实现原生静默设置。

## 复现与证据

本地 `research/evidence/normal-flow/` 保存模块元数据、COM 注册查询和 IDA 导出：`appdefaults-ui`、`appdefaults-actions`、`association-dialog`、`dialog-activation`、`openwith-server`、`openwith-authorization`、`openwith-ui-host`、`openwith-final-write`、`ucpd-authorization`、`storage-entry`、`storage-gates` 等。所有地址只适用于对应文件指纹。

分析脚本 `research/scripts/ida_export.py` 新增 `--string-pattern` 用于选择相关字符串；导出时缩短过长的 C++ 装饰名并在 JSON 中保存 export_basename，避免 Windows 路径长度限制。已完成的导出用于实际验证这两个改动。

使用示例（只分析项目副本，不执行目标程序）：

```powershell
& .\research\tools\ida-env\Scripts\python.exe .\research\scripts\ida_export.py .\research\binaries\Windows.UI.AppDefaults.dll .\research\evidence\normal-flow\appdefaults-ui --string-pattern 'SetDefault|UserChoice|ApplicationDefaults'
```

IDA 取得 Windows.UI.AppDefaults/OpenWith/WinUI3 选择组件的微软公共 PDB，符号名有助于还原上述函数；没有完整私有类型，部分反编译参数仍不可靠，不能拿伪代码直接当作调用 ABI。WinUI3 的符号通过微软 HTTPS 符号服务器取得并缓存；SystemSettings 符号获取失败，已停止相关可选分析，没有把失败当作不存在功能。Lumina 保持本地拒绝连接设置，不上传系统文件。系统副本、PDB、IDA 数据库和原始导出保持本地忽略，不纳入 Git。

结束时运行原生 `--latest-native-probe --check`，只读 Hash/Shell 检查返回 0、completed_registry_writes=0，PDF 仍为 docboxtest.pdf；结果在 `final-native-readonly-check.log`。本轮分析脚本 AST、文档链接、编码/换行和 Git diff 检查均通过，未修改产品设置代码。
