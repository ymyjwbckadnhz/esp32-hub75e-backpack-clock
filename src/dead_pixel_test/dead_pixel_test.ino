// ============================================================
//  dead_pixel_test.ino —— 96×128 HUB75E 坏点检测工具
//
//  功能：
//   1. 整屏纯色：红 / 绿 / 蓝 / 白 / 黑 / 青 / 品红 / 黄 / 灰度
//   2. 框选区域：任意矩形，可整块点亮
//   3. 在框选范围内逐行、逐列、逐点扫描（手动或自动）
//   4. 网页控制，可选反色、十字辅助、亮度
//   5. 自动切换延时、区间范围、步长均可配置
//
//  硬件：ESP32-WROOM-32 + 96×128 面板（2 块 96×64 竖向级联，1/32 扫）
//  引脚：R1=25 G1=26 B1=27 R2=14 G2=12 B2=13 A=23 B=19 C=5 D=17 E=32
//        LAT=4 OE=15 CLK=16
//
//  说明：这块屏的模组是四象限走线，坐标映射见 LogicalPanel 类。
//        不改任何第三方库即可正确显示。
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "web_page.h"

// ---------------- 面板 ----------------
#define PANEL_RES_X 96
#define PANEL_RES_Y 64
#define NUM_ROWS    2
#define NUM_COLS    1
#define PANEL_CHAIN (NUM_ROWS * NUM_COLS)

// 横向使用：逻辑分辨率 128 宽 × 96 高
#define LANDSCAPE 1
#if LANDSCAPE
  #define SCR_W 128
  #define SCR_H 96
#else
  #define SCR_W 96
  #define SCR_H 128
#endif

#define VIRT_W (NUM_COLS * PANEL_RES_X)
#define VIRT_H (NUM_ROWS * PANEL_RES_Y)

// ---------------- 引脚 ----------------
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

// ---------------- WiFi 凭据 ----------------
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #warning "未找到 secrets.h，使用占位凭据。请参照 secrets.h.example 创建。"
  #define WIFI_SSID "YOUR_WIFI_SSID"
  #define WIFI_PASS "YOUR_WIFI_PASSWORD"
#endif

// ============================================================
//  逻辑屏：把 SCR_W × SCR_H 映射到 2 块 96×64 模组的 DMA 缓冲
//  映射链路：旋转 → 竖向级联 → 四象限重映射
// ============================================================
class LogicalPanel : public MatrixPanel_I2S_DMA {
public:
  explicit LogicalPanel(const HUB75_I2S_CFG &cfg) : MatrixPanel_I2S_DMA(cfg) {
    // 基类按 DMA 尺寸（192×64）初始化 GFX，会把 y >= 64 的绘制全裁掉
    _width  = SCR_W;
    _height = SCR_H;
  }

  static void mapPoint(int16_t x, int16_t y, int16_t &ox, int16_t &oy) {
#if LANDSCAPE
    int16_t vx = y;
    int16_t vy = (VIRT_H - 1) - x;
#else
    int16_t vx = x;
    int16_t vy = y;
#endif
    int  row = vy / PANEL_RES_Y;
    int  ry  = (NUM_ROWS - row - 1);
    int16_t cx = ((NUM_ROWS - (ry + 1)) * PANEL_RES_X) + vx;
    int16_t cy = vy % PANEL_RES_Y;

    // 96×64 模组内部四象限走线
    if      (cx < 96 && cy < 32)  cx += 96;
    else if (cx < 96)             cy -= 32;
    else if (cy < 32)             cy += 32;
    else                          cx -= 96;

    ox = cx;
    oy = cy;
  }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || x >= SCR_W || y < 0 || y >= SCR_H) return;
    int16_t px, py;
    mapPoint(x, y, px, py);
    MatrixPanel_I2S_DMA::drawPixel(px, py, color);
  }

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

LogicalPanel *display = nullptr;
WebServer server(80);

// ============================================================
//  状态
// ============================================================
enum TestMode : uint8_t {
  MODE_COLOR = 0,   // 整屏纯色
  MODE_ROW   = 1,   // 框选范围内逐行
  MODE_COL   = 2,   // 框选范围内逐列
  MODE_PIXEL = 3,   // 框选范围内逐点
  MODE_BOX   = 4    // 框选范围整块点亮
};

