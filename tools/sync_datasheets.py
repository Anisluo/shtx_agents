"""与 txsemi.com 官网同步 datasheet 与产品说明。

流程：抓取下载中心 PDF（按内容比对，发现新增/更新）→ 抓取官网参数表 →
重建 products / competitors 表 → 生成变更报告 data/sync_status.json。

安全：只重建 products / competitors（以及存在 Excel 时的 users 增量），
不触碰 conversations / messages / sessions，正在运行的服务无需重启即可读到新数据。

用法：
  python tools/sync_datasheets.py           # 完整同步（逐个比对 PDF 内容，最准确）
  python tools/sync_datasheets.py --fast     # 仅下载缺失的 PDF（快，适合首次或网络慢时）
"""
import json
import os
import sqlite3
import sys
import time
from datetime import datetime
from pathlib import Path

import build_db
import crawl_datasheets

ROOT = Path(__file__).resolve().parent.parent
DB_PATH = ROOT / "data" / "shtx.db"
STATUS_PATH = ROOT / "data" / "sync_status.json"
LOCK_PATH = ROOT / "data" / "sync.lock"
LOCK_STALE_SEC = 1800  # 超过 30 分钟视为残留锁


def _acquire_lock():
    LOCK_PATH.parent.mkdir(exist_ok=True)
    try:
        fd = os.open(str(LOCK_PATH), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        os.write(fd, f"{os.getpid()} {datetime.now():%Y-%m-%d %H:%M:%S}".encode())
        os.close(fd)
        return True
    except FileExistsError:
        try:
            if time.time() - LOCK_PATH.stat().st_mtime > LOCK_STALE_SEC:
                LOCK_PATH.unlink()
                return _acquire_lock()
        except OSError:
            pass
        return False


def _release_lock():
    try:
        LOCK_PATH.unlink()
    except OSError:
        pass


def _snapshot():
    """读取当前 products：model -> datasheet 文件名。库不存在时返回空。"""
    if not DB_PATH.exists():
        return {}
    try:
        conn = sqlite3.connect(DB_PATH)
        rows = conn.execute("SELECT model, datasheet FROM products").fetchall()
        conn.close()
        return {m: (d or "") for m, d in rows}
    except sqlite3.Error:
        return {}


def _write_status(status):
    STATUS_PATH.parent.mkdir(exist_ok=True)
    STATUS_PATH.write_text(json.dumps(status, ensure_ascii=False, indent=1), "utf-8")


def run_sync(force_recheck=True):
    if not _acquire_lock():
        print("已有同步任务在进行中，本次跳过。", flush=True)
        return {"ok": False, "skipped": True, "error": "另一个同步任务正在进行中"}
    started = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    t0 = time.time()
    status = {"started_at": started, "ok": False}
    try:
        before = _snapshot()
        print(f"[{started}] 开始与官网同步…", flush=True)

        crawl_report = crawl_datasheets.crawl(force_recheck=force_recheck)
        build_stats = build_db.build()

        after = _snapshot()
        new_models = sorted(set(after) - set(before))
        removed_models = sorted(set(before) - set(after))
        changed_ds = sorted(m for m in after if m in before and after[m] != before[m])

        status.update({
            "ok": True,
            "finished_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "elapsed_sec": round(time.time() - t0, 1),
            "datasheets": crawl_report.get("pdf", {}),
            "products_total": build_stats["products"],
            "products_with_datasheet": build_stats["with_datasheet"],
            "new_models": new_models,
            "removed_models": removed_models,
            "datasheet_version_changed": changed_ds,
        })
        dss = status["datasheets"]
        print(f"同步完成：datasheet 新增 {len(dss.get('added', []))}、更新 {len(dss.get('updated', []))}；"
              f"型号新增 {len(new_models)}、下架 {len(removed_models)}、换版 {len(changed_ds)}；"
              f"共 {build_stats['products']} 个型号。用时 {status['elapsed_sec']}s", flush=True)
    except Exception as e:  # noqa: BLE001
        import traceback
        status.update({"ok": False, "finished_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                       "elapsed_sec": round(time.time() - t0, 1), "error": f"{type(e).__name__}: {e}"})
        print("同步失败：" + status["error"], flush=True)
        traceback.print_exc()
    finally:
        _release_lock()
    _write_status(status)
    return status


if __name__ == "__main__":
    ok = run_sync(force_recheck="--fast" not in sys.argv)["ok"]
    sys.exit(0 if ok else 1)
