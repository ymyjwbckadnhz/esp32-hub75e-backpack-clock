// ============================================================
//  ntp_clock_web.ino —— 96×128 HUB75E 时钟 + 空气质量 + LittleFS 配置网页
//
//  功能：
//   1. NTP 时钟（横屏 128×96）
//   2. 空气质量模式：从传感器 HTTP 接口拉 JSON 显示 PM2.5 / CO2 / 温湿度
//   3. LittleFS 配置网页：亮度、开关屏时间、显示模式、传感器地址
//   4. 开机自动把网页写入 LittleFS（不需要 LittleFS 上传插件）
//
//  依赖：ESP32-HUB75-MatrixPanel-DMA（旧版 API）、arduino-esp32 2.0.x
//  引脚：R1=25 G1=26 B1=27 R2=14 G2=12 B2=13 A=23 B=19 C=5 D=17 E=32
//        LAT=4 OE=15 CLK=16
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <time.h>
// 只包含"干净"的基础头文件：不含 Fonts/、不调用 setFont，
// 因此可以配合任何精简版 Adafruit GFX（含 gb2312 汉化版），无需改库。
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "font8x16.h"

// ---------------- 面板 ----------------
#define PANEL_RES_X 96
#define PANEL_RES_Y 64
#define NUM_ROWS    2
#define NUM_COLS    1
#define PANEL_CHAIN (NUM_ROWS * NUM_COLS)

// ---------------- 引脚（显式指定，不依赖库默认值）----------------
#define PIN_R1  25
#define PIN_G1  26
#define PIN_B1  27
#define PIN_R2  14
#define PIN_G2  12
#define PIN_B2  13
#define PIN_A   23
#define PIN_B   19
#define PIN_C   5
#define PIN_D   17
#define PIN_E   32
#define PIN_LAT 4
#define PIN_OE  15
#define PIN_CLK 16

#define LANDSCAPE 1
#if LANDSCAPE
  #define SCR_W 128
  #define SCR_H 96
#else
  #define SCR_W 96
  #define SCR_H 128
#endif

// 虚拟屏尺寸（未旋转）：宽 = 每行模组总宽，高 = 每列模组总高
#define VIRT_W (NUM_COLS * PANEL_RES_X)   // 96
#define VIRT_H (NUM_ROWS * PANEL_RES_Y)   // 128

// ============================================================
//  逻辑屏：把 SCR_W x SCR_H 的坐标映射到 2 块 96x64 模组的 DMA 缓冲
//
//  这替代了库里的 VirtualMatrixPanel，好处是：
//    · 不依赖 VirtualMatrixPanel 头文件（它强制包含 Adafruit 图形字体，精简版 GFX 会编译失败）
//    · 不需要改动库文件，升级库不受影响
//
//  映射链路与旧版驱动库等价：旋转 → 竖向级联 → 96x64 模组四象限重映射
// ============================================================
class LogicalPanel : public MatrixPanel_I2S_DMA {
public:
  explicit LogicalPanel(const HUB75_I2S_CFG &cfg) : MatrixPanel_I2S_DMA(cfg) {
    // 关键：基类把 Adafruit_GFX 初始化成 DMA 尺寸（96*2 x 64 = 192x64），
    // 于是 GFX 层会把 y >= 64 的所有绘制全部裁掉 —— 表现为"下半屏不显示"。
    // 这里改回逻辑尺寸，裁剪和换行才会按 128x96 来算。
    _width  = SCR_W;
    _height = SCR_H;
  }

  // 逻辑坐标 → DMA 物理坐标
  static void mapPoint(int16_t x, int16_t y, int16_t &ox, int16_t &oy) {
#if LANDSCAPE
    // 横屏：逻辑 128x96 → 虚拟 96x128（顺时针 90°）
    int16_t vx = y;                 // 0..95
    int16_t vy = (VIRT_H - 1) - x;  // 127..0   ← 用虚拟高度，不是逻辑高度
#else
    int16_t vx = x;                 // 0..95
    int16_t vy = y;                 // 0..127
#endif
    // ① 竖向级联：上/下半屏分别落在 DMA 的第 0 / 96 列
    int  row = vy / PANEL_RES_Y;                       // 0 或 1
    int  ry  = (NUM_ROWS - row - 1);                   // 级联方向反转
    int16_t cx = ((NUM_ROWS - (ry + 1)) * PANEL_RES_X) + vx;
    int16_t cy = vy % PANEL_RES_Y;

    // ② 96x64 模组内部四象限走线重映射
    //    缺了这段，画面会被切成四段且顺序错乱
    if      (cx < 96 && cy < 32)  cx += 96;
    else if (cx < 96)             cy -= 32;
    else if (cy < 32)             cy += 32;
    else                          cx -= 96;

    ox = cx;
    oy = cy;
  }

  // 所有绘制最终都落到 drawPixel，这里做映射
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || x >= SCR_W || y < 0 || y >= SCR_H) return;
    int16_t px, py;
    mapPoint(x, y, px, py);
    MatrixPanel_I2S_DMA::drawPixel(px, py, color);
  }

  // 基类的这三个方法走 DMA 快速通道、绕过 drawPixel，
  // 必须退回 Adafruit_GFX 的逐点实现，否则坐标不会经过映射
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    Adafruit_GFX::drawFastHLine(x, y, w, color);
  }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    Adafruit_GFX::drawFastVLine(x, y, h, color);
  }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    Adafruit_GFX::fillRect(x, y, w, h, color);
  }
};

#define CHAR_GAP     1
#define EDGE_BOTTOM  1

#if LANDSCAPE
  #define DATE_Y   0
  #define DATE_H   16
  #define TIME_Y   16
  #define TIME_H   48
  #define ROW_Y    64
  #define ROW_H    16
#else
  #define DATE_Y   90
  #define DATE_H   12
  #define TIME_Y   2
  #define TIME_H   86
  #define ROW_Y    100
  #define ROW_H    16
#endif

#define BAR_H 4
#define BAR_Y (SCR_H - EDGE_BOTTOM - BAR_H)

// ---------------- WiFi 凭据 ----------------
// 复制 secrets.h.example 为 secrets.h 并填入自己的 SSID / 密码。
// secrets.h 已列入 .gitignore，不会被提交。
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #warning "未找到 secrets.h，使用占位凭据（无法联网）。请参照 secrets.h.example 创建。"
  #define WIFI_SSID "YOUR_WIFI_SSID"
  #define WIFI_PASS "YOUR_WIFI_PASSWORD"
#endif

const long GMT_OFFSET = 8 * 3600;
const int  DST_OFFSET = 0;
const char* NTP1 = "ntp.aliyun.com";
const char* NTP2 = "cn.pool.ntp.org";
const char* NTP3 = "pool.ntp.org";

// ---------------- 配置 ----------------
// 数据源默认地址：优先用 secrets.h 里的（本机真实地址），
// 没有就用占位值，这样公开仓库不会泄漏内网 IP。
#if __has_include("secrets.h")
  #include "secrets.h"
