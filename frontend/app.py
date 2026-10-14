"""太矽电子 AI 产品推荐系统 —— Python 前端（Flask）

浏览器 <-> 本服务（页面渲染 + /api 代理，会话保存在签名 Cookie 中）<-> C++ 后端 REST API
"""
import json
import re
import secrets
import subprocess
import sys
import threading
from datetime import datetime
from functools import wraps
from pathlib import Path

from flask import (Flask, Response, abort, flash, jsonify, redirect, render_template, request, session,
                   stream_with_context, url_for)

from api_client import BackendClient, BackendError
from uploads import ALLOWED_EXTS, MAX_FILES, build_message, extract_text

ROOT = Path(__file__).resolve().parent.parent


def load_config():
    text = (ROOT / "config.json").read_text("utf-8")
    text = re.sub(r'^\s*//.*$', "", text, flags=re.M)  # 去掉整行注释
    return json.loads(text)


CFG = load_config()
FE = CFG.get("frontend", {})
BACKEND_URL = FE.get("backend_url", "http://127.0.0.1:8080")

app = Flask(__name__)
app.secret_key = FE.get("secret_key", "change-me")
app.config.update(SESSION_COOKIE_HTTPONLY=True, SESSION_COOKIE_SAMESITE="Lax", JSON_AS_ASCII=False,
                   MAX_CONTENT_LENGTH=30 * 1024 * 1024)  # 附件上传上限 30MB

ROLE_NAMES = {"admin": "管理员", "manager": "主管", "user": "普通用户"}


def backend():
    return BackendClient(BACKEND_URL, session.get("token"))


def current_user():
    return session.get("user")


def csrf_token():
    if "csrf" not in session:
        session["csrf"] = secrets.token_hex(16)
    return session["csrf"]


@app.before_request
def csrf_protect():
    if request.method in ("POST", "PATCH", "DELETE", "PUT"):
        if request.path.startswith("/api/"):
            # JSON 接口：跨站页面无法在不触发预检的情况下发送此请求头
            if request.headers.get("X-CSRF-Token") != session.get("csrf"):
                return jsonify(error="CSRF 校验失败，请刷新页面"), 400
        elif request.endpoint not in ("login", "logout") and request.form.get("csrf") != session.get("csrf"):
            abort(400)


@app.context_processor
def inject():
    return {"me": current_user(), "role_names": ROLE_NAMES, "csrf_token": csrf_token}


def login_required(fn):
    @wraps(fn)
    def wrapper(*a, **kw):
        if not session.get("token"):
            if request.path.startswith("/api/"):
                return jsonify(error="未登录"), 401
            return redirect(url_for("login", next=request.path))
        try:
            return fn(*a, **kw)
        except BackendError as e:
            if e.status == 401:
                session.clear()
                if request.path.startswith("/api/"):
                    return jsonify(error="登录已过期"), 401
                flash("登录已过期，请重新登录", "error")
                return redirect(url_for("login"))
            if request.path.startswith("/api/"):
                return jsonify(error=e.message), e.status
            flash(e.message, "error")
            return redirect(request.referrer or url_for("chat"))
    return wrapper


def role_required(*roles):
    def deco(fn):
        @wraps(fn)
        @login_required
        def wrapper(*a, **kw):
            if (current_user() or {}).get("role") not in roles:
                abort(403)
            return fn(*a, **kw)
        return wrapper
    return deco


# ------------------------------------------------------------------ 账户
@app.route("/login", methods=["GET", "POST"])
def login():
    if request.method == "POST":
        username = request.form.get("username", "").strip()
        password = request.form.get("password", "")
        try:
            r = BackendClient(BACKEND_URL).post("/api/auth/login", {"username": username, "password": password})
        except BackendError as e:
            flash(e.message, "error")
            return render_template("login.html", username=username), 401 if e.status == 401 else 200
        session.clear()
        session["token"] = r["token"]
        session["user"] = r["user"]
        nxt = request.args.get("next", "")
        return redirect(nxt if nxt.startswith("/") and not nxt.startswith("//") else url_for("chat"))
    if session.get("token"):
        return redirect(url_for("chat"))
    return render_template("login.html")


@app.route("/logout", methods=["POST", "GET"])
def logout():
    if session.get("token"):
        try:
            backend().post("/api/auth/logout")
        except BackendError:
            pass
    session.clear()
    return redirect(url_for("login"))


