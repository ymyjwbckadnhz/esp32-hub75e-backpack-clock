# 96×128 HUB75E 背包 LED 屏改造成联网信息屏

把市售的 4G 背包广告屏（主控 合宙 AIR720D + 山景 P2064AF2）改造成
**NTP 时钟 / 空气质量 / 电脑状态** 三合一的 WiFi 信息屏。

核心难点不在联网，而在这块屏的**像素映射不是标准 HUB75**——它被切成四个
象限走线，任何现成 HUB75 库直接上都会得到"画面裂成四段、顺序错乱"的结果。
本仓库给出了完整解法。

---

## 硬件

| 项 | 规格 |
|---|---|
| 面板 | 96 × 128，由 2 块 **96 × 64** 模组竖向级联 |
| 扫描 | 1/32，每地址 2 行 |
| 驱动 IC | 72 × ICN2037BP（16 通道恒流） |
| 行扫 | 16 × TC7258GN（双路 MOS = 32 路） |
| 缓冲 | 4 × SM245TS（74HC245） |
| 主控（改造后） | ESP32-WROOM-32 |
| 电平转换 | 2 × 74HCT245（**必须 HCT，不能用 HC**） |

### 引脚

| 信号 | GPIO | 信号 | GPIO |
|---|---|---|---|
| R1 | 25 | A | 23 |
| G1 | 26 | B | 19 |
| B1 | 27 | C | 5 |
| R2 | 14 | D | 17 |
| G2 | 12 | E | 32 |
| B2 | 13 | LAT | 4 |
| CLK | 16 | OE | 15 |

### 供电

- 面板 **5V / 2~4A**，必须独立供电，**不能从开发板取电**
- 面板与 ESP32 **共地**
- 上电顺序：接地线 → 屏 5V → 板子

---

## 关键：四象限映射

这块屏的 96×64 模组内部按 **4 个 96×32 象限**走线：

```c
if      (cx < 96 && cy < 32)  cx += 96;   // 左上 → 右上
else if (cx < 96)             cy -= 32;   // 左下 → 左上
else if (cy < 32)             cy += 32;   // 右上 → 右下
else                          cx -= 96;   // 右下 → 左下
```

缺少这段，画面就会被切成四段，`1234` 显示成 `3142`。

本项目**不改任何第三方库**，用继承 `MatrixPanel_I2S_DMA` 自己实现逻辑屏来落地：
见 `src/ntp_clock_bmpfont/ntp_clock_bmpfont.ino` 里的 `LogicalPanel` 类。

完整映射链路：

```
逻辑 128×96
  → 旋转 90°（vy = VIRT_H-1-x，VIRT_H = 128 是虚拟高度）
  → 竖向级联（vy/64 决定落在 DMA 第 0 / 96 列）
  → 96×64 四象限重映射
  → DMA 192×64
```

> 已验证：12288 个逻辑点 → 12288 个物理点，零越界、零重叠，完美双射。

---

## 目录结构

```
src/
  ntp_clock_bmpfont/     主程序（自带 8×16 点阵字库，零改库）
  dead_pixel_test/       坏点检测工具（网页控制）
  hub75_bringup/         点屏自检：彩条 / 行标尺 / 文字
tools/
  hub_decode.py          逻辑分析仪抓包还原画面（不用固件也能看懂协议）
docs/
  架构与流程图.md         程序结构、流程、刷新策略
  程序架构与流程图.pdf     同上 PDF 版
  硬件与调试笔记.md       踩过的坑与现象对照表
```

---

## 编译

### 环境

- **arduino-esp32 3.x**（实测 3.3.12）
- 库：`ESP32 HUB75 LED MATRIX PANEL DMA Display` **3.1.0**（原版，无需改库）
- 库：`Adafruit GFX Library` **官方版即可**
- 开发板：`ESP32 Dev Module`
- 分区：**Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** ← 支持 OTA，别用 huge_app

### 凭据

```bash
cp src/ntp_clock_bmpfont/secrets.h.example src/ntp_clock_bmpfont/secrets.h
# 填入自己的 WiFi SSID / 密码
```

`secrets.h` 已在 `.gitignore` 中。没有它也能编译（用占位值），只是连不上网。

数据源默认地址也放在 `secrets.h` 里，避免内网 IP 进公开仓库：

```c
#define DEFAULT_SENSOR_URL "http://10.0.0.27/data"
#define DEFAULT_PC_URL     "http://10.0.0.28/sse"
```

不定义就用 `192.168.1.x` 占位；运行时还可以在网页上随时改，存进 LittleFS。

---

## 使用

首次上电后串口会打印 IP，浏览器打开即可配置：

| 项目 | 说明 |
|---|---|
| 亮度 | 0~255，即时生效 |
| 显示模式 | 时钟 / 空气质量 / 电脑状态 |
| 开关屏时间 | 支持跨零点 |
| 传感器地址 | 默认 `http://192.168.1.27/data` |
| 电脑状态地址 | 默认 `http://192.168.1.28/sse` |

### 数据源

