# Framework MSBuild 虚拟机测试包

目标：将包内 DocBox.exe 设置为当前用户 PDF 默认阅读器，ProgID 为 docboxtest.pdf，图标序号为 **1**。普通用户执行，不以管理员身份运行。

DefaultFileAssociation.exe 是 x86 入口，64 位 Windows 上自动使用系统 Framework64 MSBuild。DefaultFileAssociation-x64.exe 是相同实现的 x64 构建。任务内容已经嵌入 EXE，不需要 Python、Visual Studio、IDA、Codex 或外部 .proj 文件。系统需要现有 Framework64 MSBuild 与 .NET Framework 构建组件。

## 操作顺序

先给 VM 建快照，保持 Defender 和 UCPD 正常运行。将整个 ZIP 解压到 VM 本地一个固定目录，例如 `C:\DFA Test`，保留阅读器 DLL 和 PDF 样本。

在该目录打开普通用户 PowerShell，先检查兼容性：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test-msbuild-package.ps1 -Mode Preflight
```

预检只读，不注册阅读器、不设置默认关联；通过只代表该模式的检查通过。然后执行设置：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test-msbuild-package.ps1 -Mode Set
```

只读复查：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test-msbuild-package.ps1 -Mode Check
```

也可以直接调用 EXE：

```powershell
.\DefaultFileAssociation.exe --msbuild --ext=.pdf --prog-id=docboxtest.pdf --exe-path="C:\DFA Test\DocBox.exe" --icon-index=1 --log-file="C:\DFA Test\set.log"
```

阅读器路径应替换为实际解压位置。脚本自动使用相邻 DocBox.exe 的绝对路径，支持空格和中文目录，不依赖本机 E: 盘路径。

## 完整测试清单

1. 记录 VM 的 Windows 完整构建/补丁与初始 PDF 默认程序，分别测试首次设置、从其他阅读器切换、已是目标时再次设置。
2. 设置后运行 Check；确认 DefaultIcon 与 ApplicationIcon 都是阅读器路径加 `,1`。
3. 手动双击 012.pdf，确认实际由 DocBox 打开，无首次打开选择提示。可在 Set/Check 后加 `-OpenPdf` 做真实打开测试。
4. 注销/重新登录、重启后再次运行 Check 并双击 PDF。不要在这期间移动解压目录；移动后存储的阅读器绝对路径将失效。
5. 查看 Windows 安全中心保护历史。即使关联成功，出现拦截也要报告，不能把退出码 0 当作可部署结论。

每次执行的 `results/<时间和随机ID>/` 包含 tool.log、console.log 和 report.json，记录前后关联、注册命令、两个图标项、子任务结果以及可读到的 Defender 事件。没有事件或日志查询失败均不能证明不会报毒。预检失败时把该目录带回供分析，不修改保护配置继续尝试。

## 版本限制

当前仅支持 HashVersion=1 和以下两个 windows.storage.dll SHA-256：

- `8d6ac2b451af71e913665c8459935ac862e219458f0962c99f3a3d48ab9c251b`：本机已验证的内部函数版本。
- `cf4793b9e7570f1c6621515d634e2fee115b51f6009a19e0d1a5d24cefb55703`：用户提供的 26H2 ISO 版本，两个函数地址已静态匹配，实际 VM 设置仍待验证。

预检还会用固定输入执行 Hash 自检，并核验已有记录的 Hash。固定自检不能代替实际关联写入和重启后的验证。

未知指纹、未确认新格式迁移、无效的已有 Hash 会在设置注册前拒绝。无效记录可先由用户在 Windows 设置中建立一个正常默认程序，再预检。不会把系统组件副本放进包里，不会更换系统 DLL、改 UCPD/Defender、回退 HTA，也不会自动打开 PDF。

设置不是事务：候选注册成功后，后续默认写入可能失败；报告如实返回非零并保留失败阶段。manifest.json 提供源码提交号和包内文件 SHA-256。测试包是实验交付，不代表已完成跨版本、持久性或全部防护环境验证。

## 本次交付验证

2026-10-14：x86/x64 Release 构建及静态 API 导入检查通过，6 项 CTest 通过。图标测试同时核对 DefaultIcon 和 ApplicationIcon 的序号 1。集成 EXE 在本机通过 Preflight/Check，包括固定 Hash 自检、已有记录 Hash 和 Shell 有效关联校验；子任务均报告 0 次注册表写入。

便携脚本已在 Windows PowerShell 5.1 下预检，包含中文和空格的目录可运行。本次集成版本没有在本机执行 Set，也没有自动打开 PDF；首次切换、图标实际显示、告警和重启持久性由 VM 完整测试确认。