#endif

#ifndef DEFAULT_SENSOR_URL
  #define DEFAULT_SENSOR_URL "http://192.168.1.27/data"
#endif
#ifndef DEFAULT_PC_URL
  #define DEFAULT_PC_URL     "http://192.168.1.28/sse"
#endif

struct Config {
  uint8_t  bright     = 20;      // 0-255
  uint8_t  onH        = 7,  onM  = 0;
  uint8_t  offH       = 23, offM = 0;
  uint8_t  mode       = 0;       // 0=时钟 1=空气质量
  uint16_t airRefresh = 60;      // 秒
  char     sensor[64] = DEFAULT_SENSOR_URL;
  uint16_t pcRefresh  = 1;       // 秒
  char     pcUrl[64]  = DEFAULT_PC_URL;
};

Config cfg;

MatrixPanel_I2S_DMA *dma_display = nullptr;
LogicalPanel        *display    = nullptr;
WebServer server(80);

// 行缓存绘制（定义在后面，供空气质量页提前调用）
void drawRowEx(int slot, int16_t y, int16_t labelX, const char* label,
               int16_t valueX, const char* value,
               uint16_t labelColor, uint16_t valueColor, bool force);
void drawTempRowEx(int slot, int16_t y, int16_t labelX, const char* label,
                   float t, uint16_t labelColor, uint16_t valueColor, bool force);

bool timeValid  = false;
bool screenOn   = true;
bool fullRedraw = true;
int  barLastW   = 0;         // 秒进度条上一帧的宽度（切模式/开机必须归零）
bool pcFallback = false;     // PC 模式是否处于"回退显示时钟"状态

// 空气质量数据
struct AirData {
  float pm25 = -1, pm10 = -1, co2 = -1, tvoc = -1, ch2o = -1;
  float temp = -999, hum = -999;
  bool  ok = false;
  uint32_t lastMs = 0;
} air;

// ============================================================
//  配置读写（LittleFS）
// ============================================================
String fsRead(const char* path) {
  if (!LittleFS.exists(path)) return "";
  File f = LittleFS.open(path, "r");
  if (!f) return "";
  String s = f.readString();
  f.close();
  return s;
}

void fsWrite(const char* path, const String& data) {
  File f = LittleFS.open(path, "w");
  if (!f) { Serial.printf("[fs] 写入失败 %s\n", path); return; }
  f.print(data);
  f.close();
}

// 极简 JSON 取值（不依赖 ArduinoJson）
bool jsonNum(const String& s, const char* key, long& out) {
  String pat = String("\"") + key + "\"";
  int i = s.indexOf(pat);
  if (i < 0) return false;
  i = s.indexOf(':', i + pat.length());
  if (i < 0) return false;
  i++;
  while (i < (int)s.length() && !isDigit(s[i]) && s[i] != '-' && s[i] != '.') i++;
  if (i >= (int)s.length()) return false;
  int j = i;
  while (j < (int)s.length() && (isDigit(s[j]) || s[j] == '-' || s[j] == '.')) j++;
  out = s.substring(i, j).toInt();
  return true;
}

bool jsonFloat(const String& s, const char* key, float& out) {
  String pat = String("\"") + key + "\"";
  int i = s.indexOf(pat);
  if (i < 0) return false;
  i = s.indexOf(':', i + pat.length());
  if (i < 0) return false;
  i++;
  while (i < (int)s.length() && !isDigit(s[i]) && s[i] != '-' && s[i] != '.') i++;
  if (i >= (int)s.length()) return false;
  int j = i;
  while (j < (int)s.length() && (isDigit(s[j]) || s[j] == '-' || s[j] == '.')) j++;
  out = s.substring(i, j).toFloat();
  return true;
}

bool jsonStr(const String& s, const char* key, char* out, size_t n) {
  String pat = String("\"") + key + "\"";
  int i = s.indexOf(pat);
  if (i < 0) return false;
  i = s.indexOf(':', i + pat.length());
  if (i < 0) return false;
  int a = s.indexOf('"', i);
  if (a < 0) return false;
  int b = s.indexOf('"', a + 1);
  if (b < 0) return false;
  String v = s.substring(a + 1, b);
  strncpy(out, v.c_str(), n - 1);
  out[n - 1] = 0;
  return true;
}

void loadConfig() {
  String s = fsRead("/config.json");
  if (!s.length()) { Serial.println("[cfg] 无配置，用默认值"); return; }

  long v;
  if (jsonNum(s, "bright", v)) cfg.bright = constrain(v, 0, 255);
  if (jsonNum(s, "onH", v))    cfg.onH = v;
  if (jsonNum(s, "onM", v))    cfg.onM = v;
  if (jsonNum(s, "offH", v))   cfg.offH = v;
  if (jsonNum(s, "offM", v))   cfg.offM = v;
  if (jsonNum(s, "mode", v))   cfg.mode = v;
  if (jsonNum(s, "airRefresh", v)) cfg.airRefresh = constrain(v, 10, 3600);
  if (jsonNum(s, "pcRefresh", v))  cfg.pcRefresh  = constrain(v, 1, 3600);
  jsonStr(s, "pcUrl", cfg.pcUrl, sizeof(cfg.pcUrl));
  jsonStr(s, "sensor", cfg.sensor, sizeof(cfg.sensor));
  Serial.println("[cfg] 已加载");
}

void saveConfig() {
  String s = "{";
  s += "\"bright\":" + String(cfg.bright);
  s += ",\"onH\":" + String(cfg.onH) + ",\"onM\":" + String(cfg.onM);
  s += ",\"offH\":" + String(cfg.offH) + ",\"offM\":" + String(cfg.offM);
  s += ",\"mode\":" + String(cfg.mode);
  s += ",\"airRefresh\":" + String(cfg.airRefresh);
  s += ",\"sensor\":\"" + String(cfg.sensor) + "\"";
  s += ",\"pcRefresh\":" + String(cfg.pcRefresh);
  s += ",\"pcUrl\":\"" + String(cfg.pcUrl) + "\"}";
  fsWrite("/config.json", s);
}

// ============================================================
//  文字绘制：自带 8x16 等宽点阵（GNU Unifont）
//
//  完全自包含：既不需要汉化版 Adafruit GFX，也不需要 U8g2。
//  等宽、可加字间距，观感与原版一致，且能自动按整数倍放大。
// ============================================================
#define FONT_W 8
#define FONT_H 16

// 画一个字符；scale 为整数放大倍数
void drawCharScaled(int16_t x, int16_t y, char c, uint16_t color, uint8_t scale) {
  if (c < FONT_FIRST || c > FONT_LAST) return;
  const uint8_t* g = FONT8X16[(uint8_t)c - FONT_FIRST];

  for (uint8_t row = 0; row < FONT_H; row++) {
    uint8_t bits = pgm_read_byte(&g[row]);
    if (!bits) continue;
    for (uint8_t col = 0; col < FONT_W; col++) {
      if (bits & (0x80 >> col)) {
        if (scale == 1) display->drawPixel(x + col, y + row, color);
        else            display->fillRect(x + col * scale, y + row * scale,
                                          scale, scale, color);
      }
    }
  }
}

