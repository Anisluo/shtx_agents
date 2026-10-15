"""构建 SQLite 知识库：
  - 解析 datasheets/*.pdf 提炼要点（标题/特性/应用/概述/封装/极限参数）
  - 合并官网参数表 data/web_products.json
  - 从 data_info_with_password.xlsx 导入系统用户（口令以 PBKDF2-SHA256 加盐哈希保存）

用法：python tools/build_db.py [--reset-users]
  默认只新增数据库中不存在的用户，不覆盖已在系统内修改过的密码；--reset-users 会按 Excel 重置全部用户。
"""
import hashlib
import json
import os
import re
import sqlite3
import sys
from pathlib import Path

import fitz  # PyMuPDF
import openpyxl

ROOT = Path(__file__).resolve().parent.parent
PDF_DIR = ROOT / "datasheets"
DB_PATH = ROOT / "data" / "shtx.db"
WEB_JSON = ROOT / "data" / "web_products.json"
COMP_JSON = ROOT / "data" / "competitors.json"
USER_XLSX = ROOT / "data_info_with_password.xlsx"
PBKDF2_ITER = 120000

SCHEMA = """
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL UNIQUE COLLATE NOCASE,
    name TEXT NOT NULL DEFAULT '',
    dept TEXT NOT NULL DEFAULT '',
    role TEXT NOT NULL DEFAULT 'user',
    pw_salt TEXT NOT NULL,
    pw_hash TEXT NOT NULL,
    pw_iter INTEGER NOT NULL DEFAULT 120000,
    active INTEGER NOT NULL DEFAULT 1,
    created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
    last_login TEXT
);
CREATE TABLE IF NOT EXISTS competitors (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    model TEXT NOT NULL UNIQUE COLLATE NOCASE,
    brand TEXT NOT NULL DEFAULT '',
    aliases TEXT NOT NULL DEFAULT '',
    category TEXT NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '',
    key_specs TEXT NOT NULL DEFAULT '{}',
    tx_models TEXT NOT NULL DEFAULT '',
    datasheet_url TEXT NOT NULL DEFAULT '',
    note TEXT NOT NULL DEFAULT '',
    search_text TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS products (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    model TEXT NOT NULL UNIQUE COLLATE NOCASE,
    category TEXT NOT NULL DEFAULT '',
    series TEXT NOT NULL DEFAULT '',
    title TEXT NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '',
    features TEXT NOT NULL DEFAULT '',
    applications TEXT NOT NULL DEFAULT '',
    packages TEXT NOT NULL DEFAULT '',
    key_specs TEXT NOT NULL DEFAULT '{}',
    specs_norm TEXT NOT NULL DEFAULT '{}',
    datasheet TEXT NOT NULL DEFAULT '',
    search_text TEXT NOT NULL DEFAULT ''
);
"""

# 英文术语 -> 中文检索标签（便于中文需求检索英文 datasheet）
ZH_TAGS = [
    (r"\bLDO\b|low dropout|regulator", "LDO 线性稳压 稳压器 低压差"),
    (r"negative", "负压 负电压"),
    (r"step[- ]?down|\bbuck\b", "降压 DC-DC Buck 开关电源"),
    (r"step[- ]?up|\bboost\b", "升压 DC-DC Boost 开关电源"),
    (r"buck[- ]?boost", "升降压"),
    (r"synchronous", "同步整流"),
    (r"charger|charging|li-ion|lithium", "锂电池 充电 充电管理"),
    (r"reset|supervisor", "复位 上电复位 监控"),
    (r"detector|voltage detect", "电压检测 欠压检测 LVD"),
    (r"reference|shunt", "电压基准 基准源"),
    (r"low power|quiescent|low consumption", "低功耗 低静态电流"),
    (r"PSRR|ripple", "高PSRR 纹波抑制"),
    (r"hall", "霍尔 磁传感器"),
    (r"fan", "风扇"),
    (r"motor", "马达 电机 驱动"),
    (r"darlington", "达林顿"),
    (r"\bLCD\b", "液晶 段码屏"),
    (r"RS-?485", "RS485 通信 总线"),
    (r"\bPIR\b|pyroelectric|infrared", "红外 热释电 人体感应"),
    (r"timer|555", "定时器 振荡"),
    (r"\bMCU\b|microcontroller", "单片机 MCU 微控制器"),
    (r"battery", "电池供电"),
    (r"automotive", "汽车电子"),
    (r"short[- ]circuit|current limit", "短路保护 限流"),
    (r"thermal", "过热保护"),
    (r"high (input )?voltage", "高耐压 高压"),
]

