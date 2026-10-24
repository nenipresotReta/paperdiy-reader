# 功耗行为

[English](power.md) | [简体中文](power.zh-CN.md)

本文记录已实现的空闲功耗策略、源码审查和验证结果。已检查 USB 连接状态下的运行，以及一次短时电池睡眠/唤醒；长时续航及外设验证仍未完成。没有电流表，未测量电流或续航收益。成功唤醒本身不能证明省电。

## 策略

无活动一秒后，将 CPU 从 240 MHz 降至 80 MHz。MSC 独占存储例外：进入时升到 240 MHz，直到明确的主机弹出及原始驱动成功释放前始终保持，包含请求退出和 Fault 状态。原始 SD 回调运行在 USB 任务，应用字节计数轮询无法安全决定降频时机。这一保守策略避免原始 I/O 期间切换时钟，使首批请求延迟更可预测，代价是连接空闲时 CPU 功耗更高。它不能确定已观察 USB 介质故障的原因，功耗代价也未测量。

使用电池时，另进入带一秒定时器兜底的手动轻睡眠。USB 或其他外部供电下只降频，保持处理器清醒，以便枚举、串口和固件上传。活动 AP/STA 同样禁止会中断 Wi-Fi 的手动轻睡眠，但保留 CPU 空闲降频。离开设置关闭无线电，恢复普通电池睡眠策略。

N 只在明确选择期间运行 TLS 监听。离开 N 关闭监听及连接、丢弃未完成接收、保留结果不确定的发布意图，并请求关闭 Wi-Fi。关闭失败或 SDK 仍报告无线电活动时，继续禁止睡眠。离线发送队列不会唤醒或启用阅读器。这些生命周期检查不等于测得无线电关闭电流。

处理输入、加载书籍或刷新屏幕前恢复 240 MHz。

只在应用循环边界进入睡眠：不能处于加载、显示更新、存储操作、卡去抖、按键按住、组合键或输入松开保护期。先等待显示工作完成。定时器唤醒只处理电源/按键/卡状态，不重画场景，之后可再次睡眠。检查所有唤醒源配置及睡眠返回值；睡眠被拒绝时恢复普通输入处理，不能忙循环重试。

| 输入 | 唤醒条件 |
| --- | --- |
| A，GPIO4 | 低电平 |
| B，GPIO3 | 低电平 |
| 卡检测，GPIO48 | 稳定电平的相反值：拔卡高电平、插卡低电平 |
| 定时器 | 一秒；检查外部供电及 PMIC 按钮事件 |

三个 GPIO 使用 `gpio_wakeup_enable()` 与 `esp_sleep_enable_gpio_wakeup()`。卡检测仍走应用去抖，转换未稳定时不睡眠。唤醒后继续普通按钮状态机，不重置，也不调用会吞掉唤醒按键的 `discardUntilReleased()`。长按计时期间不睡眠。

固定版本 M5Unified 从 M5PM1 寄存器 `0x42` 读取电源按钮事件，没有为 PaperDIY 配置 `_wakeupPin`，因此默认 `Power.lightSleep()` 唤醒配置不足。定时器用于轮询 PMIC 事件，不改变电源键独立的硬件复位或关机行为。

没有自动关机。轻睡眠保留已加载书籍、页索引、帧缓冲和导航状态，不应每次停顿后重启应用、重挂未变化 SD 或重载书籍。

### 睡眠前后的卡中断

清醒时，GPIO48 边沿中断即使在长同步导入阻塞应用循环时也会撤销源验证证明。固定 ESP-IDF 4.4.7 ESP32-S3 后端的 `gpio_ll_wakeup_enable()` 写入与该中断共用的 `pin.int_type`，而禁用唤醒只清除 `wakeup_enable`。若保留唤醒电平类型，清醒卡 ISR 会变成持续触发的电平中断。

应用任务的 `card_sleep::Window` 在配置相反唤醒电平前禁用卡中断。普通唤醒、取消、配置失败和睡眠失败都先禁用唤醒、恢复 `GPIO_INTR_ANYEDGE`，再启用中断。恢复边沿模式失败时保持中断禁用并撤销存储租约。实体电平变化请求普通卡去抖；定时器/按键唤醒不伪造插卡事件。