struct State {
  uint8_t  mode      = MODE_COLOR;
  uint8_t  colorIdx  = 0;
  uint32_t delayMs   = 800;
  int32_t  from      = 0;
  int32_t  to        = 0;
  int32_t  idx       = 0;
  int32_t  step      = 1;
  bool     running   = false;
  bool     invert    = false;
  bool     crosshair = false;
  uint8_t  bright    = 25;

  // 框选区域（含端点）
  int16_t  rx0 = 0, ry0 = 0;
  int16_t  rx1 = SCR_W - 1, ry1 = SCR_H - 1;
};

State st;
uint32_t lastTick = 0;

struct NamedColor { const char* name; uint8_t r, g, b; };
const NamedColor PALETTE[] = {
  { "红",  255,   0,   0 },
  { "绿",    0, 255,   0 },
  { "蓝",    0,   0, 255 },
  { "白",  255, 255, 255 },
  { "黑",    0,   0,   0 },
  { "青",    0, 255, 255 },
  { "品红",255,   0, 255 },
  { "黄",  255, 255,   0 },
  { "灰25", 64,  64,  64 },
  { "灰50",128, 128, 128 },
  { "灰75",192, 192, 192 },
};
const int PALETTE_N = sizeof(PALETTE) / sizeof(PALETTE[0]);

inline uint16_t rgbOf(const NamedColor &c) {
  return display->color565(c.r, c.g, c.b);
}

// ---------------- 框选尺寸 ----------------
inline int16_t rectW() { return st.rx1 - st.rx0 + 1; }
inline int16_t rectH() { return st.ry1 - st.ry0 + 1; }

void clampRect() {
  if (st.rx0 < 0) st.rx0 = 0;
  if (st.ry0 < 0) st.ry0 = 0;
  if (st.rx1 > SCR_W - 1) st.rx1 = SCR_W - 1;
  if (st.ry1 > SCR_H - 1) st.ry1 = SCR_H - 1;
  if (st.rx1 < st.rx0) st.rx1 = st.rx0;
  if (st.ry1 < st.ry0) st.ry1 = st.ry0;
}

// 各模式的元素总数（索引均相对于框选区域）
int32_t modeCount(uint8_t m) {
  switch (m) {
    case MODE_ROW:   return rectH();
    case MODE_COL:   return rectW();
    case MODE_PIXEL: return (int32_t)rectW() * rectH();
    case MODE_BOX:   return 1;
    default:         return PALETTE_N;
  }
}

void clampRange() {
  int32_t n = modeCount(st.mode);
  if (st.from < 0) st.from = 0;
  if (st.to >= n)  st.to = n - 1;
  if (st.to < st.from) st.to = st.from;
  if (st.step < 1) st.step = 1;
  if (st.idx < st.from) st.idx = st.from;
  if (st.idx > st.to)   st.idx = st.to;
}

// ============================================================
//  绘制
// ============================================================
void render() {
  clampRect();
  clampRange();

  uint16_t fg, bg;
  if (st.mode == MODE_COLOR) {
    fg = rgbOf(PALETTE[st.colorIdx % PALETTE_N]);
    bg = fg;
  } else {
    fg = st.invert ? display->color565(0, 0, 0)       : display->color565(255, 255, 255);
    bg = st.invert ? display->color565(255, 255, 255) : display->color565(0, 0, 0);
  }

  display->fillScreen(bg);

  int16_t w = rectW(), h = rectH();

  switch (st.mode) {
    case MODE_BOX:
      display->fillRect(st.rx0, st.ry0, w, h, fg);
      break;

    case MODE_ROW:
      if (st.idx >= 0 && st.idx < h)
        display->fillRect(st.rx0, st.ry0 + st.idx, w, 1, fg);
      break;

    case MODE_COL:
      if (st.idx >= 0 && st.idx < w)
        display->fillRect(st.rx0 + st.idx, st.ry0, 1, h, fg);
      break;

    case MODE_PIXEL: {
      if (st.idx < 0 || st.idx >= (int32_t)w * h) break;
      int16_t px = st.rx0 + (st.idx % w);
      int16_t py = st.ry0 + (st.idx / w);

      if (st.crosshair) {
        uint16_t mark = st.invert ? display->color565(0, 0, 255)
                                  : display->color565(0, 60, 160);
        display->drawFastHLine(st.rx0, py, w, mark);
        display->drawFastVLine(px, st.ry0, h, mark);
      }
      display->fillRect(px, py, 1, 1, fg);
      break;
    }
  }
}

