# Wireless DATLINK 设备扩展指南

本文分别说明如何增加一对新的ESP32-S3设备，以及如何增加新的MSPM0目标型号。
当前代码不是动态配对系统，也不是通用CMSIS-DAP；新增目标不能只修改一个芯片ID。

## 1. 增加一对Gateway和Probe

### 1.1 硬件要求

每块ESP32-S3应满足：

```text
ESP32-S3，40MHz晶振
16MB Flash
8MB Octal PSRAM
原生USB OTG可连接PC
独立UART下载/日志口
Probe侧可用GPIO1/4/5/6或重新配置等效引脚
```

若板卡Flash、PSRAM、晶振或USB布线不同，必须先修改`sdkconfig.defaults`并单独验证，
不能直接沿用当前二进制。

### 1.2 读取MAC

连接每块板的下载口：

```powershell
python -m esptool --chip esp32s3 -p COMx read-mac
```

也可先刷入诊断固件后从UART日志读取STA MAC。明确记录哪一块是Gateway、哪一块是
Probe，避免角色颠倒。

### 1.3 为新设备生成密钥文件

最安全的做法是为每对设备建立独立工作副本，或把每对密钥保存在仓库外。构建前
复制为项目实际读取的文件：

```powershell
Copy-Item D:\secure\datlink-pair-02.json .\secrets.local.json
```

格式：

```json
{
  "pmk": "32个十六进制字符",
  "lmk": "32个十六进制字符",
  "gateway_mac": "aa:bb:cc:dd:ee:01",
  "probe_mac": "aa:bb:cc:dd:ee:02"
}
```

PMK和LMK各16字节。不同设备对应使用不同密钥；不要把真实文件提交到Git、聊天记录
或公开构建日志。

### 1.4 构建和刷入

```powershell
.\build.ps1 -Role All -Clean
.\build.ps1 -Role Probe   -Flash -Port <Probe下载口>
.\build.ps1 -Role Gateway -Flash -Port <Gateway下载口>
```

启动时`security`组件会比较编译角色和本机STA MAC。MAC不符时固件失败关闭，不能靠
交换USB线让角色动态变化。

### 1.5 新设备验收

```powershell
datlink ports
datlink --port <Gateway-OTG-CDC> info
datlink --port <Gateway-OTG-CDC> link --json
datlink --port <Gateway-OTG-CDC> target-info
datlink --port <Gateway-OTG-CDC> loader-test
```

继续验证：

- 两端MAC与记录一致；
- 协议版本一致；
- PMK/LMK错误时链路不能建立；
- 单边重启5秒内恢复；
- `recover`后两端session改变且业务命令成功；
- 100次`target-info`无永久队首阻塞；
- 5%故障注入下完整镜像SHA一致。

### 1.6 多设备共处

当前每个固件只注册一个固定peer。多对设备可以使用同一Wi-Fi信道，但应使用不同
LMK和固定MAC。若需要一台Gateway选择多个Probe，需要重新设计：

- peer表和设备选择命令；
- 每个peer独立session、TX/RX窗口和operation状态；
- 每peer独立LMK；
- CLI设备枚举；
- 防止把一个目标镜像发给错误Probe。

不要简单地把广播打开；当前安全和可靠性设计建立在固定加密单播之上。

## 2. 修改Probe引脚或自制硬件

默认Kconfig：

```text
GPIO4  SWCLK
GPIO5  SWDIO
GPIO6  nRESET，开漏
GPIO1  VTref ADC
GPIO2  状态LED
```

可以通过Probe的`sdkconfig.defaults.probe`修改，但必须满足：

- SWDIO可在输出和高阻输入间快速切换；
- nRESET必须为开漏，不得推挽输出高电平；
- VTref必须使用高阻分压，ADC端不得超过ESP32允许电压；
- 没有VTref时三根目标控制线全部高阻；
- 避开ESP32启动绑带、USB、UART和Octal PSRAM引脚；
- SWCLK/SWDIO建议串联47Ω，nRESET建议约100Ω；
- Probe和目标必须共地。

修改后至少验证100/250/500/1000kHz、1000次连接和不同长度SRAM pattern。

## 3. 增加新的MSPM0型号

### 3.1 先收集器件事实

必须从目标型号数据手册、TRM、Factory Region说明和实际XDS110基准读取中确认：

```text
DPIDR
AP IDR
CPUID
TI Factory DEVICEID/USERID/SRAMFLASH
MAIN Flash地址和容量
sector大小
编程对齐和ECC要求
SRAM地址和容量
NONMAIN/BCR/BSL配置范围
调试锁定行为
适用DriverLib FlashCTL API
```

不能因为不同MSPM0的DPIDR相同就判定型号相同。Factory字段可能随封装、修订或容量
变化，应从多颗真实样片建立允许列表，避免只记录一颗芯片后误拒绝合法器件。

### 3.2 建立target descriptor

当前代码把G3507常量固化在`mspm0g3507`和`programmer`中。扩展前推荐先引入统一描述：

