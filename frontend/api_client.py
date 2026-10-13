"""C++ 后端 REST API 客户端（前端服务器 -> 后端）。"""
import requests


class BackendError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status
        self.message = message


class BackendClient:
    def __init__(self, base_url, token=None, timeout=30):
        self.base = base_url.rstrip("/")
        self.token = token
        self.timeout = timeout

    def _headers(self):
        return {"Authorization": f"Bearer {self.token}"} if self.token else {}

    def request(self, method, path, **kw):
        try:
            r = requests.request(method, self.base + path, headers=self._headers(), timeout=kw.pop("timeout", self.timeout), **kw)
        except requests.RequestException as e:
            raise BackendError(502, f"无法连接后端服务：{e.__class__.__name__}") from e
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except ValueError:
                msg = r.text
            raise BackendError(r.status_code, msg)
        return r

    def get(self, path, **kw):
        return self.request("GET", path, **kw).json()

    def post(self, path, data=None, **kw):
        return self.request("POST", path, json=data or {}, **kw).json()

    def patch(self, path, data=None, **kw):
        return self.request("PATCH", path, json=data or {}, **kw).json()

    def delete(self, path, **kw):
        return self.request("DELETE", path, **kw).json()

    def stream(self, path, data):
        """返回 requests 的流式响应（SSE 透传）。"""
        try:
            r = requests.post(self.base + path, json=data, headers=self._headers(), stream=True, timeout=(10, 600))
        except requests.RequestException as e:
            raise BackendError(502, f"无法连接后端服务：{e.__class__.__name__}") from e
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except ValueError:
                msg = r.text
            r.close()
            raise BackendError(r.status_code, msg)
        return r
