// Execute the production polling code with controlled network/timers; no server or hardware.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../web/圣人视觉识别系统.html'), 'utf8');
const pending = [], timeouts = new Map(), events = {}, intervals = [];
let timer = 0;
const fps = {textContent: '', title: ''};
const context = vm.createContext({
    AbortController, apiEndpoint: '/api', 目前阵容: 0,
    更新实时截图() {}, 更新阵容tab高亮() {}, 切换阵容界面() {},
    document: {hidden: false, getElementById: () => fps, addEventListener: (name, fn) => events[name] = fn},
    setTimeout: fn => { timeouts.set(++timer, fn); return timer; },
    clearTimeout: id => timeouts.delete(id),
    setInterval: fn => intervals.push(fn),
    fetch: (url, options) => new Promise((resolve, reject) => {
        assert.equal(url, '/api');
        options.signal.addEventListener('abort', () => reject(new Error('timeout')));
        pending.push({resolve, reject, signal: options.signal});
    }),
});
const start = html.indexOf('        let 状态查询进行中 =');
assert(start > 0);
vm.runInContext(html.slice(start, html.indexOf('        // Fine controls', start)), context);
const poll = () => vm.runInContext('fetchData()', context);
const flush = () => new Promise(resolve => setImmediate(resolve));
const ok = request => request.resolve({ok: true, json: async () => ({推理帧率: 240, 推理速度FPS: 500, 推理耗时毫秒: 2})});
(async () => {
    const first = poll();
    for (let i = 0; i < 100; ++i) { intervals[0](); poll(); }
    assert.equal(pending.length, 1, 'Slow network must have exactly one poll in flight');
    assert.equal(timeouts.size, 1);
    ok(pending.shift()); await first;
    assert.equal(fps.textContent, '推理速度  500 FPS');
    assert.equal(timeouts.size, 0);
    const stalled = poll(), request = pending.shift();
    [...timeouts.values()][0](); await stalled;
    assert(request.signal.aborted);
    assert.equal(fps.textContent, '失败');
    assert.equal(timeouts.size, 0);
    const recovery = poll(); ok(pending.shift()); await recovery;
    const failed = poll(); pending.shift().resolve({ok: false, status: 503}); await failed;
    assert.equal(fps.textContent, '失败');
    context.document.hidden = true;
    for (let i = 0; i < 100; ++i) intervals[0]();
    events.visibilitychange();
    assert.equal(pending.length, 0, 'Hidden pages must not poll');
    context.document.hidden = false;
    events.visibilitychange(); intervals[0]();
    assert.equal(pending.length, 1, 'Showing the page resumes immediately without overlap');
    ok(pending.shift()); await flush();
    assert.equal(fps.textContent, '推理速度  500 FPS');
    assert.equal(timeouts.size, 0);
    console.log('PASS polling: single in-flight request, timeout/recovery, HTTP errors, hidden tabs and immediate resume.');
})().catch(error => { console.error(error); process.exitCode = 1; });
