// 对话页：历史加载、SSE 流式渲染、工具调用步骤、产品卡片
const thread = document.getElementById('thread');
const scroller = document.getElementById('chatScroll');
const input = document.getElementById('input');
const sendBtn = document.getElementById('sendBtn');
const form = document.getElementById('composer');
const welcome = document.getElementById('welcome');
const suggests = document.getElementById('suggests');
const titleEl = document.getElementById('convTitle');
let cid = window.CURRENT_CID;
let busy = false;

marked.setOptions({ gfm: true, breaks: true });

function renderMd(el, text) {
  el.innerHTML = DOMPurify.sanitize(marked.parse(text || ''));
  el.querySelectorAll('a[href]').forEach(a => { a.target = '_blank'; a.rel = 'noopener'; });
}

function stepLabel(tool, args = {}) {
  if (tool === 'list_categories') return '浏览太矽产品线';
  if (tool === 'search_products') {
    const parts = [args.keywords, args.category, args.series].filter(Boolean).join(' · ');
    return '检索产品库' + (parts ? `：${parts}` : '');
  }
  if (tool === 'get_product_details') return '查看型号资料：' + (args.models || []).join('、');
  return tool;
}

function nearBottom() { return scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < 160; }
function scrollDown(force) { if (force || nearBottom()) scroller.scrollTop = scroller.scrollHeight; }

function setWelcome(on) {
  welcome.hidden = !on;
  suggests.hidden = !on;
  document.body.classList.toggle('welcome-mode', on);
  if (on) {
    const h = new Date().getHours();
    document.getElementById('greeting').textContent = h < 6 ? '夜深了' : h < 12 ? '上午好' : h < 18 ? '下午好' : '晚上好';
  }
}

const ATTACH_HEADER = '〔以下为用户上传的附件，供分析参考〕';

// 把发往后端的消息还原为 {text, files} 用于气泡展示（隐藏附件正文，只显示文件名）
function parseUserContent(content) {
  const i = content.indexOf(ATTACH_HEADER);
  if (i < 0) return { text: content, files: [] };
  const head = content.slice(0, i).replace(/\n+$/, '');
  const tail = content.slice(i);
  const files = [...tail.matchAll(/【文件：([^】]+)】/g)].map(m => m[1]);
  return { text: head, files };
}

function fileChipsHtml(files) {
  if (!files || !files.length) return '';
  return '<div class="bubble-files">' + files.map(n => `<span class="file-chip">📎 ${escapeHtml(n)}</span>`).join('') + '</div>';
}

function addUser(text, files) {
  const div = document.createElement('div');
  div.className = 'msg user';
  const body = (text ? `<div>${escapeHtml(text)}</div>` : '');
  div.innerHTML = `<div class="bubble">${fileChipsHtml(files)}${body}</div>`;
  thread.appendChild(div);
}

function addAssistant() {
  const div = document.createElement('div');
  div.className = 'msg assistant';
  div.innerHTML = `<div class="ai-mark">TX</div><div class="ai-body">
      <div class="run-status"></div><div class="steps"></div><div class="md"></div><div class="cards-box"></div><div class="msg-foot"></div></div>`;
  thread.appendChild(div);
  return {
    root: div,
    status: div.querySelector('.run-status'),
    steps: div.querySelector('.steps'),
    md: div.querySelector('.md'),
    cards: div.querySelector('.cards-box'),
    foot: div.querySelector('.msg-foot'),
  };
}

function renderSteps(box, steps) {
  box.innerHTML = steps.map(s =>
    `<div class="step ${s.status || 'done'}"><span class="dot"></span><span>${escapeHtml(stepLabel(s.tool, s.args))}${s.summary ? ' — ' + escapeHtml(s.summary) : ''}</span></div>`
  ).join('');
}

function renderCards(box, products) {
  if (!products || !products.length) { box.innerHTML = ''; return; }
  box.innerHTML = '<div class="cards">' + products.map(p => {
    const specs = Object.entries(p.key_specs || {}).filter(([k]) => !k.includes('极限参数') && !k.includes('版本')).slice(0, 3)
      .map(([k, v]) => `${escapeHtml(k.replace(/\s+/g, ''))}: ${escapeHtml(v)}`).join('<br>');
    return `<div class="card">
      <a class="card-model" href="/products/${encodeURIComponent(p.model)}" target="_blank">${escapeHtml(p.model)}</a>
      <div class="card-sub">${escapeHtml(p.title || p.series)}</div>
      <div class="card-sub">${specs}</div>
      <div class="card-links">
        <a href="/products/${encodeURIComponent(p.model)}" target="_blank">详情</a>
        ${p.datasheet_url ? `<a href="${escapeHtml(p.datasheet_url)}" target="_blank">数据手册 PDF</a>` : ''}
      </div></div>`;
  }).join('') + '</div>';
}