// ============================================================
//  步进（索引相对于框选区域，支持环绕）
// ============================================================
void stepBy(int32_t delta) {
  if (st.step < 1) st.step = 1;

  int32_t lo = st.from;
  int32_t hi = st.to;
  int32_t span = hi - lo + 1;
  if (span <= 0) { st.idx = lo; render(); return; }

  int64_t n = (int64_t)st.idx + (int64_t)delta * st.step - lo;
  int64_t m = n % span;
  if (m < 0) m += span;
  st.idx = lo + (int32_t)m;

  render();
}

// 切换模式：区间重置为整个框选范围
void setMode(uint8_t m) {
  st.mode = m;
  st.from = 0;
  st.to   = modeCount(m) - 1;
  st.idx  = 0;
  st.step = 1;
  render();
}

// 修改框选：区间跟着重置
void setRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  st.rx0 = x0; st.ry0 = y0;
  st.rx1 = x1; st.ry1 = y1;
  clampRect();
  st.from = 0;
  st.to   = modeCount(st.mode) - 1;
  st.idx  = 0;
  render();
}

// ============================================================
//  极简 JSON
// ============================================================
static bool jsonNum(const String& s, const char* key, long& out) {
  String pat = String("\"") + key + "\"";
  int i = s.indexOf(pat);
  if (i < 0) return false;
  i = s.indexOf(':', i + pat.length());
  if (i < 0) return false;
  i++;
  while (i < (int)s.length() && !isDigit(s[i]) && s[i] != '-') i++;
  if (i >= (int)s.length()) return false;
  int j = i;
  while (j < (int)s.length() && (isDigit(s[j]) || s[j] == '-')) j++;
  out = s.substring(i, j).toInt();
  return true;
}

static bool jsonBool(const String& s, const char* key, bool& out) {
  String pat = String("\"") + key + "\"";
  int i = s.indexOf(pat);
  if (i < 0) return false;
  i = s.indexOf(':', i + pat.length());
  if (i < 0) return false;
  i++;
  int e = i;
  while (e < (int)s.length() && s[e] != ',' && s[e] != '}') e++;
  String v = s.substring(i, e);
  v.trim();
  if (v.startsWith("true"))  { out = true;  return true; }
  if (v.startsWith("false")) { out = false; return true; }
  return false;
}

// ============================================================
//  网页接口
// ============================================================
String stateJson() {
  String s = "{";
  s += "\"mode\":"      + String(st.mode);
  s += ",\"color\":"    + String(st.colorIdx);
  s += ",\"delay\":"    + String(st.delayMs);
  s += ",\"from\":"     + String(st.from);
  s += ",\"to\":"       + String(st.to);
  s += ",\"idx\":"      + String(st.idx);
  s += ",\"step\":"     + String(st.step);
  s += ",\"running\":"  + String(st.running ? "true" : "false");
  s += ",\"invert\":"   + String(st.invert ? "true" : "false");
  s += ",\"crosshair\":"+ String(st.crosshair ? "true" : "false");
  s += ",\"bright\":"   + String(st.bright);
  s += ",\"rx0\":"      + String(st.rx0);
  s += ",\"ry0\":"      + String(st.ry0);
  s += ",\"rx1\":"      + String(st.rx1);
  s += ",\"ry1\":"      + String(st.ry1);
  s += ",\"count\":"    + String(modeCount(st.mode));

  // 当前元素的实际屏幕坐标
  int16_t px = -1, py = -1;
  int16_t w = rectW(), h = rectH();
  switch (st.mode) {
    case MODE_ROW:   py = st.ry0 + st.idx; break;
    case MODE_COL:   px = st.rx0 + st.idx; break;
    case MODE_PIXEL:
      if (st.idx >= 0 && st.idx < (int32_t)w * h) {
        px = st.rx0 + (st.idx % w);
        py = st.ry0 + (st.idx / w);
      }
      break;
    case MODE_BOX:   px = st.rx0; py = st.ry0; break;
  }
  s += ",\"px\":" + String(px) + ",\"py\":" + String(py);
  s += "}";
  return s;
}

