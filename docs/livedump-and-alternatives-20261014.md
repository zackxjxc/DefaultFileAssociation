# 本机内核转储与其他默认关联方案

研究日期：2026-10-14。本机 26H2 build 26300.9550，Defender 实时保护启用，UCPD 为 Running。用户另报告虚拟机测试包菜单 2 设置成功；该反馈尚未附带 VM 构建号与结果日志，单独记为用户测试结果。

本次确认所提供的实时内核转储可以读取 UCPD 的实际策略，验证公开 MakeDefault 入口在枚举出的处理器上为空实现，并新增 msiexec 宿主的有效切换测试。完全在自有进程中静默写入的方案仍未找到。

## 转储是否有效

文件 `C:\Users\ZACK\Desktop\livedump.DMP` 为 64 位 Kernel Bitmap Dump，大小 3,531,366,400 字节，文件签名 PAGEDU64。调试器读取的采集时间为 2026-10-14 00:54:30.089 UTC+8，内核构建 26300，16 个处理器，运行时间约四天。

Dump HeaderSize 为 0x333000，保存 0xD2493 个 4096 字节页；两者相加与文件末尾精确一致，没有发现尾部截断。SHA-256 为 `23f18867cb442e1c58a0a7dae8ebd088ffcf70ca487f82fcd9720e6501296815`。0x161 是此次实时转储的代码，参数含 Taskmgr；微软说明此代码仅标识实时转储，不能用于真实崩溃。[LIVE_SYSTEM_DUMP 官方说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0x161--live-system-dump)

KD 成功读取 UCPD 模块、全局变量、非分页池策略对象、名单数组与全部 PDF 名称。NT 内核公共符号未加载成功，但这些有静态结构依据的原始内存读取不依赖 NT 私有类型。此结论针对所需 UCPD 数据；不表示所有内核/用户态页均已保存，也不表示转储记录了某次关联写入的调用栈。

## 实际加载的 PDF 策略

UCPD 加载基址为 `0xfffff8006a800000`，映像大小 0x31000，PE /Brepro 标识 0x1B9D0D83 与已分析驱动一致。对应磁盘副本 SHA-256 为 `1b9b4998342d51e9f350e5f87c52183ea096f8e6d226bada879493a67cf4ddc7`；这里未把运行中已重定位/热修补的完整映像直接与磁盘字节求同一 Hash。

定位链为该驱动 RVA 0x257C8 的 Context 全局指针，经 Context+0x40 取得策略对象。PDF 类别为 1；拒绝描述位于 policy+0x60，允许描述位于 policy+0x1C8。每项描述为计数、指针及动态分配标志，布局由匹配版本反编译确认。

| 字段 | 快照中的值 | 含义 |
| --- | --- | --- |
| Context | `0xffff870729d87010` | 有效运行时对象 |
| Policy | `0xffff870729d86930` | 可完整读取名单描述 |
| PDF 拒绝计数 | 14 | 全部字符串页可读 |
| PDF 拒绝指针 | `UCPD+0x25450` | 使用驱动默认数组 |
| PDF 拒绝动态标志 | 0 | 没有使用动态分配的替换数组 |
| PDF 允许计数/指针/动态标志 | 0 / 0 / 0 | 允许关卡在空列表时通过 |

实际拒绝模式均为 `*\程序名.exe`：dllhost、reg、rundll32、powershell、regedit、wscript、cscript、cmd、InfDefaultInstall、pwsh、wmiprvse、regini、bssafe、mshta。

MSBuild、msiexec、InstallUtil、Regsvr32、ODBCConf 均不在这 14 项中。允许列表为空也不等于所有程序均能写入：当前来源检查还组合签名/信任、拒绝名称和受功能控制的 OriginalFilename 检查。此前自有 EXE/Python 直接写入失败依然成立。

15 个类别的允许计数在此快照中都为零。FeatureV2 原始值为 0xB36DE3EE，FeatureV3 为 0x7E；OriginalFilename 检查的 0x200000 位启用，附加门控全局 byte_259B0 为 0。位值只能解释静态条件，不能代替实际写入时的分支跟踪。

快照证明采集时名单状态，不能证明过去/将来不会更新。动态加载入口仍存在；使用默认数组不证明更新渠道从未执行，也没有确定名单由哪个系统组件下发。

## MakeDefault 的实测结果

通过公共 SHAssocEnumHandlers 枚举 .pdf 的处理器，DocBox、Edge、Chrome 等枚举对象的 IAssocHandler vtable 第 7 槽，即 MakeDefault，均指向本机 windows.storage.dll RVA 0x2974B0。对应机器码是 `xor eax,eax; ret`，直接返回 S_OK。

自有 x64 EXE 实际调用 DocBox 处理器的 MakeDefault 返回 S_OK；旧 UserChoice、新 UserChoiceLatest 与 ProgId 子键的全部值和最后写入时间均未改变，独立 Hash/Shell 检查仍为原来的 docboxtest.pdf。调用前已经是该目标；结合空实现可确认此枚举对象没有执行设置，不能仅凭 S_OK 宣称切换成功。

这只排除当前枚举实现上的此入口，不推断所有 Windows 版本或私有接口都无效。系统 OpenWith 界面的内部 MakeDefault 工作函数是另一条调用链。

微软仍公开该方法的签名与 S_OK 返回语义，但该文档未证明当前实现有实际写入动作。[MakeDefault 官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iassochandler-makedefault)

