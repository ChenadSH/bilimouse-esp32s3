# 硬件参考 — 立创·实战派 ESP32-S3

> 型号：**LCKFB-SZPI-ESP32-S3-VA**
> 主控：ESP32-S3-WROOM-1-N16R8（16MB Flash / 8MB Octal PSRAM / 240MHz 双核）
> 官方文档：https://wiki.lckfb.com/zh-hans/szpi-esp32s3/

---

## 1. 整机资源

| 类别 | 型号 / 参数 |
|---|---|
| 显示屏 | ST7789，2.0 寸 IPS，320×240，SPI 接口 |
| 触摸屏 | FT6336 电容触摸，I2C 接口 |
| 姿态传感器 | QMI8658 三轴加速度 + 三轴陀螺仪，I2C 接口 |
| 音频 | ES8311 DAC（I2C）/ ES7210 ADC（I2C）/ NS4150B 功放 / 1W 喇叭 |
| 摄像头 | GC0308 30 万像素（DVP） |
| IO 扩展 | PCA9557（I2C） |
| USB | CH334F HUB + CH340K 串口芯片，Type-C 一线搞定供电/下载/调试 |
| 存储 | TF 卡（1-bit SD 模式） |
| 扩展接口 | 2× GH1.25（I2C / 多功能 GPIO10、11） |
| 按键 | 1 复位键 + 1 用户键（BOOT 复用） |

---

## 2. 引脚分配

### I2C 总线（全部低速外设共用）

| 信号 | GPIO | 挂载设备 |
|---|---|---|
| SDA | **GPIO1** | PCA9557 / FT6336 / QMI8658 / ES8311 / ES7210 |
| SCL | **GPIO2** | 同上 |

> ⚠️ 此总线不可复用为其它功能（如 SPI/PWM）。

### LCD（ST7789，SPI3）

| 信号 | GPIO | 备注 |
|---|---|---|
| SCK | GPIO41 | |
| MOSI | GPIO40 | |
| DC | GPIO39 | |
| RST | GPIO38 | |
| BL | GPIO42 | **低电平点亮**，LEDC 通道 0 PWM 调光 |
| CS | **PCA9557 IO0** | 不是 GPIO！必须经 IO 扩展控制 |

### 摄像头（GC0308，DVP）— 本项目未使用

XCLK=GPIO5, VSYNC=GPIO3, HREF=GPIO46, PCLK=GPIO7,
D0=GPIO16, D1=GPIO18, D2=GPIO8, D3=GPIO17, D4=GPIO15, D5=GPIO6, D6=GPIO4, D7=GPIO9

### 音频 — 本项目未使用

I2S: MCLK=GPIO38, WS=GPIO13, BCLK=GPIO14, DIN=GPIO12, DOUT=GPIO45

### 其他

| 功能 | GPIO |
|---|---|
| 用户按键（BOOT 复用） | **GPIO0**，低电平按下 |
| 板载 LED | GPIO48 |
| 扩展接口 | GPIO10, GPIO11 |
| 下载模式 | IO46 必须保持低电平 |

---

## 3. PCA9557 IO 扩展

| IO | 用途 | 有效电平 |
|---|---|---|
| IO0 | LCD_CS 液晶片选 | **低电平有效** |
| IO1 | PA_EN 功放使能 | 高电平开启 |
| IO2 | DVP_PWDN 摄像头掉电 | 高电平关闭 |

**I2C 地址**：`0x19`

**寄存器**：

| 寄存器 | 地址 | 说明 |
|---|---|---|
| Input port | 0x00 | 只读 |
| Output port | 0x01 | 读写 |
| Polarity inversion | 0x02 | 读写 |
| Configuration | 0x03 | 读写，1=输入 0=输出 |

**初始化序列**（漏了会导致屏幕无响应）：

```c
pca9557_write(PCA9557_REG_OUTPUT,  0x05);  // IO0=H(LCD禁用) IO1=0 IO2=H(摄像头关)
pca9557_write(PCA9557_REG_CONFIG,  0xF8);  // IO0~IO2 输出，IO3~IO7 输入
// ... 面板初始化 ...
pca9557_set_pin(0, false);                // IO0 拉低，释放 LCD 片选
```

---

## 4. QMI8658 姿态传感器

**I2C 地址**：`0x6A`

### 寄存器

| 名称 | 地址 | 说明 |
|---|---|---|
| WHO_AM_I | 0x00 | 读回 `0x05` |
| REVISION | 0x01 | |
| CTRL1 | 0x02 | bit6 = 地址自增 |
| CTRL2 | 0x03 | bit6:4 加速度量程，bit3:0 加速度 ODR |
| CTRL3 | 0x04 | bit6:4 陀螺量程，bit3:0 陀螺 ODR |
| CTRL5 | 0x06 | 低通滤波 |
| CTRL7 | 0x08 | bit0 = 加速度使能，bit1 = 陀螺使能 |
| STATUS0 | 0x2E | bit0 加速度就绪，bit1 陀螺就绪 |
| TEMP_L/H | 0x33/0x34 | 温度 |
| AX_L/H | 0x35/0x36 | 加速度 X |
| AY_L/H | 0x37/0x38 | 加速度 Y |
| AZ_L/H | 0x39/0x3A | 加速度 Z |
| GX_L/H | 0x3B/0x3C | 陀螺 X |
| GY_L/H | 0x3D/0x3E | 陀螺 Y |
| GZ_L/H | 0x3F/0x40 | 陀螺 Z |
| RESET | 0x60 | 写 `0xB0` 软复位 |

