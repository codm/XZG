const assert = require('node:assert/strict');
const fs = require('node:fs');
const {JSDOM} = require('../../.cache/backhaul/ui/node_modules/jsdom');
const html=fs.readFileSync('src/websrc/html/PAGE_NETWORK.html','utf8');
const source=fs.readFileSync('src/websrc/js/functions.js','utf8');
const dictionaries=['en','ru'].map(lang=>JSON.parse(fs.readFileSync(`src/websrc/json/${lang}.json`,'utf8')));
for(const d of dictionaries) for(const m of html.matchAll(/data-i18n="(p\.ne\.live\.[^"]+)"/g))
    assert.equal(typeof m[1].split('.').reduce((p,k)=>p[k],d),'string',m[1]);
const dom=new JSDOM(html,{url:'http://czc.test/network',runScripts:'outside-only'}), w=dom.window;
w.eval(fs.readFileSync('src/websrc/js/jquery-min.js','utf8'));
const $=w.jQuery;
w.i18next={t:key=>key.split('.').reduce((p,k)=>p && p[k],dictionaries[1]) || key};
let tick,requests=0,fail=false;
w.setTimeout=fn=>{tick=fn;return 1;}; w.clearTimeout=()=>{};
let data={interfaces:[
    {id:'eth',enabled:true,connected:true,ipv4:'192.168.8.144',ipv6:[{address:'fe80::1234',link_local:true},{address:'fd12::144',link_local:false},{address:'2001:db8::144',link_local:false}]},
    {id:'wifi',enabled:false,connected:false,ipv4:'',ipv6:[]},
    {id:'ap',enabled:true,connected:true,ipv4:'192.168.1.1',ipv6:[]}
]};
$.getJSON=url=>{
    assert.equal(url,'/api/network/status','network page does not request peer settings or PSK'); ++requests;
    return fail ? $.Deferred().reject({}).promise() : $.Deferred().resolve(data).promise();
};
w.eval(source.slice(source.indexOf('function networkStatusUiInit()'),source.indexOf('function backhaulUiInit()')));
$('#ethIp').val('192.168.8.100'); $('#ethDhcp').prop('checked',true);
w.networkStatusUiInit();
assert.equal($('#networkAddresses tr').length,3);
const eth=$('[data-interface="eth"]');
assert(eth.text().includes('192.168.8.144'));
for(const ip of data.interfaces[0].ipv6) assert(eth.text().includes(ip.address));
assert.equal(eth.find('small').length,1,'only link-local carries a scope label');
assert($('[data-interface="ap"]').text().includes('192.168.1.1'));
assert($('[data-interface="wifi"]').text().includes('Выключен'));
assert(!$('#networkStatusCard').closest('form').length,'current addresses are not editable config fields');
data.interfaces[0].ipv4='192.168.8.145'; data.interfaces[0].ipv6=[];
tick();
assert($('[data-interface="eth"]').text().includes('192.168.8.145'));
assert($('[data-interface="eth"]').text().includes('Адрес не получен'));
assert(!$('#networkAddresses').text().includes('fd12::144'));
assert.equal($('#ethIp').val(),'192.168.8.100','refresh does not overwrite form settings');
fail=true; $('#networkStatusRefresh').trigger('click');
assert.equal($('#networkAddresses tr').length,0,'failed requests do not present stale addresses as current');
assert(!$('#networkStatusError').hasClass('d-none'));
fail=false; $('#networkStatusRefresh').trigger('click');
assert($('#networkStatusError').hasClass('d-none'));
assert.equal($('#networkAddresses tr').length,3);
const before=requests; $('#networkStatusCard').remove(); tick();
assert.equal(requests,before,'polling stops after navigation');
console.log('PASS Network UI: independent live IPv4/IPv6, DHCP versus saved fields, all IPv6 addresses, AP, refresh, errors and navigation');
w.close();
