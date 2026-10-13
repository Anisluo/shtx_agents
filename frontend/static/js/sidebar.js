// 侧边栏：对话列表（分组 / 重命名 / 删除）+ 公共请求工具
const CSRF = document.querySelector('meta[name="csrf-token"]')?.content || '';

async function api(path, opts = {}) {
  const res = await fetch(path, {
    ...opts,
    headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': CSRF, ...(opts.headers || {}) },
  });
  if (res.status === 401) { location.href = '/login?next=' + encodeURIComponent(location.pathname); throw new Error('未登录'); }
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || ('请求失败 ' + res.status));
  return data;
}

function escapeHtml(s) {
  return String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function groupLabel(ts) {
  const d = new Date(ts.replace(' ', 'T'));
  const today = new Date(); today.setHours(0, 0, 0, 0);
  const diff = (today - new Date(d.getFullYear(), d.getMonth(), d.getDate())) / 86400000;
  if (diff <= 0) return '今天';
  if (diff === 1) return '昨天';
  if (diff <= 7) return '最近 7 天';
  if (diff <= 30) return '最近 30 天';
  return '更早';
}

async function loadConversations() {
  const box = document.getElementById('convList');
  if (!box) return;
  try {
    const { conversations } = await api('/api/conversations');
    if (!conversations.length) { box.innerHTML = '<div class="muted small pad">还没有对话</div>'; return; }
    let html = '', last = '';
    for (const c of conversations) {
      const g = groupLabel(c.updated_at);
      if (g !== last) { html += `<div class="conv-group">${g}</div>`; last = g; }
      html += `<div class="conv-item ${c.id === window.CURRENT_CID ? 'active' : ''}" data-id="${c.id}">
        <a href="/chat/${c.id}" title="${escapeHtml(c.title)}">${escapeHtml(c.title)}</a>
        <span class="conv-actions">
          <button data-act="rename" title="重命名">✎</button>
          <button data-act="delete" title="删除">🗑</button>
        </span></div>`;
    }
    box.innerHTML = html;
  } catch (e) {
    box.innerHTML = `<div class="muted small pad">${escapeHtml(e.message)}</div>`;
  }
}

document.getElementById('convList')?.addEventListener('click', async (e) => {
  const btn = e.target.closest('button[data-act]');
  if (!btn) return;
  e.preventDefault();
  const item = btn.closest('.conv-item');
  const id = Number(item.dataset.id);
  const title = item.querySelector('a').textContent;
  if (btn.dataset.act === 'rename') {
    const t = prompt('重命名对话', title);
    if (t && t.trim() && t !== title) {
      await api(`/api/conversations/${id}`, { method: 'PATCH', body: JSON.stringify({ title: t.trim() }) });
      if (id === window.CURRENT_CID) { const el = document.getElementById('convTitle'); if (el) el.textContent = t.trim(); }
      loadConversations();
    }
  } else if (btn.dataset.act === 'delete') {
    if (!confirm(`删除对话「${title}」？此操作不可恢复。`)) return;
    await api(`/api/conversations/${id}`, { method: 'DELETE' });
    if (id === window.CURRENT_CID) location.href = '/chat'; else loadConversations();
  }
});

document.addEventListener('click', (e) => {
  if (!e.target.closest('.menu-wrap')) document.querySelectorAll('.menu.show').forEach(m => m.classList.remove('show'));
});

loadConversations();