### 本项目配置

| 项 | 值 | 对应位 |
|---|---|---|
| 加速度量程 | ±4g | CTRL2 = 0x10 |
| 加速度 ODR | 200Hz | CTRL2 = 0x04 |
| 陀螺量程 | ±1024 dps | CTRL3 = 0x60 |
| 陀螺 ODR | 200Hz | CTRL3 = 0x04 |
| 使能 | 加速度 + 陀螺 | CTRL7 = 0x03 |

**刻度系数**：`acc = raw × 4/32768`，`gyro = raw × 1024/32768`

**连续读取**：从 `TEMP_L(0x33)` 起连读 14 字节 = 温度(2) + 加速度(6) + 陀螺(6)。
需先开 CTRL1 的地址自增位（bit6）。

---

## 5. FT6336 触摸控制器

**I2C 地址**：`0x38`

| 寄存器 | 地址 | 说明 |
|---|---|---|
| Device Mode | 0x00 | 写 0x00 进入工作模式 |
| Gesture ID | 0x01 | |
| TD_STATUS | 0x02 | 低 4bit = 第 1 触点状态（0 = 无触摸） |
| TP1_XH | 0x03 | X 高 4bit |
| TP1_XL | 0x04 | X 低 8bit |
| TP1_YH | 0x05 | Y 高 4bit |
| TP1_YL | 0x06 | Y 低 8bit |
| TP1_WEIGHT | 0x07 | |
| TP2_* | 0x09~0x0E | 第二触点 |
| CHIP_ID | 0xA3 | |

**坐标拼接**：`x = ((XH & 0x0F) << 8) | XL`（12 位，0~4095）

**方向映射**：**不做硬编码换算**，改由 `app/touch_cal.c` 的三点校准矩阵处理。

原因：面板装配方向、坐标单位（12bit 还是像素）因批次而异，写死公式一旦不对，
现象是"能读到触摸但点哪儿都不准"，极难排查。校准做法是让用户点左上/右上/左下三个点，
用三组「屏幕坐标 ↔ 原始坐标」解出仿射矩阵，旋转/镜像/缩放全部由矩阵吸收。

**踩过的坑**：

- 按 12bit（0~4095）换算 `raw * 320 / 4096`，若面板实际输出像素坐标（0~239），
  坐标会被缩小 12.8 倍，所有触摸都挤到左下角一小块 → "点哪儿都不灵"。
- 400kHz 下和 QMI8658/PCA9557 共用总线时，一次连读 5 字节可能只回第一字节、
  坐标全 0（状态却正常）。触摸已单独降到 100kHz，并保留"连读 / 拆分读"两种方式自适应切换。

---

## 6. 已知坑位清单

| # | 现象 | 原因 | 解法 |
|---|---|---|---|
| 1 | 屏幕完全无响应 | PCA9557 IO0 默认输入，LCD_CS 悬空 | 初始化时先配 CONFIG 再拉低 IO0 |
| 2 | 屏幕花屏/镜像 | ST7789 方向参数不对 | `swap_xy(true)` + `mirror(true,false)` + `invert_color(true)` |
| 3 | 背光不亮 | 背光是低电平点亮 | LEDC 输出取反 `output_invert = true` |
| 4 | 触摸无反应/坐标反了 | 面板竖装需旋转 90° | 见上方方向映射公式 |
| 5 | 编译报 `CONFIG_FATFS_USE_LABEL` 未定义 | IDF v6.0.2 fatfs 组件需显式声明该 Kconfig | 在 `sdkconfig.defaults` 显式列出所有 FATFS 选项 |
| 6 | 编译报 `unused-label 'fail'` | 同上，fatfs 已知缺陷 | 根 `CMakeLists.txt` 加 `idf_build_set_property(COMPILE_OPTIONS "-Wno-unused-label" APPEND)` |
| 7 | `idf.py` 报 "MSys/Mingw is no longer supported" | Git Bash 继承了 MSYSTEM 环境变量 | 包装脚本里清空 `MSYSTEM` / `MSYS2_PATH_TYPE` / `MINGW_PREFIX` |
| 8 | `idf.py` 报 constraints 文件不存在 | 新版 ESP-IDF Manager 把 constraints 放在 `tools/` 下 | 设 `IDF_PYTHON_CHECK_CONSTRAINTS=0` |
| 9 | `ESP_IDF_VERSION` 报错 | 该变量不能带 `v` 前缀 | 用 `6.0.2` 而非 `v6.0.2` |

---

## 7. 参考资料

- 立创开发板技术文档中心：https://wiki.lckfb.com/zh-hans/szpi-esp32s3/
- 开源硬件平台（原理图/BOM）：https://oshwhub.com/li-chuang-kai-fa-ban/li-chuang-shi-zhan-pai-esp32-s3-kai-fa-ban
- QMI8658 寄存器资料：ESPHome / RIOT OS / NuttX 的 QMI8658 驱动实现（互相印证）
- FT6336 寄存器资料：ft6336u-driver (docs.rs) 等开源驱动
