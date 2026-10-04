# nageki-io-next

[English](README.md) | 简体中文

面向 Nageki 系列的原生 x64 IO，用 C 从零编写。它读取 Nageki 兼容的 USB HID 控制器，实现 segatools 的 MU3 IO 与 Aime IO 接口，包括按键灯和读卡。

## 接口

两个 API 共 26 个导出：

- MU3 IO 0x0101：`mu3_io_init`、`mu3_io_poll`、`mu3_io_get_opbtns`、`mu3_io_get_gamebtns`、`mu3_io_get_lever`、`mu3_io_led_init`、`mu3_io_led_set_colors`
- Aime IO 0x0101：0x0100 的五个入口，加上 0x0101 新增的十二个

## 必须构建为 64 位

segatools 通过 `LoadLibraryW` 把这个 DLL 载入 `mu3.exe` 和 `amdaemon.exe`。这两个进程都是 64 位，因此 32 位版本会在加载时失败：

```
Ongeki IO: Failed to load IO DLL: 800700c1: MU3CustomIO.dll
```

`0x800700C1` 是 `ERROR_BAD_EXE_FORMAT`(193)，即位数不匹配，与导出名和 API 版本无关。`mu3hook/dllmain.c` 在 IO DLL 初始化失败时会调用 `ExitProcess`，于是进程在 Hook 运行之前就退出，表象就是 Hook 从未启动。

`scripts/build.ps1` 会读 PE 头，DLL 不是 x64 时直接让构建失败。

## 构建

需要 Visual Studio 2022 Build Tools 的 x64 编译器和 Windows SDK。

```
pwsh ./scripts/build.ps1
pwsh ./scripts/test.ps1
pwsh ./scripts/package.ps1
```

`build.ps1` 依次构建 DLL、检查 PE 头、加载 DLL 并调用其导出，最后跑一次绑定模拟。模拟照抄 segatools 的 `mu3_dll_syms` 与 `aime_dll_syms` 符号顺序及每版本的符号数量。segatools 全有或全无地绑定，遇到第一个缺失名就停，所以模拟会报告 `mu3 7/7` 和 `aime 17/17`。

`test.ps1` 跑输入核心、LED 报文编码、卡号转换和摇杆转换的合成测试。`build/` 与 `dist/` 都在 gitignore 里。

## 故障处理

`mu3_io_init` 与 `mu3_io_poll` 恒返回 `S_OK`。segatools 从 `mu3_io4_hook_init`(`games/mu3hook/io4.c:35`) 调用 `mu3_io_init`，那里失败与 DLL 加载失败一样，对进程启动是致命的；poll 失败则会从 `io4_async_poll`(`common/board/io4.c:349`) 冒出来。因此控制器不在线时上报的是中性输入，而不是错误。

用 `/DMU3_IO_REPORT_DISCONNECT` 构建时，poll 会在超过 `MU3_DISCONNECT_REPORT_MS` 没有报文之后返回 `HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED)`。这个开关用于需要可见故障码的实验。游戏是否会因此显示 IO4 报错，尚未确定。

## 输入有效性

输入是否有效由设备连接状态决定。设备连上并收到过一份有效报文之后，那份报文一直沿用，直到设备消失，中间隔多久都一样。变化触发的控制器在摇杆和按键静止时不发报文，因此用定时器让报文过期会松开被按住的输入。

跨进程同理。`Local\MU3CustomIO-v1` 下的共享内存带一个 `published_ms` 心跳，由 HID worker 每轮刷新，只有这个心跳表示持有进程是否还活着，报文年龄从不参与判断。owner 消失主要靠进程句柄本身判定：读取方用 `OpenProcess` 打开 owner 的 PID，发现打不开或已收到信号即立刻判死。五秒的心跳窗口是 PID 被复用或映射残留时的兜底；因为空闲 worker 的心跳间隔约一秒、重连还要叠加一次完整枚举，窗口必须留足，否则负载高时会误判并丢输入。断开与重连都会清空已存样本，旧的按键或卡号不会被重放。