每次睡眠尝试也用 `sourceChanged()` 丢弃内存源指纹及驻留重开证明，覆盖 ISR 禁用期间的拔出/插回。已加载页面和页表仍可使用，但睡眠后重开书籍须重新验证源。在硬件能识别该期间所有卡变化前，不保留跨睡眠的验证证明。

主机 `card_sleep` 测试覆盖两种卡电平、正常/取消/失败窗口、每种 GPIO 适配器失败、清理顺序及证明撤销。构建后的卡回调只在 IRAM 中向 DRAM 原子字节写入，不分配内存、不调用运行时初始化。固定 Arduino GPIO 分发器本身位于 Flash，不能声称整个 GPIO 服务均驻留 IRAM。

诊断构建提供 `powerDiagnosticSleep(1..1000)`，仅用于明确请求 USB 连接下的短睡眠。它使用相同生产保护，只有睡眠及中断恢复都成功才返回成功。应在显示、无线电空闲时使用，再检查 GPIO48 中断类型、存储租约和保留页面。普通 USB/外部供电的睡眠阻止规则不变；该诊断不能单独证明电池唤醒或功耗。

## 电池图标更新

书库采样间隔不变。仅当 18 像素内部填充变化时重画；百分比或充电状态变化若产生相同像素，不请求显示更新。即使不刷新，也继续追踪最新放电估计。首次绘制、场景或方向变化仍完整绘制图标。[主机比较](research-2026-10.zh-CN.md)验证减少请求后像素轨迹相同，未测电流/能量收益。

## USB 与供电来源检测

USB 构建标志选择传输：`ARDUINO_USB_MODE=1` 时 `Serial` 是 USB Serial/JTAG `HWCDC`，无 USB 存储；`ARDUINO_USB_MODE=0` 时原生 TinyUSB 提供 CDC 和明确启用的 MSC 介质。两者是不同固件，须核对构建配置；功耗代码通过 `usb_transport` 统一访问。

HWCDC 版本的 `HWCDC::isPlugged()` 检测主机 SOF，而 `bool(Serial)` / `isCDC_Connected()` 还依赖 CDC 活动，不能保护终端已关闭但仍连接的电脑。休眠主机或纯充电电源可能完全没有 SOF。原生版本在 TinyUSB 已配置或挂起时禁止睡眠；MSC 独占期间即使总线复位或断开也继续禁止。只有接受明确主机弹出且成功移除原始驱动后才允许本地 FAT 重挂。空闲、拔线和总线复位都不能授予该权限。主机持卡期间禁用 CDC 自动重启，恢复与应用关机须等待安全弹出。维持 240 MHz 不依赖扇区活动；没有 MSC 所有权的普通 CDC 仍可降至 80 MHz。

睡眠前对 M5PM1 `PWR_SRC`（`0x04`）执行检查成功与否的寄存器读取：bit 0 为 5VIN，bit 1 为 5VINOUT，bit 2 为已供电电池节点。必须读取成功、确定是电池、没有外部来源，且 USB 传输不阻止睡眠。失败或含糊结果都保持清醒；进入前再次检查主机连接。若睡眠中插入 USB，定时器使下一次供电检查约在一秒内发生。睡眠中插线后的枚举仍须实机验收。

下方旧电池/USB 结果只描述当时测试的固件。当前原生 USB 流程验收及未决硬件结果统一见[架构报告](architecture-and-implementation.md)，不能据此证明 USB 挂起行为或功耗。

MSC 读写回调先释放存储互斥锁，再至少等待一个 RTOS tick，成功、失败、锁忙都如此，为其他任务提供调度机会；这不会抢占单次同步 SD 操作。带校验和门控的 PlatformIO 中间件编译构建目录内的 Arduino SD 源码副本，使 `sdcard_uninit()` 在释放 card 前结束 SPI 保护对象生命周期，不修改共享框架。这些源码修复不能确定某次实体掉线的原因。