// 含字间距的整串宽度
int16_t textWidthScaled(const char* s, uint8_t scale) {
  int n = strlen(s);
  if (n <= 0) return 0;
  return n * (FONT_W * scale + CHAR_GAP) - CHAR_GAP;
}

void drawStrScaled(int16_t x, int16_t y, const char* s, uint16_t color, uint8_t scale) {
  int16_t adv = FONT_W * scale + CHAR_GAP;
  for (; *s; s++) {
    drawCharScaled(x, y, *s, color, scale);
    x += adv;
  }
}

// 兼容旧接口（size 用作倍数）
uint16_t charWidth(char c, uint8_t size) {
  (void)c;
  return FONT_W * (size ? size : 1);
}

int strWidth(const char* s, uint8_t size, int16_t gap) {
  int n = strlen(s);
  if (n <= 0) return 0;
  return n * (FONT_W * (size ? size : 1) + gap) - gap;
}

// 在 [x0,y0,w,h] 内居中，自动挑能放下的最大整数倍字号
void drawTextGap(const char* s, int16_t x0, int16_t y0, int16_t w, int16_t h,
                 uint16_t color, int16_t gap) {
  (void)gap;
  uint8_t best = 1;
  for (int8_t sc = 6; sc >= 1; sc--) {
    int16_t tw = (int16_t)strlen(s) * (FONT_W * sc + CHAR_GAP) - CHAR_GAP;
    if (tw <= w && FONT_H * sc <= h) { best = sc; break; }
  }

  int16_t tw = textWidthScaled(s, best);
  int16_t x  = x0 + (w - tw) / 2;
  int16_t y  = y0 + (h - FONT_H * best) / 2;

  drawStrScaled(x, y, s, color, best);
}

inline void drawTextGap(const char* s, int16_t x0, int16_t y0, int16_t w, int16_t h,
                        uint16_t color) {
  drawTextGap(s, x0, y0, w, h, color, CHAR_GAP);
}

