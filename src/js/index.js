// ─── Message keys (must match appinfo.json appKeys) ─────────────────────────
var KEY_REQUEST_ITEMS    = 0;
var KEY_ITEM_COUNT       = 1;
var KEY_ITEM_INDEX       = 2;
var KEY_ITEM_SUMMARY     = 3;
var KEY_ITEM_STATUS      = 4;
var KEY_TOGGLE_ITEM      = 5;
var KEY_DELETE_COMPLETED = 6;
var KEY_ERROR_MESSAGE    = 7;
var KEY_STATUS_MESSAGE   = 8;
var KEY_LIST_NAME        = 9;

// ─── Stored items ─────────────────────────────────────────────────────────────
var g_items = [];   // [{uid, summary, status}]

// ─── Settings helpers ────────────────────────────────────────────────────────

function getSetting(key) {
  return localStorage.getItem(key) || '';
}

function getBaseUrl() {
  return getSetting('ha_url').replace(/\/+$/, '');
}

function getWsUrl() {
  // Convert http(s):// to ws(s)://
  return getBaseUrl().replace(/^http/, 'ws') + '/api/websocket';
}

function getToken() {
  return getSetting('ha_token');
}

function getEntityId() {
  return getSetting('todo_list');
}

// ─── Send helpers ─────────────────────────────────────────────────────────────

function sendMsg(msg, onAck, onFail) {
  Pebble.sendAppMessage(msg, onAck || function(){}, onFail || function(){});
}

function sendError(text) {
  var msg = {};
  msg[KEY_ERROR_MESSAGE] = text.substring(0, 63);
  sendMsg(msg);
}

function sendStatus(text, onAck) {
  var msg = {};
  msg[KEY_STATUS_MESSAGE] = text.substring(0, 63);
  sendMsg(msg, onAck);
}

// ─── Send items to watch ──────────────────────────────────────────────────────

function sendItemsToWatch() {
  var msg = {};
  msg[KEY_ITEM_COUNT] = g_items.length;
  sendMsg(msg, function() { sendNextItem(0); });
}

function sendNextItem(idx) {
  if (idx >= g_items.length) return;
  var item = g_items[idx];
  var msg = {};
  msg[KEY_ITEM_INDEX]   = idx;
  msg[KEY_ITEM_SUMMARY] = item.summary.substring(0, 63);
  msg[KEY_ITEM_STATUS]  = item.status === 'completed' ? 1 : 0;
  sendMsg(msg, function() { sendNextItem(idx + 1); });
}

// ─── WebSocket helper ─────────────────────────────────────────────────────────
// Opens a fresh WS connection, authenticates, runs one command, then closes.

function wsCommand(command, onSuccess, onError) {
  var ws;
  var msgId = 1;
  var authenticated = false;
  var done = false;

  function finish(err, result) {
    if (done) return;
    done = true;
    try { ws.close(); } catch(e) {}
    if (err) onError(err);
    else onSuccess(result);
  }

  try {
    ws = new WebSocket(getWsUrl());
  } catch(e) {
    onError('WS open failed');
    return;
  }

  ws.onopen = function() {};

  ws.onmessage = function(event) {
    var msg;
    try { msg = JSON.parse(event.data); } catch(e) { return; }

    if (msg.type === 'auth_required') {
      ws.send(JSON.stringify({ type: 'auth', access_token: getToken() }));
      return;
    }

    if (msg.type === 'auth_ok') {
      authenticated = true;
      var cmd = JSON.parse(JSON.stringify(command)); // clone
      cmd.id = msgId;
      ws.send(JSON.stringify(cmd));
      return;
    }

    if (msg.type === 'auth_invalid') {
      finish('Auth failed', null);
      return;
    }

    if (msg.id === msgId) {
      if (msg.success) {
        finish(null, msg.result);
      } else {
        finish(msg.error ? msg.error.message : 'Command failed', null);
      }
    }
  };

  ws.onerror = function() { finish('WS error', null); };
  ws.onclose = function() { if (!done) finish('WS closed', null); };
}

// ─── Fetch items via WebSocket ────────────────────────────────────────────────

function fetchAndSendItems() {
  var entityId = getEntityId();
  if (!entityId) { sendError('No list configured'); return; }
  if (!getBaseUrl() || !getToken()) { sendError('No HA config'); return; }

  wsCommand(
    {
      type: 'call_service',
      domain: 'todo',
      service: 'get_items',
      target: { entity_id: entityId },
      return_response: true
    },
    function(result) {
      // result.response is { "todo.entity_id": { items: [...] } }
      var response = (result && result.response) ? result.response : result;
      var bucket = response[entityId] || {};
      var raw = bucket.items || [];
      g_items = raw.map(function(it) {
        return {
          uid:     it.uid     || it.summary,
          summary: it.summary || '(no text)',
          status:  it.status  || 'needs_action'
        };
      });
      // Incomplete first, completed at bottom, alphabetical within each group
      g_items.sort(function(a, b) {
        var aD = a.status === 'completed' ? 1 : 0;
        var bD = b.status === 'completed' ? 1 : 0;
        if (aD !== bD) return aD - bD;
        return a.summary.toLowerCase() < b.summary.toLowerCase() ? -1 : 1;
      });
      sendItemsToWatch();
    },
    function(err) { sendError(err); }
  );
}

// ─── Toggle via REST (service call, no return value needed) ──────────────────

