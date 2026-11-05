# BiliMouse OTA 在线升级与红外码备份指南

> 版本 v2.3 · 2026-11-05
> 适用固件：`PROJECT_VER ≥ 2.3.0`（分区表含 `ota_0 / ota_1 / otadata`）

本文覆盖三件事：

1. **红外码备份/恢复**（串口导出导入，不依赖网络）
2. **OTA 升级服务器部署**（一个静态 HTTP 服务即可，无需后端代码）
3. **自动升级的完整逻辑**（设备端判定流程、安全网、失败排查）

---

## 0. 一次性准备：刷入 OTA 版分区表

v2.3 之前的固件用的是单 app 分区表，**必须先按下面步骤刷一次**，
之后才能使用 OTA：

```bash
tools\build.bat D:\pro\bilimouse-esp32s3 build
idf.py -p COM口 flash
```

`idf.py flash` 会自动写入新分区表 + bootloader + otadata（空）+ app 到 `ota_0`。
**NVS（0x9000）不被触碰**，校准、蓝牙配对、WiFi 配置全部保留；
`irbank` 分区偏移有变化，分区镜像会丢一次，但固件启动时会自动从 NVS
副本重建（这就是双副本设计的用途），**已学的红外码不会丢**。

新分区表（16MB Flash）：

