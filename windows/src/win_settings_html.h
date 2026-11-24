#ifndef YUNTYPE_WIN_SETTINGS_HTML_H
#define YUNTYPE_WIN_SETTINGS_HTML_H

namespace yuntype::win {

inline constexpr const char *kSettingsPageHtml = R"YUNTYPE_HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>云打 YunType 设置</title>
<style>
  :root {
    --bg: #f5f6f8; --card: #ffffff; --fg: #181a1f; --muted: #646b78;
    --border: #dfe3ea; --accent: #2563eb; --accent-fg: #ffffff;
    --danger: #dc2626; --ok: #16a34a; --input-bg: #ffffff;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #16181d; --card: #1f2229; --fg: #eceff4; --muted: #9aa2b2;
      --border: #2f3440; --accent: #3b82f6; --accent-fg: #ffffff;
      --danger: #ef4444; --ok: #22c55e; --input-bg: #17191f;
    }
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 20px; background: var(--bg); color: var(--fg);
    font: 14px/1.5 "Microsoft YaHei UI", "Segoe UI", system-ui, sans-serif;
  }
  .wrap { max-width: 780px; margin: 0 auto; display: flex; flex-direction: column; gap: 16px; }
  header { display: flex; align-items: center; justify-content: space-between; }
  h1 { margin: 0; font-size: 20px; font-weight: 600; }
  .sub { color: var(--muted); font-size: 12px; }
  .card {
    background: var(--card); border: 1px solid var(--border);
    border-radius: 12px; padding: 16px 18px;
  }
  .card h2 { margin: 0 0 12px; font-size: 15px; font-weight: 600; }
  .row { display: grid; grid-template-columns: 180px 1fr; gap: 10px 14px; align-items: center; margin-bottom: 10px; }
  .row:last-child { margin-bottom: 0; }
  label { color: var(--fg); font-weight: 500; }
  .hint { color: var(--muted); font-size: 12px; margin-top: 2px; }
  input[type="text"], input[type="password"], input[type="number"], select, textarea {
    width: 100%; padding: 7px 10px; border-radius: 8px;
    border: 1px solid var(--border); background: var(--input-bg); color: var(--fg);
    font: inherit;
  }
  textarea { min-height: 76px; resize: vertical; font-family: Consolas, monospace; font-size: 12.5px; }
  .btn {
    display: inline-flex; align-items: center; justify-content: center; gap: 6px;
    padding: 7px 14px; border-radius: 8px; border: 1px solid var(--border);
    background: var(--card); color: var(--fg); cursor: pointer; font: inherit; font-weight: 500;
  }
  .btn:hover { filter: brightness(0.96); }
  .btn.primary { background: var(--accent); border-color: var(--accent); color: var(--accent-fg); }
  .btn.danger { color: var(--danger); border-color: var(--danger); }
  .btn.sm { padding: 4px 10px; font-size: 12.5px; }
  .actions { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
  .model-list { display: flex; flex-direction: column; gap: 8px; margin-bottom: 14px; }
  .model-item {
    display: flex; align-items: center; justify-content: space-between; gap: 10px;
    padding: 10px 12px; border: 1px solid var(--border); border-radius: 10px;
  }
  .model-item.active { border-color: var(--accent); }
  .badge {
    display: inline-block; padding: 1px 7px; border-radius: 999px; font-size: 11px;
    background: var(--accent); color: var(--accent-fg); margin-left: 6px;
  }
  .editor { border-top: 1px solid var(--border); padding-top: 14px; margin-top: 10px; }
  .toast {
    position: fixed; right: 20px; bottom: 20px; padding: 10px 16px; border-radius: 10px;
    background: var(--card); border: 1px solid var(--border); box-shadow: 0 8px 24px rgba(0,0,0,.18);
    opacity: 0; pointer-events: none; transition: opacity .2s;
  }
  .toast.show { opacity: 1; }
  .toast.ok { border-color: var(--ok); color: var(--ok); }
  .toast.err { border-color: var(--danger); color: var(--danger); }
  .switch { display: inline-flex; align-items: center; gap: 8px; cursor: pointer; }
  .switch input { width: 16px; height: 16px; accent-color: var(--accent); }
  .model-field { position: relative; }
  .model-input-row { display: flex; gap: 8px; align-items: center; }
  .model-input-row input { flex: 1; }
  .model-picker {
    position: absolute; left: 0; right: 0; top: calc(100% + 6px); z-index: 50;
    background: var(--card); border: 1px solid var(--border); border-radius: 10px;
    box-shadow: 0 12px 28px rgba(0,0,0,.22); padding: 8px; display: none;
  }
  .model-picker.open { display: block; }
  .picker-search { margin-bottom: 6px; }
  .picker-list { max-height: 240px; overflow-y: auto; display: flex; flex-direction: column; gap: 2px; }
  .picker-item {
    text-align: left; width: 100%; border: 0; background: transparent; color: var(--fg);
    padding: 6px 8px; border-radius: 6px; cursor: pointer; font: inherit;
    font-family: Consolas, monospace; font-size: 13px;
  }
  .picker-item:hover, .picker-item.selected { background: var(--accent); color: var(--accent-fg); }
  .picker-empty { color: var(--muted); font-size: 12.5px; padding: 8px; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div>
      <h1>云打 YunType（Windows）</h1>
      <div class="sub">跨平台大模型整句 & 短打多候选伴随输入法</div>
    </div>
    <div class="actions">
      <button class="btn primary" id="btnSave">保存并应用</button>
    </div>
  </header>

  <section class="card">
    <h2>模型配置</h2>
    <div class="model-list" id="modelList"></div>
    <div class="actions" style="margin-bottom:8px;">
      <button class="btn sm" id="btnAddModel">+ 新建模型</button>
      <button class="btn sm" id="btnTestModel">测试当前编辑模型</button>
      <span id="testResult" class="sub"></span>
    </div>
    <div class="editor" id="modelEditor">
      <div class="row"><label>配置名称</label><input type="text" id="mName" placeholder="例如：DeepSeek"></div>
      <div class="row"><label>接口地址 Base URL</label><input type="text" id="mBaseUrl" placeholder="https://api.deepseek.com/v1"></div>
      <div class="row"><label>API Key</label><input type="password" id="mApiKey" placeholder="sk-..."></div>
      <div class="row">
        <label>模型名称</label>
        <div class="model-field" id="modelField">
          <div class="model-input-row">
            <input type="text" id="mModel" placeholder="deepseek-chat">
            <button type="button" class="btn sm" id="btnFetchModels">获取模型</button>
          </div>
          <div class="model-picker" id="modelPicker">
            <input type="text" class="picker-search" id="pickerSearch" placeholder="输入关键词筛选模型 ID…">
            <div class="picker-list" id="pickerList"></div>
          </div>
        </div>
      </div>
      <div class="row">
        <label>思考模式</label>
        <select id="mThinking">
          <option value="default">默认（不额外传参）</option>
          <option value="reasoning_effort_none">关闭思考 (reasoning_effort: none)</option>
          <option value="reasoning_effort_minimal">极简思考 (reasoning_effort: minimal)</option>
          <option value="reasoning_effort_low">低思考 (reasoning_effort: low)</option>
          <option value="enable_thinking_false">关闭思考 (enable_thinking: false / Qwen 等)</option>
          <option value="thinking_disabled">关闭思考 (thinking.type: disabled / 火山・GLM・Claude 等)</option>
          <option value="chat_template_enable_thinking_false">关闭思考 (chat_template_kwargs / vLLM・SGLang)</option>
          <option value="think_false">关闭思考 (think: false / Ollama)</option>
          <option value="custom">自定义额外请求 JSON</option>
        </select>
      </div>
      <div class="row" id="rowExtraJson" style="display:none;">
        <label>自定义额外 JSON</label>
        <input type="text" id="mExtraJson" placeholder='{"reasoning_effort":"none"}'>
      </div>
      <div class="actions" style="margin-top:10px;">
        <button class="btn sm primary" id="btnApplyModel">应用到当前配置</button>
      </div>
    </div>
  </section>

  <section class="card">
    <h2>触发与短输入候选</h2>
    <div class="row">
      <label>启用云打</label>
      <label class="switch"><input type="checkbox" id="enabled"> 监听触发快捷键</label>
    </div>
    <div class="row">
      <label>触发快捷键</label>
      <select id="triggerKey">
        <option value="Alt+U">Alt + U（默认）</option>
        <option value="Ctrl+U">Ctrl + U</option>
        <option value="Ctrl+Shift+U">Ctrl + Shift + U</option>
        <option value="Alt+S">Alt + S</option>
        <option value="Ctrl+Shift+S">Ctrl + Shift + S</option>
        <option value="Alt+Space">Alt + Space</option>
        <option value="F9">F9</option>
      </select>
    </div>
    <div class="row">
      <label>短输入候选拼音长度阈值</label>
      <div>
        <input type="number" id="shortThreshold" min="0" max="200">
        <div class="hint">拼音字符数 ≤ 阈值时显示多候选（设为 0 关闭短打候选，仅走整句）</div>
      </div>
    </div>
    <div class="row">
      <label>候选框单页显示上限</label>
      <input type="number" id="pageSize" min="2" max="10">
    </div>
    <div class="row">
      <label>开机自动启动</label>
      <label class="switch"><input type="checkbox" id="autoStart"> 登录 Windows 时自动运行</label>
    </div>
  </section>

  <section class="card">
    <h2>历史上下文与提示词</h2>
    <div class="row">
      <label>记住近期输入历史</label>
      <label class="switch"><input type="checkbox" id="historyEnabled"> 启用跨窗口多轮 KV 缓存与改错学习</label>
    </div>
    <div class="row">
      <label>完整历史条数上限</label>
      <input type="number" id="historyCount" min="1" max="300">
    </div>
    <div class="row">
      <label>自定义整句提示词</label>
      <textarea id="sentencePrompt" placeholder="留空使用内置默认提示词"></textarea>
    </div>
    <div class="row">
      <label>历史记录管理</label>
      <div class="actions">
        <button class="btn sm danger" id="btnClearHistory">清空历史记录</button>
        <span class="sub" id="historyInfo"></span>
      </div>
    </div>
  </section>
</div>
<div class="toast" id="toast"></div>

<script>
const TOKEN = new URLSearchParams(location.search).get('token') || '';
let state = { profiles: [], active: '' };
let editIndex = 0;

function toast(msg, kind = 'ok') {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'toast show ' + kind;
  clearTimeout(toast._t);
  toast._t = setTimeout(() => el.classList.remove('show'), 2600);
}

async function api(path, method = 'GET', body = null) {
  const res = await fetch(path, {
    method,
    headers: { 'Content-Type': 'application/json', 'X-YunType-Token': TOKEN },
    body: body ? JSON.stringify(body) : null,
  });
  const data = await res.json();
  if (!res.ok) throw new Error(data.error || ('HTTP ' + res.status));
  return data;
}

function renderModels() {
  const list = document.getElementById('modelList');
  list.innerHTML = '';
  state.profiles.forEach((p, idx) => {
    const div = document.createElement('div');
    div.className = 'model-item' + (p.name === state.active ? ' active' : '');
    const left = document.createElement('div');
    const title = document.createElement('div');
    title.innerHTML = '<strong>' + escapeHtml(p.name) + '</strong>' +
      (p.name === state.active ? '<span class="badge">当前使用</span>' : '');
    const sub = document.createElement('div');
    sub.className = 'sub';
    sub.textContent = (p.model || '(未填模型)') + ' · ' + (p.base_url || '(未填地址)');
    left.appendChild(title);
    left.appendChild(sub);

    const right = document.createElement('div');
    right.className = 'actions';
    if (p.name !== state.active) {
      const bUse = document.createElement('button');
      bUse.className = 'btn sm';
      bUse.textContent = '启用';
      bUse.onclick = () => { state.active = p.name; renderModels(); };
      right.appendChild(bUse);
    }
    const bEdit = document.createElement('button');
    bEdit.className = 'btn sm';
    bEdit.textContent = (idx === editIndex ? '编辑中' : '编辑');
    bEdit.onclick = () => { loadEditor(idx); renderModels(); };
    right.appendChild(bEdit);

    if (state.profiles.length > 1) {
      const bDel = document.createElement('button');
      bDel.className = 'btn sm danger';
      bDel.textContent = '删除';
      bDel.onclick = () => {
        const wasActive = (p.name === state.active);
        state.profiles.splice(idx, 1);
        if (wasActive && state.profiles.length) state.active = state.profiles[0].name;
        if (editIndex >= state.profiles.length) editIndex = Math.max(0, state.profiles.length - 1);
        loadEditor(editIndex);
        renderModels();
      };
      right.appendChild(bDel);
    }
    div.appendChild(left);
    div.appendChild(right);
    list.appendChild(div);
  });
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

function loadEditor(idx) {
  editIndex = idx;
  const p = state.profiles[idx] || { name:'', base_url:'', model:'', api_key:'', thinking_mode:'default', extra_request_json:'' };
  document.getElementById('mName').value = p.name || '';
  document.getElementById('mBaseUrl').value = p.base_url || '';
  document.getElementById('mModel').value = p.model || '';
  document.getElementById('mApiKey').value = p.api_key || '';
  document.getElementById('mThinking').value = p.thinking_mode || 'default';
  document.getElementById('mExtraJson').value = p.extra_request_json || '';
  updateExtraVisibility();
}

function updateExtraVisibility() {
  document.getElementById('rowExtraJson').style.display =
    document.getElementById('mThinking').value === 'custom' ? 'grid' : 'none';
}

function readEditorIntoState() {
  if (!state.profiles[editIndex]) return;
  const oldName = state.profiles[editIndex].name;
  const newName = document.getElementById('mName').value.trim() || oldName || 'Model';
  state.profiles[editIndex] = {
    name: newName,
    base_url: document.getElementById('mBaseUrl').value.trim(),
    model: document.getElementById('mModel').value.trim(),
    api_key: document.getElementById('mApiKey').value.trim(),
    thinking_mode: document.getElementById('mThinking').value,
    extra_request_json: document.getElementById('mExtraJson').value.trim(),
  };
  if (state.active === oldName) state.active = newName;
}

function collectState() {
  readEditorIntoState();
  state.enabled = document.getElementById('enabled').checked;
  state.trigger_key = document.getElementById('triggerKey').value.trim() || 'Alt+U';
  state.short_candidate_threshold = parseInt(document.getElementById('shortThreshold').value, 10) || 0;
  state.candidate_page_size = parseInt(document.getElementById('pageSize').value, 10) || 5;
  state.auto_start = document.getElementById('autoStart').checked;
  state.history_enabled = document.getElementById('historyEnabled').checked;
  state.history_count = parseInt(document.getElementById('historyCount').value, 10) || 30;
  state.sentence_prompt = document.getElementById('sentencePrompt').value;
  return state;
}

let fetchedModels = [];

function closeModelPicker() {
  document.getElementById('modelPicker').classList.remove('open');
}

function renderPickerList() {
  const list = document.getElementById('pickerList');
  const q = document.getElementById('pickerSearch').value.trim().toLowerCase();
  const current = document.getElementById('mModel').value.trim();
  const matched = fetchedModels.filter(id => !q || id.toLowerCase().includes(q));
  list.innerHTML = '';
  if (!matched.length) {
    const empty = document.createElement('div');
    empty.className = 'picker-empty';
    empty.textContent = fetchedModels.length ? '无匹配的模型 ID' : '接口未返回模型列表，请手动输入';
    list.appendChild(empty);
    return;
  }
  matched.forEach(id => {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'picker-item' + (id === current ? ' selected' : '');
    btn.textContent = id;
    btn.onclick = () => {
      document.getElementById('mModel').value = id;
      closeModelPicker();
    };
    list.appendChild(btn);
  });
}

document.getElementById('pickerSearch').oninput = renderPickerList;
document.getElementById('pickerSearch').onkeydown = (e) => {
  if (e.key === 'Escape') closeModelPicker();
};
document.addEventListener('click', (e) => {
  if (!document.getElementById('modelField').contains(e.target)) {
    closeModelPicker();
  }
});
document.getElementById('btnFetchModels').onclick = async () => {
  const base_url = document.getElementById('mBaseUrl').value.trim();
  const api_key = document.getElementById('mApiKey').value.trim();
  if (!base_url) {
    toast('请先填写接口地址 Base URL', 'err');
    return;
  }
  const btn = document.getElementById('btnFetchModels');
  btn.disabled = true;
  btn.textContent = '获取中…';
  try {
    const res = await api('/api/models', 'POST', { name: 'probe', base_url, api_key });
    fetchedModels = Array.isArray(res.models) ? res.models : [];
    if (fetchedModels.length === 1) {
      document.getElementById('mModel').value = fetchedModels[0];
      closeModelPicker();
      toast('已自动填入唯一模型：' + fetchedModels[0]);
    } else if (fetchedModels.length > 1) {
      const search = document.getElementById('pickerSearch');
      search.value = '';
      search.placeholder = '输入关键词筛选模型 ID（共 ' + fetchedModels.length + ' 个）…';
      renderPickerList();
      document.getElementById('modelPicker').classList.add('open');
      search.focus();
    } else {
      closeModelPicker();
      toast('接口未返回模型列表，请手动输入模型 ID', 'err');
    }
  } catch (e) {
    closeModelPicker();
    toast('获取模型失败：' + e.message, 'err');
  } finally {
    btn.disabled = false;
    btn.textContent = '获取模型';
  }
};

document.getElementById('mThinking').onchange = updateExtraVisibility;
document.getElementById('btnApplyModel').onclick = () => {
  readEditorIntoState();
  renderModels();
  toast('已暂存修改，点击右上角「保存并应用」生效');
};
document.getElementById('btnAddModel').onclick = () => {
  readEditorIntoState();
  const name = '模型 ' + (state.profiles.length + 1);
  state.profiles.push({
    name, base_url: 'https://api.deepseek.com/v1', model: '', api_key: '',
    thinking_mode: 'default', extra_request_json: ''
  });
  loadEditor(state.profiles.length - 1);
  renderModels();
};
document.getElementById('btnSave').onclick = async () => {
  try {
    const payload = collectState();
    const res = await api('/api/save', 'POST', payload);
    state = res;
    renderModels();
    toast('设置已保存并立即生效');
  } catch (e) {
    toast('保存失败：' + e.message, 'err');
  }
};
document.getElementById('btnTestModel').onclick = async () => {
  const out = document.getElementById('testResult');
  out.textContent = '正在测试连接…';
  try {
    readEditorIntoState();
    const p = state.profiles[editIndex];
    const res = await api('/api/test', 'POST', p);
    out.textContent = '成功 (' + res.ms + ' ms): ' + res.output;
    toast('模型连接测试通过');
  } catch (e) {
    out.textContent = '失败: ' + e.message;
    toast('测试失败：' + e.message, 'err');
  }
};
document.getElementById('btnClearHistory').onclick = async () => {
  try {
    await api('/api/clear-history', 'POST', {});
    document.getElementById('historyInfo').textContent = '当前历史大小：0 B';
    toast('已清空历史记录');
  } catch (e) {
    toast('清空失败：' + e.message, 'err');
  }
};

(async () => {
  try {
    state = await api('/api/state');
    document.getElementById('enabled').checked = !!state.enabled;
    const rawKey = (state.trigger_key || 'Alt+U').replace(/^Control\+/i, 'Ctrl+');
    const tkEl = document.getElementById('triggerKey');
    if (!Array.from(tkEl.options).some(o => o.value === rawKey)) {
      const opt = document.createElement('option');
      opt.value = rawKey;
      opt.textContent = rawKey;
      tkEl.appendChild(opt);
    }
    tkEl.value = rawKey;
    document.getElementById('shortThreshold').value = state.short_candidate_threshold ?? 50;
    document.getElementById('pageSize').value = state.candidate_page_size ?? 5;
    document.getElementById('autoStart').checked = !!state.auto_start;
    document.getElementById('historyEnabled').checked = !!state.history_enabled;
    document.getElementById('historyCount').value = state.history_count ?? 30;
    document.getElementById('sentencePrompt').value = state.sentence_prompt || '';
    document.getElementById('historyInfo').textContent =
      '当前历史大小：' + ((state.history_bytes || 0) / 1024).toFixed(1) + ' KB';
    const activeIdx = Math.max(0, state.profiles.findIndex(p => p.name === state.active));
    loadEditor(activeIdx);
    renderModels();
  } catch (e) {
    toast('加载配置失败：' + e.message, 'err');
  }
})();
</script>
</body>
</html>
)YUNTYPE_HTML";

} // namespace yuntype::win

#endif