- **空气质量**：任意返回 JSON 的 HTTP 接口，自动匹配
  `pm2_5 / pm25 / PM2.5`、`co2`、`temp`、`hud` 等字段名
- **电脑状态**：AIDA64 的 RemoteSensor，走 `/sse` 流
  （`GET /` 只是静态快照，数值不会变）

---

## 设计要点

| 点 | 做法 |
|---|---|
| 不掉帧 | 分区比较，只重绘变化的 16px 条，绝不整屏清空 |
| 标签不闪 | 标签/数值分开缓存，数值变化只清右侧区域 |
| 弱信号不闪屏 | 数据有 15s 保鲜期，失败先重试再判死 |
| 无信号自动回退 | PC 模式 15s 无数据 → 显示时钟；恢复后自动切回 |
| 字体不依赖库 | 自带 GNU Unifont 8×16 点阵，`font8x16.h` |
| 等宽可控间距 | 逐字绘制，`CHAR_GAP` 可调 |

---

## 已知陷阱（都踩过）

1. **`_width/_height` 裁剪**：`MatrixPanel_I2S_DMA` 把 GFX 初始化为 DMA 尺寸
   （192×64），导致 `y ≥ 64` 的绘制全被丢弃。继承后必须改回 128×96。
2. **C 注释行尾反斜杠**：`// 5C \` 会触发行拼接，吞掉下一行初值，
   字库整体错位一格（`ug/m3` 显示成 `vh/n3`）。详见 docs。
3. **GPIO12 是 strapping 脚**：接屏后被拉高会让 flash 切到 1.8V，表现为无限重启。
   本项目用 GPIO12 驱动 G2，实测可用，但换板子要留意。
4. **GPIO11（CMD）接地**：会表现为"能烧录但写入失败 + 无限重启"。
5. **5V 逻辑**：必须 74HCT245，HC 的输入门槛 3.5V，3.3V 驱动不了。

---

## 致谢

- 原始硬件方案与驱动思路来自立创开源项目
  [捡垃圾的 RGB-LED 显示屏 HUB75e 接口驱动板](https://oshwhub.com/shukkkk/hub75e_96_128)（作者：爆改车间）
- 点阵字形来自 [GNU Unifont](https://unifoundry.com/unifont/)，8×16 点阵

## 许可

代码采用 MIT 协议，见 [LICENSE](LICENSE)。
注意 `font8x16.h` 的字形数据来自 GNU Unifont，其授权为
GPLv2+（含字体嵌入例外）／OFL-1.1，商用或再分发前请自行确认。

---

## 坏点检测工具

`src/dead_pixel_test/` 是独立的坏点（死灯）排查工具，同样复用四象限映射。

### 功能

| 模式 | 说明 |
|---|---|
| 整屏纯色 | 红 / 绿 / 蓝 / 白 / 黑 / 青 / 品红 / 黄 / 灰25 / 灰50 / 灰75 |
| 区块点亮 | 框选的矩形整块点亮 |
| 逐行 | 在框选范围内逐行，行数 = 框高 |
| 逐列 | 在框选范围内逐列，列数 = 框宽 |
| 逐点 | 在框选范围内逐点，点数 = 框宽 × 框高 |

### 框选

网页顶部是整屏缩略图，**直接在图上拖动**即可框选任意矩形；
也可以输入 X0/Y0/X1/Y1，或点预设（全屏 / 左上 ¼ / 右下 ¼ / 居中 64×48）。

框选之后：

- 「区块点亮」只亮这一块
- 「逐行 / 逐列 / 逐点」只在框内扫描，序号与坐标都相对于框
- 缩略图同步显示当前扫描位置，十字辅助线也限制在框内

### 控制

浏览器打开设备 IP 即可：

- **手动**：◀ / ▶ 单步，或直接输入序号跳转
- **自动**：设定延时后自动推进，到区间末尾自动环绕
- **键盘**：`←` `→` 切换，`空格` 播放/暂停
- **反色**：白底黑点，用来找**常亮**的坏点（黑底亮点找不亮的）
- **十字辅助**：逐点模式下显示十字线，方便在 12288 个点里定位
- **亮度**：1~255 可调

### ⚠️ 亮度别开满

这块屏 96×128 全白满亮约 **4A**。如果从 USB 取电，调到高亮度会直接拉垮电压，
表现为**板子不断重启、串口时有时无**。默认亮度设为 25 就是为了避开这个坑。
需要全白检查时，先把屏接到独立 5V 电源上再调高。

### 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/state` | 当前状态（模式、索引、坐标、进度） |
| POST | `/api/set` | 修改配置 `{mode,color,delay,from,to,step,running,invert,crosshair,bright}` |
| POST | `/api/step` | 步进 `{delta}`，正负均可环绕 |
| POST | `/api/goto` | 跳转 `{idx}` |
### 框选实现细节

扫描索引全部**相对于框**，方便按小块反复排查：