应用控制台输出为尽力而为。原生 CDC 适配器有限次尝试 TinyUSB FIFO，避免固定 SDK `USBCDC::write()` 缓冲区满时无界循环。等待期间会让出调度：无进展上限 100 ms、单次写入上限两秒，`SCREEN` / `PAGE_TEXT` 批量帧总上限十秒。长导入等诊断计算不计入帧期限。帧失败后不再逐行等待；完整截图才发 `SCREEN_DONE`，中断后尝试发 `SCREEN_ABORTED`。主机在期限内持续读取可获得完整帧。丢日志不弹出存储，也不证明同步。RX、串口初始化和所有权相关的重启保护仍使用原路径；CDC 重开验收见上述硬件记录。

不能从 `isCharging()` 推断 USB 不在：充满时可能不充电但线仍连接。也不能把 `getPowerSource()` / `getVBUSVoltage()` 返回零当成成功的缺席检查，这些便捷方法在通信失败时也返回零。

## 状态保留与时钟

源码审查使用 PlatformIO `espressif32@6.12.0`、Arduino-ESP32 2.0.17（`framework-arduinoespressif32` 包 `3.20017.241212+sha.dcc1105b`）、ESP-IDF **4.4.7** 和 `dio_opi` SDK 配置：

- `CONFIG_PM_ENABLE` 及 FreeRTOS tickless idle 未启用，因此使用明确轻睡眠，不使用自动 PM。
- OPI PSRAM 为 80 MHz；启用 PSRAM/Flash 漏电规避及 MSPI 上拉规避。
- `CONFIG_FREERTOS_HZ=1000`，任务看门狗五秒。
- Arduino `millis()` 使用 `esp_timer_get_time()`，轻睡眠后按 RTC 经过时间推进。周期 `esp_timer` 回调可能在唤醒后补执行，适合时应设置 `skip_unhandled_events`。
- 受支持的 80/240 MHz CPU 档共用 480 MHz PLL，使用不同 CPU 分频。APB 维持 80 MHz，不重配 80 MHz MSPI。使用并检查 `setCpuFrequencyMhz()` 返回值；显示/存储工作中不切频，低于 80 MHz 需另行审查。

保持 PSRAM、Flash、SD 卡和 ESP32 电源。不要强关 `ESP_PD_DOMAIN_VDDSDIO`、禁用 PMIC DCDC 电源轨或全局隔离/保持 GPIO。SD 总线空闲时 CS 保持高；空闲路径不关闭或重新初始化 SD。

CPU 唤醒不调用 `M5.Display.wakeup()`：轻睡眠不是显示重初始化。保留帧缓冲与刷新策略，由显示驱动管理面板。

M5PM1 I2C 空闲睡眠保持禁用，其唤醒后首次 I2C 事务可能失败。启用前需独立审查所有寄存器读写与电源轨保留。

## 来源

以下是源码证据，并非本阅读器测量：

- ESP-IDF 4.4.7: [sleep modes](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/system/sleep_modes.html),
  [timer behavior](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/system/esp_timer.html),
  and [USB Serial/JTAG limitations](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-guides/usb-serial-jtag-console.html).
