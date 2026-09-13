const assert = require('node:assert/strict');
const fs = require('node:fs');
const {JSDOM} = require('../../.cache/backhaul/ui/node_modules/jsdom');
const html = fs.readFileSync('src/websrc/html/PAGE_ZIGBEE.html', 'utf8');
const tools = fs.readFileSync('src/websrc/html/PAGE_TOOLS.html', 'utf8');
const source = fs.readFileSync('src/websrc/js/functions.js', 'utf8');
const dictionaries = fs.readdirSync('src/websrc/json').map(name=>name.replace('.json','')).sort().map(lang => JSON.parse(fs.readFileSync(`src/websrc/json/${lang}.json`, 'utf8')));
function leaves(value,prefix='') {
    return Object.entries(value).flatMap(([k,v])=>typeof v==='object' ? leaves(v,prefix+k+'.') : [prefix+k]);
}
const english=JSON.parse(fs.readFileSync('src/websrc/json/en.json','utf8'));
for(const d of dictionaries) {
    assert.deepEqual(leaves(d.p.bh).sort(),leaves(english.p.bh).sort(),'all node labels, errors and states must be localized');
    for(const key of leaves(d.p.bh)) assert(key.split('.').reduce((p,k)=>p[k],d.p.bh).trim());
}
for(const d of dictionaries) for(const match of html.matchAll(/data-i18n="(p\.bh\.[^"]+)"/g)) {
    assert.equal(typeof match[1].split('.').reduce((p,k)=>p[k],d),'string',match[1]);
}
const dom = new JSDOM('<!doctype html><html><body>'+html+tools+'</body></html>', {url:'http://czc.test/zigbee',runScripts:'outside-only'});
const w = dom.window;
w.eval(fs.readFileSync('src/websrc/js/jquery-min.js','utf8'));
const $ = w.jQuery;
const russian=JSON.parse(fs.readFileSync('src/websrc/json/ru.json','utf8'));
w.i18next = {t: key => key.split('.').reduce((p,k)=>p && p[k],russian) || key};
w.setTimeout = () => 1; w.confirm = () => true;
let copied = false; w.document.execCommand = cmd => { copied = cmd === 'copy'; return copied; };
const storedKey = '12'.repeat(32), generatedKey = '34'.repeat(32);
let config = {token:'test-session', mode:2, peer_host:'192.168.8.144', peer_port:7443, admin_port:7444, peer_ieee:'0x00124b002e11424d',
    ipv6:true, raw_tcp:true, debug_mode:false, key_set:true, psk:storedKey, status:{own_ieee:'0x00124b0000000001',peer:true,fault:'none',af:6,ip6:'fd12::145',join:'idle'}};
config.radio_role=1; config.role_supported=true;
const calls=[];
const debugLogs=[]; w.console.info=(...args)=>debugLogs.push(args);
$.getJSON = url => { assert.equal(url,'/api/backhaul'); return $.Deferred().resolve(JSON.parse(JSON.stringify(config))).promise(); };
$.ajax = req => {
    calls.push(req);
    assert.equal(req.type,'POST'); assert.equal(req.headers['X-Backhaul-Token'],config.token);
    let response = {};
    if(req.url === '/api/backhaul/key') response.psk = generatedKey;
    else if(req.url === '/api/backhaul') response.result = 'saved_restarting';
    else if(req.url === '/api/backhaul/join') response.result = 'joining';
    else if(req.url === '/updateZB') { assert(req.data instanceof w.FormData); response.result = 'radio_updated'; }
    else throw new Error(req.url);
    return $.Deferred().resolve(response).promise();
};
w.eval(source.slice(source.indexOf('function backhaulUiInit()')));
assert($('#backhaulSection').hasClass('d-none'));
w.backhaulUiInit();
assert(!$('#backhaulSection').hasClass('d-none'));
for (const role of ['router','thread']) {
    $('.zfs_'+role).trigger('click');
    assert($('#backhaulSection').hasClass('d-none'));
    assert($('#bhMode').prop('disabled'));
    const count=calls.length; $('#backhaulConfig').trigger('submit'); assert.equal(calls.length,count);
}
$('.zfs_coordinator').trigger('click');
assert(!$('#backhaulSection').hasClass('d-none')); assert(!$('#bhMode').prop('disabled'));
config.role_supported=false; config.radio_role=3; $('#bhRefresh').trigger('click');
assert($('#backhaulSection').hasClass('d-none'));
config.role_supported=true; config.radio_role=1; $('#bhRefresh').trigger('click');
assert(!$('#backhaulSection').hasClass('d-none'));