| 模式 | 索引范围 | 实际坐标 |
|---|---|---|
| 区块 | 固定 0 | `(rx0, ry0)` |
| 逐行 | `0 .. 框高-1` | `y = ry0 + idx`，横跨 `rx0 .. rx1` |
| 逐列 | `0 .. 框宽-1` | `x = rx0 + idx`，纵跨 `ry0 .. ry1` |
| 逐点 | `0 .. 框宽×框高-1` | `x = rx0 + idx%框宽`，`y = ry0 + idx/框宽` |

改框选会重置区间和索引，避免旧的 from/to 落到新范围之外。
越界或反向框（x1 < x0）会被自动钳制成合法值。
---

## 无线升级（Arduino OTA）

主程序内置 ArduinoOTA，密码 `2018aptx`，主机名 `led-backpack`。

### ⚠️ 分区必须支持 OTA

`huge_app` 只有一块 app 分区，**OTA 会直接失败**。本项目用：

```
PartitionScheme = min_spiffs   (1.9MB APP ×2 + 190KB SPIFFS)
```

从 `huge_app` 换成 `min_spiffs` 需要 **USB 重刷一次**，之后就能全程无线升级。

### 升级方式

**Arduino IDE**：工具 → 端口 → 选 `led-backpack` 或 `led-backpack.local`（网络端口），
输密码后正常点上传。

**一键脚本**（推荐，绕开 IDE 的端口发现）：

```powershell
cd led-backpack-hub75e
.\tools\ota.ps1                    # 默认 10.0.0.43
.\tools\ota.ps1 -Ip 10.0.0.50      # 指定 IP
.\tools\ota.ps1 -BuildOnly         # 只编译
```

脚本会：编译 → espota 推送 → 等设备重启 → 拉 `/api/status` 验证在线。

**命令行**（不插 USB）：

```powershell
# 先编译出 bin
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs `
  --build-path .\_build src\ntp_clock_bmpfont

# 再用 espota 推送
& "$env:LOCALAPPDATA\Arduino15\packages\esp32\hardware\esp32\3.3.12\tools\espota.exe" `
  -i <设备IP> -p 3232 -a 2018aptx -f .\_build\ntp_clock_bmpfont.ino.bin -r
```

`-r` 表示上传完自动重启。

### 升级时屏幕会显示

`OTA UPDATE` + 百分比 + 进度条；失败显示 `OTA FAIL`，完成显示 `OTA DONE`。
升级过程中暂停常规刷新，不会和进度画面打架。

### 注意事项

- **端口 3232 是 UDP，不是 TCP**。用 `telnet`/TCP 去探测会显示"拒绝连接"，
  这并不代表 OTA 不可用——新版 esp32 核心的 OTA 邀请走 UDP，真正的数据传输
  用另外协商出的 TCP 端口。
- **设备拉数据时会阻塞主循环**（最长 4s），期间可能错过 OTA 的 UDP 邀请，
  表现为 `espota` 报 `No response from device`。代码里已在阻塞点前后补了
  `ArduinoOTA.handle()`；若仍失败，重试一次即可。
- 升级需要设备与电脑同一网段，且设备已连上 WiFi。

### 关于密码

`OTA_PASSWORD` 明文写在源码里，仅用于防止误刷，**不是安全凭证**。
公网环境请自行更换并考虑加 mDNS 隔离。
### ⚠️ 换分区会清空 LittleFS

从 `huge_app` 切到 `min_spiffs` 是**整表重写**，`/config.json` 和 `/index.html` 都会没。
重新上电后：

- `index.html` 会自动重新写入（首次开机检测）
- **配置回到默认值**：亮度、开关屏时间、数据源地址

如果发现数据源变成 `192.168.1.x` 拉不到数据，去网页改一次地址即可；
或在 `secrets.h` 里定义 `DEFAULT_SENSOR_URL` / `DEFAULT_PC_URL`，让默认值就是对的。

> 注意：`loadConfig()` 只在 `/config.json` **不存在**时才用默认值。
> 如果之前已经生成过 config.json（哪怕内容是占位地址），改代码里的默认值不会生效，
> 需要网页改一次、或格式化 LittleFS。
### IDE 上传报 `invalid int value: '{upload.port.properties.port}'`

这不是 OTA 服务的问题，而是**IDE 没拿到端口号**。

判断方法：

```powershell
arduino-cli board list
# 如果列表里没有设备的 IP，说明 mDNS 发现失败
```

已确认的两个诱因（本机实测）：

1. **Bonjour 服务处于 Stopped** —— Windows 上 `.local` 域名解析不了，
   `ping led-backpack.local` 失败。管理员启动：
   ```powershell
   net start "Bonjour Service"
   ```
2. **IDE 里残留了旧的网络端口条目**，属性为空，于是模板变量没被替换。

**临时方案**：重启 IDE 后重新选择端口；或直接 `.\tools\ota.ps1`。

> 注意 `arduino-cli board list` 里其它 ESP8266 设备能显示网络端口、而这台 ESP32
> 不显示时，基本可以断定是本机 mDNS 解析（Bonjour）出了问题，而不是设备没广播。