const assert = require('node:assert/strict');
const fs = require('node:fs');
const {JSDOM} = require('../../.cache/backhaul/ui/node_modules/jsdom');
const source = fs.readFileSync('src/websrc/js/functions.js', 'utf8');
const loader = fs.readFileSync('src/websrc/html/PAGE_LOADER.html', 'utf8');
const page = fs.readFileSync('src/websrc/html/PAGE_TOOLS.html', 'utf8');
const dom = new JSDOM(loader + page, {url:'http://czc.test/tools', runScripts:'outside-only'});
const w = dom.window;
w.eval(fs.readFileSync('src/websrc/js/jquery-min.js','utf8'));
const $ = w.jQuery;
assert.equal($('#upload_form_zb').closest('.card').find('#upd_zb_git').length,1,'file and Git share the Zigbee card');
assert.equal($('[data-i18n="p.to.zgu"]').length,0,'no separate Git update card');
const dict = JSON.parse(fs.readFileSync('src/websrc/json/ru.json','utf8'));
w.i18next = {t(key, values={}) {
    let text = key.split('.').reduce((p,k)=>p && p[k],dict);
    if(typeof text!=='string') text=values.defaultValue;
    assert.equal(typeof text,'string',key);
    for(const [k,v] of Object.entries(values)) text = text.replaceAll('{{'+k+'}}',String(v));
    return text;
}};
const consoleErrors=[]; w.console.error=(...args)=>consoleErrors.push(args);
const timeouts=[], intervals=new Map(); let timer=0;
w.setTimeout = fn => { timeouts.push(fn); return ++timer; };
w.setInterval = fn => { intervals.set(++timer,fn); return timer; };
w.clearInterval = id => intervals.delete(id);
function flush() { for(let i=0; timeouts.length; i++) { assert(i<100); timeouts.shift()(); } }
function pulse() { for(const fn of intervals.values()) fn(); }
$.fn.modal = function(action) { this.attr('data-open',action==='show'); if(action==='hide') this.trigger('hidden.bs.modal'); return this; };
w.confirm = () => { throw new Error('The stock popup must provide confirmation'); };
class Events {
    static OPEN=1;
    constructor() { this.readyState=0; this.listeners={}; }
    addEventListener(name,fn) { this.listeners[name]=fn; }
    close() { this.readyState=2; }
    emit(name,data='') { if(name==='open') this.readyState=1; this.listeners[name]({data,target:this}); }
}
w.EventSource=Events;
const requests=[]; let clients=0, preflightError=false, upload, uploadXHR, flashRequest;
$.get = (url,callback) => {
    requests.push(url); const deferred=$.Deferred();
    const cmd=new URL(url,w.location.href).searchParams.get('cmd');
    if(cmd==='7') { flashRequest=deferred; return deferred.promise(); }
    if(preflightError) return deferred.reject({}).promise();
    const result=cmd==='16' ? clients : 'OK';
    if(callback) callback(result);
    return deferred.resolve(result).promise();
};
$.getJSON = url => { assert.equal(url,'/api/backhaul'); return $.Deferred().resolve({token:'popup-token'}).promise(); };
$.ajax = req => {
    assert.equal(req.url,'/updateZB'); assert.equal(req.type,'POST');
    assert.equal(req.headers['X-Backhaul-Token'],'popup-token');
    assert.equal(req.processData,false); assert.equal(req.contentType,false);
    assert.equal(req.data.get('radio').name,'radio.bin');
    assert.equal(req.data.get('fwMode'),'coordinator');
    uploadXHR=req.xhr(); upload=$.Deferred(); return upload.promise();
};
w.eval(source.slice(0,source.indexOf('let intervalIdUpdateRoot')) +
    source.slice(source.indexOf('function closeModal()'),source.indexOf('function showWifiCreds(')) +
    source.slice(source.indexOf('function sub_zb('),source.indexOf('async function fetchReleaseData(')) +
    source.slice(source.indexOf('function backhaulUiInit()')));
Object.defineProperty(w.document.getElementById('file_zb'),'files',{value:[new w.File(['BIN'],'radio.bin')]});
w.sub_zb(w.document.getElementById('file_zb'));
assert.equal($('#file-input_zb').text(),'radio.bin');
assert.equal($('#updButton_zb').prop('disabled'),false);
function openLocal() { upload=null; $('#upload_form_zb').trigger('submit'); flush(); }
function allowFlash() { w.sourceEvents.emit('open'); pulse(); assert.equal($('.modal-footer .btn-warning').prop('disabled'),false); }
function clickFlash() { $('.modal-footer .btn-warning').trigger('click'); flush(); }

openLocal();
assert.equal($('#modal').attr('data-open'),'true');
assert($('.modal-title').text().includes('Zigbee'));
assert($('.modal-body').text().includes('radio.bin'));
assert.equal($('.modal-footer .btn-warning').prop('disabled'),true,'wait for SSE to open');
assert(!upload,'no flash before confirmation');
assert(!requests.some(u=>u.includes('cmd=15')),'local upload does not require DNS');
$('.modal-footer .btn-primary').trigger('click'); assert.equal(intervals.size,0); assert(!upload);

