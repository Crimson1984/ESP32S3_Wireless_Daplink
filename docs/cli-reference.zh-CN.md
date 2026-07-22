# Wireless DATLINK 通用烧录命令说明

从零安装ESP-IDF、TI Loader工具、两端固件、接线和CLI请先阅读
[完整安装与使用指南](getting-started.zh-CN.md)。新增设备对或目标型号请阅读
[设备扩展指南](device-extension.zh-CN.md)。

## 1. 当前适用范围

Wireless DATLINK 当前可以把任意合法的 MSPM0G3507 MAIN Flash 固件作为输入，
通过 Gateway、ESP-NOW 和 Probe 完成擦除、烧写、读回校验与复位运行。

它目前不是标准 CMSIS-DAP/DAPLink 设备：

- CCS、UniFlash、OpenOCD 和 pyOCD 不会把它枚举成调试探针。
- 当前应使用本项目的 `datlink` 命令烧录。
- 暂不支持断点、单步、寄存器窗口、GDB Server 或 CCS Debug Session。
- 目标型号固定为 MSPM0G3507，不是任意 ARM 或任意 MSPM0 型号。

允许写入的范围只有 MAIN Flash：

```text
0x00000000..0x0001FFFF（128 KiB）
```

不会自动执行 mass erase、factory reset、安全解锁，也不会写入 NONMAIN、
BCR 或 BSL 配置区。

## 2. 端口用途

```text
COM5  Gateway OTG TinyUSB CDC：执行 datlink 命令
COM8  Gateway CH343 UART：更新 Gateway 固件和查看日志
COM7  Probe CH343 UART：更新 Probe 固件和查看日志
```

COM 号可能随 USB 插口或 Windows 枚举结果变化，应先执行：

```powershell
datlink ports
```

VID `303a`、PID `4001` 的 USB 串行设备是 Gateway 命令口。不要把 COM8
下载/日志口传给 `datlink --port`。

## 3. 安装或更新 PC CLI

在仓库根目录执行：

```powershell
Set-Location D:\Project\Wireless-DATLINK
python -m venv tools\.venv
.\tools\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -e .\tools\datlink_cli
datlink --help
```

如果虚拟环境已经存在，只需激活并重新执行可编辑安装命令。

所有全局参数必须放在子命令之前：

```powershell
datlink --port COM5 --timeout 5 target-info
```

- `--port`：Gateway OTG CDC 端口。
- `--timeout`：单个 USB 请求/响应的超时时间，默认 3 秒；它不是完整烧录
  操作的超时时间。

## 4. 更新两块 ESP32-S3 固件

首次配置并构建：

```powershell
.\build.ps1 -Setup -Role All
```

普通构建：

```powershell
.\build.ps1 -Role All
.\build.ps1 -Role Gateway
.\build.ps1 -Role Probe
```

清理后重新构建：

```powershell
.\build.ps1 -Role Gateway -Clean
.\build.ps1 -Role Probe -Clean
```

更新固件时建议先 Probe、后 Gateway，并保证两端使用同一个
`secrets.local.json`：

```powershell
.\build.ps1 -Role Probe   -Flash -Port COM7
.\build.ps1 -Role Gateway -Flash -Port COM8
```

指定 ESP32-S3 下载波特率：

```powershell
.\build.ps1 -Role Probe -Flash -Port COM7 -Baud 921600
```

查看 UART 日志：

```powershell
.\build.ps1 -Role Gateway -Monitor -Port COM8
.\build.ps1 -Role Probe   -Monitor -Port COM7
```

这些命令只更新 ESP32-S3，不会烧写 MSPM0G3507。

## 5. 烧录前检查命令

### 5.1 查看 Gateway 信息

```powershell
datlink --port COM5 info
```

输出 Gateway 角色、启动 session、本机 MAC 和固定 Probe MAC。

### 5.2 检查无线链路

```powershell
datlink --port COM5 link
```

输出 `up` 才能继续。`up` 表示 Gateway 与 Probe 心跳正常，不表示目标芯片
已经供电或 SWD 一定正常。

协议 v2 可查看可靠队列和自动恢复详情：

```powershell
datlink --port COM5 link --json
```

如果链路有心跳但业务命令没有返回，可手动触发双向 session 重同步：

```powershell
datlink --port COM5 recover
```