assert.equal($('#ccModeSwitchWrapper #backhaulConfig').length,1,'inside existing Role card');
assert.equal($('.selectable-card').length,3,'existing Coordinator/Router/OpenThread choices preserved');
assert.equal($('#bhKey').val(),storedKey,'saved PSK returned into form');
assert.equal($('#bhDebug').length,0,'debug is a build choice, never a UI setting');
assert.equal($('#bhAdminPort').length,0,'administrative port is not configurable in UI');
assert.equal($('#bhKey').attr('type'),'password');
$('#bhShowKey').trigger('click'); assert.equal($('#bhKey').attr('type'),'text');
$('#bhShowKey').trigger('click'); assert.equal($('#bhKey').attr('type'),'password');
$('#bhCopyKey').trigger('click'); assert(copied); assert.equal($('#bhKey').attr('type'),'password');
assert($('#bhStatus').text().includes('TLS / IPv6'));
assert($('#bhStatus').text().includes('Подключено к Master'));
assert(!$('#bhStatus').text().includes('IPv4:'));
config.mode=0; config.status={fault:'unconfigured'};
$('#bhRefresh').trigger('click');
assert.equal($('#bhStatus').text(),'Обычный режим CZC');
config.mode=1; config.status={fault:'awaiting_peer'};
$('#bhRefresh').trigger('click');
assert.equal($('#bhStatus').text(),'Ожидание Satellite');
config.status={peer:true,peers_online:8,peer_limit:8,fault:'none'};
$('#bhRefresh').trigger('click');
assert.equal($('#bhStatus').text(),'Подключено Satellite: 8/8');
config.status.peers=[
    {ieee:'00124b003cb89484',ip:'192.168.8.186',online:true},
    {ieee:'00124b003cb89484',ip:'192.168.8.185',online:false,fault:'peer_disconnected'},
    {ieee:'00124b0000000002',ip:'fd12::187',online:true}
];
$('#bhRefresh').trigger('click');
assert.equal($('#bhPeers li').length,2);
assert.equal($('#bhPeers li').first().text(),'00:12:4B:00:3C:B8:94:84 · 192.168.8.186 · Соединено');
assert($('#bhPeers').text().includes('fd12::187'));
config.status.peers.reverse(); $('#bhRefresh').trigger('click');
assert.equal($('#bhPeers li').length,2);
assert(!$('#bhPeers').text().includes('192.168.8.185'));
config.status.peers=[{ieee:'00124b003cb89484',ip:'192.168.8.186',online:false,fault:'peer_disconnected'}];
$('#bhRefresh').trigger('click');assert.equal($('#bhPeers li').length,1);
assert($('#bhPeers').text().includes('Узел отключён'));
config.mode=2; config.status={fault:'radio_revision',radio_revision:20260913,radio_required:20260917};
$('#bhRefresh').trigger('click');
assert($('#bhStatus').text().includes('20260917'));
assert($('#bhStatus').text().includes('20260913'));
assert(!$('#bhStatus').text().includes('Нет соединения'));
assert.equal(debugLogs.length,0,'normal mode does not log browser diagnostics');
config.debug_mode=true;
$('#bhRefresh').trigger('click'); $('#bhRefresh').trigger('click');
assert.equal(debugLogs.length,1,'diagnostics log state changes, not every poll');
assert.equal(debugLogs[0][1].fault,'radio_revision');
assert(!JSON.stringify(debugLogs).includes(storedKey));
delete config.status.radio_required; $('#bhRefresh').trigger('click');
assert($('#bhStatus').text().endsWith('—'),'missing required revision is unknown');
assert.equal(debugLogs.at(-1)[1].required,null);
config.status={}; $('#bhRefresh').trigger('click');
assert($('#bhStatus').text().includes('Не удалось прочитать'));
assert(!$('#bhStatus').text().includes('Подключение к Master'),'missing status is not a pending connection');
config.status={fault:'master_network'}; $('#bhRefresh').trigger('click');
assert.equal($('#bhStatus').text(),russian.p.bh.faults.master_network);
$('#bhHost').val('unsaved.local');
$('#bhRefresh').trigger('click');
assert.equal($('#bhHost').val(),'unsaved.local','status refresh preserves unsaved fields');
assert.equal($('[data-i18n="p.bh.requirements"]').length,0);
assert(!$('#bhJoin').hasClass('d-none'));
$('#bhJoin').trigger('click'); assert.equal(calls.at(-1).url,'/api/backhaul/join');
$('#bhGenerateKey').trigger('click'); assert.equal($('#bhKey').val(),generatedKey);
$('#bhMode').val('1').trigger('change'); assert.equal($('#bhHost').prop('required'),false);
$('#bhMode').val('2').trigger('change'); assert.equal($('#bhHost').prop('required'),true);
$('#bhHost').val('fd12::144'); $('#bhPort').val('8743');
$('#backhaulConfig').trigger('submit');
const save = JSON.parse(calls.at(-1).data);
assert.equal(save.psk,generatedKey); assert.equal(save.peer_host,'fd12::144');
assert.equal(save.peer_port,8743); assert.equal(save.mode,2);
assert(!('ipv6' in save)); assert(!('raw_tcp' in save));
assert.equal($('#bhIpv6').length,0); assert.equal($('#bhRaw').length,0);
assert(!('debug_mode' in save)); assert(!('admin_port' in save));
assert(!$('#bhMessage').text().includes(generatedKey));
$('#bhMode').val('0').trigger('change'); assert($('#bhFields').hasClass('d-none'));
assert.equal($('#bhPeer').length,0,'IEEE is discovered automatically');
assert(!('peer_ieee' in save));
assert.equal($('#upload_form_zb').closest('[hidden]').length,0,'local radio uploader visible');
assert.equal($('#file_zb').attr('accept'),'.bin');
assert.equal($('#esp_upload_form').length,1,'stock ESP32 uploader remains');
console.log('PASS Role UI: saved PSK, show/hide/copy/generate, modes, IPv6/ports, save payload, join, existing roles and both updaters');
dom.window.close();