clients=1; preflightError=true; openLocal();
assert.equal($('.modal-footer .btn-warning').length,1,'connected clients do not block flashing');
assert(!requests.some(u=>u.includes('cmd=16')),'no client-count preflight');
preflightError=false; allowFlash(); clickFlash(); assert(upload);
assert.equal($('#zbFlshPrgs').closest('.modal-body').length,1);
uploadXHR.upload.dispatchEvent(new w.ProgressEvent('progress',{lengthComputable:true,loaded:50,total:100}));
assert.equal($('#zbFlshPrgs')[0].style.width,'50%'); assert($('#zbFlshPgsTxt').text().includes('Загрузка файла'));
w.sourceEvents.emit('zb.fi','startFlash'); w.sourceEvents.emit('zb.fp','25');
assert.equal($('#zbFlshPrgs')[0].style.width,'25%'); assert($('#zbFlshPgsTxt').text().includes('Запись прошивки'));
uploadXHR.upload.dispatchEvent(new w.ProgressEvent('progress',{lengthComputable:true,loaded:100,total:100}));
assert.equal($('#zbFlshPrgs')[0].style.width,'25%','late upload progress must not replace flash progress');
w.sourceEvents.emit('zb.fi','finishFlash'); assert.equal($('.modal-footer .btn-primary').length,0,'wait for HTTP result');
upload.resolve({result:'radio_updated'}); flush();
assert.equal($('#zbFlshPgsTxt').text(),dict.md.zg.fu.fn); assert.equal($('.modal-footer button').length,1);
assert.equal($('#prg_zb').length,0,'no duplicate inline progress');

openLocal(); allowFlash(); clickFlash(); w.sourceEvents.emit('zb.fi','finishFlash');
upload.reject({status:400,responseJSON:{result:'incomplete_upload',stage:'upload',expected_bytes:4100,received_bytes:4100,stored_bytes:4096,erase_started:false,psk:'must-not-be-logged'}}); flush();
assert($('.modal-body').text().includes(dict.md.zg.errors.incomplete_upload)); assert.equal($('.modal-footer button').length,1);
const diagnostic=JSON.parse($('.modal-body pre').text());
assert.equal(diagnostic.stored_bytes,4096); assert.equal(diagnostic.http_status,400); assert.equal(diagnostic.erase_started,false);
assert(!JSON.stringify(consoleErrors).includes('must-not-be-logged'));
assert(!$('.modal-body').text().includes('must-not-be-logged'));
w.sourceEvents.emit('zb.fp','100'); w.sourceEvents.emit('zb.fi','finishFlash'); assert($('.modal-body').text().includes(dict.md.zg.errors.incomplete_upload));
openLocal(); allowFlash(); clickFlash(); w.sourceEvents.emit('zb.fi','verifyFlash');
assert.equal($('#zbFlshPgsTxt').text(),dict.md.zg.fu.verify);
upload.reject({status:400,responseJSON:{result:'radio_verify_failed',stage:'verify',bsl_error:'bsl_crc_mismatch',bsl_status:64,written_bytes:720896}}); flush();
assert.equal(JSON.parse($('.modal-body pre').text()).bsl_error,'bsl_crc_mismatch');
assert($('.modal-body').text().includes(dict.md.zg.errors.radio_verify_failed));
openLocal(); allowFlash(); clickFlash(); upload.resolve({result:'flash_failed'}); flush(); assert($('.modal-body').text().includes(dict.md.zg.errors.flash_failed));
openLocal(); allowFlash(); clickFlash(); upload.reject({status:0}); flush(); assert($('.modal-body').text().includes(dict.md.zg.errors.network_error));

const url='https://firmware.example/radio.bin?b=115200&release=stable';
w.modalConstructor('flashZBM'); w.startZbFlash(url,'coordinator'); flush(); allowFlash(); clickFlash();
const args=new URL(requests.at(-1),w.location.href).searchParams;
assert.equal(args.get('cmd'),'7'); assert.equal(args.get('url'),url); assert.equal(args.get('fwMode'),'coordinator');
assert(requests.some(u=>u.includes('cmd=15')));
w.sourceEvents.emit('zb.dw','40'); assert.equal($('#zbFlshPrgs')[0].style.width,'40%');
w.sourceEvents.emit('zb.fi','finishFlash'); assert.equal($('#zbFlshPgsTxt').text(),dict.md.zg.fu.fn);
flashRequest.reject({}); assert.equal($('.modal-body').text(),dict.p.bh.flashFailed);
for(const lang of ['en','ru']) {
    const title=JSON.parse(fs.readFileSync('src/websrc/json/'+lang+'.json','utf8')).p.to.zu;
    assert(!/develop|разработке/.test(title));
}
console.log('PASS Zigbee popup: local file/URL, confirmation/cancel, no client-count gate, upload + SSE progress, final HTTP result, specific safe error details, translations');
dom.window.close();
