# 太矽产品选型助手（上海太矽电子 产品推荐系统）

B/S 架构、前后端分离：**前端 Python（Flask）**，**后端 C++（REST API + SSE 流式）**，知识库为 **SQLite**，大模型为 **智谱 GLM**（默认 `glm-5.3`，支持工具调用）。

## 架构

```
浏览器 ──HTTP──▶ Python 前端 :5000 ──REST/SSE──▶ C++ 后端 :8080 ──HTTPS──▶ 智谱 GLM API
 (页面/JS)       frontend/app.py                 backend/shtx_backend.exe
                 · 页面渲染 (Jinja2)              · 账户认证 (PBKDF2 + Token 会话)
                 · 会话 Cookie / CSRF            · 用户管理 / 对话日志 API
                 · /api 代理、SSE 透传            · 产品检索 API
                                                 · 推荐 Agent（GLM 工具调用循环）
                                                         │
                                                         ▼
                                                 SQLite  data/shtx.db
                                                 users / sessions / products / conversations / messages
```

- C++ 后端默认只监听 `127.0.0.1`，浏览器只访问 Python 前端；前端用登录后获得的 Token 调用后端。
- Agent 工具：`list_categories`（产品线）、`search_products`（关键词/大类/系列检索）、`find_alternative`（竞品/标准料号的国产替代交叉对照）、`get_product_details`（完整参数、特性、应用、数据手册）。
- 回复按 token 流式输出（SSE），界面实时显示工具调用步骤，最后附推荐型号卡片与 datasheet 链接。

## 目录

| 路径 | 说明 |
|---|---|
| `config.json` | **唯一配置文件**：在 `glm.api_key` 处直接填入 API Key |
| `datasheets/` | 从 txsemi.com 下载的 70 份产品数据手册 PDF |
| `data/web_products.json` | 官网产品中心的参数表（122 个型号） |
| `data/competitors.json` | 竞品 / 国产替代交叉对照（可编辑扩充，改后重跑 build_db.py 生效） |
| `data/shtx.db` | SQLite 知识库 + 用户 + 对话记录 |
| `tools/crawl_datasheets.py` | 爬虫：下载 datasheet、抓取官网参数表 |
| `tools/build_db.py` | 提炼 PDF 要点并合并参数表入库；从 Excel 导入用户 |
| `backend/` | C++ 后端源码（cpp-httplib、nlohmann/json、SQLite3、OpenSSL） |
| `frontend/` | Python 前端（Flask 模板 + 原生 JS） |
| `scripts/` | 编译 / 启动脚本 |
| `logs/` | `backend.log` 运行日志、`access.log` 访问日志、`chat.log` 对话审计（JSON Lines） |

## 快速开始

1. 编辑 `config.json`，填写 `glm.api_key`（如需更换模型，修改 `glm.model`）。
2. 一键启动（后端、前端各开一个窗口）：
   ```bat
   scripts\start_all.bat
   ```
3. 浏览器打开 `http://127.0.0.1:5000`，用 `data_info_with_password.xlsx` 中的 `user_name` / `pass_word` 登录。

## 账户与权限

- 用户来自 `data_info_with_password.xlsx`（52 人），导入时口令以 **PBKDF2-SHA256 + 随机盐** 保存，数据库中不存明文。
- 角色：`admin` 管理员（用户增删改、停用、重置密码、查看全部对话日志）；`manager` 主管（查看对话日志）；其他为普通用户（对话、产品库、改密码）。
- 重新导入用户：`python tools/build_db.py`（只新增不存在的用户）；`--reset-users` 按 Excel 重置所有用户密码。

## 重新生成知识库

```bat
python tools\crawl_datasheets.py   :: 增量下载新的 datasheet 并更新官网参数表
python tools\build_db.py           :: 重建 products 表（不影响用户和对话记录）
```

## 编译后端

依赖 MSYS2 MinGW64（`pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-openssl mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-make`）和 CMake：

```bat
scripts\build_backend.bat
```

## 后端 API 摘要

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/auth/login` · `/api/auth/logout` · `/api/auth/password` | 登录 / 退出 / 改密码 |
| GET | `/api/auth/me` | 当前用户 |
| GET/PATCH/DELETE | `/api/conversations[/{id}]` | 我的对话列表、详情、重命名、删除 |
| POST | `/api/chat/stream` | 流式对话（SSE 事件：meta / step / delta / error / done） |
| GET | `/api/categories` · `/api/products?q=&category=&series=` · `/api/products/{model}` | 产品库（详情含 `replaces` 可替代竞品） |
| GET | `/api/competitors?q=` | 竞品库 / 国产替代交叉对照 |
| GET | `/api/datasheets/{file}.pdf` | 数据手册（需登录） |
| GET/POST/PATCH/DELETE | `/api/admin/users[/{id}]` | 用户管理（admin） |
| GET | `/api/admin/logs` · `/api/admin/conversations/{id}` | 对话日志（admin / manager） |
| GET | `/api/health` | 健康检查（含 API Key 是否已配置） |
