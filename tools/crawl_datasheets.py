"""爬取上海太矽电子 (txsemi.com) 官网：
1. 技术文档下载中心的全部 datasheet PDF -> datasheets/
2. 产品中心各系列参数表 -> data/web_products.json
"""
import json
import re
import time
from html import unescape
from pathlib import Path

import requests
from bs4 import BeautifulSoup

BASE = "http://txsemi.com"
ROOT = Path(__file__).resolve().parent.parent
PDF_DIR = ROOT / "datasheets"
DATA_DIR = ROOT / "data"
HEADERS = {"User-Agent": "Mozilla/5.0 (shtx-agents datasheet crawler)"}


def get(url):
    for i in range(3):
        try:
            r = requests.get(url, headers=HEADERS, timeout=30)
            r.raise_for_status()
            return r
        except requests.RequestException:
            if i == 2:
                raise
            time.sleep(2)


def soup(url):
    return BeautifulSoup(get(url).content.decode("utf-8", "ignore"), "html.parser")


def crawl_pdfs():
    PDF_DIR.mkdir(exist_ok=True)
    seen = {}
    page = 1
    while True:
        s = soup(f"{BASE}/cn/download.html?type=2&page={page}")
        links = [a["href"] for a in s.find_all("a", href=True) if a["href"].lower().endswith(".pdf")]
        if not links:
            break
        for href in links:
            name = unescape(href.rsplit("/", 1)[-1])
            seen.setdefault(name, href)
        page += 1
    print(f"共发现 {len(seen)} 份 datasheet（{page - 1} 页）")
    for name, href in sorted(seen.items()):
        dst = PDF_DIR / name
        if dst.exists() and dst.stat().st_size > 0:
            continue
        data = get(BASE + href).content
        if not data.startswith(b"%PDF"):
            print("  跳过(非PDF):", name)
            continue
        dst.write_bytes(data)
        print(f"  下载 {name} ({len(data) // 1024} KB)")
    return seen


def cell_text(td):
    return re.sub(r"\s+", " ", td.get_text(" ", strip=True)).replace("/td>", "").strip()


def crawl_product_tables():
    """每个大类页面 productd.html?p_id=X&id=Y 中都含该大类全部系列的参数表。"""
    home = soup(f"{BASE}/cn/Index.html")
    pairs = sorted({(int(m.group(1)), int(m.group(2)))
                    for a in home.find_all("a", href=True)
                    if (m := re.search(r"productd\.html\?p_id=(\d+)&id=(\d+)", a["href"]))})
    products = []
    for p_id, sid in pairs:
        s = soup(f"{BASE}/cn/productd.html?p_id={p_id}&id={sid}")
        # 当前大类名 / 系列名
        category = series = ""
        for a in s.find_all("a", href=True):
            if f"p_id={p_id}&id={sid}" in a["href"] and a.get_text(strip=True):
                series = a.get_text(strip=True)
        # 只解析当前系列对应的表：页面里每个系列一个 tab，表格按顺序排列
        tables = s.find_all("table")
        series_links = []
        for a in s.find_all("a", href=True):
            m = re.search(rf"productd\.html\?p_id={p_id}&id=(\d+)", a["href"])
            if m and a.get_text(strip=True) and int(m.group(1)) not in [x[0] for x in series_links]:
                series_links.append((int(m.group(1)), a.get_text(strip=True)))
        products.append({"p_id": p_id, "id": sid, "series": series,
                         "series_list": series_links, "tables": [parse_table(t) for t in tables]})
    return products


def parse_table(t):
    """展开 rowspan/colspan，返回规整的二维表。"""
    grid, pending = [], {}  # pending[(row, col)] = text （rowspan 占位）
    for r, tr in enumerate(t.find_all("tr")):
        row, c = [], 0
        cells = tr.find_all(["td", "th"])
        i = 0
        while i < len(cells) or (r, c) in pending:
            if (r, c) in pending:
                row.append(pending.pop((r, c)))
                c += 1
                continue
            td = cells[i]
            i += 1
            text = cell_text(td)
            cs = int(td.get("colspan", 1) or 1)
            rs = int(td.get("rowspan", 1) or 1)
            for k in range(cs):
                row.append(text)
                for dr in range(1, rs):
                    pending[(r + dr, c + k)] = text
            c += cs
        if any(row):
            grid.append(row)
    return grid


MODEL_RE = re.compile(r"^TX[A-Z0-9]", re.I)


def split_header(rows):
    """表头可能有多行（如 磁场特性 -> B_op/B_rp/B_hys），合并为一行列名。"""
    n = 0
    while n < len(rows) and not MODEL_RE.match(rows[n][0] or ""):
        n += 1
    head_rows, body = rows[:n], rows[n:]
    width = max((len(r) for r in rows), default=0)
    header = []
    for c in range(width):
        parts = []
        for hr in head_rows:
            if c < len(hr) and hr[c] and hr[c] not in parts:
                parts.append(hr[c])
        header.append(" ".join(parts))
    return header, body


def build_web_products(raw):
    """把 (大类页 -> 多个表) 规整成 {model: {series, params}}。"""
    cat_names = {25: "电源管理芯片", 26: "霍尔传感器芯片", 36: "驱动芯片", 37: "MCU",
                 38: "通信协议芯片", 59: "红外热释/微波雷达处理芯片", 60: "电子烟芯片", 61: "定时器芯片"}
    result = {}
    by_cat = {}
    for item in raw:
        by_cat.setdefault(item["p_id"], item)  # 同一大类页面的表格相同，取一份即可
    for p_id, item in by_cat.items():
        series_list = item["series_list"]
        for idx, rows in enumerate(item["tables"]):
            if len(rows) < 2:
                continue
            header, body = split_header(rows)
            series = re.sub(r"\s+", " ", series_list[idx][1]) if idx < len(series_list) else ""
            for r in body:
                if not r or not MODEL_RE.match(r[0] or ""):
                    continue
                model = r[0].strip()
                params = {header[i]: r[i] for i in range(1, min(len(header), len(r))) if r[i]}
                result[model] = {"model": model, "category": cat_names.get(p_id, ""),
                                 "series": series, "params": params}
    return result


if __name__ == "__main__":
    DATA_DIR.mkdir(exist_ok=True)
    crawl_pdfs()
    raw = crawl_product_tables()
    web = build_web_products(raw)
    (DATA_DIR / "web_products.json").write_text(json.dumps(web, ensure_ascii=False, indent=1), "utf-8")
    print(f"官网参数表共 {len(web)} 个型号 -> data/web_products.json")