void setupWeb() {
  server.on("/", []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/state", []() {
    server.send(200, "application/json", stateJson());
  });

  server.on("/api/set", HTTP_POST, []() {
    String b = server.arg("plain");
    long v;

    // 框选：只要有任意一个端点变化就整体重设
    bool rectChanged = false;
    long x0 = st.rx0, y0 = st.ry0, x1 = st.rx1, y1 = st.ry1;
    if (jsonNum(b, "rx0", v)) { x0 = v; rectChanged = true; }
    if (jsonNum(b, "ry0", v)) { y0 = v; rectChanged = true; }
    if (jsonNum(b, "rx1", v)) { x1 = v; rectChanged = true; }
    if (jsonNum(b, "ry1", v)) { y1 = v; rectChanged = true; }
    if (rectChanged) setRect((int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1);

    if (jsonNum(b, "mode", v)) {
      if ((uint8_t)v != st.mode) setMode((uint8_t)v);
    }
    if (jsonNum(b, "color", v)) {
      st.colorIdx = constrain(v, 0, PALETTE_N - 1);
      if (st.mode == MODE_COLOR) st.idx = st.colorIdx;
    }
    if (jsonNum(b, "delay", v))  st.delayMs = constrain(v, 50, 60000);
    if (jsonNum(b, "step", v))   st.step    = constrain(v, 1, 65535);
    if (jsonNum(b, "from", v))   st.from    = v;
    if (jsonNum(b, "to", v))     st.to      = v;
    if (jsonNum(b, "bright", v)) {
      st.bright = constrain(v, 1, 255);
      display->setBrightness8(st.bright);
    }

    bool bv;
    if (jsonBool(b, "running", bv))   st.running   = bv;
    if (jsonBool(b, "invert", bv))    st.invert    = bv;
    if (jsonBool(b, "crosshair", bv)) st.crosshair = bv;

    if (jsonNum(b, "idx", v)) st.idx = v;

    render();
    server.send(200, "application/json", stateJson());
  });

  server.on("/api/step", HTTP_POST, []() {
    String b = server.arg("plain");
    long d = 1;
    jsonNum(b, "delta", d);
    st.running = false;
    stepBy((int32_t)d);
    server.send(200, "application/json", stateJson());
  });

  server.on("/api/goto", HTTP_POST, []() {
    String b = server.arg("plain");
    long v;
    if (jsonNum(b, "idx", v)) {
      st.idx = v;
      st.running = false;
      render();
    }
    server.send(200, "application/json", stateJson());
  });

  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== HUB75E 坏点检测 ===");

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

  display = new LogicalPanel(mxconfig);
  if (!display->begin()) {
    Serial.println("!! begin() 失败：DMA 内存不足");
    while (1) delay(1000);
  }
  display->setBrightness8(st.bright);
  display->fillScreen(0);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("未连上 WiFi，网页不可用");
  }

  setupWeb();

  // 开机画面：灰 50，避免全白把 USB 供电拉垮
  st.mode = MODE_COLOR;
  st.colorIdx = 9;
  st.from = 0;
  st.to   = PALETTE_N - 1;
  st.idx  = st.colorIdx;
  render();
}

// ============================================================
void loop() {
  server.handleClient();

  if (st.running && (millis() - lastTick >= st.delayMs)) {
    lastTick = millis();

    if (st.mode == MODE_COLOR) {
      st.colorIdx = (st.colorIdx + 1) % PALETTE_N;
      st.idx = st.colorIdx;
      render();
    } else if (st.mode != MODE_BOX) {
      stepBy(1);
    }
  }

  delay(2);
}