@app.route("/profile", methods=["GET", "POST"])
@login_required
def profile():
    if request.method == "POST":
        old, new, confirm = (request.form.get(k, "") for k in ("old_password", "new_password", "confirm_password"))
        if new != confirm:
            flash("两次输入的新密码不一致", "error")
        else:
            try:
                backend().post("/api/auth/password", {"old_password": old, "new_password": new})
                flash("密码修改成功", "success")
            except BackendError as e:
                if e.status == 401:
                    raise
                flash(e.message, "error")
        return redirect(url_for("profile"))
    return render_template("profile.html")


# ------------------------------------------------------------------ 对话
@app.route("/")
def index():
    return redirect(url_for("chat"))


@app.route("/chat")
@app.route("/chat/<int:cid>")
@login_required
def chat(cid=None):
    return render_template("chat.html", cid=cid)


@app.get("/api/conversations")
@login_required
def api_conversations():
    return jsonify(backend().get("/api/conversations"))


@app.route("/api/conversations/<int:cid>", methods=["GET", "PATCH", "DELETE"])
@login_required
def api_conversation(cid):
    b = backend()
    if request.method == "GET":
        return jsonify(b.get(f"/api/conversations/{cid}"))
    if request.method == "PATCH":
        return jsonify(b.patch(f"/api/conversations/{cid}", request.get_json(silent=True) or {}))
    return jsonify(b.delete(f"/api/conversations/{cid}"))


