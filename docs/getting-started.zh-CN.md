# Wireless DATLINK 安装与使用指南

本文面向 Windows 10/11、两块 ESP32-S3 和官方 LP-MSPM0G3507。当前协议版本为 2，
Gateway、Probe 和 Python CLI 必须使用同一版本。项目是专用无线烧录器，不是标准
CMSIS-DAP 调试器。

## 1. 系统组成

```text
PC
 ├─ Gateway CH343 UART：下载固件、115200 日志（当前实验机通常为 COM8）
 └─ Gateway USB OTG：TinyUSB CDC，datlink 命令使用的端口（当前通常为 COM5）

Gateway ESP32-S3
 └─ 加密 ESP-NOW，信道 1
     └─ Probe ESP32-S3
         ├─ CH343 UART：下载固件、115200 日志（当前通常为 COM7）
         └─ SWD：连接 LP-MSPM0G3507
```

不要把 Gateway 的 CH343 COM 口传给 `datlink --port`。`datlink` 只使用 Gateway
原生 USB OTG 枚举出的 CDC 端口。

## 2. 已验证环境

```text
ESP-IDF       v6.0
CMake         4.0.3
Ninja         1.12.1
Python        3.10 或更高；本机 ESP-IDF 环境为 3.14
MSPM0 SDK     D:\ti\mspm0_sdk_2_10_00_04
TI Arm Clang  D:\ti\ccs2050\ccs\tools\compiler\ti-cgt-armllvm_4.0.4.LTS
SysConfig     D:\ti\ccs2050\ccs\utils\sysconfig_1.27.0
```

ESP32 固件必须使用 ESP-IDF、CMake 和 Ninja 构建，不使用 PlatformIO。TI Arm Clang
只负责生成运行在 MSPM0 SRAM 中的 Flash Loader，以及构建 TI DriverLib 示例。

## 3. 第一次安装

### 3.1 获取仓库

```powershell
Set-Location D:\Project
git clone <仓库地址> Wireless-DATLINK
Set-Location D:\Project\Wireless-DATLINK
```

如果仓库已经存在，先检查本地修改，不要直接覆盖：

```powershell
git status --short
```

### 3.2 安装 ESP-IDF 6.0 工具

仓库默认使用：

```text
IDF_PATH=D:\Espressif\.espressif\v6.0\esp-idf
IDF_TOOLS_PATH=C:\Espressif
```

首次安装或修复 Python 环境：

```powershell
$env:IDF_TOOLS_PATH = 'C:\Espressif'
Set-Location 'D:\Espressif\.espressif\v6.0\esp-idf'
.\install.ps1 esp32s3
. .\export.ps1
```

也可以让项目脚本检查并安装：

```powershell
Set-Location D:\Project\Wireless-DATLINK
.\build.ps1 -Setup -Role All
```

如果 ESP-IDF 位于其他目录，运行脚本前设置：

```powershell
$env:IDF_PATH = 'D:\path\to\esp-idf'
$env:IDF_TOOLS_PATH = 'C:\Espressif'
```

不要同时启动多个首次构建；ESP-IDF 可能正在初始化相同的managed component和
Python环境。

### 3.3 安装 TI SDK 和编译器

默认 Loader 构建脚本查找：

```text
D:\ti\mspm0_sdk_2_10_00_04
D:\ti\ccs2050\ccs\tools\compiler\ti-cgt-armllvm_4.0.4.LTS
```

单独验证 Loader：

```powershell
.\tools\mspm0_loader\build_loader.ps1
```

使用其他安装路径时：

```powershell
.\tools\mspm0_loader\build_loader.ps1 `
  -Sdk 'D:\path\to\mspm0_sdk' `
  -Compiler 'D:\path\to\ti-cgt-armllvm'
```

当前 `build.ps1` 会使用 Loader 脚本的默认路径。若长期迁移环境，应同步修改默认值，
而不是手工提交生成的Loader二进制。

### 3.4 创建无线密钥和固定设备身份

仓库只提交 `secrets.example.json`。第一次执行 `build.ps1` 时，如果不存在
`secrets.local.json`，脚本会生成随机16字节PMK和LMK。

本实验设备：

```json
{
  "gateway_mac": "14:c1:9f:cd:33:4c",
  "probe_mac": "14:c1:9f:cc:80:5c"
}
```

