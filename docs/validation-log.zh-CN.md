# Wireless DATLINK 实机验证记录

本文记录已经执行的硬件验证，不把计划项目描述为已完成。测试目标为官方
LP-MSPM0G3507，Gateway/Probe为ESP32-S3 rev0.2、16MB Flash、8MB PSRAM。

## 2026-07-20 基础烧录链路

设备身份：

```text
Gateway STA MAC  14:c1:9f:cd:33:4c
Probe STA MAC    14:c1:9f:cc:80:5c
DPIDR            0x6BA02477
AP IDR           0x84770001
CPUID            0x410CC601
Factory DEVICEID 0x2BB8802F
Factory USERID   0x80C7AE2D
Factory SRAMFLASH 0x00200080
```

已验证：

- 100、250、500和1000kHz SWD连接；
- connect-under-reset、MEM-AP和SRAM pattern；
- 864字节SRAM Loader上传、逐字节回读和PROBE命令；
- 完整128KiB双遍备份；
- sector 127擦除、64位ECC编程、读回和恢复；
- 恢复后完整MAIN SHA-256：
  `0d1be9fd350cc02a766ef4b4572b396327d6537e58182e88ed4aa2d2b85b9cdb`；
- TI `gpio_toggle_output`和`systick_periodic_timer`烧录及复位运行。

## 2026-07-22 协议v2可靠传输

实现并验证：

- ACK只确认应用已经提交的帧；
- RX token和Gateway/Probe独立应用worker；
- COMMAND_ERROR可靠回执；
- sequence gap重传和directional session epoch恢复；
- `datlink recover`与`link --json`；
- 单边重启后session重建；
- 连续、混合和恢复命令测试完成，未再出现永久`event 24 did not arrive`；
- 主机协议和可靠模型测试增加到23项并全部通过。

实机发现并修复：gap看门狗曾在期望帧仍为`IN_PROGRESS`时，仅因后续帧到达且
`rx_base`时间较旧而误判丢包，导致健康的镜像流水触发epoch切换。修复后只有期望
sequence确实不在任何RX slot中才启动gap resync。

该修复已经刷入Probe。验证当时Gateway CH343下载口COM8未枚举，因此Gateway也应在
COM8恢复后刷入同一源码版本，避免两端transport行为不一致。

## 2026-07-22 两个DriverLib例程

烧录前完整备份：

```text
文件    backups/pre-driverlib-two-example-test-20260722.bin
长度    131072
SHA-256 d3e5bf55c6b1a7235611e4f55df119da509a152d1d7c973a1808181855f2a8a3
遍数    2，逐字节一致
```

### gpio_toggle_output

```text
OUT镜像长度 472
镜像SHA-256 46b6ef4575a542c7fccdeebf1cf62acf8e1313a2cd4e011799e9a5f557d4cb2b
Initial MSP  0x20208000
Reset Handler 0x000001CD
烧录operation 0xA9509093；后续再次烧录operation 0x86D665E1
结果 phase=8, status=0
```

观察现象：板载RGB约每0.5秒在红色和蓝色+绿色之间交替。

### tima_timer_mode_periodic_repeat_count

```text
OUT镜像长度 992
镜像SHA-256 cc4f951abc2a5293f0c02bb08ad8a9eefff4ee61aa56907a19d96c6c5f6048fa
Initial MSP  0x20208000
Reset Handler 0x000003A7
烧录operation 0x2A4F261F
结果 phase=8, status=0
```

观察现象：LED1每2秒翻转一次。烧录后再次双遍读取完整128KiB：

```text
文件    backups/post-tima-periodic-repeat-count-20260722.bin
SHA-256 b5661e3f28288e2184e2a4bcd974c133442074797d9df28279764aa75fdc5e43
```

Flash `0x00000000..0x000003DF`与OUT可加载段逐字节比较结果为MATCH。

## 当前边界

- 已证明当前实验设备可稳定进行目标识别、Loader执行、完整备份和小型应用烧录。
- 当前只支持MSPM0G3507 MAIN Flash，不支持NONMAIN或自动解锁。
- 当前不是CMSIS-DAP/GDB调试器。
- 尚未完成所有保护目标、目标掉电、擦写中断电和长期Flash耐久测试。
- 短镜像会擦除其触及的完整sector；同一sector内没有重新写入的字节为`0xFF`。
- 测试日志中的COM号是当时Windows枚举结果，不应作为其他计算机的固定值。