PKG_RE = re.compile(r"\b(SOT-?23(?:-\d+L?|-\dB)?|SOT-?89(?:-\d)?B?|SOT-?223|SOT-?25|SOT-?353|TO-?92S?|TO-?94|TO-?252|TO-?220|TO-?263|"
                    r"E?SOP-?\d+|SSOP-?\d+|TSSOP-?\d+|MSOP-?\d+|DIP-?\d+|DFN[\d\*xX\.\-]*|QFN[\d\*xX\.\-]*|LQFP-?\d+|TSOT-?23(?:-\d+)?)",
                    re.I)

HEADINGS = ["Features", "Applications", "General Description", "Description", "Selection Table", "Order Information",
            "Ordering Information", "Pin Configuration", "Pin Assignment", "Typical Application", "Block Diagram",
            "Absolute Maximum Ratings", "Marking"]


def norm(s):
    return re.sub(r"\s+", " ", s or "").strip()


def clean_lines(text):
    out = []
    for ln in text.splitlines():
        ln = ln.strip().replace("⚫", "").replace("●", "").replace("•", "").strip()
        if ln:
            out.append(ln)
    return out


def section(lines, name, stop_names):
    """从行列表中取 name 标题之后直到下一个标题的内容。"""
    try:
        i = next(k for k, ln in enumerate(lines) if ln.lower().startswith(name.lower()) and len(ln) < len(name) + 4)
    except StopIteration:
        return []
    body = []
    for ln in lines[i + 1:]:
        if any(ln.lower().startswith(s.lower()) and len(ln) < len(s) + 6 for s in stop_names):
            break
        body.append(ln)
    return body


def join_bullets(lines):
    """把 PDF 断行合并成要点：以大写字母/数字开头视为新要点。"""
    items = []
    for ln in lines:
        if re.match(r"^(Ver|http|www\.|\d+$|[A-Z][a-z]{2,4}\.? ?\d{1,2},? ?\d{4})", ln):
            continue
        if items and (ln[0].islower() or items[-1].endswith((",", "and", "&", "-", "/", "("))):
            items[-1] += " " + ln
        else:
            items.append(ln)
    return [norm(x) for x in items if len(norm(x)) > 1]


def join_paragraph(lines):
    lines = [ln for ln in lines if not re.match(r"^(Ver\d|http|\d+$)", ln)]
    return norm(" ".join(lines))


def parse_pdf(path):
    doc = fitz.open(path)
    first = doc[0].get_text()
    full = "\n".join(p.get_text() for p in doc)
    lines = clean_lines(first)
    all_lines = clean_lines(full)

    # 页眉： "  TX6216" / "http://www.txsemi.com    300mA Low Power LDO"
    model, title = "", ""
    for ln in lines[:4]:
        m = re.match(r"^(TX[\w\-]+)(?:\s+series)?$", ln, re.I)
        if m and not model:
            model = m.group(1)
        m2 = re.match(r"^https?://www\.txsemi\.com\s+(.+)$", ln, re.I)
        if m2:
            title = norm(m2.group(1))
    if not model:
        model = re.sub(r"(_E)?_V[\d\.]+.*$", "", Path(path).stem).strip()

    stops = HEADINGS + ["Electrical Characteristics", "Recommended Operating", "Thermal", "Pin Description"]
    features = join_bullets(section(lines, "Features", stops))
    apps = join_bullets(section(lines, "Applications", stops))
    desc = join_paragraph(section(lines, "General Description", stops) or section(lines, "Description", stops))

    abs_max = section(all_lines, "Absolute Maximum Ratings", stops + ["Note", "Electrical"])
    abs_max = norm(" ".join(abs_max))[:500]

    pkgs = []
    for m in PKG_RE.finditer(" ".join(features) + " " + desc + " " + " ".join(section(lines, "Order Information", stops))):
        p = m.group(1).upper().replace("SOT-", "SOT").replace("TO-", "TO")
        if p not in pkgs:
            pkgs.append(p)
    ver = re.search(r"Ver\s*([\d\.]+)", first)
    return {"model": model, "title": title, "features": features, "applications": apps, "description": desc,
            "abs_max": abs_max, "packages": pkgs, "version": ver.group(1) if ver else "",
            "file": Path(path).name, "pages": doc.page_count}