源码 `research/native-probes/assoc_handler_probe.cpp` 默认需显式 --list；实际调用使用 --make-default 加已注册处理器完整路径。启用 DFA_BUILD_HOST_PROBES 的研究构建提供 PdfAssociationApiProbe，不影响默认发布构建。

## 新宿主与更强的切换测试

新增候选为系统 msiexec.exe：其公开的 /y module 参数调用 DLL 的 DllRegisterServer。候选来自微软记录的 DLL 注册能力，再与本机实际策略、文件存在性和回调测试对照；没有改文件名、注册服务或安装 MSI 产品。[Windows Installer 官方命令说明](https://learn.microsoft.com/en-us/windows/win32/msi/command-line-options)

| 宿主 | 只读回调 | 不同 ProgID 切换 | 目标值写入数 | 独立 Hash 和 Shell |
| --- | --- | --- | --- | --- |
| msiexec.exe | 通过 | 通过 | 2 | 通过 |
| Framework64 InstallUtil.exe | 通过 | 通过 | 2 | 通过 |
| regsvr32.exe | 通过 | 通过 | 2 | 通过 |
| odbcconf.exe | 通过 | 通过 | 2 | 通过 |

每个写入阶段之前，使用已验证 MSBuild 把默认 ProgID 切换到唯一的临时控制 ProgID，仍指向同一 DocBox。之后由候选宿主写回 docboxtest.pdf；回调记录实际宿主 EXE、完成两次写入，外部 EXE 独立验证 Hash、有效 ProgID 与打开命令。MSBuild 只用于建立对照和恢复，候选的目标写入不经 MSBuild。

因此本次加强了“确实能从其他 ProgID 切换”的证据，但没有测试不同阅读器 EXE、首次用户、重启持久性或每个宿主下的真实 PDF 打开。四次切换均保留同一阅读器路径，不把它描述为四次不同阅读器的完整首次设置。

正常权限下执行，Defender 与 UCPD 保持启用。测试时段的 Defender 1116/1117 查询成功，未发现新增检测事件；这是该时段观察，不承诺其他系统或未来规则不告警。结束时 docboxtest.pdf 的 Hash/Shell 验证通过，唯一临时注册项已清理，原有阅读器注册未改写。

复现脚本 `research/scripts/run_host_switch_tests.py` 需显式 --only 选择宿主，顺序执行、只读阶段失败跳过写入、发现防护事件停止并恢复。HTA/工作流编译器仍按先前结论排除。

## 宿主输入与动态配置设计

五种方案都由宿主加载并调用我们的代码，但输入形式和编译时机不同：

| 宿主 | 提供给宿主的内容 | 编译时机 | 我们的执行入口 |
| --- | --- | --- | --- |
| 当前 MSBuild 实现 | XML 项目中内嵌的 C# 源码 | 运行时编译成托管程序集 | Task.Execute |
| InstallUtil | C#/.NET 托管 DLL | 开发阶段预先编译 | Installer.Install 或 Uninstall；当前实验使用 Uninstall |
| Regsvr32 | C++ 原生 DLL | 开发阶段预先编译 | DllRegisterServer |
| ODBCConf | 同一个 C++ 原生 DLL与 REGSVR 配置指令 | 开发阶段预先编译 | DllRegisterServer |
| msiexec | 同一个 C++ 原生 DLL | 开发阶段预先编译 | DllRegisterServer |

MSBuild 当前执行路径是 C# 源码到 .NET 程序集，再由 CLR 加载和 JIT 执行；不是 C/C++ 编译 OBJ、链接成新 EXE 再启动。后四种都加载 DLL，但 InstallUtil 的托管 DLL与后三种的原生 DLL不是同一种构建产物。后三种可以共用现有原生研究 DLL。

业务逻辑可以保持在固定 DLL 中，每次操作参数由本地 JSON 动态提供。以下是拟议配置格式，尚未实现：

```json
{
  "extension": ".pdf",
  "progId": "docboxtest.pdf",
  "readerPath": "C:\\DFA Test\\DocBox.exe",
  "iconIndex": 1
}
```

拟议流程为：我们的 EXE 根据用户选择生成独立临时 JSON，通过子进程环境变量传递配置文件路径；宿主加载 DLL并调用约定入口；DLL读取并校验配置，执行设置并输出报告；我们的 EXE 独立检查有效关联后清理临时文件。环境变量适合 DllRegisterServer 这种没有自定义参数的入口，避免把配置直接拼入宿主命令行。

当前 MSBuild 已动态接收 ProgID、操作模式和报告路径。其他宿主的研究 DLL 仍固定使用 docboxtest.pdf，尚未实现 JSON 参数化；本节只记录设计结论，没有修改实现。系统 DLL 指纹、内部函数地址和 HashVersion 支持范围仍由版本适配代码管理，增加配置文件不自动扩大 Windows 兼容范围。

代码需要在宿主进程内实际执行。仅让 MSBuild 或另一个宿主启动我们的普通 EXE，写入进程身份仍是该 EXE，不能把父进程身份当作可继承的放行授权。

## 证据与后续方向

原始内存、名单解析和 KD 日志保存在忽略目录 `research/evidence/livedump-20261014`；切换报告为 `research/evidence/host-probes/20261014-064912-32ad4e2c-switch-summary.json`。DMP 保留在桌面，没有上传或纳入 Git。

优先把 msiexec 与其他候选带到相同 VM 做首次设置、不同阅读器切换、真实打开和重启测试，再决定产品后端。自有进程方向继续观察系统正常确认动作的实际 writer 和调用栈，审查可调用授权入口；本次没有发现可转移授权。无需为离线转储分析启用本机内核调试或安装 REA。