该命令只清理和重建 ESP-NOW 可靠序列，不复位或擦除 MSPM0。正常情况下
停滞看门狗会自动执行相同恢复；`recover` 是人工兜底。Gateway、Probe 和
Python CLI 必须同时使用协议 v2，版本不一致时命令会明确报错。

### 5.3 识别目标芯片

```powershell
datlink --port COM5 target-info
```

成功时输出 DPIDR、AP IDR、CPUID、TI Factory Region 标识和当前 SWD
频率。Probe 会按固定白名单拒绝非 MSPM0G3507 目标。

### 5.4 非破坏性 Loader 测试

```powershell
datlink --port COM5 loader-test
```

该命令连接 SWD、检查 SRAM、上传并运行 SRAM Flash Loader，但不擦除或修改
目标 Flash。成功输出中应包含：

```text
"loader_test": "passed"
"flash_modified": false
```

## 6. 支持的固件文件

| 文件 | 命令要求 | 说明 |
|---|---|---|
| TI/CCS `.out` | 不需要 `--base` | 必须是 ELF32 little-endian |
| `.elf` / `.axf` | 不需要 `--base` | 读取可加载 MAIN Flash 段 |
| Intel `.hex` / `.ihex` | 不需要 `--base` | 支持扩展段地址和扩展线性地址 |
| 原始 `.bin` | 必须提供 `--base` | 文件本身没有地址信息 |

输入限制：

- 段起始地址必须按 8 字节对齐。
- 所有段必须完整位于 `0x00000000..0x0001FFFF`。
- 最多允许 8 个互不重叠的离散段。
- ELF/OUT 只提取 MAIN Flash 的可加载段，不向 MSPM0 写入 SRAM 段。
- 只擦除输入段触及的1 KiB Flash sector；其他sector保持不变，但同一已擦sector内
  没有由输入段重新写入的字节会变为`0xFF`。
- 末尾不足 8 字节的编程单元由 Probe 以 `0xFF` 补齐。

## 7. 所有烧录命令

### 7.1 烧录 CCS/TI OUT

```powershell
datlink --port COM5 program D:\path\to\application.out
```

### 7.2 烧录 ELF 或 AXF

```powershell
datlink --port COM5 program D:\path\to\application.elf
datlink --port COM5 program D:\path\to\application.axf
```

### 7.3 烧录 Intel HEX

```powershell
datlink --port COM5 program D:\path\to\application.hex
datlink --port COM5 program D:\path\to\application.ihex
```

### 7.4 烧录原始 BIN

烧录到 MSPM0 MAIN Flash 起始地址：

```powershell
datlink --port COM5 program D:\path\to\application.bin --base 0x00000000
```

烧录到其他合法且按 8 字节对齐的 MAIN 地址：

```powershell
datlink --port COM5 program D:\path\to\sector-data.bin --base 0x0001FC00
```

### 7.5 指定完整操作超时

`--wait` 是 Gateway 接受 `PROGRAM_START` 之后，无线传输、擦写、读回验证
和复位运行阶段的等待上限，默认 120 秒。PC 到 Gateway 的镜像上传使用独立
的分块请求超时：

```powershell
datlink --port COM5 program application.out --wait 180
```

### 7.6 指定操作 ID

通常无需指定，CLI 会随机生成非零 ID。自动化系统需要固定追踪编号时可用：

```powershell
datlink --port COM5 program application.out --operation-id 0x12345678
```

相同操作 ID 的重复 `PROGRAM_START` 应返回当前状态，而不重复执行擦除。

### 7.7 同时指定所有常用参数

```powershell
datlink --port COM5 --timeout 5 program application.out `
    --operation-id 0x12345678 --wait 180
