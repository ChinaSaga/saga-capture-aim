const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const page = fs.readFileSync(path.join(__dirname, '../web/圣人视觉识别系统.html'), 'utf8');
for (const match of page.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/gi)) new vm.Script(match[1]);
const elements = new Map();
for (const name of ['模型名称', '模型列表状态', '配置读写状态', 'NCNN', 'ONNX', 'TensorRT']) {
    elements.set(name, { value: '', textContent: '', disabled: false,
        replaceChildren(...children) { this.children = children; this.value = children[0]?.value || ''; },
        parentElement: { classList: { toggle() {} } } });
}
let response;
const context = vm.createContext({
    模型名称: '三角洲416', 推理引擎: 2,
    document: { getElementById: id => elements.get(id), createElement: () => ({}) },
    fetch: async () => response
});
const start = page.indexOf('let 模型列表请求序号 =');
const end = page.indexOf('window.onload =', start);
assert.ok(start > 0 && end > start);
vm.runInContext(page.slice(start, end), context);
const refresh = () => vm.runInContext('刷新模型列表()', context);
(async () => {
    response = { ok: false, status: 500, json: async () => ({ error: '拒绝访问 (5)', directory: 'C:\\Users\\用户😀\\Desktop\\模型数据' }) };
    await refresh();
    assert.match(elements.get('模型列表状态').textContent, /拒绝访问.*用户😀/);
    assert.equal(elements.get('模型名称').disabled, true);
    response = { ok: true, json: async () => ({ models: [{ name: '三角洲416', engines: [2] }], directory: 'D:\\桌面软件\\模型数据' }) };
    await refresh();
    assert.equal(elements.get('模型名称').value, '三角洲416');
    assert.equal(elements.get('ONNX').disabled, false);
    assert.equal(elements.get('NCNN').disabled, true);
    assert.match(elements.get('模型列表状态').textContent, /找到 1 个模型.*D:\\桌面软件/);
    response = { ok: true, json: async () => ({ models: [], directory: 'D:\\空文件夹' }) };
    await refresh();
    assert.match(elements.get('模型列表状态').textContent, /没有可用模型.*空文件夹/);
    response = { ok: false, status: 404, json: async () => { throw new Error('HTML response'); } };
    await refresh();
    assert.match(elements.get('模型列表状态').textContent, /HTTP 404/);
    const configStart = page.indexOf('function 显示配置错误(');
    vm.runInContext(page.slice(configStart, page.indexOf('function 发送消息给服务器(',configStart)), context);
    await assert.rejects(vm.runInContext('检查配置响应({ok:false,json:async()=>({error:"只读配置"})})',context), /只读配置/);
    vm.runInContext('显示配置错误(new Error("只读配置"))',context);
    assert.match(elements.get('配置读写状态').textContent,/只读配置/);
    await vm.runInContext('检查配置响应({ok:true,json:async()=>({})})',context);
    assert.equal(elements.get('配置读写状态').textContent,'');
    console.log('Model UI passed: syntax, permission error/path, retry recovery, engine availability, empty directory and non-JSON error.');
})().catch(error => { console.error(error); process.exitCode = 1; });