// ============================================================
//  时钟画面（分区刷新）
// ============================================================
void clockDate(const struct tm& t) {
  char buf[24];
  display->fillRect(0, DATE_Y, SCR_W, DATE_H, 0);
  sprintf(buf, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  drawTextGap(buf, 0, DATE_Y, SCR_W, DATE_H, display->color565(110, 110, 110));
}

void clockWeekday(const struct tm& t) {
  static const char* WD[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  display->fillRect(0, ROW_Y, SCR_W / 2, ROW_H, 0);
  drawTextGap(WD[t.tm_wday % 7], 0, ROW_Y, SCR_W / 2, ROW_H,
              display->color565(0, 170, 255));
}

void clockTime(const struct tm& t) {
  char buf[8];
  display->fillRect(0, TIME_Y, SCR_W, TIME_H, 0);
#if LANDSCAPE
  sprintf(buf, "%02d:%02d", t.tm_hour, t.tm_min);
  drawTextGap(buf, 0, TIME_Y, SCR_W, TIME_H, display->color565(255, 170, 0));
#else
  sprintf(buf, "%02d", t.tm_hour);
  drawTextGap(buf, 0, TIME_Y, SCR_W, TIME_H / 2 - 2, display->color565(255, 170, 0));
  sprintf(buf, "%02d", t.tm_min);
  drawTextGap(buf, 0, TIME_Y + TIME_H / 2 + 2, SCR_W, TIME_H / 2 - 2,
              display->color565(255, 170, 0));
#endif
}

void clockSec(int s) {
  char buf[4];
  display->fillRect(SCR_W / 2, ROW_Y, SCR_W / 2, ROW_H, 0);
  sprintf(buf, "%02d", s);
  drawTextGap(buf, SCR_W / 2, ROW_Y, SCR_W / 2, ROW_H, display->color565(0, 200, 90));

  int w = (s * SCR_W) / 59;
  if (w < barLastW) {
    // 秒回绕、或刚切回时钟模式：先清整条，再把当前进度补画上
    display->fillRect(0, BAR_Y, SCR_W, BAR_H, 0);
    if (w > 0) display->fillRect(0, BAR_Y, w, BAR_H, display->color565(0, 120, 60));
  } else if (w > barLastW) {
    display->fillRect(barLastW, BAR_Y, w - barLastW, BAR_H,
                      display->color565(0, 120, 60));
  }
  barLastW = w;
}

// 时钟画面（分区刷新）。force=true 时全部重画，用于切模式 / 从 PC 回退
void clockRender(const struct tm& t, bool force) {
  static int lastDay = -1, lastHour = -1, lastMin = -1, lastSec = -1;

  if (force) {
    lastDay = lastHour = lastMin = lastSec = -1;
    barLastW = 0;
  }

  if (force || t.tm_mday != lastDay) {
    clockDate(t);
    clockWeekday(t);
    lastDay = t.tm_mday;
  }
  if (force || t.tm_hour != lastHour || t.tm_min != lastMin) {
    clockTime(t);
    lastHour = t.tm_hour;
    lastMin  = t.tm_min;
  }
  if (force || t.tm_sec != lastSec) {
    clockSec(t.tm_sec);
    lastSec = t.tm_sec;
  }
}

// ============================================================
//  空气质量画面
// ============================================================
void airHeader() {
  // 这套 gb2312 字体是 unifont，最小字号也是 16px 高，
  // 所以标题必须给足 16px 的带子，否则会被下面的数值区清掉下半截
  display->fillRect(0, 0, SCR_W, 16, 0);
  drawTextGap("PM2.5", 0, 0, 64, 16, display->color565(90, 90, 90));
  drawTextGap("ug/m3", 64, 0, 64, 16, display->color565(90, 90, 90));
}

void airValue() {
  char buf[16];
  display->fillRect(0, 16, SCR_W, 48, 0);
  if (!air.ok) {
    drawTextGap("--", 0, 16, SCR_W, 48, display->color565(80, 80, 80));
    return;
  }
  int v = (int)round(air.pm25);
  sprintf(buf, "%d", v);

  // 按 PM2.5 数值上色（参考国标分级）
  uint16_t c;
  if (v <= 35)       c = display->color565(0, 220, 90);
  else if (v <= 75)  c = display->color565(255, 200, 0);
  else if (v <= 115) c = display->color565(255, 120, 0);
  else               c = display->color565(255, 50, 50);

  drawTextGap(buf, 0, 16, SCR_W, 48, c);
}

// 左对齐绘制（16px 等宽），返回右沿 x
int16_t drawTextLeft(const char* s, int16_t x, int16_t y, int16_t h, uint16_t color,
                     int16_t gap = CHAR_GAP) {
  (void)gap;
  drawStrScaled(x, y + (h - FONT_H) / 2, s, color, 1);
  return x + textWidthScaled(s, 1);
}

int16_t textW(const char* s, int16_t h, int16_t gap) {
  (void)h; (void)gap;
  return textWidthScaled(s, 1);
}

// 温度：数字 + 手绘度数圈 + C
void drawTempStr(const char* num, int16_t x, int16_t y, int16_t h, uint16_t color) {
  int16_t xe  = drawTextLeft(num, x, y, h, color);
  int16_t top = y + (h - FONT_H) / 2;

  int16_t r = h / 6;
  if (r < 2) r = 2;

  xe += CHAR_GAP;
  display->drawCircle(xe + r + 1, top + r + 1, r, color);
  xe += 2 * r + 3 + CHAR_GAP;

  drawTextLeft("C", xe, y, h, color);
}

void drawTemp(int16_t x, int16_t y, int16_t h, float t, uint16_t color) {
  char b[12];
  sprintf(b, "%.1f", t);
  drawTempStr(b, x, y, h, color);
}

int16_t airX0 = 2, airXr = 66;   // 两列的左沿，由温湿度宽度算出

// 根据当前温湿度重算两列位置（整块居中）
void airLayout() {
  char tb[12], hb[12];
  sprintf(tb, "%.1f", air.temp);
  sprintf(hb, "%.0f%%", air.hum);

  int16_t r = 16 / 6;
  if (r < 2) r = 2;
  int16_t circleW = 2 * r + 3;

  int16_t wT = textW(tb, 16, CHAR_GAP) + CHAR_GAP + circleW + CHAR_GAP
             + textW("C", 16, CHAR_GAP);
  int16_t wH = textW(hb, 16, CHAR_GAP);

  const int16_t gap = 18;
  int16_t total = wT + gap + wH;
  airX0 = (SCR_W - total) / 2;
  if (airX0 < 0) airX0 = 0;
  airXr = airX0 + wT + gap;
}

// 第一行：CO2 标签 + 数值。标签不动，只有数值区重绘，标签不闪
void airCo2Row(bool force) {
  char cb[12];
  if (!air.ok) return;

  if (air.co2 >= 0) sprintf(cb, "%d", (int)air.co2);
  else              strcpy(cb, "--");

  uint16_t c = display->color565(0, 170, 255);
  drawRowEx(6, 64, airX0, "CO2", airXr, cb, c, c, force);
}

// 第二行：温度 + 湿度，同样只清这一条
void airTempHumRow() {
  char hb[12];
  display->fillRect(0, 80, SCR_W, 16, 0);
  if (!air.ok) {
    drawTextGap("NO DATA", 0, 80, SCR_W, 16, display->color565(80, 80, 80));
    return;
  }

  sprintf(hb, "%.0f%%", air.hum);
  drawTemp(airX0, 80, 16, air.temp, display->color565(160, 160, 160));
  drawTextLeft(hb, airXr, 80, 16, display->color565(160, 160, 160));
}

// 整块重画（切模式 / 整屏重绘时用）
void airBottom() {
  airLayout();
  airCo2Row(true);
  airTempHumRow();
}

// 空气质量局部刷新：按行比较，只有内容真的变了才重绘那一行
static char snapPm[12]  = "";
static char snapCo2[12] = "";
static char snapTH[24]  = "";

void airUpdate(bool force) {
  char b[24];

  // PM2.5 数值区
  if (air.ok) sprintf(b, "%d", (int)round(air.pm25));
  else        strcpy(b, "--");
  if (force || strcmp(b, snapPm)) {
    airValue();
    strcpy(snapPm, b);
  }

  if (!air.ok) {
    if (force || strcmp(snapCo2, "nodata")) {
      display->fillRect(0, 64, SCR_W, 32, 0);
      drawTextGap("NO DATA", 0, 80, SCR_W, 16, display->color565(80, 80, 80));
      strcpy(snapCo2, "nodata");
      strcpy(snapTH,  "nodata");
    }
    return;
  }

  char kc[12], kt[24];
  sprintf(kc, "%d", (int)air.co2);
  sprintf(kt, "%.1f|%.0f", air.temp, air.hum);

  bool thChanged  = force || strcmp(kt, snapTH);
  bool co2Changed = force || strcmp(kc, snapCo2);

  if (thChanged) {
    airLayout();        // 列位置由温湿度宽度决定，变了要重算
    airTempHumRow();    // 只清 y=80..95
    airCo2Row(true);    // 列可能位移了，强制重画
    strcpy(snapTH,  kt);
    strcpy(snapCo2, kc);
  } else if (co2Changed) {
    airCo2Row(false);   // 只重画数值区，CO2 标签不动
    strcpy(snapCo2, kc);
  }
}

// ============================================================
//  传感器抓取
// ============================================================
void fetchAir() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setTimeout(4000);
  http.setConnectTimeout(4000);
  if (!http.begin(String(cfg.sensor))) {
    Serial.println("[air] begin 失败");
    air.ok = false;
    return;
  }

  // 拉数据是阻塞的（最长 4s），期间主循环停摆会错过 OTA 的 UDP 邀请，
  // 所以在之前先处理一次，提升 OTA 成功率。
  ArduinoOTA.handle();
  int code = http.GET();
  ArduinoOTA.handle();
  if (code != 200) {
    Serial.printf("[air] HTTP %d\n", code);
    air.ok = false;
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  AirData d;
  bool got = false;

  // 传感器返回 {"co2":[..],"pm2_5":[..],...,"current":{...}}
  // 历史数组里也有同名字段，所以只在 "current" 之后解析，否则会读到数组第一个值
  int ci = body.indexOf("\"current\"");
  String scope = (ci >= 0) ? body.substring(ci) : body;

  got |= jsonFloat(scope, "pm2_5", d.pm25);
  if (!got) got |= jsonFloat(scope, "pm25", d.pm25);
  if (!got) got |= jsonFloat(scope, "PM2.5", d.pm25);
  if (!got) got |= jsonFloat(scope, "pm2p5", d.pm25);

  jsonFloat(scope, "pm10", d.pm10);
  jsonFloat(scope, "co2", d.co2);
  jsonFloat(scope, "CO2", d.co2);
  jsonFloat(scope, "tvoc", d.tvoc);
  jsonFloat(scope, "ch2o", d.ch2o);
  jsonFloat(scope, "temp", d.temp);
  jsonFloat(scope, "temperature", d.temp);
  jsonFloat(scope, "hud", d.hum);
  jsonFloat(scope, "hum", d.hum);
  jsonFloat(scope, "humidity", d.hum);

  d.ok = got;
  d.lastMs = millis();

  // 全局缓存（互斥简单处理：单线程 loop 内调用）
  float keepT = (d.temp <= -900) ? air.temp : d.temp;
  float keepH = (d.hum  <= -900) ? air.hum  : d.hum;
  air = d;
  air.temp = keepT;
  air.hum  = keepH;

  Serial.printf("[air] pm2.5=%.1f co2=%.1f t=%.1f h=%.1f ok=%d\n",
                air.pm25, air.co2, air.temp, air.hum, air.ok);
}

// ============================================================
//  电脑状态（AIDA64 RemoteSensor 页面）
//  页面形如：<span id="Simple1">CPU&nbsp;使用率 15%</span>
// ============================================================
struct PcData {
  float cpuUse = -1, memUse = -1, gpuUse = -1, cpuTemp = -999;
  float netUp = -1, netDown = -1;
  bool ok = false;
  uint32_t lastMs = 0;
} pc;

// 取字符串末尾的数字
static float tailNumber(const String& t, bool& ok) {
  int n = t.length();
  int e = -1;
  for (int i = n - 1; i >= 0; i--) {
    if (isdigit(t[i])) { e = i; break; }
  }
  if (e < 0) { ok = false; return 0; }
  int s = e;
  while (s > 0 && (isdigit(t[s - 1]) || t[s - 1] == '.')) s--;
  ok = true;
  return t.substring(s, e + 1).toFloat();
}

static PcData pcAcc;
static bool   pcAny = false;

// 把一段"标签 数值"文本按关键词塞进 pcAcc
void pcFeed(const String& raw) {
  String t = raw;
  t.replace("&nbsp;", " ");
  t.replace("data:", "");
  t.trim();
  if (!t.length()) return;

  bool ok = false;
  float v = tailNumber(t, ok);
  if (!ok) return;

  if (t.indexOf("使用率") >= 0) {
    if (t.indexOf("CPU") >= 0)       pcAcc.cpuUse = v;
    else if (t.indexOf("GPU") >= 0)  pcAcc.gpuUse = v;
    else if (t.indexOf("内存") >= 0)  pcAcc.memUse = v;
    else return;
  } else if (t.indexOf("℃") >= 0 || t.indexOf("°C") >= 0 || t.indexOf("温度") >= 0) {
    pcAcc.cpuTemp = v;
  } else if (t.indexOf("上传") >= 0) {
    pcAcc.netUp = v;
  } else if (t.indexOf("下载") >= 0) {
    pcAcc.netDown = v;
  } else {
    return;
  }
  pcAny = true;
}

// 支持两种格式：SSE 的 "Simple1|文本{|}Simple3|文本" 和 HTML 的 <span>…</span>
void parsePcPayload(const String& body) {
  pcAcc = PcData();
  pcAny = false;

  // 1) HTML 快照
  int idx = 0;
  while (true) {
    int s = body.indexOf("<span", idx);
    if (s < 0) break;
    int gt = body.indexOf('>', s);
    if (gt < 0) break;
    int e = body.indexOf("</span>", gt);
    if (e < 0) break;
    pcFeed(body.substring(gt + 1, e));
    idx = e + 7;
  }

  // 2) SSE 帧
  if (!pcAny) {
    int pos = 0;
    while (pos < (int)body.length()) {
      int sep = body.indexOf("{|}", pos);
      String item = (sep < 0) ? body.substring(pos) : body.substring(pos, sep);
      pos = (sep < 0) ? body.length() : sep + 3;

      int bar = item.indexOf('|');
      if (bar > 0) item = item.substring(bar + 1);   // 去掉 SimpleN|
      pcFeed(item);
    }
  }

  if (pcAny) {
    pcAcc.ok = true;
    pcAcc.lastMs = millis();
    pc = pcAcc;
  } else {
    pc.ok = false;
  }
}

// 拆 http://host[:port]/path
bool parseUrl(const String& url, String& host, uint16_t& port, String& path) {
  String u = url;
  if (u.startsWith("http://")) u = u.substring(7);
  int slash = u.indexOf('/');
  String hp = (slash < 0) ? u : u.substring(0, slash);
  path = (slash < 0) ? "/" : u.substring(slash);
  int colon = hp.indexOf(':');
  if (colon >= 0) {
    host = hp.substring(0, colon);
    port = (uint16_t)hp.substring(colon + 1).toInt();
  } else {
    host = hp;
    port = 80;
  }
  return host.length() > 0;
}

// 抓一次，返回原始响应体。readMs=读取窗口，connMs=连接超时
bool pcFetchOnce(const String& host, uint16_t port, const String& path, String& body,
                 uint32_t readMs = 800, int32_t connMs = 1500) {
  WiFiClient cli;
  ArduinoOTA.handle();                 // 连接前先处理一次 OTA 邀请
  if (!cli.connect(host.c_str(), port, connMs)) return false;

  cli.print(String("GET ") + path + " HTTP/1.1\r\n"
            "Host: " + host + "\r\n"
            "Accept: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n");

  body = "";
  body.reserve(4096);
  uint32_t t0 = millis();
  while (millis() - t0 < readMs) {
    while (cli.available()) {
      body += (char)cli.read();
      if (body.length() > 5000) break;
    }
    if (body.indexOf("{|}") >= 0) break;    // 拿到一帧就够
    delay(2);
  }
  cli.stop();
  return body.length() > 0;
}

static bool pcUseSse = false;   // 学会正确路径后就不再试错

// 抓一次并解析出 SSE 帧；成功返回 true
bool pcTryFetch(String& body, uint32_t readMs, int32_t connMs) {
  String host, path;
  uint16_t port = 80;
  if (!parseUrl(String(cfg.pcUrl), host, port, path)) return false;

  // 已知要连 /sse 就直接连，省掉一次多余请求
  String tryPath = path;
  if (pcUseSse) {
    if (!tryPath.endsWith("/")) tryPath += "/";
    tryPath += "sse";
  }

  String b1;
  if (!pcFetchOnce(host, port, tryPath, b1, readMs, connMs)) return false;

  // 配置填的是根路径 → 拿到静态 HTML，自动改用 /sse 并记住
  bool looksHtml = (b1.indexOf("{|}") < 0) &&
                   (b1.indexOf("<html") >= 0 || b1.indexOf("<span") >= 0);
  if (!pcUseSse && looksHtml) {
    String p2 = path;
    if (!p2.endsWith("/")) p2 += "/";
    p2 += "sse";

    String b2;
    if (pcFetchOnce(host, port, p2, b2, readMs, connMs) && b2.indexOf("{|}") >= 0) {
      b1 = b2;
      pcUseSse = true;

      // 顺手把正确地址写回配置，网页上显示的就对了
      if (port == 80) {
        snprintf(cfg.pcUrl, sizeof(cfg.pcUrl), "http://%s%s", host.c_str(), p2.c_str());
      } else {
        snprintf(cfg.pcUrl, sizeof(cfg.pcUrl), "http://%s:%u%s",
                 host.c_str(), (unsigned)port, p2.c_str());
      }
      saveConfig();
      Serial.printf("[pc] 已自动改用 %s\n", cfg.pcUrl);
    }
  }

  if (b1.indexOf("{|}") < 0) return false;
  body = b1;
  return true;
}

// AIDA64 实时数据在 /sse 的 SSE 流里，普通 GET 只会拿到不变的 HTML 快照
void fetchPc(bool quick) {
  if (WiFi.status() != WL_CONNECTED) { pc.ok = false; return; }

  // 回退状态只是"探一探"，用很短的时间窗，别把主循环卡住
  uint32_t readMs = quick ? 250 : 800;
  int32_t  connMs = quick ? 250 : 1500;

  String body;
  bool got = pcTryFetch(body, readMs, connMs);

  // 正常状态偶发失败时快速重试一次；探测模式不重试
  if (!got && !quick) {
    delay(80);
    got = pcTryFetch(body, readMs, connMs);
  }

  if (!got) {
    pc.ok = false;      // 只标记失败，旧数据保留，由 pcUpdate 决定是否显示 NO PC DATA
    return;
  }

  parsePcPayload(body);

  Serial.printf("[pc] cpu=%.0f mem=%.0f gpu=%.0f t=%.0f up=%.1f dn=%.1f ok=%d len=%u\n",
                pc.cpuUse, pc.memUse, pc.gpuUse, pc.cpuTemp,
                pc.netUp, pc.netDown, pc.ok, (unsigned)body.length());
}

// 六行，每行 16px，各自缓存，只有变化的那一行才重绘
static char pcLn[6][32] = {{0}};

// 网速数值部分（不含 UP/DN 标签，标签交给行缓存，永不重绘）
void netSpeedStr(char* out, float kb) {
  if (kb >= 1000.0f) sprintf(out, "%.2f MB/s", kb / 1000.0f);
  else               sprintf(out, "%.1f KB/s", kb);
}

// 6 行 x 16px = 96，正好铺满屏幕，不能再留偏移，否则最后一行被顶出屏幕
#define PC_TOP      0
#define PC_STALE_MS 15000   // 超过这么久没拿到数据才认为掉线
#define PC_FALLBACK_MS PC_STALE_MS   // 无信号多久后回退显示时钟
#define PC_RETRY_MS   5000           // 回退期间探测间隔（别用 1 秒，会拖慢时钟）

// ============================================================
//  "标签 + 数值" 行缓存
//
//  数值每秒都在变，如果每次都整行清空重画，左边的标签就会跟着闪。
//  这里把标签和数值分开记录：标签没变就一个像素都不碰，只重画右侧数值区。
// ============================================================
#define ROW_SLOTS 10

static char    rowLabel[ROW_SLOTS][12];
static char    rowValue[ROW_SLOTS][40];
static int16_t rowValX[ROW_SLOTS];
static int16_t rowLabelX[ROW_SLOTS];   // 标签上次绘制的位置（位置变了也得重画）

void rowReset(int slot) {
  if (slot < 0 || slot >= ROW_SLOTS) return;
  rowLabel[slot][0] = 0;
  rowValue[slot][0] = 0;
  rowValX[slot] = -1;
  rowLabelX[slot] = -32768;
}

// labelX：标签左沿；valueX 传 -1 表示"紧跟标签之后"
void drawRowEx(int slot, int16_t y, int16_t labelX, const char* label,
               int16_t valueX, const char* value,
               uint16_t labelColor, uint16_t valueColor, bool force) {
  if (slot < 0 || slot >= ROW_SLOTS) return;

  bool labelChanged = force || strcmp(rowLabel[slot], label) != 0
                      || rowLabelX[slot] != labelX;
  bool valueChanged = force || strcmp(rowValue[slot], value) != 0;
  if (!labelChanged && !valueChanged) return;

  if (labelChanged) {
    display->fillRect(0, y, SCR_W, 16, 0);
    int16_t right = drawTextLeft(label, labelX, y, 16, labelColor);
    rowValX[slot] = (valueX < 0) ? (right + 3) : valueX;
    strcpy(rowLabel[slot], label);
    rowLabelX[slot] = labelX;
    valueChanged = true;                 // 整行重画过，数值必须补画
  }

  if (valueChanged) {
    int16_t vx = rowValX[slot];
    if (vx < 0) vx = 0;
    if (vx > SCR_W - 1) vx = SCR_W - 1;
    display->fillRect(vx, y, SCR_W - vx, 16, 0);
    drawTextLeft(value, vx, y, 16, valueColor);
    strcpy(rowValue[slot], value);
  }
}

// 温度行：数值 + 手绘度数圈 + C
void drawTempRowEx(int slot, int16_t y, int16_t labelX, const char* label,
                   float t, uint16_t labelColor, uint16_t valueColor, bool force) {
  if (slot < 0 || slot >= ROW_SLOTS) return;

  char num[12];
  sprintf(num, "%.0f", t);

  bool labelChanged = force || strcmp(rowLabel[slot], label) != 0
                      || rowLabelX[slot] != labelX;
  bool valueChanged = force || strcmp(rowValue[slot], num) != 0;
  if (!labelChanged && !valueChanged) return;

  if (labelChanged) {
    display->fillRect(0, y, SCR_W, 16, 0);
    rowValX[slot] = drawTextLeft(label, labelX, y, 16, labelColor) + 3;
    strcpy(rowLabel[slot], label);
    rowLabelX[slot] = labelX;
    valueChanged = true;
  }

  if (valueChanged) {
    int16_t vx = rowValX[slot];
    if (vx < 0) vx = 0;
    display->fillRect(vx, y, SCR_W - vx, 16, 0);
    drawTempStr(num, vx, y, 16, valueColor);
    strcpy(rowValue[slot], num);
  }
}

void pcRowsReset() {
  for (int i = 0; i < 6; i++) rowReset(i);
}

void pcUpdate(bool force) {
  char v[32];

  if (force) {
    display->fillScreen(0);
    pcRowsReset();
  }

  uint32_t now = millis();
  bool never = (pc.lastMs == 0);
  bool stale = never ? (now > 10000)            // 开机 10 秒内先别急着报错
                     : (now - pc.lastMs > PC_STALE_MS);

  if (stale) {
    if (strcmp(pcLn[0], "NO PC DATA")) {
      display->fillScreen(0);
      drawTextGap("NO PC DATA", 0, 40, SCR_W, 16, display->color565(90, 90, 90));
      strcpy(pcLn[0], "NO PC DATA");
      pcRowsReset();
    }
    return;
  }

  // 这一轮没抓到，但数据还新鲜 —— 保持画面不动，避免信号抖动导致闪屏
  if (!pc.ok && !force) return;

  const uint16_t C_CPU  = display->color565(0, 200, 255);
  const uint16_t C_MEM  = display->color565(0, 220, 120);
  const uint16_t C_GPU  = display->color565(120, 170, 255);
  const uint16_t C_TEMP = display->color565(255, 160, 0);
  const uint16_t C_NET  = display->color565(150, 150, 150);

  // 标签固定，只有右侧数值变化 —— 标签不会跟着闪
  sprintf(v, "%d%%", (int)pc.cpuUse);
  drawRowEx(0, PC_TOP + 0, 2, "CPU", -1, v, C_CPU, C_CPU, force);

  sprintf(v, "%d%%", (int)pc.memUse);
  drawRowEx(1, PC_TOP + 16, 2, "MEM", -1, v, C_MEM, C_MEM, force);

  sprintf(v, "%d%%", (int)pc.gpuUse);
  drawRowEx(2, PC_TOP + 32, 2, "GPU", -1, v, C_GPU, C_GPU, force);

  drawTempRowEx(3, PC_TOP + 48, 2, "TEMP", pc.cpuTemp, C_TEMP, C_TEMP, force);

  netSpeedStr(v, pc.netUp);
  drawRowEx(4, PC_TOP + 64, 2, "UP", -1, v, C_NET, C_NET, force);

  netSpeedStr(v, pc.netDown);
  drawRowEx(5, PC_TOP + 80, 2, "DN", -1, v, C_NET, C_NET, force);
}

// ============================================================
//  开关屏判断
// ============================================================
bool inOnWindow(const struct tm& t) {
  int now  = t.tm_hour * 60 + t.tm_min;
  int on   = cfg.onH * 60 + cfg.onM;
  int off  = cfg.offH * 60 + cfg.offM;
  if (on == off) return true;            // 相同视为常开
  if (on < off)  return (now >= on && now < off);
  return (now >= on || now < off);       // 跨零点
}

void applyScreenState(bool on) {
  if (on == screenOn) return;
  screenOn = on;
  if (on) {
    dma_display->setBrightness8(cfg.bright);
    display->fillScreen(0);
    fullRedraw = true;
  } else {
    display->fillScreen(0);
    dma_display->setBrightness8(0);
  }
  Serial.printf("[screen] %s\n", on ? "ON" : "OFF");
}

// ============================================================
//  网页
// ============================================================
#include "web_page.h"

void serveIndex() {
  if (LittleFS.exists("/index.html")) {
    File f = LittleFS.open("/index.html", "r");
    server.streamFile(f, "text/html");
    f.close();
  } else {
    server.send_P(200, "text/html", INDEX_HTML);
  }
}

void setupWeb() {
  server.on("/", serveIndex);
  server.on("/index.html", serveIndex);

  server.on("/api/config", HTTP_GET, []() {
    String s = "{";
    s += "\"bright\":" + String(cfg.bright);
    s += ",\"onH\":" + String(cfg.onH) + ",\"onM\":" + String(cfg.onM);
    s += ",\"offH\":" + String(cfg.offH) + ",\"offM\":" + String(cfg.offM);
    s += ",\"mode\":" + String(cfg.mode);
    s += ",\"airRefresh\":" + String(cfg.airRefresh);
    s += ",\"sensor\":\"" + String(cfg.sensor) + "\"";
    s += ",\"pcRefresh\":" + String(cfg.pcRefresh);
    s += ",\"pcUrl\":\"" + String(cfg.pcUrl) + "\"}";
    server.send(200, "application/json", s);
  });

  server.on("/api/config", HTTP_POST, []() {
    String body = server.arg("plain");
    long v;

    if (jsonNum(body, "bright", v)) cfg.bright = constrain(v, 0, 255);
    if (jsonNum(body, "onH", v))    cfg.onH = constrain(v, 0, 23);
    if (jsonNum(body, "onM", v))    cfg.onM = constrain(v, 0, 59);
    if (jsonNum(body, "offH", v))   cfg.offH = constrain(v, 0, 23);
    if (jsonNum(body, "offM", v))   cfg.offM = constrain(v, 0, 59);
    if (jsonNum(body, "airRefresh", v)) cfg.airRefresh = constrain(v, 10, 3600);
    if (jsonNum(body, "pcRefresh", v))  cfg.pcRefresh  = constrain(v, 1, 3600);

    bool modeChanged = false;
    if (jsonNum(body, "mode", v)) {
      if ((uint8_t)v != cfg.mode) modeChanged = true;
      cfg.mode = (v > 2) ? 0 : v;
    }
    jsonStr(body, "sensor", cfg.sensor, sizeof(cfg.sensor));
    jsonStr(body, "pcUrl",  cfg.pcUrl,  sizeof(cfg.pcUrl));
    pcUseSse = false;   // 地址变了，重新学习正确路径

    saveConfig();

    if (screenOn) dma_display->setBrightness8(cfg.bright);
    if (modeChanged) { display->fillScreen(0); fullRedraw = true; }

    server.send(200, "text/plain", "ok");
    Serial.println("[web] 配置已更新");
  });

  server.on("/api/status", HTTP_GET, []() {
    struct tm t;
    String tm = "--";
    if (getLocalTime(&t, 10)) {
      char b[12];
      sprintf(b, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
      tm = b;
    }
    String s = "{";
    s += "\"time\":\"" + tm + "\"";
    s += ",\"screen\":" + String(screenOn ? 1 : 0);
    s += ",\"mode\":" + String(cfg.mode);
    s += ",\"rssi\":" + String(WiFi.RSSI());
    s += ",\"pm25\":" + String(air.ok ? air.pm25 : -1, 1);
    s += ",\"co2\":" + String(air.co2, 1);
    s += ",\"temp\":" + String(air.temp, 1);
    s += ",\"hum\":" + String(air.hum, 1);
    s += "}";
    server.send(200, "application/json", s);
  });

  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });

  server.begin();
  Serial.println("[web] http://<ip>/");
}

// ============================================================
//  Arduino OTA 无线升级
//
//  ⚠️ 分区要求：必须用带 OTA 的分区方案（本项目用 Min SPIFFS，1.9MB APP ×2）。
//     huge_app / no_ota 没有备份 app 分区，OTA 会直接失败。
//     从 huge_app 换成 min_spiffs 需要 USB 重刷一次，之后就能无线升级了。
// ============================================================
#define OTA_HOSTNAME "led-backpack"
#define OTA_PASSWORD "2018aptx"

bool otaActive = false;

// OTA 期间在屏上显示进度，避免和正常刷新打架
void otaShow(const char* line, int percent) {
  display->fillScreen(0);
  drawTextGap(line, 0, 20, SCR_W, 16, display->color565(255, 255, 255));

  if (percent >= 0) {
    char b[8];
    sprintf(b, "%d", percent);
    drawTextGap(b, 0, 40, SCR_W, 32, display->color565(0, 200, 255));

    int x0 = 10, w = SCR_W - 20;
    display->drawRect(x0, 80, w, 8, display->color565(80, 80, 80));
    int fill = w * percent / 100;
    if (fill > 2) display->fillRect(x0 + 1, 81, fill - 2, 6, display->color565(0, 200, 80));
  }
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    otaActive = true;
    Serial.println("\n[ota] 开始接收固件…");
    otaShow("OTA UPDATE", 0);
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static int last = -1;
    int p = total ? (int)((uint64_t)progress * 100 / total) : 0;
    if (p != last) { last = p; otaShow("OTA UPDATE", p); }
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[ota] 完成，即将重启");
    otaShow("OTA DONE", 100);
    delay(500);
    otaActive = false;
  });

  ArduinoOTA.onError([](ota_error_t e) {
    const char* msg = "?";
    switch (e) {
      case OTA_AUTH_ERROR:    msg = "认证失败"; break;
      case OTA_BEGIN_ERROR:   msg = "开始失败"; break;
      case OTA_CONNECT_ERROR: msg = "连接失败"; break;
      case OTA_RECEIVE_ERROR: msg = "接收失败"; break;
      case OTA_END_ERROR:     msg = "结束失败"; break;
    }
    Serial.printf("[ota] 错误 %u: %s\n", e, msg);
    otaShow("OTA FAIL", -1);
    otaActive = false;
  });

  ArduinoOTA.begin();
  Serial.printf("[ota] 就绪：%s.local 或当前 IP，密码已设置\n", OTA_HOSTNAME);
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== LED Clock + Air + Web ===");

  if (!LittleFS.begin(true)) {
    Serial.println("[fs] LittleFS 挂载失败");
  } else {
    Serial.println("[fs] LittleFS OK");
    // 首次开机把网页写进 LittleFS（之后可从浏览器直接改）
    // 网页有改动（长度不同）就重写，避免升级固件后还是旧页面
    String cur = fsRead("/index.html");
    if (cur.length() != strlen(INDEX_HTML)) {
      fsWrite("/index.html", String(INDEX_HTML));
      Serial.printf("[fs] index.html 已更新 (%u -> %u)\n",
                    (unsigned)cur.length(), (unsigned)strlen(INDEX_HTML));
    }
  }

  loadConfig();

  // 显示：显式指定全部 14 根引脚，不依赖库的默认值
  HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);
  mxconfig.gpio.r1  = PIN_R1;
  mxconfig.gpio.g1  = PIN_G1;
  mxconfig.gpio.b1  = PIN_B1;
  mxconfig.gpio.r2  = PIN_R2;
  mxconfig.gpio.g2  = PIN_G2;
  mxconfig.gpio.b2  = PIN_B2;
  mxconfig.gpio.a   = PIN_A;
  mxconfig.gpio.b   = PIN_B;
  mxconfig.gpio.c   = PIN_C;
  mxconfig.gpio.d   = PIN_D;
  mxconfig.gpio.e   = PIN_E;
  mxconfig.gpio.lat = PIN_LAT;
  mxconfig.gpio.oe  = PIN_OE;
  mxconfig.gpio.clk = PIN_CLK;

  // 只创建一个对象：LogicalPanel 本身就是 MatrixPanel_I2S_DMA，
  // 两个指针指向它即可，避免重复初始化 DMA 缓冲
  display = new LogicalPanel(mxconfig);

  if (!display->begin()) {
    Serial.println("!! begin() 失败：DMA 内存不足");
    while (1) delay(1000);
  }
  display->setBrightness8(cfg.bright);   // 新版库要求 begin() 之后再设亮度
  display->setTextWrap(false);           // 不让长文本自动换行到下一行
  dma_display = display;                 // 同一个对象，供亮度控制等复用
  display->fillScreen(0);

  // 联网
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    drawTextGap("IP", 0, 30, SCR_W, 14, display->color565(120, 120, 120));
    drawTextGap(WiFi.localIP().toString().c_str(), 0, 46, SCR_W, 20,
                display->color565(255, 255, 255));
    setupOTA();          // 无线升级：led-backpack.local，密码见 OTA_PASSWORD
  }

  // 对时
  configTime(GMT_OFFSET, DST_OFFSET, NTP1, NTP2, NTP3);
  struct tm t;
  for (int i = 0; i < 40; i++) {
    if (getLocalTime(&t, 1000) && t.tm_year >= 120) { timeValid = true; break; }
    delay(300);
  }

  setupWeb();

  display->fillScreen(0);
  fullRedraw = true;

  if (cfg.mode == 1) fetchAir();
}