- ESP-IDF 4.4.7 implementations:
  [`rtc_clk.c`](https://github.com/espressif/esp-idf/blob/v4.4.7/components/esp_hw_support/port/esp32s3/rtc_clk.c),
  [`sleep_modes.c`](https://github.com/espressif/esp-idf/blob/v4.4.7/components/esp_hw_support/sleep_modes.c),
  and [`sleep_gpio.c`](https://github.com/espressif/esp-idf/blob/v4.4.7/components/esp_hw_support/sleep_gpio.c).
- Arduino-ESP32 2.0.17:
  [`HWCDC.cpp`](https://github.com/espressif/arduino-esp32/blob/2.0.17/cores/esp32/HWCDC.cpp),
  [`esp32-hal-cpu.c`](https://github.com/espressif/arduino-esp32/blob/2.0.17/cores/esp32/esp32-hal-cpu.c),
  and [`esp32-hal-misc.c`](https://github.com/espressif/arduino-esp32/blob/2.0.17/cores/esp32/esp32-hal-misc.c).
- M5Unified at `e125e73f28b6b83e621e48df432af426bb2ff935`:
  [`M5Unified.inl`](https://github.com/m5stack/M5Unified/blob/e125e73f28b6b83e621e48df432af426bb2ff935/src/M5Unified.inl),
  [`Power_Class.inl`](https://github.com/m5stack/M5Unified/blob/e125e73f28b6b83e621e48df432af426bb2ff935/src/utility/Power_Class.inl),
  and [`M5PM1_Class.inl`](https://github.com/m5stack/M5Unified/blob/e125e73f28b6b83e621e48df432af426bb2ff935/src/utility/power/M5PM1_Class.inl).
- M5Stack's [PaperDIY power circuitry and PMIC behavior](https://docs.m5stack.com/en/arduino/paperdiy/m5pm1).
  在线硬件资料可能变化；上方 SDK/库结论仅针对固定版本。

## 验证状态

原生 CTest 在 ASan/UBSan 下通过，公开与诊断固件均构建成功。设备验收仅限下方合成诊断契约；电池耐久、SD 热插拔和电气测量仍未完成。

遥测限制：`unknown_source_seen=1` 是黏滞标志，不是读取失败次数。`sourceKnown` 初始为 false，诊断启用/观察可能在同一应用循环首次供电采样前运行，因此该标志可能表示尚未采样或实际 I2C 读取失败；报告无法区分，也不能据此认定驱动故障。运行时睡眠门仍要求成功且经检查的供电来源、电池供电且无外部电源/USB 主机；未知来源阻止睡眠。

公开证据不包含私人书籍、阅读记录、设备图片或 NVS 内容。

## 可选电池状态保留诊断

同时定义 `PAPERDIY_DIAGNOSTICS`、`PAPERDIY_DEVICE_TESTS`，启用 [`tests/device_power.h`](../tests/device_power.h) 的一次性遥测；普通构建不启用。选取可前后翻页的合成文档位置，并在测试中保持同一文档打开。

1. USB 连接时发送 `POWER_ARM`，在主机保留返回的八位十六进制 `nonce`。
2. 拔 USB，至少等待两秒空闲。按下再松开 B 一次，等页面与再次空闲，再按下松开 A 返回。检查每次恰好翻一页。
3. 电池运行至少 60 秒后重连 USB，等待设备及串口至少三秒。
4. 发送 `POWER_REPORT <nonce>`，要求 nonce 匹配且报告完成；`armed`、`stale`、`missing_or_invalid` 都不算通过。

观察窗口从首次观察到睡眠开始；60 秒、睡眠后检测到外部供电或明确 `POWER_FINISH` 均可结束。提前因外部供电结束不能证明运行满 60 秒。不带 nonce 的 `POWER_REPORT` 只读最近结果，不能证明属于本次实验。

检查 `valid_book=1`、`sleep_seen=1`、`battery_sleep_seen=1`，正文及页索引校验和相同，切频/睡眠失败为零。未知来源标志按上方 schema 限制另行解释，不计为 I2C 失败次数。核对最终位置和 A/B 动作数。遥测只查正文/页索引 CRC，帧缓冲保留及可见刷新仍需独立检查。

启用和普通诊断轮询只改 RAM。完成时在独立 NVS 命名空间 `powercheck` 尝试写一份结果，验证大小、schema、CRC 和读回。失败会报告，轮询不重试。报告只读结果，不写偏好；诊断不写阅读书签命名空间，但普通翻页仍正常保存书签。完成前复位会丢失当前实验，必须保留并检查 nonce。

原始报告含书籍标识和阅读位置，不应放入公开日志；只公开相关检查与匿名结果。遥测限制见上。

## 仍需硬件验收

- 数百次电池睡眠/唤醒，检查正文、页偏移和帧缓冲；诊断周期前后比较 PSRAM 校验和。
- 用受控实体输入重测首次按键、长按重复、错开松开 A+B 和空闲后的两秒关机组合键。
- 睡眠中插拔 SD，包含触点抖动，验证挂载/错误路径，不改写书籍。
- USB 连接测试覆盖关闭终端、电脑挂起、充电器及电池充满；睡眠中插 USB，检查枚举、诊断和上传恢复。
- 电池唤醒时检查 80/240 MHz、定时器单调、无看门狗/复位循环、局刷行为不变。
- 有电流表后，在固定卡、页面、充电状态和固件下测电池输入：工作、仅 CPU 空闲、轻睡眠、刷新及关机。记录仪器、持续时间与条件后再谈电流/续航。

新增结果应注明测试固件及条件；上方长时电池、外设、电气检查仍待完成。
