# UCPD 已加载策略的虚拟机内存采集计划

状态：2026-10-14 已从用户提供的本机实时内核转储读出实际 PDF 名单；详见 [转储分析结果](livedump-and-alternatives-20261014.md)。虚拟机采集和实时内核调试尚未执行。原计划记录日期：2026-10-13。

## 目的与工具分工

目标是读取虚拟机运行时 UCPD 对 PDF 实际使用的允许/拒绝名单，而不是再次提取磁盘驱动中的默认字符串。

虚拟机用于运行被观察的 Windows；本机用于分析。虚拟机无需安装 Codex、IDA、REA 或额外常驻研究服务。采集脚本可以记录版本与日志，但普通 PowerShell/Python 不具备直接读取任意内核内存的能力。

本机已有 IDA 和 Windows Debugging Tools 中的 KD、KDNET。IDA 用于从目标版本驱动还原结构，WinDbg/KD 用于读取内核转储或连接实时调试。符号应匹配目标 Windows，微软公开符号不保证包含 UCPD 私有结构名。

## 方案 A：正常运行状态下采集内核内存快照

优先候选适用于具有任务管理器实时内核转储功能的 Windows 11。微软记载其正式可用起点为 build 22621.1992；不能假定 Win10 有相同入口。

准备与采集：

1. 创建可恢复的虚拟机快照，使用测试账户，记录 Windows 构建/UBR、UCPD 文件版本和 SHA-256、服务状态、Defender 状态以及采集时间。
2. 保持正常保护和启动配置，让系统进入稳定状态；本阶段不设置 PDF 默认方式，也不启用内核调试。
3. 在虚拟机以管理员权限启动任务管理器，在详细信息中找到 System，选择创建实时内核内存转储文件，并选择完整实时内核转储。仅内核堆栈转储不适合遍历策略数据。
4. 默认转储目录是 `%LocalAppData%\Microsoft\Windows\TaskManager\LiveKernelDumps`，以实际完成提示为准。采集需要空闲内存和磁盘空间；默认无需额外包含用户态或 Hyper-V 内存。
5. 把 `.dmp`、虚拟机的 `C:\Windows\System32\drivers\UCPD.sys` 和版本元数据复制到本机私有研究目录。动态策略注册表只读导出可作为补充，不替代内核内存证据。

微软说明实时内核转储不会故意使系统崩溃；内存压力会影响响应或使采集不完整。转储可能包含用户信息，留在本地分析，不提交 Git。[微软操作文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/task-manager-live-dump)

离线分析步骤：

1. 对目标 UCPD 文件核对指纹并重新定位配置全局变量和相关结构。不同版本不能直接使用本机研究地址。
2. 在转储中确认 UCPD 已加载及其基址，依据该版本代码取得运行时配置指针。
3. 验证指针和计数合理性，找到 PDF 对应规则类别，读取允许/拒绝条目及名称模式。
4. 导出可读名单和定位依据，注明采集时间、驱动指纹、结构来源和缺失内存页情况。

如果转储没有包含相关页、UCPD 未加载或结构无法确认，结果应记为证据不足，不能把读不到名单解释为空。一次快照也不能证明所有时刻配置相同，或证明某次写入的实际执行分支。

## 方案 B：按需建立实时内核调试

内核调试可以暂停目标虚拟机，查看驱动参数、内存和返回路径，然后继续执行，用于确认写入为何被拒绝/放行，以及名单何时改变。

本机运行 WinDbg/KD，虚拟机提供内核调试连接。微软推荐 KDNET 网络连接，也提供虚拟串口方式；需先确认 VMware、Hyper-V 或其他虚拟化平台及网卡支持，再选择具体配置。

KDNET 通常在目标使用 `kdnet.exe` 与 `VerifiedNICList.xml` 检查支持并配置连接，在本机使用匹配端口/密钥连接。启用目标调试需要管理员权限和重启，可能受到 Secure Boot、BitLocker 限制。只调整专用测试 VM，物理机不启用被调试模式、不重启；连接密钥不写入 Git。

来源：[微软内核调试入口](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/setting-up-kernel-mode-debugging-in-windbg--cdb--or-ntsd)、[KDNET 设置](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/setting-up-a-network-debugging-connection-automatically)、[虚拟串口设置](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/attaching-to-a-virtual-machine--kernel-mode-)。

初期仅读取名单和记录分支，不修改驱动内存、判定返回值或保护配置以制造成功。暂停会让整个虚拟机暂时不响应；错误的写内存或运行控制可能导致蓝屏，因此准备快照且不使用日常账户。调试状态可能影响观察结果，须与正常启动下的快照和行为对照。

## 验收及后续用途

- 得到可核对的实际 PDF 名单，或明确记录无法获取的原因，不能只输出猜测。
- 名单、驱动文件和 Windows 构建来自同一次目标采集，地址解释有相同版本静态分析依据。
- 继续区分名单结果、签名条件、关联 Hash 有效性及 Defender 检测。
- 如研究动态更新来源，比较不同时刻的名单/动态数据，收集写入者和更新时间证据；不根据 Defender 更新机制直接推断 UCPD 的下发渠道。
- 如研究原生独立设置，优先对照 Windows 正常设置流程。读出名单是诊断证据，并不自动提供无需宿主的设置能力。

Win10 或不具备该入口的目标需另选转储/调试方式。本机所提供转储的相关 UCPD 内存页和结构已验证可读；其他目标仍需分别确认，不能承诺一份转储一定能导出全部名单。
