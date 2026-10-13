# PDF 首次关联：虚拟机实验说明

本轮用于定位“新用户从未打开 PDF 时，图标已更新但双击仍要求选择应用”的问题。
第一轮用户实测反馈：选项 2 有效、选项 3 无效、选项 4 有效。
新版 `vm-first-use-v2` 已将选项 2 纳入默认流程，默认不增加同步通知与等待。
此前的 `vm-first-use-v3-review` 增加跨分钟哈希重算与 Windows Unicode 转小写。
`vm-first-use-v4-architecture` 将主发布程序改为 x86，用同一个 EXE 测试 32 位和 64 位 Windows。
当前包标识为 `vm-first-use-v5-browser`，增加协议和已安装浏览器批量设置；原有 PDF BAT 的用法保持不变。
日志中 `process_bits=32` 表示程序位数，`native_architecture=0` 表示 x86 系统、`9` 表示 x64 系统。
请分别回归 32 位 Windows 和 x64/WOW64；本机 x64 上的自动测试不能证明 32 位虚拟机也通过。
当次系统的精确版本及逐版本回归结果尚待收集，不能据此认定所有旧版系统都已修复。

## 文件与准备

测试包包含 `DefaultFileAssociation.exe`、中文 `register_docbox_pdf.bat`、中文 `test_pdf_default.bat` 和本说明。
把它们放到虚拟机中原来含有 `docbox.exe` 的文件夹，保留阅读器所需的 DLL 和其他文件。
普通权限运行 BAT；PDF 图标仍使用从原阅读器确认的索引 `1`，ProgID 为 `DocBox.pdf`。

首先建立一个快照：已进入桌面，但从未双击 PDF、从未在系统界面为 PDF 选择默认应用。
**每个实验都从这个快照恢复，不能直接在前一个实验上继续运行另一个实验。**
仅删除 UserChoice 不能认为恢复了干净状态，因为系统可能已经保存其他首次确认记录。

## 本轮建议：只验证新版默认方案

不需要把四组全部重测。恢复上述干净快照后，运行 `register_docbox_pdf.bat`。
它不传实验参数，直接使用默认的确认记录补齐流程，并记录设置前后与第一次双击后的状态。
若出现选择列表，请先不要选择应用，返回 BAT 按键采集日志。
把生成的 `dfa-default-*.log` 和首次双击表现返回即可。

之后再用同一个流程分别回归 1703、1709、1803、1809、1903、1909、2004；
1607 和 20H2 可用于确认原本正常的版本没有受到影响。

## 四组实验

| BAT 选项 | 参数 | 改动 |
| --- | --- | --- |
| 1 | `--experiment=none` | 原方案，加诊断日志 |
| 2 | `--experiment=toasts` | 与新版默认方案相同：给目标 ProgID，以及当前扩展名 OpenWith 索引中登记的候选应用，写入 ApplicationAssociationToasts DWORD 0 |
| 3 | `--experiment=notify` | 注册后同步发送关联通知并等待 1 秒，再执行原 UserChoice 流程 |
| 4 | `--experiment=toasts-notify` | 同时应用选项 2 和 3 |

选项 2 只涉及当前扩展名的确认记录，不禁用全局打开方式提示，也不改其他扩展名的记录。
选项 3 不重启 Explorer、不提权，也不自动打开 PDF。四组均保留原有 HTA 回退和最终关联校验。

## 每组操作

1. 恢复干净快照，双击 BAT，选择实验编号。
2. 脚本执行设置，保存设置前的注册表快照、结果及设置后的注册表/Shell 快照。
3. 按脚本提示去资源管理器双击一个真实 PDF。若弹出选择列表，**先不要选择应用**，保持列表打开，回 BAT 按键采集状态。
4. 如果出现列表，再选择 Edge 等任意应用并勾选“始终使用”，确认打开一次。若已直接打开，则无需人为改默认应用。
5. 回 BAT 按键，脚本采集该阶段状态，再以相同实验参数设置一次。
6. 再双击 PDF，记录表现；回 BAT 按键采集最终状态。

脚本不替你选择应用。请同时报告第一次和第二次双击是否弹出列表、是否真正打开了阅读器。
返回的 `.log` 文件位于 BAT 同目录，名称包含实验参数和随机编号，每次运行新建一份。
日志内阶段标记便于区分第一次设置、第一次双击、人工选择和第二次设置。

## 日志内容与判读

- 工具标识 `vm-first-use-v3-review`、UTC 时间、实际命令行、系统 build/UBR、进程位数、用户 SID。
- 当前扩展名的 FileExts、UserChoice、OpenWith 记录；目标 ProgID 的图标和打开命令。
- UserChoice 最后写入时间、所有者/DACL，以及按当前键时间重算的 Hash 是否匹配。
- 当前扩展名的 ApplicationAssociationToasts、Shell Associations 的初始化记录。
- 独立 Shell 查询的有效 ProgID、打开命令及 HRESULT；设置操作退出码与 HTA 是否被尝试。

`--diagnose` 只采集信息，返回 0 仅表示采集完成，不代表默认关联生效。
最终通过标准仍是：首次双击 PDF 直接用目标阅读器打开，不出现选择列表。
日志包含账户 SID 和文件路径，便于核对是否在同一用户、同一路径上测试。

设置前的快照不调用 Shell 查询，以减少对首次初始化的干扰。
设置后的查询可能影响 Shell 缓存；如果选项 1 加日志后不再复现，需要恢复快照，先执行下面的无日志基线命令，双击后再单独诊断。

```bat
DefaultFileAssociation.exe --ext=.pdf --exe-path="%CD%\docbox.exe" --prog-id=DocBox.pdf --icon-index=1 --experiment=none
DefaultFileAssociation.exe --diagnose --ext=.pdf --prog-id=DocBox.pdf --log-file=after-first-open.log
```

也可随时只采集一份快照，日志会追加，不覆盖此前内容：

```bat
DefaultFileAssociation.exe --diagnose --ext=.pdf --prog-id=DocBox.pdf --log-file=manual-snapshot.log
```

## 本地自动测试范围

新增用例覆盖实验参数校验、诊断只读模式、默认方案不写确认记录、目标与现有候选确认记录类型/值、
其他扩展名保持原值，以及 UserChoice 缺失/有效/被篡改时的日志判读。
注册表测试使用私有进程内沙箱，不等同于旧版 Windows 干净用户配置的实际双击测试。
