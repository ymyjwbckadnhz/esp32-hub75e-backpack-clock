// ============================================================
//  hub75_bringup.ino —— 96 × 128 HUB75E 点屏验证
//
//  面板：96 宽 × 128 高 = 两块 96×64 模组竖向级联，1/32 扫描
//  依据：开源项目 hub75e_96_128（爆改车间 / 捡垃圾 RGB-LED 屏）
//
//  库：ESP32-HUB75-MatrixPanel-DMA（本项目附带的那份，API 是 HUB75_I2S_CFG）
//      注意别装成新版库 —— 新版把类名改成了 HUB75_I2S_CFLAGS，编译不过。
//
//  ⚠️ 两种板子二选一，改下面这个宏：
//      USE_S3_BOARD 0 —— ESP32 经典款（原方案，引脚用库默认值 + E=32）
//      USE_S3_BOARD 1 —— ESP32-S3（微雪 S3-ETH 等，E 必须换脚，不能是 32）
// ============================================================

#define USE_S3_BOARD 0

// ---- 面板参数（两块 96×64 竖向拼接）----
#define PANEL_RES_X 96     // 单块模组宽
#define PANEL_RES_Y 64     // 单块模组高
#define NUM_ROWS    2      // 上下 2 块
#define NUM_COLS    1
#define PANEL_CHAIN (NUM_ROWS * NUM_COLS)

// 新版库：级联方式用枚举，CHAIN_BOTTOM_LEFT_UP_ZZ = 旧库 SERPENT=false/TOPDOWN=false
#define CHAIN_MODE CHAIN_BOTTOM_LEFT_UP_ZZ

#include <ESP32-VirtualMatrixPanel-I2S-DMA.h>

MatrixPanel_I2S_DMA *dma_display = nullptr;
VirtualMatrixPanel  *virtualDisp = nullptr;

// 整屏逻辑尺寸
#define W (PANEL_RES_X * NUM_COLS)   // 96
#define H (PANEL_RES_Y * NUM_ROWS)   // 128

// ---------------- 初始化 ----------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== 96x128 HUB75E bring-up ===");

  HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);

#if USE_S3_BOARD
  // ESP32-S3：必须全部显式指定，且 E 不能用 32（S3 的 32 是 flash 引脚）
  // 这套分配避开 PSRAM(33~37)、UART0(43/44)、strapping(0/45/46)、USB(19/20)
  mxconfig.gpio.r1  = 1;
  mxconfig.gpio.g1  = 2;
  mxconfig.gpio.b1  = 3;
  mxconfig.gpio.r2  = 15;
  mxconfig.gpio.g2  = 16;
  mxconfig.gpio.b2  = 17;
  mxconfig.gpio.a   = 18;
  mxconfig.gpio.b   = 38;
  mxconfig.gpio.c   = 39;
  mxconfig.gpio.d   = 40;
  mxconfig.gpio.e   = 41;
  mxconfig.gpio.lat = 47;
  mxconfig.gpio.oe  = 48;
  mxconfig.gpio.clk = 42;
#else
  // ESP32 经典款：其余用库默认值，但下面两个必须显式指定
  // 默认：R1=25 G1=26 B1=27 R2=14 G2=12 B2=13 A=23 B=19 C=5 D=17 LAT=4 OE=15 CLK=16
  mxconfig.gpio.e  = 32;

  // ⚠️ 关键修正：G2 默认是 GPIO12，而 GPIO12 是 strapping 脚（MTDI）。
  //    接上屏后被拉高 → flash 电压切到 1.8V → 读出垃圾 → 无限重启
  //    现象：load:0xffff0a00,len:-1 + rst:0x10 (RTCWDT_RTC_RESET)
  //    所以挪到 GPIO22（非 strapping）。接线也要跟着把 G2 移到 GPIO22。
  mxconfig.gpio.g2 = 22;
#endif

  // 驱动 IC 是 ICN2037BP（移位寄存器型），默认 SHIFTREG 即可
  // 若发现颜色错乱/有条纹，再试 FM6126A 之类
  // mxconfig.driver = HUB75_I2S_CFG::SHIFTREG;

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->setBrightness8(60);          // 0-255，先给 ~24%，防掉压

  if (!dma_display->begin()) {
    Serial.println("****** begin() 失败：I2S DMA 内存分配不足 ***********");
    Serial.println("       试试降 MATRIX_HEIGHT 或 PIXEL_COLOR_DEPTH_BITS");
    while (1) delay(1000);
  }

  virtualDisp = new VirtualMatrixPanel((*dma_display), NUM_ROWS, NUM_COLS,
                                       PANEL_RES_X, PANEL_RES_Y, CHAIN_MODE);
  virtualDisp->setRotation(1);   // 新版库用 setRotation(uint8_t)，1 = 90°
  Serial.printf("init OK: %dx%d (2 x %dx%d)\r\n", W, H, PANEL_RES_X, PANEL_RES_Y);

  // 开机自检：红 → 绿 → 蓝 → 白
  const uint16_t seq[] = {
    virtualDisp->color565(255, 0, 0),
    virtualDisp->color565(0, 255, 0),
    virtualDisp->color565(0, 0, 255),
    virtualDisp->color565(80, 80, 80),
  };
  for (int i = 0; i < 4; i++) {
    virtualDisp->fillScreen(seq[i]);
    delay(500);
  }
  // virtualDisp->fillScreen(0);
}

// ---------------- 主循环 ----------------
void loop() {
  // static int frame = 0;

  // virtualDisp->fillScreen(0);

  // // 1) 边框 + 网格：一眼看出有没有错位/镜像
  // virtualDisp->drawRect(0, 0, W, H, virtualDisp->color565(60, 60, 60));
  // for (int x = 0; x < W; x += 16)
  //   virtualDisp->drawFastVLine(x, 0, H, virtualDisp->color565(0, 30, 0));
  // for (int y = 0; y < H; y += 16)
  //   virtualDisp->drawFastHLine(0, y, W, virtualDisp->color565(0, 30, 0));

  // // 2) 四角锚点：判断有没有翻转/上下颠倒
  // virtualDisp->fillRect(0,     0,     8, 8, virtualDisp->color565(255, 0, 0));    // 左上 红
  // virtualDisp->fillRect(W - 8, 0,     8, 8, virtualDisp->color565(0, 255, 0));    // 右上 绿
  // virtualDisp->fillRect(0,     H - 8, 8, 8, virtualDisp->color565(0, 0, 255));    // 左下 蓝

  // // 3) 上下两块模组的分界线：如果两块拼接位置错了，这里会看出来
  // virtualDisp->drawFastHLine(0, PANEL_RES_Y, W, virtualDisp->color565(255, 200, 0));

  // // 4) 移动方块：检查刷新率和撕裂
  // int x = (frame * 3) % (W - 12);
  // virtualDisp->fillRect(x, 60, 12, 12, virtualDisp->color565(0, 200, 255));

  // // 5) 文字
  // virtualDisp->setTextSize(2);
  // virtualDisp->setTextColor(virtualDisp->color565(255, 220, 0));
  // virtualDisp->setCursor(6, 8);
  // virtualDisp->print("96x128");

  // virtualDisp->setTextSize(1);
  // virtualDisp->setTextColor(virtualDisp->color565(0, 255, 0));
  // virtualDisp->setCursor(6, 34);
  // virtualDisp->print("HUB75E 1/32");

  // frame++;
  // delay(40);
}
