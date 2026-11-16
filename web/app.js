// Web UI for the search server. Contract: docs/API.md (REST sections 6-9,
// WebSocket section 10). Vanilla JS, no dependencies.
(function () {
  'use strict';

  var $ = function (id) { return document.getElementById(id); };
  var STAGES = ['query_expansion', 'keyword_search', 'done'];

  // ---------- helpers ----------
  function setText(el, text) { el.textContent = text; }
  function show(el, text) { if (text !== undefined) { el.textContent = text; } el.hidden = false; }
  function hide(el) { el.hidden = true; el.textContent = ''; }
  function clear(el) { while (el.firstChild) { el.removeChild(el.firstChild); } }
  function mk(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) { e.className = cls; }
    if (text !== undefined) { e.textContent = text; }
    return e;
  }

  // ---------- pipeline view ----------
  var stepEls = {};
  STAGES.forEach(function (s) {
    stepEls[s] = document.querySelector('.step[data-stage="' + s + '"]');
  });

  function setStep(stage, state, timeMs) {
    var el = stepEls[stage];
    el.className = 'step ' + state;
    setText(el.querySelector('.state'), state);
    setText(el.querySelector('.time'), timeMs === undefined || timeMs === null ? '' : timeMs + ' ms');
  }

  function resetPipeline() {
    STAGES.forEach(function (s) { setStep(s, 'pending'); });
    $('terms-box').hidden = true;
    clear($('chips'));
    hide($('pipe-error'));
  }

  // ---------- search state ----------
  var ws = null;
  var wsReady = null;       // Promise resolved when ws is open
  var running = false;
  var current = null;       // { terms:Set, original:Set, stage }

  function wsState(text) { setText($('ws-state'), 'WebSocket: ' + text); }

  function connect() {
    if (ws && ws.readyState === WebSocket.OPEN) { return Promise.resolve(ws); }
    if (ws && ws.readyState === WebSocket.CONNECTING && wsReady) { return wsReady; }
    var proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
    wsState('connecting...');
    wsReady = new Promise(function (resolve, reject) {
      var sock;
      try { sock = new WebSocket(proto + location.host + '/ws/search'); }
      catch (e) { reject(e); return; }
      ws = sock;
      sock.onopen = function () { wsState('connected'); resolve(sock); };
      sock.onmessage = function (ev) { onMessage(ev.data); };
      sock.onerror = function () { reject(new Error('connection failed')); };
      sock.onclose = function () {
        wsState('closed (will reconnect on next search)');
        if (ws === sock) { ws = null; }
        if (running) { failCurrent('connection_closed', 'The WebSocket connection was closed.'); }
      };
    });
    return wsReady;
  }

  function finishRun() {
    running = false;
    $('search-btn').disabled = false;
  }

  function failCurrent(code, message) {
    var stage = current && current.stage === 'query_expansion_done' ? 'keyword_search' : 'query_expansion';
    setStep(stage, 'failed');
    var text = code + ': ' + message;
    if (code === 'index_not_built') {
      text += ' Use "Build index" in the Admin panel below, then search again.';
    }
    show($('pipe-error'), text);
    finishRun();
  }

  function onSubmit(ev) {
    ev.preventDefault();
    if (running) { return; }
    var query = $('query').value;
    var msg = { query: query, mode: 'keyword', expand: $('expand').checked };
    var lim = $('limit').value.trim();
    if (lim !== '') {
      var n = Number(lim);
      if (!Number.isInteger(n) || n < 1 || n > 100) {
        show($('search-error'), 'Limit must be an integer between 1 and 100.');
        return;
      }
      msg.limit = n;
    }
    hide($('search-error'));
    resetPipeline();
    clear($('results'));
    setText($('res-summary'), 'Searching...');
    running = true;
    $('search-btn').disabled = true;
    current = { terms: new Set(), original: new Set(), stage: 'start' };
    setStep('query_expansion', 'active');

    connect().then(function (sock) {
      sock.send(JSON.stringify(msg));
    }).catch(function (e) {
      setText($('res-summary'), 'No results.');
      failCurrent('connection_failed', e.message || 'cannot open the WebSocket.');
    });
  }

  function onMessage(raw) {
    var m;
    try { m = JSON.parse(raw); } catch (e) { return; }
    if (!m || typeof m.stage !== 'string' || !current) { return; }
    var d = m.data || {};
    if (m.stage === 'query_expansion') {
      setStep('query_expansion', 'complete', d.time_ms);
      renderTerms(d);
      current.stage = 'query_expansion_done';
      setStep('keyword_search', 'active');
    } else if (m.stage === 'keyword_search') {
      setStep('keyword_search', 'complete', d.time_ms);
      renderResults(d);
      current.stage = 'keyword_search_done';
      setStep('done', 'active');
    } else if (m.stage === 'done') {
      setStep('done', 'complete');
      current.stage = 'done';
      finishRun();
    } else if (m.stage === 'error') {
      setText($('res-summary'), 'No results.');
      failCurrent(String(d.code || 'error'), String(d.error || 'unknown error'));
    }
  }

  function renderTerms(d) {
    var terms = Array.isArray(d.expanded_terms) ? d.expanded_terms : [];
    // Original terms: lowercase alphanumeric tokens of the query (API.md 4.1).
    var orig = new Set();
    String(d.original || '').toLowerCase().replace(/[a-z0-9]+/g, function (t) { orig.add(t); return t; });
    var box = $('terms-box');
    var chips = $('chips');
    clear(chips);
    terms.forEach(function (t) {
      current.terms.add(String(t).toLowerCase());
      var li = mk('li', 'chip ' + (orig.has(t) ? 'orig' : 'added'), t);
      chips.appendChild(li);
    });
    setText($('expansion-src'), d.expansion || 'unknown');
    box.hidden = false;
  }

  function highlight(parent, text, terms) {
    var re = /[A-Za-z0-9]+/g, last = 0, m;
    while ((m = re.exec(text)) !== null) {
      if (terms.has(m[0].toLowerCase())) {
        if (m.index > last) { parent.appendChild(document.createTextNode(text.slice(last, m.index))); }
        parent.appendChild(mk('mark', null, m[0]));
        last = m.index + m[0].length;
      }
    }
    if (last < text.length) { parent.appendChild(document.createTextNode(text.slice(last))); }
  }

  function renderResults(d) {
    var list = $('results');
    clear(list);
    var results = Array.isArray(d.results) ? d.results : [];
    var total = typeof d.total_matches === 'number' ? d.total_matches : results.length;
    if (results.length === 0) {
      setText($('res-summary'), 'No results. Total matches: ' + total + '.');
      return;
    }
    setText($('res-summary'), 'Showing ' + results.length + ' of ' + total + ' matching documents.');
    results.forEach(function (r, i) {
      var li = mk('li', 'result');
      var head = mk('div', 'head');
      head.appendChild(mk('span', 'rank', String(i + 1) + '.'));
      head.appendChild(mk('span', 'docid', String(r.doc_id)));
      var sc = mk('span', 'score');
      var score = Number(r.score) || 0;
      sc.appendChild(mk('span', null, score.toFixed(2)));
      var bar = mk('span', 'bar');
      var fill = document.createElement('i');
      fill.style.width = Math.max(0, Math.min(100, score * 100)) + '%';
      bar.appendChild(fill);
      sc.appendChild(bar);
      head.appendChild(sc);
      li.appendChild(head);
      var p = mk('p', 'snippet');
      highlight(p, String(r.snippet || ''), current ? current.terms : new Set());
      li.appendChild(p);
      list.appendChild(li);
    });
  }

  // ---------- admin panel (REST) ----------
  var pollTimer = null;

  function yesNo(el, v, goodWhenTrue) {
    setText(el, v ? 'yes' : 'no');
    el.className = (v === goodWhenTrue) ? 'good' : '';
  }

  function adminMsg(kind, text) {
    var el = $('admin-msg');
    el.className = 'msg ' + kind;
    show(el, text);
  }

  function getJson(url) {
    return fetch(url).then(function (r) { return r.json(); });
  }

  function refresh() {
    return Promise.all([getJson('/api/health'), getJson('/api/stats')]).then(function (res) {
      var h = res[0], s = res[1];
      setText($('h-server'), h.status === 'ok' ? 'ok' : 'error');
      $('h-server').className = h.status === 'ok' ? 'good' : 'bad';
      yesNo($('h-ollama'), !!h.ollama, true);
      yesNo($('h-ready'), !!h.index_ready, true);
      yesNo($('h-indexing'), !!h.indexing, false);
      setText($('s-docs'), String(s.documents));
      setText($('s-terms'), String(s.unique_terms));
      setText($('s-time'), s.index_build_time_ms + ' ms');
      setText($('s-threads'), String(s.threads_used));
    }).catch(function () {
      setText($('h-server'), 'unreachable');
      $('h-server').className = 'bad';
    });
  }

  function onBuild(ev) {
    ev.preventDefault();
    var btn = $('index-btn');
    var body = {};
    var t = $('threads').value.trim();
    if (t !== '') {
      var n = Number(t);
      if (!Number.isInteger(n) || n < 1 || n > 64) {
        adminMsg('error', 'Threads must be an integer between 1 and 64.');
        return;
      }
      body.threads = n;
    }
    btn.disabled = true;
    adminMsg('', 'Building index... this can take a while for large datasets.');
    pollTimer = setInterval(refresh, 3000);
    fetch('/api/index', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }).then(function (r) {
      return r.json().then(function (j) { return { status: r.status, body: j }; });
    }).then(function (res) {
      if (res.status === 200) {
        var j = res.body;
        adminMsg('ok', 'Index built: ' + j.documents_indexed + ' documents in ' + j.time_ms +
          ' ms using ' + j.threads_used + ' threads' +
          (j.files_skipped ? ' (' + j.files_skipped + ' files skipped)' : '') + '.');
      } else {
        adminMsg('error', (res.body.code || 'error') + ': ' + (res.body.error || 'request failed') +
          ' (HTTP ' + res.status + ')');
      }
    }).catch(function (e) {
      adminMsg('error', 'Request failed: ' + (e && e.message ? e.message : 'server unreachable'));
    }).then(function () {
      clearInterval(pollTimer);
      pollTimer = null;
      btn.disabled = false;
      refresh();
    });
  }

  // ---------- init ----------
  $('search-form').addEventListener('submit', onSubmit);
  $('index-form').addEventListener('submit', onBuild);
  $('refresh-btn').addEventListener('click', function () { hide($('admin-msg')); refresh(); });
  refresh();
})();