| 分区 | 偏移 | 大小 | 用途 |
|---|---|---|---|
| nvs | 0x9000 | 24K | 配置/校准/配对密钥/**红外码主副本**（偏移永不变） |
| otadata | 0x10000 | 8K | OTA 启动选择器 |
| ota_0 | 0x20000 | 2M | app 槽 A |
| ota_1 | 0x220000 | 2M | app 槽 B（OTA 时写这个） |
| irbank | 0x420000 | 64K | 红外码镜像（A/B 双扇区） |

当前固件 1.49MB，单槽 2MB，剩余约 540KB 增长空间。

---

## 1. 红外码导出 / 导入 / 备份

### 1.1 入口

设备端内置了串口命令行（esp_console REPL），就是日志那个口：

```bash
idf.py -p COM口 monitor
# 看到 "bilimouse> " 提示符后直接敲命令
```

> monitor 里输入命令时日志可能插在中间，不影响命令解析；
> 按 `Ctrl+]` 退出 monitor。

### 1.2 命令一览

| 命令 | 作用 |
|---|---|
| `ir_list` | 列出 10 个槽的状态和码库序号 |
| `ir_export all` | 全部导出（每槽一行） |
| `ir_export 3` | 只导出 3 号槽 |
| `ir_import 3 <b64>` | 把一行 base64 导入 3 号槽 |
| `ir_clear confirm` | 清空全部（危险，需带 confirm） |

### 1.3 备份流程

```
bilimouse> ir_export all
# BiliMouse IR backup, seq=12. Restore one line at a time:
#   ir_import <slot> <b64>
BMIR1 slot=0 syms=67 b64=AQAA...（67 符号的 NEC 码）
BMIR1 slot=1 state=empty
...
```

把输出整体复制存成 `ir_backup.txt` 即完成备份。
**每一行都是自包含的**——以后任何时候（甚至换一块板子），
把需要的行粘贴回去执行 `ir_import <slot> <那行b64>` 就能恢复。

### 1.4 格式说明（供程序化处理）

一行 = `BMIR1 slot=<n> syms=<count> b64=<base64>`，base64 解码后为：

```
[0:2)   count，小端 uint16
[2:2+count*4)  rmt_symbol_word_t 原始数组（每个 4 字节：
        duration0:16 | level0:1.. | duration1:16 | level1:1..，小端）
```

每个 symbol 的 duration0/duration1 是电平持续时间（微秒），
level0/level1 是电平。用 Python 重建 NEC 波形或转成 JSON 都很简单：

```python
import base64, struct
raw = base64.b64decode(b64_part)
count = struct.unpack("<H", raw[:2])[0]
words = struct.unpack(f"<{count}I", raw[2:2+count*4])
for w in words:
    d0, d1 = w & 0xFFFF, (w >> 16) & 0xFFFF
    l0, l1 = (w >> 15) & 1, (w >> 31) & 1   # 电平位（1 bit each）
    print(f"{l0} {d0}us, {l1} {d1}us")
```

导入时固件校验：count ∈ [1,96]、总长匹配，写 NVS + irbank 双副本。

---

## 2. OTA 服务器部署

设备只会发两个 HTTP GET：`manifest.json`（版本清单）和清单里的
固件 bin。**任何能提供静态文件的服务都行**。

### 2.1 最快方式：Python 静态服务器（局域网）

在电脑上建发布目录：

```
publish/
├── manifest.json
└── bilimouse-2.3.1.bin      # build/bilimouse.bin 改名放这里
```

`manifest.json`：

```json
{
  "version": "2.3.1",
  "url": "http://192.168.1.100:8070/bilimouse-2.3.1.bin",
  "notes": "修复xxx"
}
```

> `url` 必须是设备能访问到的地址：用电脑的**局域网 IP**，
> 不是 localhost。版本号必须是 `数字.数字.数字` 三段式。

启动服务（在 publish 目录下）：

```bash
python -m http.server 8070
```

### 2.2 设备端配置（一次性）

`idf.py monitor` 里：

```
bilimouse> wifi set MyHomeWiFi mypassword
ok: credentials saved, connecting...
wifi connected

bilimouse> ota url http://192.168.1.100:8070/manifest.json
ok: manifest url saved
```

WiFi 凭据和 manifest 地址都存 NVS，掉电不丢；
Kconfig 里的 `BILIMOUSE_WIFI_SSID/PASS/OTA_URL` 只是出厂默认值，
被 NVS 覆盖。

### 2.3 触发升级

- **屏幕**：遥控模式 → 次行「升级」按钮；
- **串口**：`ota check`。

### 2.4 长期方案（可选）

| 场景 | 建议 |
|---|---|
| 家里常开 | 路由器/树莓派上跑 nginx 或 caddy 指向 publish 目录 |
| 公网分发 | GitHub Releases + 固定跳转 URL，或对象存储 + CDN |
| 安全要求高 | 换 HTTPS：Kconfig/cert 配置服务器证书，
  `ESP_HTTPS_OTA_ALLOW_HTTP` 改回 n（当前为方便局域网调试已打开） |

---

## 3. 自动升级逻辑（设备端实现）

### 3.1 触发

| 方式 | 入口 | 行为 |
|---|---|---|
| 自动 | 开机 30s 后（`BILIMOUSE_OTA_AUTO_CHECK=y`） | 有新版→直接升级→自动重启；失败**静默** |
| 手动 | 屏幕按钮 / `ota check` | 每一步都有屏幕提示，失败给出原因 |

### 3.2 完整流程

```
ota_run()
 ├─ 1. WiFi 未连接 → 读 NVS/Kconfig 凭据 → esp_wifi_connect
 │       20s 超时 → 失败退出（手动模式屏幕提示）
 ├─ 2. GET manifest.json（10s 超时，HTTP 200）
 ├─ 3. 版本比较 semver_cmp(manifest.version, 当前版本)
 │       当前版本来自固件镜像头 esp_app_desc（CMakeLists 里的 PROJECT_VER）
 │       manifest ≤ 当前 → 「已是最新」退出
 ├─ 4. 发现新版本 → 屏幕显示 "旧版本 → 新版本"
 │     esp_https_ota_begin：选备槽（当前在 ota_0 就写 ota_1）
 │     循环 esp_https_ota_perform 流式下载 → 写 flash
 │       · 读到镜像头后顺带打印新固件自身声明的版本
 │       · 每 1% 更新屏幕 "下载 xx%"，并喂省电看门狗保持亮屏
 │     esp_https_ota_finish：内置镜像校验（magic/芯片/长度）
 │       校验失败 → 中止，旧固件继续运行
 ├─ 5. 成功 → esp_ota_set_boot_partition(备槽)（finish 内部已做）
 │     屏幕倒计时 5 秒 → esp_restart()
 └─ 6. 新固件第一次启动处于 PENDING_VERIFY 状态：
         app_main 走完（UI/蓝牙/红外全就绪）才调
         esp_ota_mark_app_valid_cancel_rollback() 确认；
         如果新固件起不来（崩溃/看门狗重启），
         bootloader 自动回滚到上一个槽 —— 用户最多损失一次重启时间。
```

### 3.3 三道安全网

1. **镜像校验**：esp_https_ota_finish 拒绝损坏/不完整的镜像，
   失败时**不切换启动分区**；
2. **回滚**：`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`。新固件未经
   确认就崩溃 → bootloader 自动退回旧槽。所以升级变砖的最坏结果
   是"回滚到旧版本"；
3. **数据隔离**：OTA 只写 app 槽，NVS（校准/配置/红外码主副本）和
   irbank 分区全程不被触碰。

### 3.4 发布新版本的操作步骤

1. 改两处版本号：`CMakeLists.txt` 的 `PROJECT_VER`、
   `docs/PRD.md` 版本行；
2. 编译：`tools\build.bat D:\pro\bilimouse-esp32s3 build`；
3. 把 `build\bilimouse.bin` 复制进 publish 目录并按版本改名；
4. 更新 `manifest.json` 的 `version` 和 `url`；
5. 完成。在线设备最迟下一次开机 30 秒后自动升级；
   急用就让设备点一下「升级」。

### 3.5 常见问题

| 现象 | 原因/处理 |
|---|---|
| 屏幕显示"未配置 WiFi" | 串口执行 `wifi set <ssid> <pass>` |
| "无法获取清单" | 电脑防火墙拦 8070 / URL 写了 localhost / manifest 不是合法 JSON |
| 检查到新版本但下载失败 | url 404（bin 没上传或名字不一致） |
| 升级完成后又退回旧版 | 新固件启动失败被回滚——连串口看 panic 日志 |
| 版本明明更新但不升级 | manifest.version 不是三段数字（如 "v2.3.1" 带了 v 前缀） |

---

## 4. 相关文件

| 文件 | 职责 |
|---|---|
| `main/app/ota_update.c` | WiFi 管理、manifest 解析、semver 比较、esp_https_ota 流程 |
| `main/app/cli.c` | 串口 REPL 启动（ir/wifi/ota 命令入口） |
| `main/app/ir_backup.c` | 红外码导出/导入命令 |
| `main/app/ir_remote.c` | `ir_remote_read_slot / write_slot` 读写槽接口 |
| `partitions.csv` | ota_0/ota_1/otadata/irbank 分区定义 |
| `main/Kconfig.projbuild` | WiFi/OTA 默认配置项 |