## 报文布局

报文 65 字节：一个 Report ID 字节加 64 字节负载。

| 负载偏移 | 字段 |
|---|---|
| 0-9 | 按键字节，先五个左、再五个右 |
| 10-11 | 摇杆，小端 |
| 12 | 卡片扫描状态 |
| 13-22 | 卡号 |
| 23 | 操作按钮 |

按键字节非零即视为按下，所以上报 `0x00`/`0xFF` 的设备与上报 0/1 的设备表现一致。扫描状态不是 1 或 2 时按无卡处理，同一份报文的其余部分照旧使用。

已用 `hid_probe.exe dump` 的抓包确认 Report ID 字节为 `0x00`，摇杆字段位于第 11-12 字节、小端，对应负载偏移 10-11。按键、扫描状态、卡号与操作按钮的偏移仍来自对既有实现的协议分析，没有与抓包核对过，确认方式是用 `hid_probe.exe`。

## 摇杆换算

换算在 `src/lever.c`。原版公式为：

```
lever = (short)( LeverOffset * 100 + raw * 32 * LeverSensitivity )
```

默认值 `LeverSensitivity = 1`、`LeverOffset = 0`，没有 clamp，`(short)` 收窄按模 65536 回绕。它不减零点，等于假设固件在摇杆居中时上报零。

用 `hid_probe.exe dump` 在实机的部署控制器上直接读到摇杆字段：

| 位置 | raw |
|---|---|
| 左端 | `0x032A`(810) |
| 静置 | `0x0402`(1026) |
| 右端 | `0x04A4`(1188) |

电气中心是 `0x0400`(1024)，因此本构建减掉它：

```
lever = (short)( (raw - neutral) * 32 * sensitivity ),   neutral = 1024
```

### 为什么要减掉中心

中位输出变成 `(C - C) * 32 * sens = 0`，对任意灵敏度都成立。原式输出的是 `C * 32 * sens (mod 65536)`，只有 `C * sens` 为 2048 的整数倍时才归零：

| 灵敏度 | 原式中位输出 |
|---|---|
| 1, 3, 5, ... | `0x8000`（半量程） |
| 2, 4, 6, ... | `0x0000` |

2048 即 2^11，因此偶数灵敏度在 `v2(C) >= 10` 时居中，奇数灵敏度需要 `v2(C) >= 11`。两个条件同时成立，强制 `v2(C) = 10`，即 `C = 1024 * 奇数`。探针数据选定 1024：10 位范围内另一个候选 3072 距离实测静置值 2046 格。

在部署基线灵敏度 2 下，减零点的换算对**每一个** raw 都与原版逐位相同——两者相差 `1024 * 32 * sens`，当且仅当灵敏度为偶数时是 65536 的整数倍，`tests/lever_tests.c` 对全部 65536 个输入做了断言。这个改动换来的是奇数档从不可用变为可用。

### 范围与灵敏度上限

行程约 378 格，810 到 1188，因此离中心最大偏离 214 格。要让 `偏离 * 32 * sens` 留在有符号 16 位正半轴内，灵敏度上限为 4：`214 * 32 * 5 = 34240` 会回绕成负数。`MU3_LEVER_SENSITIVITY_MAX` 取 4。

只有减去中心后的低 10 位会影响结果，因为灵敏度 2 时 `1024 * 64 = 65536`。这来自原式本身，不是本实现的缺陷。

### 调参

DLL 同目录下的 `MU3CustomIO.ini` 可覆盖默认值：

```
lever_neutral=1024
lever_sensitivity=2
```

`lever_neutral` 取 0..65535，`lever_sensitivity` 取 1..4。文件不存在、或部分内容非法时保留默认值；单个键缺失时保留已有值。设 `lever_neutral=0` 可还原原版未减零点的输出。包内的 `MU3CustomIO.ini.txt` 是带注释的模板，需改名后生效。文件放在 DLL 同目录而非 exe 同目录，因为 `mu3.exe` 与 `amdaemon.exe` 加载的是同一份 DLL。