def model_pattern(m):
    """TX75XXH -> 正则；X/XX 视为通配。"""
    m = m.upper().replace(" ", "")
    p = ""
    i = 0
    while i < len(m):
        if m[i] == "X" and (i + 1 < len(m) and m[i + 1] == "X"):
            p += r"[0-9A-Z]{1,3}"
            i += 2
        elif m[i] == "X" and i > 2:
            p += r"[0-9A-Z]"
            i += 1
        else:
            p += re.escape(m[i])
            i += 1
    return re.compile("^" + p)


def ver_key(v):
    return tuple(int(x) for x in re.findall(r"\d+", v or "0"))


def infer_category(text):
    t = text.lower()
    if "hall" in t:
        return "霍尔传感器芯片", "霍尔传感器芯片"
    rules = [("charger", "锂电池充电管理芯片 (Charge)"), ("reset", "复位芯片 (Reset)"), ("supervisor", "复位芯片 (Reset)"),
             ("detector", "电压检测芯片 (LVD)"), ("reference", "电压基准芯片 (Reference)"), ("shunt", "电压基准芯片 (Reference)"),
             ("buck-boost", "直流升压降压 (Buck-Boost)"), ("step-down", "直流降压 DC-DC Buck"), ("buck", "直流降压 DC-DC Buck"),
             ("step-up", "直流升压 DC-DC Boost"), ("boost", "直流升压 DC-DC Boost"),
             ("negative", "负压线性稳压器"), ("ldo", "线性稳压 LDO"), ("regulator", "线性稳压 LDO")]
    for k, s in rules:
        if k in t:
            return "电源管理芯片", s
    return "其他", "其他"


def build_products():
    web = json.loads(WEB_JSON.read_text("utf-8")) if WEB_JSON.exists() else {}
    products = {k.upper(): {**v, "title": "", "features": [], "applications": [], "description": "", "abs_max": "",
                            "packages": [], "datasheet": "", "ds_version": ""} for k, v in web.items()}

    # 解析 PDF，同型号多版本取最新
    sheets = {}
    for pdf in sorted(PDF_DIR.glob("*.pdf")):
        try:
            info = parse_pdf(pdf)
        except Exception as e:  # noqa: BLE001
            print("  解析失败", pdf.name, e)
            continue
        key = info["model"].upper()
        if key not in sheets or ver_key(info["version"]) > ver_key(sheets[key]["version"]):
            sheets[key] = info
    print(f"解析 datasheet {len(sheets)} 个型号")

    def attach(p, ds):
        p.update(title=ds["title"], features=ds["features"], applications=ds["applications"],
                 description=ds["description"], abs_max=ds["abs_max"], datasheet=ds["file"], ds_version=ds["version"])
        p["packages"] = list(dict.fromkeys(p.get("packages", []) + ds["packages"]))

    # 第一轮：型号精确匹配
    rest = {}
    for key, ds in sheets.items():
        if key in products:
            attach(products[key], ds)
        else:
            rest[key] = ds
    # 第二轮：通配匹配（TX71XXM / TX6051-adj 等）仅关联尚无 datasheet 的型号；仍无匹配则作为独立型号入库
    for key, ds in sorted(rest.items(), key=lambda kv: -len(kv[0])):  # 更具体的型号优先
        pat, base = model_pattern(key), key.split("-")[0]
        similar = [k for k in products if pat.match(k) or model_pattern(k).match(key)
                   or (len(base) >= 5 and k.split("-")[0] == base)]
        free = [k for k in similar if not products[k].get("datasheet")]
        for k in free:
            attach(products[k], ds)
        if not free:
            if similar:  # 同系列变体，沿用其分类
                cat, ser = products[similar[0]]["category"], products[similar[0]]["series"]
            else:
                cat, ser = infer_category(ds["title"] + " " + ds["description"])
            products[key] = {"model": ds["model"], "category": cat, "series": ser, "params": {}}
            attach(products[key], ds)
    return products


def _nums(s):
    return [float(x) for x in re.findall(r"-?\d+(?:\.\d+)?", str(s))]


def _left(v):
    # 取 @ 之前的主数值部分（如 "170@100mA" -> "170"，"1.5V@100mA" -> "1.5V"）
    return re.split(r"[@＠]", str(v))[0]