```

## 8. 烧录命令的内部流程

一次 `program` 自动完成以下阶段：

```text
PC解析OUT/ELF/HEX/BIN
→ 上传完整镜像到Gateway并校验
→ ESP-NOW可靠传输到Probe并再次校验
→ Probe确认目标身份和VTref
→ 计算并擦除镜像覆盖的sector
→ SRAM Loader编程
→ SWD全量读回并比较SHA-256
→ SYSRESETREQ，必要时nRESET脉冲
→ 目标恢复运行
```

镜像未完整到达 Probe 并通过 SHA-256 前，Probe 不会擦除目标 Flash。

## 9. 备份、恢复和状态命令

### 9.1 双遍备份完整 MAIN Flash

```powershell
datlink --port COM5 backup .\backups\mspm0g3507-main.bin
```

默认读取两遍完整 128 KiB，比较每个字节及 SHA-256 后才生成输出文件。

指定读取遍数和单遍超时：

```powershell
datlink --port COM5 backup .\backups\main.bin --passes 3 --wait 240
```

覆盖已经存在的输出文件：

```powershell
datlink --port COM5 backup .\backups\main.bin --force
```

### 9.2 恢复完整备份

```powershell
datlink --port COM5 program .\backups\mspm0g3507-main.bin `
    --base 0x00000000 --wait 180
```

恢复前应核对备份长度为 131072 字节并保存原 SHA-256。

### 9.3 查看最近一次烧录状态

```powershell
datlink --port COM5 verify
```

注意：当前 `verify` 只读取 Gateway 保存的最近 programmer 状态/进度，
不是重新发起一次独立的 Flash 全量读取校验。`program` 本身已经包含读回校验。

### 9.4 复位并运行目标

```powershell
datlink --port COM5 reset
```

### 9.5 请求中止当前操作

```powershell
datlink --port COM5 abort
```

中止是异步请求。不要在擦写过程中直接拔掉 Probe 或目标电源；应等待当前
Flash 命令退出并检查状态。

## 10. 推荐的标准烧录顺序

```powershell
Set-Location D:\Project\Wireless-DATLINK
.\tools\.venv\Scripts\Activate.ps1

datlink ports
datlink --port COM5 info
datlink --port COM5 link
datlink --port COM5 target-info
datlink --port COM5 loader-test
datlink --port COM5 program D:\path\to\application.out --wait 180
datlink --port COM5 verify
```

第一次连接一块新的 LaunchPad 时，建议先执行只读备份：

```powershell
datlink --port COM5 backup .\backups\factory-main.bin
```

## 11. 批量和连续烧录

连续烧录同一个文件：

```powershell
1..20 | ForEach-Object {
    Write-Host "Programming pass $_"
    datlink --port COM5 program D:\path\to\application.out --wait 180
    if ($LASTEXITCODE -ne 0) {
        throw "program failed at pass $_"
    }
}
```

连续执行目标识别测试：

```powershell
1..50 | ForEach-Object {
    datlink --port COM5 target-info
    if ($LASTEXITCODE -ne 0) {
        throw "target-info failed at pass $_"
    }
}
```

## 12. CCS 和 VSCode 的使用边界

CCS 构建产生的 `.out` 可以直接交给 `datlink program`。可以把该命令配置为
CCS 外部工具或构建后的外部步骤，但不能在 CCS 的 Connection 列表中选择
Wireless DATLINK，也不能点击 CCS Debug 按钮启动调试会话。

VSCode、CMake 或其他构建系统同样可以在构建成功后调用：

```powershell
datlink --port COM5 program <构建输出.out|elf|hex|bin>
```

要成为真正的通用 DAPLink，还需要实现 CMSIS-DAP v2 USB 接口，或实现
GDB Remote Serial Protocol 并提供 OpenOCD/GDB 兼容层；这些功能当前不在
固件中。

## 13. 常见错误

### `ESP-NOW link is down`

检查 Probe 是否供电、两端是否刷入匹配密钥的最新固件，并确认固定 MAC 与
信道配置一致。

### `target_power/VTref (-8)`

LaunchPad 未独立供电、没有共地，或 GPIO1 的 VTref 分压检测不正确。

### `target_id (-12)`

目标身份读取失败或不符合 MSPM0G3507 固定白名单。不要绕过身份检查。

### `target_locked (-13)`

目标受保护。本项目不会自动解锁、mass erase 或 factory reset。

### `loader (-14)` 或 `verify (-15)`

停止继续烧录，保留 Probe/Gateway UART 日志，并先执行 `target-info` 和
`loader-test` 定位 SWD、SRAM Loader 或 Flash 读回问题。

### `gateway response timeout` / `event ... did not arrive`

确认正在使用 Gateway OTG CDC 端口而不是 COM8，更新 PC CLI 和两端固件，
然后检查 COM7/COM8 日志。不要仅根据 `link` 输出判断业务通道一定正常。