@app.post("/api/chat")
@login_required
def api_chat():
    # 带附件时为 multipart；否则为 JSON
    if request.content_type and request.content_type.startswith("multipart/form-data"):
        message = request.form.get("message", "")
        cid = int(request.form.get("conversation_id") or 0)
        attachments = []
        for f in request.files.getlist("files")[:MAX_FILES]:
            if not f or not f.filename:
                continue
            ext = ("." + f.filename.rsplit(".", 1)[-1].lower()) if "." in f.filename else ""
            data = f.read()
            if not data:
                continue
            if ext not in ALLOWED_EXTS:
                attachments.append((f.filename, f"(不支持的文件类型 {ext}，未解析)", False))
                continue
            try:
                text, truncated = extract_text(f.filename, data)
            except ValueError as e:
                text, truncated = f"(无法解析该文件：{e})", False
            except Exception as e:  # noqa: BLE001
                text, truncated = f"(解析出错：{e.__class__.__name__})", False
            attachments.append((f.filename, text, truncated))
        full_message = build_message(message, attachments)
        if not full_message.strip():
            return jsonify(error="请输入内容或上传可解析的附件"), 400
    else:
        payload = request.get_json(silent=True) or {}
        message = payload.get("message", "")
        cid = int(payload.get("conversation_id") or 0)
        full_message = message
    upstream = backend().stream("/api/chat/stream", {"message": full_message, "conversation_id": cid})

    def generate():
        try:
            for chunk in upstream.iter_content(chunk_size=None):
                if chunk:
                    yield chunk
        finally:
            upstream.close()

    return Response(stream_with_context(generate()), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


# ------------------------------------------------------------------ 产品库
@app.get("/products")
@login_required
def products():
    q, category = request.args.get("q", ""), request.args.get("category", "")
    b = backend()
    cats = b.get("/api/categories")["categories"]
    items = b.get("/api/products", params={"q": q, "category": category, "limit": 50 if (q or category) else 200})["products"]
    return render_template("products.html", cats=cats, items=items, q=q, category=category)


@app.get("/products/<path:model>")
@login_required
def product_detail(model):
    p = backend().get(f"/api/products/{model}")
    return render_template("product_detail.html", p=p)


@app.get("/competitors")
@login_required
def competitors():
    q = request.args.get("q", "")
    items = backend().get("/api/competitors", params={"q": q, "limit": 200})["competitors"]
    return render_template("competitors.html", items=items, q=q)


@app.get("/datasheets/<path:name>")
@login_required
def datasheet(name):
    r = backend().request("GET", f"/api/datasheets/{name}", timeout=60)
    return Response(r.content, mimetype="application/pdf",
                    headers={"Content-Disposition": f"inline; filename*=UTF-8''{name}"})


# ------------------------------------------------------------------ 管理
@app.route("/admin/users", methods=["GET", "POST"])
@role_required("admin")
def admin_users():
    b = backend()
    if request.method == "POST":
        action = request.form.get("action")
        uid = request.form.get("id")
        try:
            if action == "create":
                b.post("/api/admin/users", {k: request.form.get(k, "").strip() for k in ("username", "password", "name", "dept", "role")})
                flash("用户已创建", "success")
            elif action == "update":
                data = {k: request.form.get(k, "").strip() for k in ("name", "dept", "role")}
                if request.form.get("password"):
                    data["password"] = request.form["password"]
                b.patch(f"/api/admin/users/{uid}", data)
                flash("用户已更新", "success")
            elif action in ("enable", "disable"):
                b.patch(f"/api/admin/users/{uid}", {"active": action == "enable"})
                flash("已" + ("启用" if action == "enable" else "停用"), "success")
            elif action == "delete":
                b.delete(f"/api/admin/users/{uid}")
                flash("用户已删除", "success")
        except BackendError as e:
            if e.status == 401:
                raise
            flash(e.message, "error")
        return redirect(url_for("admin_users"))
    users = b.get("/api/admin/users")["users"]
    return render_template("admin_users.html", users=users)


# ---------------- 官网 datasheet 同步 ----------------
SYNC_STATUS_FILE = ROOT / "data" / "sync_status.json"
_sync = {"running": False, "started": "", "finished": "", "returncode": None, "log": "", "mode": ""}
_sync_lock = threading.Lock()


def _run_sync(mode):
    args = [sys.executable, str(ROOT / "tools" / "sync_datasheets.py")]
    if mode == "fast":
        args.append("--fast")
    _sync.update(running=True, started=datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                 finished="", returncode=None, log="", mode=mode)
    try:
        p = subprocess.Popen(args, cwd=str(ROOT), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, encoding="utf-8", errors="replace", bufsize=1)
        for line in p.stdout:
            _sync["log"] += line
            if len(_sync["log"]) > 20000:
                _sync["log"] = _sync["log"][-20000:]
        p.wait()
        _sync["returncode"] = p.returncode
    except Exception as e:  # noqa: BLE001
        _sync["log"] += f"\n启动同步失败：{e}"
        _sync["returncode"] = -1
    finally:
        _sync["running"] = False
        _sync["finished"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def _last_sync():
    if SYNC_STATUS_FILE.exists():
        try:
            return json.loads(SYNC_STATUS_FILE.read_text("utf-8"))
        except (ValueError, OSError):
            pass
    return {}


@app.route("/admin/sync", methods=["GET", "POST"])
@role_required("admin")
def admin_sync():
    if request.method == "POST":
        mode = "fast" if request.form.get("mode") == "fast" else "full"
        with _sync_lock:
            if _sync["running"]:
                flash("同步正在进行中，请等待完成", "error")
            else:
                threading.Thread(target=_run_sync, args=(mode,), daemon=True).start()
                flash("已开始与官网同步，可在本页查看实时进度", "success")
        return redirect(url_for("admin_sync"))
    return render_template("admin_sync.html", status=_last_sync(), sync=_sync)


@app.get("/api/admin/sync-status")
@role_required("admin")
def api_sync_status():
    return jsonify(running=_sync["running"], started=_sync["started"], finished=_sync["finished"],
                   returncode=_sync["returncode"], mode=_sync["mode"], log=_sync["log"], last=_last_sync())


@app.get("/admin/logs")
@role_required("admin", "manager")
def admin_logs():
    q, user = request.args.get("q", ""), request.args.get("user", "")
    data = backend().get("/api/admin/logs", params={"q": q, "user": user})
    return render_template("admin_logs.html", convs=data["conversations"], stats=data["stats"], q=q, user=user)


@app.get("/admin/logs/<int:cid>")
@role_required("admin", "manager")
def admin_log_detail(cid):
    c = backend().get(f"/api/admin/conversations/{cid}")
    for m in c["messages"]:
        try:
            m["meta"] = json.loads(m.get("meta") or "{}")
        except ValueError:
            m["meta"] = {}
    return render_template("admin_log_detail.html", c=c)


@app.errorhandler(413)
def too_large(_):
    if request.path.startswith("/api/"):
        return jsonify(error="上传内容过大（上限 30MB）"), 413
    return render_template("error.html", code=413, msg="上传内容过大"), 413


@app.errorhandler(403)
def forbidden(_):
    return render_template("error.html", code=403, msg="没有权限访问该页面"), 403


@app.errorhandler(404)
def not_found(_):
    return render_template("error.html", code=404, msg="页面不存在"), 404


if __name__ == "__main__":
    app.run(host=FE.get("host", "0.0.0.0"), port=int(FE.get("port", 5000)), threaded=True, debug=False)
