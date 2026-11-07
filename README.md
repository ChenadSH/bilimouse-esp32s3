# BiliMouse — 躺床看 B 站视频的无线遥控鼠标

> 基于 **立创·实战派 ESP32-S3** 开发板（LCKFB-SZPI-ESP32-S3-VA）的 BLE HID 无线遥控器。
> 躺在床上不用伸手够键鼠，按一下就能切换 B 站视频。

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.2-blue) ![License](https://img.shields.io/badge/license-Unlicense%20%7C%20CC0--1.0-green)

---

## 这是什么

一块能当无线鼠标 + 遥控器用的开发板：

- **BLE HID 复合设备** —— 电脑同时识别为鼠标、键盘、媒体遥控器
- **单键多义** —— 一个按键表达四种意图（单击/双击/长按/超长按）
- **体感手势** —— 甩腕快进、翻转播放/暂停、倾斜调音量
- **屏幕状态反馈** —— 2.0 寸 IPS 屏常驻显示连接状态与刚触发什么动作
- **零外部依赖** —— 自研轻量 UI 渲染器 + 本地生成中文点阵字库，离线可编译

---

## 快速开始

### 1. 编译与烧录

```bash
# Windows（Git Bash）
tools/idf.bat "D:\path\to\bilimouse-esp32s3" build
tools/idf.bat "D:\path\to\bilimouse-esp32s3" -p COM5 flash monitor

# Linux / macOS
source tools/env.sh
idf.py build && idf.py -p /dev/ttyUSB0 flash monitor
```

> `tools/idf.bat` 已适配本机环境（IDF v6.0.2 + xtensa-esp-elf 15.2.0），
> 并自动绕过 MSYS 环境限制。详见 `tools/env.bat`。

### 1.5 不装 ESP-IDF：macOS / Linux 直接用 esptool 烧仓库里的 bin

仓库的 `ota/` 只有 **app 镜像**，`ota/factory/` 才是启动引导。**四个文件缺一不可**，
而且 app 的偏移是 **0x20000**，不是 0x0：

```bash
# 1) 装 esptool（只需一次）
pip3 install --user esptool

# 2) 看串口名：macOS 形如 /dev/cu.usbserial-1420 或 /dev/cu.wchusbserial1420
ls /dev/cu.*

# 3) 完整烧录（先 cd 到仓库根目录）
python3 -m esptool --chip esp32s3 -p /dev/cu.usbserial-1420 -b 460800 \
  --before default-reset --after hard-reset write_flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0     ota/factory/bootloader.bin \
  0x8000  ota/factory/partition-table.bin \
  0x10000 ota/factory/ota_data_initial.bin \
  0x20000 ota/bilimouse.bin
```

| 偏移 | 文件 | 漏了会怎样 |
|---|---|---|
| `0x0` | bootloader.bin | **黑屏**，串口一个字都不打印（ROM 找不到二级引导） |
| `0x8000` | partition-table.bin | 黑屏或 `invalid magic number` 反复重启 |
| `0x10000` | ota_data_initial.bin | 卡在旧固件 / 引导到空槽，屏幕停在开机第一帧 |
| `0x20000` | bilimouse.bin（app） | 前面三个对、这个写错偏移 → 黑屏或跑旧版本 |

> ⚠️ **常见错误**：把 `bilimouse.bin` 写到 `0x0`（多数教程对单片 app 用 0x0，
> 但本项目的 app 在 `ota_0` = 0x20000）。写到 0x0 会直接覆盖 bootloader，
> 设备从此既不启动也没串口输出，必须按上表四个文件全烧才能救回来。
>
> 只更新固件、不需要救砖时，可以只烧 app：
> ```bash
> python3 -m esptool --chip esp32s3 -p /dev/cu.usbserial-1420 -b 460800 \
>   write_flash 0x20000 ota/bilimouse.bin
> ```

### 2. 配对

1. 烧录后上电，屏幕显示「未连接」
2. 在电脑 **设置 → 蓝牙和设备 → 添加设备 → 蓝牙**
3. 搜索 **BiliMouse** → 配对（Windows 下会提示「输入设备 PIN」，留空直接回车即可）
4. 屏幕顶栏变绿、显示「已连接」

#### 连不上时怎么办

蓝牙配对是**双方各存一把钥匙**。只要一边换了钥匙（重刷固件、改了安全参数），
另一边还拿着旧钥匙，就会表现为「连一下立刻断」或「点了连接没反应」。
设备现在会在连续 3 次配对失败后自动清除本机密钥，屏幕提示「配对失败」，
但**电脑端的旧记录必须手动删掉**，否则永远不会成功：

| 系统 | 操作 |
|---|---|
| macOS | 系统设置 → 蓝牙 → BiliMouse 右侧 `⋯` → **移除设备**；再把蓝牙开关关掉 10 秒后重开，然后重新配对 |
| Windows | 设置 → 蓝牙和设备 → BiliMouse → **删除设备**，再重新「添加设备」 |
| 通用 | 设备端串口执行 `idf.py erase-flash` 可连同配对记录一起清空（**注意：会连校准数据和红外学习码一起物理抹掉**；只想清配对请用上表前两种方式） |

> **烧录不会丢数据**：`idf.py flash` 只写 bootloader / 分区表 / app，
> 不动 `nvs`（0x9000）和 `irbank`（0x420000），所以校准、设置、红外学习码
> 重新烧录固件后都还在。会丢的只有两种操作：
> ① `idf.py erase-flash`（整片擦除）；② 改动 `partitions.csv` 里 `nvs` 的 offset。
> （v2.3 升级到 OTA 分区表时 `irbank` 从 0x200000 挪到 0x420000 了一次，
> 分区镜像会重建，但红外码的主副本在 NVS 里，不受影响。）

排查用的关键日志（`idf.py monitor`）：

```
adv data set ok                          → 广播数据配置成功
advertising started as 'BiliMouse'       → 广播真的跑起来了（没有这行=电脑搜不到）
BLE connected: xx:xx:...                 → 链路已建立
pairing success                          → 加密完成，可以发报告
pairing failed, reason=0x??              → 密钥对不上，去电脑上移除设备
```

如果 `advertising started` 这行缺失，说明广播没起来——设备在电脑上相当于不存在。

### 3. 触摸校准（首次开机自动进行）

触摸面板的坐标方向和缩放**因装配而异**，写死公式必然踩坑，所以设备用三点校准：

1. 首次开机屏幕出现「触摸校准」，一个红框出现在左上
2. **按住红框中心**（不是轻点，按住约 0.1 秒再松），松手后自动跳到下一个点
3. 依次点完 左上 → 右上 → 左下 三个点
4. 屏幕进入验证界面：手指在屏上滑动，十字准星应该跟着指尖走
5. 6 秒后自动返回，映射存 NVS，以后开机不用再做

> 任何时候想重做：**长按实体键 4 秒** → 松开即重新进入校准。
> 若 30 秒内没检测到任何触摸，向导会自动放弃并回到主界面（不会把设备锁死）。

### 4. 校准「下一集」按钮（可选，但强烈建议）

B 站网页端的「下一集」按钮**没有官方键盘快捷键**，只能靠鼠标点击。
设备用「增量路径」方案解决：记录一段位移，重放即可命中。

1. 在电脑上打开 B 站视频页面，把鼠标停在播放器右下角的**「下一集」按钮**上
2. 设备上 **超长按**（按住 1.5 秒）进入鼠标模式
3. 鼠标模式下 **双击** 进入校准界面，屏幕出现「下一集 / 上一集 / 完成」三个按钮
4. 点「下一集」→ 设备记录起点，屏幕提示「用鼠标移到下一集后点此确认」
5. **用设备屏幕上滑动**（或直接动电脑鼠标）把光标移到「下一集」按钮上
6. 再点一次「下一集」→ 记录完成，屏幕显示「下一集 已记录」
7. 点「完成」退出

校准数据存 NVS，**断电不丢**，只需做一次。

> 未校准时的行为：单击会回退到键盘 `]` 键（对多 P / 分集视频有效），
> 屏幕会提示「未校准，用键盘」。

---

## 操作速查

### 实体按键

| 操作 | 效果 |
|---|---|
| **按一下** | 空格键（B 站播放/暂停） |
| **按住 5 秒** | 清空本机蓝牙配对密钥并重启（"搜不到设备"时的自救） |

### 屏幕（触摸）

| 操作 | 动作 |
|---|---|
| 任意空白处**拖动** | 移动主机鼠标指针（自适应增益：微调 1:1，快速滑动 2 倍） |
| **轻点一下** | 左键单击（两种模式都生效） |
| **快速点两下** | 双击（由主机判定，两次点击间隔 < 系统双击窗口即可） |
| **上一集 / 下一集 / 播放暂停** | 对应动作（未校准时回退到键盘 `[` `]` 空格） |
| **音量- / 音量+ / 全屏** | 对应动作 |
| **鼠标** | 切换遥控 / 鼠标模式（长按 = 触摸校准） |
| **红外** | 红外学习 / 发射 / 切槽（10 槽） |
| **校准** | 重新校准触摸（只有主动点它才会进入） |
| **升级** | 检查固件更新（OTA），有新版自动下载重启 |

### 鼠标模式

| 操作 | 动作 |
|---|---|
| 屏幕滑动 | 触控板移动指针（微调 1:1，快滑 2 倍） |
| 轻点空白处 | 左键单击；快速点两下 = 双击 |
| 左右两侧「左键 / 右键」 | **按住型**：按下即按住，抬起才松开；按住后拖动 = 拖拽 |
| 中下「遥控」按钮 | 回到遥控模式 |

### 红外码备份与 OTA（串口）

`idf.py monitor` 里可用串口命令（详见 **docs/OTA.md**）：

| 命令 | 作用 |
|---|---|
| `ir_export all` | 导出全部红外码（每槽一行 base64，可存为备份文件） |
| `ir_import <slot> <b64>` | 恢复一个槽 |
| `wifi set <ssid> <pass>` | 配置 OTA 用的 WiFi |
| `ota url <manifest 地址>` | 配置升级服务器 |
| `ota check` | 手动检查更新 |

### 体感手势

已停用。躺在床上翻身、拿放设备都会误触发，实测弊大于利；
`imu_gesture` 模块保留在代码里，需要时把 `app_main.c` 里的 `imu_task` 加回来即可。

## 硬件

### 开发板资源

| 功能 | 引脚 | 说明 |
|---|---|---|
| I2C SDA / SCL | GPIO1 / GPIO2 | PCA9557 / FT6336 / QMI8658 / 音频芯片共用 |
| LCD SCK / MOSI | GPIO41 / GPIO40 | SPI3 |
| LCD DC / RST | GPIO39 / GPIO38 | |
| LCD 背光 | GPIO42 | 低电平点亮，LEDC PWM 调光 |
| LCD 片选 | PCA9557 IO0 | **必须先经 IO 扩展拉低，否则屏幕无响应** |
| 用户按键 | GPIO0 | 低电平按下（BOOT 键复用） |
| 板载 LED | GPIO48 | |

### 关键硬件坑位（踩过的坑）

1. **PCA9557 上电默认 IO 为输入**，LCD_CS 悬空 → 屏幕完全无响应。
   初始化必须先写 `OUTPUT=0x05`、`CONFIG=0xF8`，再拉低 IO0 释放屏幕。
2. **ST7789 方向参数**：横屏 320×240 需 `swap_xy(true)` + `mirror(true, false)` + `invert_color(true)`。
3. **背光是低电平点亮**，LEDC 输出需取反。

---

## 工程结构

```
bilimouse-esp32s3/
├── docs/
│   ├── PRD.md                 产品需求文档
│   └── HARDWARE.md            硬件引脚与坑位
├── main/
│   ├── app_main.c             任务编排
│   ├── app/
│   │   ├── button_fsm.[ch]    单键多义手势状态机（纯逻辑，可主机测试）
│   │   ├── actions.[ch]       语义动作 -> HID 报告映射
│   │   ├── pointer.[ch]       虚拟光标（解决相对位移定位难题）
│   │   ├── calibrate.[ch]     校准流程状态机
│   │   ├── imu_gesture.[ch]   体感手势识别
│   │   ├── ir_remote.[ch]     红外学习/回放（10 槽）
│   │   ├── ir_store.[ch]      红外码 irbank 分区 A/B 双扇区存储
│   │   ├── ir_backup.[ch]     红外码串口导出/导入
│   │   ├── ota_update.[ch]    WiFi + manifest 检查 + esp_https_ota 升级
│   │   ├── cli.[ch]           串口命令行 REPL
│   │   └── settings.[ch]      NVS 配置持久化
│   ├── bsp/
│   │   ├── bsp_pca9557.[ch]   IO 扩展芯片
│   │   ├── bsp_lcd.[ch]       ST7789 + 背光 PWM
│   │   ├── bsp_touch.[ch]     FT6336 触摸
│   │   ├── bsp_imu.[ch]       QMI8658 IMU
│   │   ├── bsp_ir.[ch]        RMT 红外收发
│   │   └── bsp_button.[ch]    GPIO0 按键 + 消抖
│   ├── ui/
│   │   ├── ui.[ch]            自研轻量渲染器
│   │   └── ui_font.[ch]       点阵字库（由 tools/gen_font.py 生成）
│   └── hid/
│       ├── hid_device.[ch]    HID 应用层（状态机 + 报告发送）
│       └── esp_hidd_*.c/h     ESP-IDF 官方 BLE HID profile（Unlicense）
├── tests/
│   ├── test_button_fsm.py     手势状态机单元测试
│   └── test_ir_store.py       红外 A/B 扇区存储的掉电/擦除用例
├── tools/
│   ├── gen_font.py            中文字库生成器
│   ├── env.bat / env.sh       ESP-IDF 环境
│   └── idf.bat                构建封装
└── main/ui/ui_font.c          自动生成，勿手改
```

---

## 测试

```bash
# 手势状态机单元测试（15 项）
python tests/test_button_fsm.py

# 红外码存储（A/B 扇区、掉电、CRC）6 项
python tests/test_ir_store.py
```

测试会自动选择编译后端：

1. 系统 `gcc`（有则直接跑）
2. Docker 容器内的 `gcc`（本机已验证可用）
3. ESP-IDF 交叉编译器（仅做语法检查）

覆盖 PRD 验收标准：四种手势正确区分、双击不触发单击、
长按只执行一个动作、超长按覆盖长按、无效按压无动作。

---

## 技术选型说明

| 项 | 选择 | 理由 |
|---|---|---|
| 框架 | ESP-IDF v6.0.2 | 本机已装；蓝牙栈成熟 |
| 蓝牙 | Bluedroid BLE HID | HID over GATT 官方支持最完整 |
| HID 实现 | 复用官方 `ble_hid_device_demo` profile | 避免自研 GATT 服务踩坑 |
| UI | 自研渲染器 + 本地字库 | **不引 LVGL**：省 98MB 依赖、编译快、可离线 |
| 字体 | `tools/gen_font.py` (PIL + 微软雅黑) | 中文字形准确，体积仅 27KB |

**固件体积**：800 KB Flash / 119 KB RAM（预算 2MB / 200KB）。

---

## 已知限制

- **校准后不要大幅移动电脑鼠标**，否则点击会偏移（BLE 鼠标是相对位移，这是协议本身的限制）
- Windows 配对时可能提示输入 PIN，留空回车即可
- 体感手势在设备剧烈晃动时可能误触发，已加 800ms 冷却 + 静止抑制
- B 站 UI 改版后「下一集」按钮位置变化，需重新校准

---

## 许可

- `main/hid/esp_hidd_prf_api.*`、`hid_dev.*`、`hid_device_le_prf.c`、`hidd_le_prf_int.h`
  来自 ESP-IDF 官方示例，遵循 **Unlicense OR CC0-1.0**
- 其余代码遵循 MIT
