#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
hub_decode.py —— 从逻辑分析仪抓包还原 HUB75 / HUB12 点阵屏画面

用途：不用碰固件，直接把"模组 -> 驱动链"上的时序翻译成图像。
      还原出来后，照抄位序/扫描方式，用 ESP32 重放即可。

输入：PulseView / sigrok 导出的 CSV（第一行是表头，每列一个通道，值 0/1）
      采样率越高越好；只要完整覆盖一帧即可（通常几十 ms）。

示例
----
  单色 1/4 扫（HUB12，常见 P10 模组）:
    python hub_decode.py cap.csv --clk CLK --lat LAT --oe OE \
        --addr A,B --data R --width 128 --rows-per-scan 1

  HUB75 1/16 扫（取红色通道演示）:
    python hub_decode.py cap.csv --clk CLK --lat LAT --oe OE \
        --addr A,B,C,D --data R1 --width 128 --rows-per-scan 1

输出
----
  <out>_<通道名>.png   位图（需要 Pillow，没有就只出 txt）
  <out>_<通道名>.txt   ASCII 版，0/1 两色，快速核对用

注意
----
  * 位序可能是反的（移位寄存器先出的那个 bit 在屏幕最右边），图像左右镜像
    属正常，用 --mirror 翻转。
  * 多扫面板的"行"在物理上是交织的，这里按地址顺序堆叠，先看形状对不对，
    再决定怎么重排。
"""

import argparse
import csv
import sys


def load_csv(path):
    with open(path, newline="", encoding="utf-8-sig") as f:
        r = csv.reader(f)
        try:
            header = [h.strip() for h in next(r)]
        except StopIteration:
            sys.exit("空文件")
        rows = []
        for row in r:
            if not row:
                continue
            try:
                rows.append([int(float(c)) for c in row])
            except ValueError:
                continue          # 跳过注释/非数字行
    if not rows:
        sys.exit("CSV 里没有数据行")
    return header, rows


def pick(header, names, what):
    idx = []
    for n in names:
        for i, h in enumerate(header):
            if h.lower() == n.lower():
                idx.append(i)
                break
        else:
            hit = [i for i, h in enumerate(header) if n.lower() in h.lower()]
            if len(hit) != 1:
                sys.exit(f"找不到唯一的{what}通道 '{n}'；表头: {header}")
            idx.append(hit[0])
    return idx


def decode(rows, i_clk, i_lat, i_oe, i_addr, i_data, width, n_addr):
    """返回 {addr: 位串}，每个地址一次 LAT 锁存的内容。"""
    latched = {}
    seen_bits = 0
    cur = 0
    prev_clk = rows[0][i_clk]
    prev_lat = rows[0][i_lat]

    for row in rows[1:]:
        clk = row[i_clk]
        lat = row[i_lat]

        if clk and not prev_clk:                      # CLK 上升沿：移入一位
            cur = (cur << 1) | row[i_data]
            seen_bits += 1

        if lat and not prev_lat:                      # LAT 上升沿：锁存
            addr = 0
            for bit_pos, ia in enumerate(i_addr):     # 低位在前
                addr |= (row[ia] & 1) << bit_pos
            latched[addr] = cur
            cur = 0

        prev_clk, prev_lat = clk, lat

    if not latched:
        sys.exit("没抓到 LAT 上升沿：检查 --lat 通道名，或把采样率降低/加长抓取时间")
    return latched, seen_bits


def to_text(bits, width):
    s = format(bits & ((1 << (width + 1)) - 1), "0{}b".format(width))
    return s[-width:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--clk", required=True)
    ap.add_argument("--lat", required=True)
    ap.add_argument("--oe", default=None, help="可选，仅用于统计有效时间")
    ap.add_argument("--addr", default="", help="逗号分隔，低位在前，如 A,B,C,D")
    ap.add_argument("--data", required=True, help="逗号分隔，可多条，逐条单独出图")
    ap.add_argument("--width", type=int, default=128)
    ap.add_argument("--rows-per-scan", type=int, default=1,
                    help="每个扫描地址对应几行（HUB75 常见 2，单色常见 1）")
    ap.add_argument("--mirror", action="store_true", help="左右翻转")
    ap.add_argument("--out", default="frame")
    a = ap.parse_args()

    header, rows = load_csv(a.csv)
    i_clk = pick(header, [a.clk], "CLK")[0]
    i_lat = pick(header, [a.lat], "LAT")[0]
    i_oe = pick(header, [a.oe], "OE")[0] if a.oe else None
    addr_names = [s.strip() for s in a.addr.split(",") if s.strip()]
    data_names = [s.strip() for s in a.data.split(",") if s.strip()]
    i_addr = pick(header, addr_names, "地址")
    i_data = pick(header, data_names, "数据")

    n_addr = 1 << len(i_addr)
    print(f"通道: clk={a.clk} lat={a.lat} addr={addr_names} data={data_names}")
    print(f"样本: {len(rows)} 行；扫描地址数: {n_addr}")

    try:
        from PIL import Image
        have_pil = True
    except ImportError:
        have_pil = False

    for name, idx in zip(data_names, i_data):
        latched, seen_bits = decode(rows, i_clk, i_lat, i_oe, i_addr, idx,
                                   a.width, n_addr)
        hits = sorted(latched)
        lens = {len(to_text(v, a.width).lstrip("0")) for v in latched.values()}
        print(f"\n[{name}] 抓到 {len(latched)}/{n_addr} 个地址，移位约 {seen_bits} bit")
        print(f"[{name}] 出现过的地址: {hits[:32]}{' ...' if len(hits) > 32 else ''}")

        lines = []
        for addr in range(n_addr):
            bits = to_text(latched.get(addr, 0), a.width)
            if a.mirror:
                bits = bits[::-1]
            for _ in range(a.rows_per_scan):
                lines.append(bits)

        txt = f"{a.out}_{name}.txt"
        with open(txt, "w", encoding="utf-8") as f:
            f.write(f"# {name}  width={a.width} addr={len(i_addr)} "
                    f"rows_per_scan={a.rows_per_scan}\n")
            f.write("\n".join(lines) + "\n")
        print(f"  -> {txt}")

        if have_pil:
            scale = max(1, 512 // max(1, a.width))
            w, h = a.width * scale, len(lines) * scale
            img = Image.new("1", (w, h), 0)
            px = img.load()
            for y, line in enumerate(lines):
                for x, ch in enumerate(line):
                    if ch == "1":
                        for dy in range(scale):
                            for dx in range(scale):
                                px[x * scale + dx, y * scale + dy] = 1
            png = f"{a.out}_{name}.png"
            img.save(png)
            print(f"  -> {png} ({w}x{h})")
        else:
            print("  (未安装 Pillow，跳过 PNG)")

    print("\n完成。若图像是镜像/旋转的，试 --mirror；行序不对就先看形状。")


if __name__ == "__main__":
    main()
