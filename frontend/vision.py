"""图片理解：用 GLM 视觉模型(默认 glm-4.5v)识别对话中上传的图片，
提取其中的文字/参数/器件信息，转成文本供后续 glm-5.3 选型 Agent 使用。
glm-5.3 本身不支持图片，故由视觉模型先做 图片 -> 文本 转换。"""
import base64
import io
import os

MAX_CHARS = 8000
# 超过该字节数或长边过大的图片先缩放，避免视觉接口拒绝/超时
MAX_BYTES = 4 * 1024 * 1024
MAX_SIDE = 1600

PROMPT = (
    "你是硬件选型助手的图像理解模块，面向电子元器件选型场景。请完成：\n"
    "1) 逐行原样识别图片中的所有文字（型号、参数表、引脚、封装、标注等），保持数值与单位；\n"
    "2) 如果是芯片/器件的 datasheet 或规格截图，提炼关键电气参数（输入/输出电压、电流、静态电流、精度、封装、温度等）；\n"
    "3) 如果是电路图/框图，描述主要器件与关键连接；\n"
    "只输出图中真实存在的信息，不要臆造型号或参数。"
)


def _maybe_shrink(data):
    """尽量缩小过大的图片（需要 Pillow；没有则原样返回）。"""
    if len(data) <= MAX_BYTES:
        return data
    try:
        from PIL import Image
    except ImportError:
        return data
    try:
        im = Image.open(io.BytesIO(data))
        im = im.convert("RGB")
        w, h = im.size
        scale = min(1.0, MAX_SIDE / max(w, h))
        if scale < 1.0:
            im = im.resize((max(1, int(w * scale)), max(1, int(h * scale))))
        out = io.BytesIO()
        im.save(out, format="JPEG", quality=82)
        return out.getvalue()
    except Exception:
        return data


def describe_image(glm, filename, data, timeout=90):
    """调用视觉模型识别图片，返回 (text, truncated)。失败抛出 ValueError。"""
    import requests

    api_key = os.environ.get("GLM_API_KEY") or glm.get("api_key", "")
    if not api_key or api_key.startswith("请填写") or api_key == "YOUR_GLM_API_KEY_HERE":
        raise ValueError("未配置 GLM API Key")
    data = _maybe_shrink(data)
    ext = (filename.rsplit(".", 1)[-1].lower() if "." in filename else "png")
    mime = {"jpg": "jpeg", "jpeg": "jpeg", "png": "png", "webp": "webp", "bmp": "bmp", "gif": "gif"}.get(ext, "png")
    url = f"data:image/{mime};base64," + base64.b64encode(data).decode()

    base = glm.get("base_url", "https://open.bigmodel.cn").rstrip("/")
    path = glm.get("chat_path", "/api/coding/paas/v4/chat/completions")
    model = glm.get("vision_model", "glm-4v-flash")
    body = {"model": model, "stream": False, "temperature": 0.2, "max_tokens": 1024,
            "messages": [{"role": "user", "content": [
                {"type": "text", "text": PROMPT},
                {"type": "image_url", "image_url": {"url": url}}]}]}
    try:
        r = requests.post(base + path, headers={"Authorization": "Bearer " + api_key,
                                                "Content-Type": "application/json"},
                          json=body, timeout=timeout)
    except requests.RequestException as e:
        raise ValueError(f"调用视觉模型失败：{e.__class__.__name__}") from e
    if r.status_code != 200:
        try:
            msg = r.json().get("error", {}).get("message", r.text[:150])
        except ValueError:
            msg = r.text[:150]
        raise ValueError(f"视觉模型返回 {r.status_code}：{msg}")
    try:
        content = r.json()["choices"][0]["message"]["content"] or ""
    except (ValueError, KeyError, IndexError):
        raise ValueError("视觉模型返回格式异常")
    content = content.strip()
    if len(content) > MAX_CHARS:
        return content[:MAX_CHARS], True
    return content, False