```c
typedef struct {
    uint32_t target_id;
    uint32_t flash_base;
    uint32_t flash_size;
    uint32_t sector_size;
    uint32_t program_alignment;
    uint32_t sram_base;
    uint32_t sram_size;
    uint32_t factory_base;
    const uint8_t *loader_blob;
    size_t loader_size;
} datlink_target_descriptor_t;
```

descriptor只保存几何信息；型号识别、保护检查和Loader结果解释仍由对应target driver负责。

### 3.3 新增target组件

以现有`components/mspm0g3507`为参考，为新型号建立独立组件，并实现：

```text
identify
range_is_main
validate_manifest
sram_self_test
protection_status
```

识别必须失败关闭。所有可写范围先限制到MAIN Flash，确认安全模型后才考虑其他区域。
第一版不得开放NONMAIN、BCR、BSL配置、mass erase或自动解锁。

### 3.4 扩展协议和PC镜像解析

需要同步修改：

- `DATLINK_TARGET_*`目标编号；
- manifest目标字段；
- CLI的目标选择或自动识别流程；
- ELF/HEX/BIN地址范围验证；
- 最大镜像大小和segment约束；
- `target-info`返回的型号和容量信息；
- 协议版本或能力协商。

当前CLI把G3507的`0x00000000..0x0001FFFF`和target `0x3507`写死，不能只改Probe。
Gateway、Probe、CLI必须作为同一协议版本发布。

### 3.5 为新型号构建SRAM Loader

检查新器件DriverLib是否仍支持：

```text
DL_FlashCTL_eraseMemoryFromRAM
DL_FlashCTL_programMemoryFromRAM64WithECCGenerated
DL_FlashCTL_waitForCmdDone
```

若API、Flash控制器或ECC不同，应建立独立Loader源文件和构建参数。重新规划：

```text
Loader代码区
mailbox
program buffer
initial MSP
SRAM自检保留区
目标应用可能使用的SRAM恢复策略
```

构建必须检查：Loader非空、代码不越界、入口符号正确、buffer对齐、初始MSP在SRAM
顶端以内。上传后继续逐字节回读Loader，再允许执行。

### 3.6 改造programmer

将以下硬编码改为descriptor驱动：

- sector位图；
- 擦除地址；
- 1KiB buffer大小；
- 64位编程补齐；
- Flash读回边界；
- SRAM Loader地址；
- 完整备份长度；
- 进度总量。

对局部镜像应实现sector级Read-Modify-Write，或明确拒绝没有覆盖整个sector的输入。
当前实现会擦除所有被段触及的sector，同一sector内未重新写入的字节变为`0xFF`。

### 3.7 保护和恢复策略

为新目标定义确定错误：

```text
未供电
身份不匹配
Debug锁定
Flash写保护
Loader超时
擦除失败
编程失败
读回不一致
复位后仍halt
```

锁定或保护状态不得自动触发mass erase。任何会删除用户数据的恢复必须成为显式命令，
并要求用户二次确认。

## 4. 新目标验收顺序

严格按以下阶段推进，不要直接烧完整应用：

1. XDS110基准读取身份和内存几何。
2. 只连接SWD，读取DPIDR/AP IDR/CPUID/Factory Region。
3. SRAM保存、pattern写读、原值恢复。
4. 上传Loader并执行不修改Flash的PROBE。
5. 对完整MAIN做两遍备份并比较SHA。
6. 选择一个非关键sector，备份、擦除、写入、验证、恢复。
7. 烧录最小LED例程并观察自动复位运行。
8. 烧录完整应用，使用独立工具逐字节比较。
9. 测试越界、错误SHA、掉电、WAIT/FAULT、锁定和超时。
10. 连续20次完整烧录和1000次只读连接。

只有所有阶段完成后，才能把型号加入默认支持列表。

## 5. 扩展为IDE调试器

当前PC接口是自定义CDC命令，不是CMSIS-DAP。若要被CCS、OpenOCD或pyOCD作为调试器
识别，需要增加：

```text
CMSIS-DAP v2 USB Bulk或PC侧GDB Server
DAP_Transfer批处理
halt/run/step
核心寄存器
硬件断点和watchpoint
内存批量读写
目标复位策略
调试与烧录互斥
```

不建议逐个SWD bit通过无线往返。应由PC批量提交调试操作，Probe在本地执行，再返回
结果；高速下载继续复用当前SRAM Loader路径。

## 6. 发布检查清单

- Gateway、Probe、CLI协议版本一致；
- 两角色干净构建；
- Loader尺寸、入口和SHA记录；
- 真实密钥未进入Git；
- 新目标范围测试覆盖首尾地址和溢出；
- 完整备份可恢复；
- 保护目标不被自动擦除；
- 协议故障注入和硬件在环测试通过；
- README、协议、硬件、CLI参考和CHANGELOG同步更新；
- 保存固件BIN、Git commit、构建环境版本和验收日志。
