// Run: node tools/test_web_models.cjs (no browser, GPU or application startup).
// Execute the production page functions and handlers against a minimal DOM,
// with deferred HTTP responses to exercise refresh races and failed requests.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../web/圣人视觉识别系统.html'), 'utf8');
for (const block of html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)) new vm.Script(block[1]);
for (const id of ['NCNN', 'ONNX', 'TensorRT']) {
    assert(new RegExp('<input[^>]*id="' + id + '"[^>]*disabled').test(html));
}
class Element {
    constructor() {
        this.options = []; this.disabled = false; this.checked = false; this._value = '';
        this.handlers = {}; this.classes = new Set();
        this.parentElement = {title: '', classList: {toggle: (name, enabled) => enabled ? this.classes.add(name) : this.classes.delete(name)}};
    }
    get value() { return this._value; }
    set value(value) { this._value = this.options.some(option => option.value === value) ? value : ''; }
    replaceChildren(...options) { this.options = options; this._value = options.length ? options[0].value : ''; }
    addEventListener(event, handler) { this.handlers[event] = handler; }
    change() { this.handlers.change.call(this); }
}
const elements = new Map(), pending = [], posts = [];
const get = id => {
    if (!elements.has(id)) elements.set(id, new Element());
    return elements.get(id);
};
const context = vm.createContext({
    模型名称: '中文模型', 推理引擎: 2,
    document: {getElementById: get, createElement: () => ({}), querySelectorAll: () => []},
    fetch: url => new Promise((resolve, reject) => pending.push({url, resolve, reject})),
    发送消息给服务器: (key, value) => posts.push([key, value]),
});
vm.runInContext(html.slice(html.indexOf('        let 模型列表请求序号'), html.indexOf('        window.onload = function()')), context);
vm.runInContext(html.slice(html.indexOf('        function initSliders()'), html.lastIndexOf('    </script>')), context);
vm.runInContext('initSliders()', context);
const select = get('模型名称'), ids = ['NCNN', 'ONNX', 'TensorRT'];
const refresh = () => vm.runInContext('刷新模型列表()', context);
const respond = (request, models) => request.resolve({ok: true, json: async () => ({models})});
const names = () => select.options.filter(option => option.value).map(option => option.value);
const check = engines => ids.forEach((id, index) => {
    const disabled = !engines.includes(index + 1);
    assert.equal(get(id).disabled, disabled, id + ' disabled');
    assert.equal(get(id).classes.has('engine-disabled'), disabled, id + ' gray style');
    if (disabled) assert.equal(get(id).checked, false, id + ' must not show selected');
});
async function load(models) {
    const task = refresh(); check([]); assert(select.disabled);
    const request = pending.shift(); assert.equal(request.url, '/model-catalog.json');
    respond(request, models); await task;
}
(async () => {
    // All eight engine combinations, with an unrelated full-format model to
    // ensure another model's files cannot unlock the selected model's buttons.
    for (let mask = 1; mask < 8; ++mask) {
        const engines = [1, 2, 3].filter(engine => mask & (1 << (engine - 1)));
        await load([{name: '中文模型', engines}, {name: '其他模型', engines: [1, 2, 3]}]);
        assert.equal(select.value, '中文模型'); check(engines);
    }
    for (let index = 0; index < ids.length; ++index) {
        get(ids[index]).checked = true; get(ids[index]).change();
        assert.deepEqual(posts.at(-1), ['推理引擎', String(index + 1)]);
        assert.equal(select.value, '中文模型');
        assert.deepEqual(ids.map(id => get(id).checked), ids.map((id, i) => i === index));
    }
    await load([{name: '中文模型', engines: [2]}, {name: '仅TRT', engines: [3]}]); check([2]);
    const previousPosts = posts.length;
    get('NCNN').checked = true; get('NCNN').change();
    assert.equal(posts.length, previousPosts, 'Disabled engine must not send configuration');
    select.value = '仅TRT'; select.change(); check([3]);
    assert.deepEqual(posts.at(-1), ['模型名称', '仅TRT']);
    get('TensorRT').checked = true; get('TensorRT').change();
    assert.deepEqual(posts.at(-1), ['推理引擎', '3']);
    const missingModelPosts = posts.length;
    await load([{name: '新模型', engines: [1]}]);
    assert.deepEqual(names(), ['新模型']); assert.equal(select.value, ''); check([]);
    assert(get('模型列表状态').textContent.includes('重新选择模型'), 'Missing saved model must explain how to enable engines');
    assert(ids.every(id => get(id).parentElement.title === '请先选择模型'));
    assert.equal(posts.length, missingModelPosts, 'Missing saved model must not overwrite configuration');
    await load([]); check([]); assert(select.disabled); assert.deepEqual(names(), []);
    let old = refresh(), oldRequest = pending.shift(), fresh = refresh(), newRequest = pending.shift();
    respond(newRequest, [{name: '最新模型', engines: [3]}]); await fresh;
    respond(oldRequest, [{name: '过时模型', engines: [1]}]); await old;
    assert.deepEqual(names(), ['最新模型']); check([]);
    const failed = refresh(); pending.shift().reject(new Error('offline')); await failed;
    assert(select.disabled); check([]); assert.deepEqual(names(), []);
    await load([{name: '恢复模型', engines: [1, 2]}]);
    select.value = '恢复模型'; select.change(); check([1, 2]);
    await load([{name:'自适应组',engines:[1,2,3],adaptive:true,sizes:{1:[640],2:[320,416,640],3:[416,640]}}]);
    select.value='自适应组'; select.change(); check([1,2,3]);
    assert(select.options.some(option=>option.textContent==='自适应组（自适应）'));
    assert(get('模型列表状态').textContent.includes('自适应截图'));
    await load([{name:'自适应组',engines:[2],adaptive:true,sizes:{1:[],2:[320],3:[]}}]);
    check([2]);
    get('ONNX').checked=true; get('ONNX').change();
    assert(get('模型列表状态').textContent.includes('上限为 320'));
    assert(get('模型列表状态').textContent.includes('只有一档时始终使用该档位'));
    await load([{name:'自适应组',engines:[2,3],adaptive:true,sizes:{1:[],2:[320,416,512],3:[320]}}]);
    check([2,3]); assert(get('模型列表状态').textContent.includes('上限为 512'));
    get('TensorRT').checked=true; get('TensorRT').change();
    assert(get('模型列表状态').textContent.includes('上限为 320'));
    await load([{name:'自适应组',engines:[2],adaptive:true,sizes:{1:[],2:[320],3:[]}}]);
    check([2]); assert(get('模型列表状态').textContent.includes('没有可用尺寸模型'));
    const allSizes=Array.from({length:16},(_,i)=>160+i*32);
    await load([{name:'全档位',engines:[2],adaptive:true,sizes:{1:[],2:allSizes,3:[]}}]);
    select.value='全档位'; select.change(); get('ONNX').checked=true; get('ONNX').change();
    assert(get('模型列表状态').textContent.includes('16 档（'+allSizes.join(' / ')));
    assert(get('模型列表状态').textContent.includes('根据目标距离和大小'));
    assert(get('模型列表状态').textContent.includes('连续 3 帧'));
    vm.runInContext('更新实时截图({"截图范围":192,"自适应截图":true})',context);
    assert.equal(get('模型实时范围').textContent,'当前自适应截图：192 × 192');
    vm.runInContext('更新实时截图({"截图范围":640,"自适应截图":false})',context);
    assert.equal(get('模型实时范围').textContent,'当前截图：640 × 640');
    vm.runInContext('更新实时截图({})',context);
    assert.equal(get('模型实时范围').textContent,'');
    console.log('PASS page syntax, same-name engine switching, gray/disabled states, missing model guidance, unrelated models, deletion, empty directory, refresh races and recovery');
})().catch(error => { console.error(error); process.exitCode = 1; });