def _range(v):
    """解析电压/温度等区间，返回 (lo, hi)。正确区分 '-' 作区间分隔符还是负号。
    例： '1.2-3.6'->(1.2,3.6)  '3.5~20'->(3.5,20)  '-40~85'->(-40,85)  '-5~-15'->(-15,-5)  '6.5'->(6.5,6.5)"""
    s = _left(v)
    t = re.sub(r"[~～至]", "|", s)            # 统一波浪号区间符
    if "|" in t:
        parts = [p for p in t.split("|") if _nums(p)]
        if len(parts) >= 2:
            a, b = _nums(parts[0])[0], _nums(parts[-1])[0]
            return (min(a, b), max(a, b))
    m = re.search(r"(\d+(?:\.\d+)?)\s*[-－—]\s*(\d+(?:\.\d+)?)", s)  # 正数之间的短横线=区间
    if m:
        a, b = float(m.group(1)), float(m.group(2))
        return (min(a, b), max(a, b))
    nums = _nums(s)
    return (min(nums), max(nums)) if nums else None


def normalize_specs(params):
    """把中文自由文本参数解析为规范数值字段，用于数值过滤检索。
    电压 V、电流 mA、静态电流 uA、压差 mV、温度 ℃。解析不出的字段省略。"""
    out = {}
    for k, v in params.items():
        if v is None or str(v).strip() in ("", "-", "/"):
            continue
        key = str(k)
        val = str(v)
        low = val.lower()
        left = _left(val)
        nums = _nums(left)

        if "封装" in key:
            continue  # 封装单独走 packages 列
        if "极限" in key or ("耐压" in key):
            if nums:
                out["vabs_max_v"] = max(nums)
            continue
        if any(t in key for t in ("工作电压", "输入电压", "最大工作电压", "工作电压范围", "输入电压范围")):
            rg = _range(val)
            if rg:
                out["vin_min_v"], out["vin_max_v"] = rg
            continue
        if "输出电压" in key:
            if "adj" in low or "可调" in val:
                out["vout_adj"] = 1
            rg = _range(val)
            if rg:
                out["vout_min_v"], out["vout_max_v"] = rg
            continue
        if any(t in key for t in ("输出电流", "最大持续电流", "平均电流", "驱动电流")):
            if nums:
                cur = max(nums)
                if ("a" in low and "ma" not in low and "ua" not in low) or cur <= 10:
                    # 以 A 为单位（或数值太小不像 mA）时换算
                    if "a" in low and "ma" not in low and "ua" not in low:
                        cur *= 1000
                out["iout_ma"] = cur
            continue
        if "峰值电流" in key:
            if nums:
                out["ipk_ma"] = max(nums)
            continue
        if "静态" in key:  # 静态电流 / 静态功耗
            if nums:
                iq = nums[0]
                if "ma" in low or "(ma" in key.lower() or "（ma" in key.lower() or "mA" in key:
                    iq *= 1000
                out["iq_ua"] = iq
            continue
        if "压差" in key:
            if nums:
                d = nums[0]
                if "v" in low or "(v" in key.lower() or "（v" in key.lower():
                    if "mv" not in low:
                        d *= 1000
                out["dropout_mv"] = d
            continue
        if "纹波抑制" in key or "psrr" in key.lower():
            if nums:
                out["psrr_db"] = nums[0]
            continue
        if "精度" in key:
            if nums:
                out["acc_pct"] = abs(nums[0])
            continue
        if "工作温度" in key or "温度范围" in key:
            rg = _range(val)
            if rg and rg[0] != rg[1]:
                out["topr_min_c"], out["topr_max_c"] = rg
            continue
    return out


def to_row(p):
    params = dict(p.get("params", {}))
    specs_norm = normalize_specs(params)  # 从干净的参数表解析数值字段
    pkg_web = next((v for k, v in params.items() if "封装" in k), "")
    pkgs = list(dict.fromkeys([x.strip() for x in re.split(r"[/、,，]", pkg_web) if x.strip()] + p.get("packages", [])))
    if p.get("abs_max"):
        params["极限参数(datasheet)"] = p["abs_max"]
    if p.get("ds_version"):
        params["datasheet版本"] = p["ds_version"]
    title = p.get("title") or params.get("功能说明") or params.get("类型") or p.get("series", "")
    desc = p.get("description") or ("；".join(f"{k}: {v}" for k, v in params.items()))
    features = "\n".join(p.get("features", []))
    apps = "\n".join(p.get("applications", []))
    blob = " ".join([p["model"], p.get("category", ""), p.get("series", ""), title, desc, features, apps,
                     " ".join(pkgs), json.dumps(params, ensure_ascii=False)])
    tags = " ".join(zh for pat, zh in ZH_TAGS if re.search(pat, blob, re.I))
    return (p["model"], p.get("category", ""), p.get("series", ""), title, desc, features, apps, "/".join(pkgs),
            json.dumps(params, ensure_ascii=False), json.dumps(specs_norm, ensure_ascii=False),
            p.get("datasheet", ""), blob + " " + tags)