function renderFoot(view, text, elapsed) {
  view.foot.innerHTML = (elapsed ? `<span>用时 ${elapsed.toFixed(1)} 秒</span>` : '') + '<button type="button">复制</button>';
  view.foot.querySelector('button').onclick = async (e) => {
    await navigator.clipboard.writeText(text);
    e.target.textContent = '已复制';
    setTimeout(() => (e.target.textContent = '复制'), 1500);
  };
}

async function loadConversation(id) {
  try {
    const c = await api(`/api/conversations/${id}`);
    titleEl.textContent = c.title;
    document.title = c.title + ' · 太矽产品选型助手';
    for (const m of c.messages) {
      if (m.role === 'user') { const p = parseUserContent(m.content); addUser(p.text, p.files); continue; }
      const v = addAssistant();
      let meta = {};
      try { meta = JSON.parse(m.meta || '{}'); } catch (_) {}
      renderSteps(v.steps, meta.steps || []);
      renderMd(v.md, m.content);
      renderCards(v.cards, meta.products);
      renderFoot(v, m.content, meta.elapsed);
    }
    setWelcome(false);
    scrollDown(true);
  } catch (e) {
    thread.innerHTML = `<p class="muted">${escapeHtml(e.message)}</p>`;
  }
}

async function send(text, files) {
  files = files || [];
  if (busy || (!text.trim() && !files.length)) return;
  busy = true;
  sendBtn.disabled = true;
  setWelcome(false);
  addUser(text, files.map(f => f.name));
  const v = addAssistant();
  scrollDown(true);

  let content = '', steps = [], frame = 0;
  // 运行状态 + 实时计时，避免看起来像卡死
  const t0 = performance.now();
  let phase = '正在分析需求';
  const renderStatus = () => {
    const secs = ((performance.now() - t0) / 1000).toFixed(1);
    v.status.innerHTML = `<span class="spark">✻</span> ${phase}… <span class="elapsed">已用时 ${secs} 秒</span>`;
  };
  renderStatus();
  const timer = setInterval(renderStatus, 100);
  const paint = () => {
    frame = 0;
    renderMd(v.md, content);
    v.md.classList.add('cursor');
    scrollDown();
  };

  if (files.length) phase = '正在读取附件';
  try {
    let opts;
    if (files.length) {
      const fd = new FormData();
      fd.append('message', text);
      fd.append('conversation_id', cid || 0);
      for (const f of files) fd.append('files', f, f.name);
      opts = { method: 'POST', headers: { 'X-CSRF-Token': CSRF }, body: fd };
    } else {
      opts = { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': CSRF },
               body: JSON.stringify({ message: text, conversation_id: cid || 0 }) };
    }
    const res = await fetch('/api/chat', opts);
    if (res.status === 401) { location.href = '/login'; return; }
    if (!res.ok) {
      const err = await res.json().catch(() => ({}));
      throw new Error(err.error || ('请求失败 ' + res.status));
    }
    const reader = res.body.getReader();
    const decoder = new TextDecoder();
    let buf = '';
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += decoder.decode(value, { stream: true });
      let idx;
      while ((idx = buf.indexOf('\n\n')) >= 0) {
        const raw = buf.slice(0, idx);
        buf = buf.slice(idx + 2);
        const line = raw.split('\n').find(l => l.startsWith('data:'));
        if (!line) continue;
        const ev = JSON.parse(line.slice(5).trim());
        if (ev.type === 'meta') {
          if (!cid) {
            cid = ev.conversation_id;
            window.CURRENT_CID = cid;
            history.replaceState(null, '', `/chat/${cid}`);
            titleEl.textContent = text.slice(0, 30);
            loadConversations();
          }
        } else if (ev.type === 'step') {
          const i = steps.findIndex(s => s.tool === ev.tool && JSON.stringify(s.args) === JSON.stringify(ev.args) && s.status === 'running');
          if (i >= 0) steps[i] = ev; else steps.push(ev);
          renderSteps(v.steps, steps);
          if (!content) { phase = ev.status === 'running' ? '正在查询产品库' : '正在整理结果'; renderStatus(); }
          scrollDown();
        } else if (ev.type === 'delta') {
          if (!content) { phase = '正在生成回复'; renderStatus(); }
          content += ev.text;
          if (!frame) frame = requestAnimationFrame(paint);
        } else if (ev.type === 'error') {
          content += (content ? '\n\n' : '') + '> ⚠️ ' + ev.message;
        } else if (ev.type === 'done') {
          if (frame) cancelAnimationFrame(frame);
          renderMd(v.md, content);
          renderCards(v.cards, ev.products);
          renderFoot(v, content, ev.elapsed);
        }
      }
    }
  } catch (e) {
    content += (content ? '\n\n' : '') + '> ⚠️ ' + e.message;
    renderMd(v.md, content);
  } finally {
    clearInterval(timer);
    v.status.remove();
    v.md.classList.remove('cursor');
    busy = false;
    sendBtn.disabled = !input.value.trim() && !pending.length;
    loadConversations();
    scrollDown();
  }
}