// ============================================================
void loop() {
  ArduinoOTA.handle();          // 必须在最前，且不能被下面的 early return 跳过

  if (otaActive) {              // 升级中不做常规刷新，避免覆盖进度画面
    delay(5);
    return;
  }

  server.handleClient();

  static uint32_t lastAir = 0;
  static uint32_t lastPc  = 0;
  static uint32_t lastWifi = 0;

  struct tm t;
  bool gotTime = getLocalTime(&t, 50);

  // 10 秒一次：WiFi 自愈
  if (millis() - lastWifi > 10000) {
    lastWifi = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    } else if (!timeValid && gotTime && t.tm_year >= 120) {
      timeValid = true;
    }
  }

  if (!gotTime || t.tm_year < 120) {
    delay(50);
    return;
  }

  // 开关屏
  applyScreenState(inOnWindow(t));

  if (!screenOn) {
    delay(50);
    return;
  }

  // 整屏重绘时进度条状态必须归零，否则切回时钟后只会补一段、前面缺一截
  if (fullRedraw) barLastW = 0;

  if (cfg.mode == 0) {
    // ---------- 时钟 ----------
    clockRender(t, fullRedraw);
  } else if (cfg.mode == 1) {
    // ---------- 空气质量（局部刷新）----------
    // 刚切过来且还没数据时立即抓一次，别等满一个刷新周期
    bool needFetch = (millis() - lastAir > (uint32_t)cfg.airRefresh * 1000UL) ||
                     (fullRedraw && air.lastMs == 0);

    if (needFetch) {
      lastAir = millis();
      fetchAir();
    }

    if (fullRedraw) {
      display->fillScreen(0);
      airHeader();        // 标题只在切过来时画一次
      airUpdate(true);
    } else if (needFetch) {
      airUpdate(false);   // 只有数值变化的区域会重绘
    }
  } else {
    // ---------- 电脑状态（无信号自动回退到时钟）----------
    // 回退期间只每 5 秒探一次，且用极短的超时，避免把主循环拖住
    uint32_t pcInterval = pcFallback ? PC_RETRY_MS
                                     : (uint32_t)cfg.pcRefresh * 1000UL;

    bool needFetch = (millis() - lastPc > pcInterval) ||
                     (fullRedraw && pc.lastMs == 0);

    if (needFetch) {
      lastPc = millis();
      fetchPc(pcFallback);          // 回退时走"探测"模式
    }

    // 超过 PC_FALLBACK_MS 没拿到新数据 → 先显示时钟；数据回来再自动切回
    uint32_t nowMs = millis();
    bool pcStale = (pc.lastMs == 0) ? (nowMs > PC_FALLBACK_MS)
                                    : (nowMs - pc.lastMs > PC_FALLBACK_MS);

    bool switched = (pcStale != pcFallback);
    pcFallback = pcStale;

    if (switched) {
      Serial.printf("[pc] %s\n", pcFallback ? "无信号 → 回退显示时钟" : "信号恢复 → 切回电脑状态");
      display->fillScreen(0);
      barLastW = 0;
    }

    if (pcFallback) {
      clockRender(t, fullRedraw || switched);
    } else {
      if (fullRedraw || switched) pcUpdate(true);
      else if (needFetch)         pcUpdate(false);
    }
  }
  fullRedraw = false;

  delay(20);
}