def build_competitors(conn, product_models):
    if not COMP_JSON.exists():
        print("未找到竞品库", COMP_JSON)
        return
    data = json.loads(COMP_JSON.read_text("utf-8"))
    rows = []
    for c in data:
        aliases = c.get("aliases", [])
        tx = [m.strip() for m in c.get("tx_models", []) if m.strip()]
        missing = [m for m in tx if m.upper() not in product_models]
        if missing:
            print(f"  竞品 {c['model']} 的替代型号不在产品库: {missing}")
        specs = c.get("key_specs", {})
        blob = " ".join([c["model"], c.get("brand", ""), " ".join(aliases), c.get("category", ""),
                         c.get("description", ""), " ".join(tx), c.get("note", ""),
                         json.dumps(specs, ensure_ascii=False)])
        rows.append((c["model"], c.get("brand", ""), " ".join(aliases), c.get("category", ""),
                     c.get("description", ""), json.dumps(specs, ensure_ascii=False), ",".join(tx),
                     c.get("datasheet_url", ""), c.get("note", ""), blob))
    with conn:
        conn.execute("DELETE FROM competitors")
        conn.executemany("INSERT INTO competitors(model, brand, aliases, category, description, key_specs, tx_models, "
                         "datasheet_url, note, search_text) VALUES(?,?,?,?,?,?,?,?,?,?)", rows)
    print(f"竞品库入库 {len(rows)} 条交叉对照")


def pbkdf2(password, salt_hex, iters=PBKDF2_ITER):
    return hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), bytes.fromhex(salt_hex), iters).hex()


def import_users(conn, reset):
    if not USER_XLSX.exists():
        print("未找到用户表", USER_XLSX)
        return
    ws = openpyxl.load_workbook(USER_XLSX, read_only=True, data_only=True).worksheets[0]
    rows = list(ws.iter_rows(values_only=True))
    head = [str(h or "").strip() for h in rows[0]]
    col = {h: i for i, h in enumerate(head)}
    added = updated = 0
    for r in rows[1:]:
        username = str(r[col["user_name"]] or "").strip()
        password = str(r[col["pass_word"]] or "").strip()
        if not username or not password:
            continue
        name = str(r[col.get("name")] or "").strip()
        dept = str(r[col.get("depts")] or "").strip()
        role = str(r[col.get("role")] or "").strip().lower() or "user"
        exists = conn.execute("SELECT id FROM users WHERE username=?", (username,)).fetchone()
        if exists and not reset:
            continue
        salt = os.urandom(16).hex()
        h = pbkdf2(password, salt)
        if exists:
            conn.execute("UPDATE users SET name=?, dept=?, role=?, pw_salt=?, pw_hash=?, pw_iter=?, active=1 WHERE id=?",
                         (name, dept, role, salt, h, PBKDF2_ITER, exists[0]))
            updated += 1
        else:
            conn.execute("INSERT INTO users(username, name, dept, role, pw_salt, pw_hash, pw_iter) VALUES(?,?,?,?,?,?,?)",
                         (username, name, dept, role, salt, h, PBKDF2_ITER))
            added += 1
    print(f"用户导入：新增 {added}，重置 {updated}")


def build(reset_users=False):
    """解析 datasheet + 参数表，重建 products / competitors；users 仅在存在 Excel 时增量导入。
    只改 products/competitors/users，不动 conversations/messages/sessions。返回统计 dict。"""
    DB_PATH.parent.mkdir(exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    conn.execute("DROP TABLE IF EXISTS products")  # 重建以应用最新表结构(含 specs_norm)
    conn.executescript(SCHEMA)

    products = build_products()
    rows = [to_row(p) for p in products.values()]
    with conn:
        conn.execute("DELETE FROM products")
        conn.executemany("INSERT INTO products(model, category, series, title, description, features, applications, packages, "
                         "key_specs, specs_norm, datasheet, search_text) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", rows)
    with_ds = sum(1 for r in rows if r[9])
    print(f"产品入库 {len(rows)} 个型号（其中 {with_ds} 个关联 datasheet）-> {DB_PATH}", flush=True)

    build_competitors(conn, {p["model"].upper() for p in products.values()})
    with conn:
        import_users(conn, reset_users)
    conn.close()
    return {"products": len(rows), "with_datasheet": with_ds}


def main():
    build(reset_users="--reset-users" in sys.argv)


if __name__ == "__main__":
    main()