检查实际ESP32 MAC后再构建新设备。Gateway和Probe必须使用同一份PMK/LMK，并且
两端编译的MAC角色必须相反匹配。真实密钥不得提交到Git。

## 4. 构建ESP32固件

```powershell
# 两个角色
.\build.ps1 -Role All

# 单独构建
.\build.ps1 -Role Gateway
.\build.ps1 -Role Probe

# 删除对应build目录后重新配置
.\build.ps1 -Role Gateway -Clean
.\build.ps1 -Role Probe -Clean
```

输出：

```text
build\gateway\datlink_gateway.bin
build\probe\datlink_probe.bin
tools\mspm0_loader\build\mspm0_loader.bin
```

协议、可靠传输、密钥或MAC发生变化时必须重刷两端。只更新一端可能出现链路不通、
恢复计数异常增加或业务sequence反复重同步。

## 5. 下载Gateway和Probe

先枚举端口：

```powershell
Get-CimInstance Win32_SerialPort |
  Select-Object DeviceID, Name, PNPDeviceID |
  Format-Table -AutoSize
```

下载ESP32固件不会烧写MSPM0：

```powershell
.\build.ps1 -Role Probe   -Flash -Port COM7
.\build.ps1 -Role Gateway -Flash -Port COM8
```

查看日志：

```powershell
.\build.ps1 -Role Probe   -Monitor -Port COM7
.\build.ps1 -Role Gateway -Monitor -Port COM8
```

日志应包含角色、协议版本、MAC、Flash、PSRAM、信道和加密peer。若端口不存在，不要
猜测其他COM号；重新插拔对应CH343 USB后再枚举。

## 6. Probe与LaunchPad接线

LaunchPad由自己的USB供电，Probe不向目标供电。两块板必须共地。

| Probe ESP32-S3 | LP-MSPM0G3507 J103 | 作用 |
|---|---|---|
| GPIO1 | pin 1 | 3.3V VTref检测，只感测不供电 |
| GPIO5 | pin 2 | SWDIO，建议串联47Ω |
| GND | pin 3、5或9 | 公共地，三者在LaunchPad上属于同一地网 |
| GPIO4 | pin 4 | SWCLK，建议串联47Ω |
| GPIO6 | pin 10 | nRESET，开漏，建议串联约100Ω |

使用外部Probe时移除J101：

```text
11:12  NRST
13:14  SWDIO
15:16  SWCLK
```

保留LaunchPad电源和GND跳线。不要让板载XDS110与ESP32 Probe同时驱动同一组SWD线。
GPIO19/20保留给ESP32原生USB，GPIO43/44保留给CH343 UART，GPIO35/36/37由PSRAM占用。

## 7. 安装PC CLI

推荐在仓库根目录建立独立虚拟环境：

```powershell
Set-Location D:\Project\Wireless-DATLINK
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -e .\tools\datlink_cli
datlink --help
```

PowerShell禁止激活脚本时，可以只为当前用户设置：

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```

或不激活环境，直接调用：

```powershell
.\.venv\Scripts\datlink.exe ports
```

## 8. 首次上电检查

```powershell
datlink ports
datlink --port COM5 info
datlink --port COM5 link --json
datlink --port COM5 target-info
datlink --port COM5 loader-test
```

健康链路应满足：

```json
{
  "up": true,
  "recovering": false,
  "tx_pending": 0,
  "rx_pending": 0,
  "head_state": 0,
  "last_error": 0
}
```

`target-info`必须读到当前允许列表中的DPIDR、AP IDR、CPUID和TI Factory Region。
`loader-test`只上传和运行SRAM Loader的PROBE命令，不擦除目标Flash。

## 9. 备份和烧录

### 9.1 烧录前完整备份

```powershell
datlink --port COM5 backup `
  .\backups\mspm0g3507-main-before.bin `
  --passes 2 `
  --wait 240
```

CLI只有在两遍131072字节完全一致、Probe SHA和PC SHA一致后才生成最终文件。

### 9.2 支持的输入格式

```powershell
datlink --port COM5 program app.out --wait 180
datlink --port COM5 program app.elf --wait 180
datlink --port COM5 program app.axf --wait 180
datlink --port COM5 program app.hex --wait 180
datlink --port COM5 program app.bin --base 0x00000000 --wait 180
```

