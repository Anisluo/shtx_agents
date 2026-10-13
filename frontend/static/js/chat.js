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

function addUser(text) {
  const div = document.createElement('div');
  div.className = 'msg user';
  div.innerHTML = `<div class="bubble">${escapeHtml(text)}</div>`;
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
      if (m.role === 'user') { addUser(m.content); continue; }
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

async function send(text) {
  if (busy || !text.trim()) return;
  busy = true;
  sendBtn.disabled = true;
  setWelcome(false);
  addUser(text);
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

  try {
    const res = await fetch('/api/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': CSRF },
      body: JSON.stringify({ message: text, conversation_id: cid || 0 }),
    });
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
    sendBtn.disabled = !input.value.trim();
    loadConversations();
    scrollDown();
  }
}

function autosize() {
  input.style.height = 'auto';
  input.style.height = Math.min(input.scrollHeight, 220) + 'px';
  sendBtn.disabled = busy || !input.value.trim();
}

input.addEventListener('input', autosize);
input.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) { e.preventDefault(); form.requestSubmit(); }
});
form.addEventListener('submit', (e) => {
  e.preventDefault();
  const text = input.value;
  if (!text.trim() || busy) return;
  input.value = '';
  autosize();
  send(text);
});
suggests.addEventListener('click', (e) => {
  const b = e.target.closest('button');
  if (b) send(b.textContent);
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