function haPost(path, body, onSuccess, onError) {
  var xhr = new XMLHttpRequest();
  xhr.open('POST', getBaseUrl() + path, true);
  xhr.setRequestHeader('Authorization', 'Bearer ' + getToken());
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.timeout = 10000;
  xhr.onload    = function() {
    if (xhr.status >= 200 && xhr.status < 300) onSuccess();
    else onError('HTTP ' + xhr.status);
  };
  xhr.onerror   = function() { onError('Network error'); };
  xhr.ontimeout = function() { onError('Timeout'); };
  xhr.send(JSON.stringify(body));
}

function toggleItem(idx, newStatusInt) {
  if (idx < 0 || idx >= g_items.length) return;
  var item      = g_items[idx];
  var newStatus = (newStatusInt === 1) ? 'completed' : 'needs_action';
  g_items[idx].status = newStatus;

  haPost(
    '/api/services/todo/update_item',
    { entity_id: getEntityId(), item: item.uid, status: newStatus },
    function() {
      // Re-fetch so JS array order stays in sync with the watch's sorted display
      fetchAndSendItems();
    },
    function(err) {
      sendError('Toggle failed: ' + err);
      g_items[idx].status = (newStatus === 'completed') ? 'needs_action' : 'completed';
    }
  );
}

function deleteCompleted() {
  haPost(
    '/api/services/todo/remove_completed_items',
    { entity_id: getEntityId() },
    function() {
      sendStatus('Done!', function() { fetchAndSendItems(); });
    },
    function(err) { sendError('Delete failed: ' + err); }
  );
}

// ─── AppMessage listener ──────────────────────────────────────────────────────

Pebble.addEventListener('appmessage', function(e) {
  var p = e.payload;
  if (p[KEY_REQUEST_ITEMS]    !== undefined) { fetchAndSendItems(); }
  if (p[KEY_TOGGLE_ITEM]      !== undefined) { toggleItem(p[KEY_TOGGLE_ITEM], p[KEY_ITEM_STATUS]); }
  if (p[KEY_DELETE_COMPLETED] !== undefined) { deleteCompleted(); }
});

Pebble.addEventListener('ready', function() {
  // Send formatted list name to watch
  var entityId = getEntityId();
  if (entityId) {
    // Strip "todo." prefix and title-case with spaces
    var name = entityId.replace(/^todo\./, '').replace(/_/g, ' ').toUpperCase();
    var msg = {};
    msg[KEY_LIST_NAME] = name.substring(0, 47);
    sendMsg(msg);
  }
  fetchAndSendItems();
});

// ─── Configuration page ───────────────────────────────────────────────────────

Pebble.addEventListener('showConfiguration', function() {
  var haUrl = getSetting('ha_url');
  var token = getSetting('ha_token');
  var list  = getSetting('todo_list');

  var html = [
    '<!DOCTYPE html><html><head>',
    '<meta charset="utf-8">',
    '<meta name="viewport" content="width=device-width,initial-scale=1">',
    '<title>HA To-Do Settings</title>',
    '<style>',
    'body{font-family:sans-serif;background:#1a1a2e;color:#eee;padding:16px;margin:0}',
    'h1{font-size:20px;color:#e94560;margin-bottom:20px}',
    'label{display:block;font-size:13px;color:#aaa;margin-top:14px;margin-bottom:4px}',
    'input{width:100%;box-sizing:border-box;padding:10px;font-size:15px;background:#16213e;color:#eee;border:1px solid #e94560;border-radius:6px}',
    'small{display:block;color:#888;font-size:11px;margin-top:3px}',
    'button{margin-top:24px;width:100%;padding:14px;font-size:16px;font-weight:bold;background:#e94560;color:#fff;border:none;border-radius:8px;cursor:pointer}',
    '</style></head><body>',
    '<h1>Home Assistant To-Do</h1>',
    '<label>Home Assistant URL</label>',
    '<input id="u" type="url" placeholder="http://192.168.1.100:8123" value="' + escHtml(haUrl) + '">',
    '<small>Local IP of your HA instance</small>',
    '<label>Long-Lived Access Token</label>',
    '<input id="t" type="password" placeholder="ey..." value="' + escHtml(token) + '">',
    '<small>HA Profile &rarr; Long-Lived Access Tokens</small>',
    '<label>To-Do List Entity ID</label>',
    '<input id="l" type="text" placeholder="todo.shopping_list" value="' + escHtml(list) + '">',
    '<small>Developer Tools &rarr; States, filter by todo.</small>',
    '<button onclick="save()">Save &amp; Close</button>',
    '<script>',
    'function save(){',
    '  var c=JSON.stringify({haUrl:document.getElementById("u").value.trim(),haToken:document.getElementById("t").value.trim(),todoList:document.getElementById("l").value.trim()});',
    '  window.location.href="pebblejs://close#"+encodeURIComponent(c);',
    '}',
    '<\/script></body></html>'
  ].join('');

  Pebble.openURL('data:text/html,' + encodeURIComponent(html));
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e.response || e.response === 'CANCELLED') return;
  try {
    var cfg = JSON.parse(decodeURIComponent(e.response));
    if (cfg.haUrl)    localStorage.setItem('ha_url',    cfg.haUrl);
    if (cfg.haToken)  localStorage.setItem('ha_token',  cfg.haToken);
    if (cfg.todoList) localStorage.setItem('todo_list', cfg.todoList);
  } catch (ex) {}
});

// ─── Util ─────────────────────────────────────────────────────────────────────

function escHtml(s) {
  return (s || '')
    .replace(/&/g, '&amp;')
    .replace(/"/g, '&quot;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}