限制：

- ELF/OUT必须是ELF32 little-endian；CLI读取MAIN Flash范围内的PT_LOAD段。
- BIN没有地址信息，必须指定`--base`。
- 最多8个不重叠地址段，段起始地址必须8字节对齐。
- 当前只允许`0x00000000..0x0001FFFF`。
- 不允许BCR、BSL配置和NONMAIN。

烧录成功结束于：

```text
phase=8 ... status=0
```

流程包括目标识别、Loader上传、sector擦除、64位ECC编程、MEM-AP读回、SHA比较和
自动复位运行。

### 9.3 恢复完整备份

```powershell
datlink --port COM5 program `
  .\backups\mspm0g3507-main-before.bin `
  --base 0x00000000 `
  --wait 240
```

完整128KiB备份会覆盖全部MAIN Flash，适合恢复。短BIN只覆盖到的sector会被擦除；
同一已擦sector中未由镜像重新写入的字节会保持擦除值`0xFF`，不是自动保留原内容。

## 10. 日常命令

```powershell
datlink --port COM5 link
datlink --port COM5 link --json
datlink --port COM5 target-info
datlink --port COM5 loader-test
datlink --port COM5 verify
datlink --port COM5 reset
datlink --port COM5 abort
datlink --port COM5 recover
```

`verify`显示Gateway缓存的最近进度，不等价于重新读取整个目标。实际program命令已经
包含逐字节读回和SHA验证。

`recover`只重建无线可靠序列，不擦除MSPM0。`target-info`和`loader-test`在一次超时
恢复后可自动重试；破坏性program命令不会被CLI自动重放。

## 11. TI DriverLib示例

以RGB例程为例：

```powershell
$Example = 'D:\ti\mspm0_sdk_2_10_00_04\examples\nortos\LP_MSPM0G3507\driverlib\gpio_toggle_output\ticlang'
Set-Location $Example

& 'D:\ti\ccs2050\ccs\utils\bin\gmake.exe' -j4 `
  'TICLANG_ARMCOMPILER=D:/ti/ccs2050/ccs/tools/compiler/ti-cgt-armllvm_4.0.4.LTS' `
  'SYSCONFIG_TOOL=D:/ti/ccs2050/ccs/utils/sysconfig_1.27.0/sysconfig_cli.bat' `
  all

Set-Location D:\Project\Wireless-DATLINK
datlink --port COM5 program "$Example\gpio_toggle_output.out" --wait 180
```

预期RGB每0.5秒在红色与蓝色+绿色之间交替。不要手工编辑`ticlang`目录内生成的
`ti_msp_dl_config.c/.h`；需要改引脚时修改`.syscfg`并重新生成。

## 12. 故障处理

### Gateway没有响应

```powershell
datlink ports
datlink --port COM5 info
```

确认使用OTG CDC而不是COM8。关闭占用COM5的串口工具。协议v1 Gateway不会响应
协议v2 USB帧，应同时更新Gateway、Probe和CLI。

### `link`为up但命令超时

```powershell
datlink --port COM5 link --json
datlink --port COM5 recover
datlink --port COM5 target-info
```

记录session、`rx_base`、`head_state`、`last_error`和`recovery_count`。如果恢复计数在
正常烧录中持续增加，先确认两端是否刷入相同transport修复版本。

### SWD ACK FAULT

依次检查：目标独立供电、共地、VTref、J101三组SWD跳线已移除、SWDIO/SWCLK没有
接反、XDS110没有同时驱动。然后运行：

```powershell
datlink --port COM5 target-info
datlink --port COM5 loader-test
```

项目不会因ACK FAULT自动mass erase或解锁目标。

### 程序烧录成功但不运行

检查OUT/ELF向量表、初始MSP、Thumb Reset Handler、板载LED跳线和应用时钟配置。
不要把“读回验证成功”直接等同于“应用逻辑一定正确”。

## 13. 安装验收

最低验收：

```powershell
python -m unittest discover -s tests -v
.\build.ps1 -Role All
datlink --port COM5 link --json
datlink --port COM5 target-info
datlink --port COM5 loader-test
```

生产或长期实验使用前还应完成：完整备份、已知LED固件烧录、读回备份比较、单边
重启恢复、混合命令压力测试和断电边界测试。