// ---------- 附件选择 ----------
const attachBtn = document.getElementById('attachBtn');
const fileInput = document.getElementById('fileInput');
const attachList = document.getElementById('attachList');
const MAX_FILES = 5, MAX_SIZE = 25 * 1024 * 1024;
let pending = [];  // 待发送文件

function humanSize(n) {
  return n < 1024 ? n + ' B' : n < 1048576 ? (n / 1024).toFixed(0) + ' KB' : (n / 1048576).toFixed(1) + ' MB';
}
function renderAttachments() {
  attachList.hidden = !pending.length;
  attachList.innerHTML = pending.map((f, i) =>
    `<span class="attach-chip">📎 ${escapeHtml(f.name)} <span class="muted">${humanSize(f.size)}</span>` +
    `<button type="button" data-i="${i}" aria-label="移除">✕</button></span>`).join('');
  autosize();
}
function addFiles(list) {
  for (const f of list) {
    if (pending.length >= MAX_FILES) { alert(`最多上传 ${MAX_FILES} 个文件`); break; }
    if (f.size > MAX_SIZE) { alert(`「${f.name}」超过 25MB，未添加`); continue; }
    if (!pending.some(p => p.name === f.name && p.size === f.size)) pending.push(f);
  }
  renderAttachments();
}
attachBtn.addEventListener('click', () => fileInput.click());
fileInput.addEventListener('change', () => { addFiles(fileInput.files); fileInput.value = ''; });
attachList.addEventListener('click', (e) => {
  const b = e.target.closest('button[data-i]');
  if (b) { pending.splice(Number(b.dataset.i), 1); renderAttachments(); }
});
// 拖拽 / 粘贴上传
const comp = document.querySelector('.composer');
['dragover', 'dragenter'].forEach(ev => comp.addEventListener(ev, e => { e.preventDefault(); comp.classList.add('drag'); }));
['dragleave', 'drop'].forEach(ev => comp.addEventListener(ev, e => { e.preventDefault(); comp.classList.remove('drag'); }));
comp.addEventListener('drop', e => { if (e.dataTransfer?.files?.length) addFiles(e.dataTransfer.files); });
input.addEventListener('paste', e => { const fs = [...(e.clipboardData?.files || [])]; if (fs.length) { e.preventDefault(); addFiles(fs); } });

function autosize() {
  input.style.height = 'auto';
  input.style.height = Math.min(input.scrollHeight, 220) + 'px';
  sendBtn.disabled = busy || (!input.value.trim() && !pending.length);
}

input.addEventListener('input', autosize);
input.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) { e.preventDefault(); form.requestSubmit(); }
});
form.addEventListener('submit', (e) => {
  e.preventDefault();
  const text = input.value;
  if (busy || (!text.trim() && !pending.length)) return;
  const files = pending;
  pending = [];
  renderAttachments();
  input.value = '';
  autosize();
  send(text, files);
});
suggests.addEventListener('click', (e) => {
  const b = e.target.closest('button');
  if (b) send(b.textContent, []);
});

// 从产品详情页「咨询」带入的预填问题：填进输入框并聚焦，用户可直接发送或修改
const askParam = new URLSearchParams(location.search).get('ask');
if (cid) {
  loadConversation(cid);
} else {
  setWelcome(true);
  if (askParam) {
    input.value = askParam;
    autosize();
    history.replaceState(null, '', '/chat');
    input.focus();
    input.setSelectionRange(input.value.length, input.value.length);
  }
}
