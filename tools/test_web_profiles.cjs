// Run with jsdom on NODE_PATH; exercises the complete production page and handlers.
const {JSDOM, VirtualConsole} = require('jsdom');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const html = fs.readFileSync(path.join(__dirname, '../web/圣人视觉识别系统.html'), 'utf8');
const fixture = {
    目前阵容: 0, 置信度: .55, 准星精度: .2, 交并比: .25, 锁定范围: 256, 中心点范围: 50,
    锁定频率: 100, 延时频率: 200, 移动延时: 5, 自瞄主键: 2, 自瞄副键: 0, 自瞄副键2: 0, 自瞄副键3: 0,
    优先锁定: 1, 其次锁定: 2, 再次锁定: 3, 优先锁定位置: 50, 其次锁定位置: 50, 再次锁定位置: 50,
    准星类别: 0, 移动系数: .618, 开镜时间: 350, 准星扳机: false, 中心扳机: false, 扳机范围: .66,
    开枪间隔: 80, 框内移动: false, 线程移动: false, 自动截图: false, 吸附常驻: false,
    自瞄连续按下: true, 移动方式: 1, Kp: .1, Ki: .01, 模型名称: '测试模型', 推理引擎: 1
};
for (let i = 1; i <= 4; ++i) { fixture['准星绑定' + i] = 0; fixture['扳机热键' + i] = 0; }
for (let i = 0; i < 10; ++i) for (const prefix of ['type', 'team1type', 'team2type']) fixture[prefix + i] = false;
async function open(config) {
    const posts = [], errors = [];
    const virtualConsole = new VirtualConsole();
    virtualConsole.on('jsdomError', e => { if (e.type !== 'css-parsing') errors.push(e); });
    const dom = new JSDOM(html, {url: 'http://localhost:18889/', runScripts: 'dangerously', virtualConsole,
        beforeParse(window) {
            Object.assign(window, config);
            window.fetch = async (url, options) => {
                if (options?.method === 'POST' && !options.body.startsWith('获取所有配置名|')) posts.push(options.body);
                return {ok: true, json: async () => String(url).includes('model-catalog')
                    ? {models: [{name: '测试模型', engines: [1]}]} : {}};
            };
        }});
    await new Promise(resolve => dom.window.addEventListener('load', resolve));
    assert.deepEqual(errors, [], 'Page initialization errors');
    const get = id => dom.window.document.getElementById(id);
    const switchTo = n => get('hotkey-tab-' + n).click();
    const edit = (id, value, event = 'change') => {
        if (get(id).type === 'checkbox') get(id).checked = value;
        else get(id).value = value;
        get(id).dispatchEvent(new dom.window.Event(event, {bubbles: true}));
    };
    return {dom, posts, get, switchTo, edit};
}
(async () => {
    let page = await open(fixture);
    const {dom, posts, get, switchTo, edit} = page;
    assert.equal(posts.length, 0, 'Opening the editor must not write settings');
    const rangeOptions = [...get('测距按下').options];
    assert.equal(get('测距按下').value, '0', 'Old config defaults to blank');
    assert.equal(rangeOptions.length, 105, 'Blank plus 104 CAT keyboard keys');
    assert.ok(rangeOptions.every(o => o.value === '0' || (+o.value > 1000 && +o.value <= 1127)), 'Keyboard only');
    edit('测距按下', '1057');
    assert.ok(posts.pop().includes('测距按下|1057'), 'Selected keyboard key sent to server');
    const ids = [...dom.window.document.querySelectorAll('[id]')].map(el => el.id);
    assert.equal(new Set(ids).size, ids.length, 'Duplicate DOM IDs');
    assert.equal(get('panel-target').querySelectorAll('[role="tab"]').length, 0, 'Target section must not have its own tabs');
    const keys = ['自瞄主键', '自瞄副键', '自瞄副键2', '自瞄副键3'];
    for (let group = 1; group <= 3; ++group) {
        const before = posts.length;
        switchTo(group);
        assert.equal(posts.length, before, 'Switching the editor must not activate or overwrite a runtime profile');
        assert.equal(get('heading-target').textContent, '目标锁定' + group);
        assert.equal(get('always-on-row').hidden, group !== 1);
        assert.equal(get('hotkey-profile-content').querySelectorAll('select').length, 4);
        const prefix = group === 1 ? '' : '热键' + group + '_';
        keys.forEach((key, slot) => { edit(key, [1, 2, 5, 6][slot]); assert.equal(posts.at(-1), prefix + key + '|' + [1, 2, 5, 6][slot]); });
        edit('优先锁定', group + 3);
        assert.equal(posts.at(-1), prefix + '优先锁定|' + (group + 3));
        get('优先锁定').closest('.control-row').querySelector('button').click();
        assert.equal(posts.at(-1), prefix + '优先锁定|' + (group + 4));
        edit('优先锁定位置精调', group * 20);
        assert.equal(posts.at(-1), prefix + '优先锁定位置|' + group * 20);
        edit('锁定范围', group * 100, 'input');
        edit('中心点范围精调', group * 10);
        edit('框内移动', group === 2);
    }
    const expectedPosts = posts.length;
    for (let group = 1; group <= 3; ++group) {
        switchTo(group);
        assert.equal(get('优先锁定').value, String(group + 4));
        assert.equal(get('优先锁定位置精调').value, String(group * 20));
        assert.equal(get('优先锁定位置显示').textContent, String(group * 20));
        assert.equal(get('锁定范围精调').value, String(group * 100));
        assert.equal(get('中心点范围').value, String(group * 10));
        assert.equal(get('框内移动').checked, group === 2);
    }
    assert.equal(posts.length, expectedPosts);
    get('hotkey-tab-3').dispatchEvent(new dom.window.KeyboardEvent('keydown', {key: 'ArrowRight', bubbles: true}));
    assert.equal(get('heading-target').textContent, '目标锁定1');
    assert.equal(dom.window.document.activeElement.id, 'hotkey-tab-1');
    edit('吸附常驻', true);
    switchTo(2); switchTo(1);
    assert(get('吸附常驻').checked);
    const saved = {...fixture, 测距按下: 1057};
    for (const post of posts) { const [key, value] = post.split('|'); saved[key] = value === 'true' ? true : value === 'false' ? false : Number(value); }
    dom.window.close();
    page = await open(saved);
    assert.equal(page.get('测距按下').value, '1057', 'Saved CAT keyboard selection restored');
    for (let group = 1; group <= 3; ++group) {
        page.switchTo(group);
        assert.equal(page.get('优先锁定').value, String(group + 4));
        assert.equal(page.get('优先锁定位置精调').value, String(group * 20));
    }
    assert.equal(page.posts.length, 0, 'Reloading persisted profiles must be read-only');
    page.dom.window.close();
    console.log('PASS full-page initialization, 3 linked pages, independent keys/targets, fine controls, persistence, keyboard tabs and no writes on navigation');
})().catch(error => {console.error(error); process.exit(1);});
