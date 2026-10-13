# 默认文件关联逻辑：开源实现对照

审阅日期：2026-10-13。对照本项目、Firefox、Chromium、PS-SFTA 与 FTA-Manager。
下述结论针对所列源码，不据此承诺所有 Windows 版本或补丁均能使用直接注册表方案。

## 本轮已修正

| 检查项 | 本项目原有问题 | 调整 |
| --- | --- | --- |
| 哈希写入时间 | 在权限/删除操作之前计算 Hash，HTA 回退沿用同一个 Hash；耗时跨分钟可能失效 | 先检查算法可用性，清理后重新取 UTC 时间；直接写入最多 3 次、HTA 最多 2 次，仅在跨分钟且校验失败时重算重试 |
| Unicode 小写 | CRT 默认 C 区域设置可能不转换重音字母，扩展名和哈希输入与 Windows 规则不一致 | 使用 Windows `CharLowerBuffW`，命令行与哈希共用同一转换 |
| 应用注册名称 | Capabilities 的 ApplicationName 使用描述，而 RegisteredApplications 使用 ProgID，名称不一致 | 两者使用相同的稳定 ProgID，保留单独的描述与 ProgID Application 显示字段 |
| Win7 的旧默认选择 | 只更新 Classes、不写 Hash，但旧 UserChoice 可能继续覆盖新的 Classes | 仅在 Win7 分支尚未验证为目标默认时清理旧 UserChoice、通知 Shell 后重新校验，与原阅读器行为一致 |

直接写入正常情况下不增加等待；HTA 仍等待子进程退出，只有跨分钟才允许额外尝试。
测试覆盖 59.999 秒与下一分钟的哈希区别、旧分钟 Hash 的拒绝与修复、`.ÉXT` 大小写等价、注册名称一致。
这些测试不等同于强制让真实 HTA 进程在分钟边界启动；该场景仍需实际系统运行验证。

## 与 Firefox 的对照

[WindowsUserChoice.cpp](https://github.com/mozilla-firefox/firefox/blob/main/browser/components/shell/WindowsUserChoice.cpp)
使用 Windows 转小写、UTF-16 含末尾 NUL、CNG MD5、两组 32 位混合和 crypt32 Base64。
本项目的现代 Hash 输入与混合步骤一致；Win7/8 与早期 Win10 分支仍保留原阅读器的规则，未套用 Firefox 的最低版本限制。

[SetDefaultBrowser.cpp](https://github.com/mozilla-firefox/firefox/blob/main/toolkit/mozapps/defaultagent/SetDefaultBrowser.cpp)
检查分钟时间窗口，并在写入后通过系统接口验证默认 ProgID。
Firefox 还包含安装实例标识、MSIX 路径和可选的注册表重命名处理，本项目不直接移植这些机制。
本项目使用当前用户真实 token/SID、写后重算 Hash，并核对 Shell 的有效 ProgID 和打开命令。

## 与 Chromium 的对照

[当前 shell_util.cc](https://github.com/chromium/chromium/blob/main/chrome/installer/util/shell_util.cc)
将应用注册、默认状态查询和打开系统默认应用界面分开，使用 QueryCurrentDefault 的 AL_EFFECTIVE 查询实际默认处理器。
本项目的有效默认查询选择与之相同，但还检查打开命令和自己写入的用户注册项。

[历史直接设置实现](https://chromium.googlesource.com/chromium/src/+/4a8573cb240df29b0e4d9820303538fb28e31d84/chrome/installer/util/shell_util.cc)
的 WriteUserChoiceValues 允许有限重试，应对分钟边界，并读取键的实际写入时间验证 Hash。
这是历史代码的对照依据，不代表当前 Chrome 仍采用同一静默写入方案。

## 与关联设置工具的对照

[PS-SFTA](https://github.com/DanysysTeam/PS-SFTA/blob/master/SFTA.ps1)
在设置 UserChoice 前更新目标与其他候选应用的 ApplicationAssociationToasts。
[FTA-Manager](https://github.com/feiglein74/FTA-Manager/blob/master/FTA-Manager.psm1)
也在常规设置流程中写目标确认记录；它参考了 PS-SFTA，不能视为完全独立的算法验证。
本项目已采用按扩展名补齐确认记录的方式；1809 / 17763.1 的首次双击由用户与日志共同确认通过。

本项目目前从 OpenWith 索引收集候选项，不枚举所有浏览器 Capabilities，也不清理全局通知记录。
这与 PS-SFTA 的候选收集范围不同，目前没有实测失败证据要求扩大范围。

## 保留的边界与后续验证

- 当前 `--check` 校验本工具的注册项、UserChoice 和 Shell 状态，并非检查任意安装器/系统界面生成的关联的通用查询器。
  例如旧式 Classes 默认值或 Explorer 的 Progid 缺失时，即使系统界面设置了默认应用，也可能报告不匹配。
  如需支持这种额外范围，应改为优先依据系统有效默认查询，并为不同来源的注册布局增加独立测试。
- 哈希版本、HTA 和确认记录都涉及系统内部兼容行为；未增加新 UCPD 绕过方式，也不修改驱动或跨用户权限。
- 1809 的用户日志仍对应 v2；v3 的时间边界、Unicode、注册名称及 Win7 清理分支需要实际虚拟机回归，不能将本机自动测试当作跨版本通过。
- 本轮未引入注册表事务或自动恢复其他应用配置。设置失败可能已完成部分注册，退出码与日志保留这个结果。

## 微软接口参考

- [CharLowerBuffW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-charlowerbuffw)：使用 Windows Unicode 转换，满足 Win7 的 API 范围。
- [Default Programs / Capabilities](https://learn.microsoft.com/en-us/windows/win32/shell/default-programs)：ApplicationName 与 RegisteredApplications 注册名称一致。
  该文档的默认设置 API 部分不适用于 Win10；这里只引用其注册字段约定。