### 标定画面的读数

机台摇杆标定画面在灵敏度 2 下读左端 `B0FFH`、右端 `557FH`。两者都是奇数，且对 64 取模都为 63。而 lever 值恒为 `32 * sensitivity` 的整数倍、因此恒为偶数，不可能是这两个读数。灵敏度 2 的比例是 64，所以 `adcs[0] = 0x7FFF - lever`（`common/board/io4.c:125`）对 64 取模恒为 63，与读数完全吻合。因此画面显示的是标定信号，而不是 `mu3_io_get_lever` 的返回值。反推得到 raw 约 828 与 1194，跨度 366；探针直接测得的跨度是 378，相差 3%。

该同余关系在比例为 64 的整数倍时成立，即灵敏度 2 与 4；灵敏度 1、3 下 `adcs` 还可能 ≡ 31，而这两个读数取自灵敏度 2。

居中时读到 `0000H` 则是 lever 输出本身——在偶数灵敏度下，原式与减零点式在中心都是 0。这两个读数是不同的量，此前看上去互相矛盾正源于此。

## HID 匹配

DLL 按 VID `2341` 与 PID `8036` 匹配，不限制报文长度，输入输出都为 65 的接口优先但不作要求。读超时既不刷新也不作废已持有的输入。

报文在本 DLL 固定的 65 字节帧与描述符上报的长度之间做编解码。上报 64 字节的设备只带负载，因此写时丢掉帧的 Report ID 字节、读时补 0；若把 65 字节整帧发给这种设备，ID 字节会落到负载首字节的位置，整体前移一位并丢掉最后一个负载字节——表现为灯错位而不是明显报错。上报超过 65 字节的设备补零。这些映射由 `tests/hid_pack_tests.c` 覆盖。

## 读卡

`scan == 1` 直接返回十个卡片字节。`scan == 2` 把前八个字节视为大端十六进制数，将其二十位十进制数字编码为十个 BCD 字节后返回。MIFARE、FeliCa 交易、读卡器灯和 VFD 入口是返回 `S_FALSE` 的占位实现，不宣称硬件并不具备的能力。

## 探针

`build/hid_probe.exe` 是独立工具，游戏不会加载它。

```
hid_probe.exe             列出 HID 设备及其 VID、PID 与 caps
hid_probe.exe dump 30000  打开控制器，只打印发生变化的报文，持续 30 秒
```

在插着控制器的那台机器上运行。一次只按下一个键再松开，发生变化的字节属于该键。把摇杆推到两端读取真实范围。部署控制器上的抓包已经确定了 Report ID、摇杆字段与其中位；本 DLL 里的按键、扫描与卡号偏移仍来自推断，探针是把它们变成实测值的手段。

## 部署

1. 备份在用的 IO DLL、segatools 配置和游戏存档，记录它们的路径与哈希。
2. 把新 DLL 放在单独目录，只让 segatools 的 IO 与 Aime 配置指向它。对照被替换的 DLL 逐项检查按键及其极性、摇杆中位与两端、读卡和灯光。
3. 确认断开控制器后输入转中性且不沿用上一次的按键或卡号，重新连接后输入恢复。segatools 或游戏是否会为此 DLL 报出 IO4 故障、报什么码，尚未确定。
4. 一旦出现加载失败、按键方向相反、卡片错误、灯光错位或游戏异常，退出游戏，恢复原配置与原 DLL，重启两个进程。不要在游戏运行时替换 DLL。

`amdaemon.exe` 被假定为唯一持有 HID 设备的进程。若两个进程都配置了这个 DLL 而其中只有一个能打开设备，请先确认是哪一个，再依赖共享内存通路。

## 许可证

[BSD 2-Clause "Simplified" License](LICENSE)

