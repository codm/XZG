const assert = require('node:assert/strict');
const fs = require('node:fs');
const {JSDOM} = require('../../.cache/backhaul/ui/node_modules/jsdom');
const source = fs.readFileSync('src/websrc/js/functions.js', 'utf8');
const dom = new JSDOM(fs.readFileSync('src/websrc/html/PAGE_LOADER.html', 'utf8') +
    fs.readFileSync('src/websrc/html/PAGE_TOOLS.html', 'utf8'), {url:'http://czc.test/tools', runScripts:'outside-only'});
const w = dom.window;
w.eval(fs.readFileSync('src/websrc/js/jquery-min.js','utf8'));
const $ = w.jQuery, dict = JSON.parse(fs.readFileSync('src/websrc/json/ru.json','utf8'));
w.i18next = {t(key, values={}) {
    let text=key.split('.').reduce((p,k)=>p && p[k],dict);
    if(typeof text!=='string') text=values.defaultValue;
    assert.equal(typeof text,'string',key);
    for(const [k,v] of Object.entries(values)) text=text.replaceAll('{{'+k+'}}',String(v));
    return text;
}};
let now=0, nextTimer=0;
const timers=new Map();
w.Date.now=()=>now;
w.setTimeout=(fn,ms=0)=>{const id=++nextTimer; timers.set(id,{fn,at:now+ms}); return id;};
w.clearTimeout=id=>timers.delete(id);
w.setInterval=()=>{throw new Error('ESP update must not use unbounded interval polling');};
function advance(ms) {
    const until=now+ms;
    for(let n=0;;++n) {
        const next=[...timers].sort((a,b)=>a[1].at-b[1].at)[0];
        if(!next || next[1].at>until) break;
        assert(n<1000); now=next[1].at; timers.delete(next[0]); next[1].fn();
    }
    now=until;
}
$.fn.modal=function(action) { this.attr('data-open',action==='show'); if(action==='hide') this.trigger('hidden.bs.modal'); return this; };
class Events {
    static OPEN=1;
    constructor() { this.readyState=1; this.listeners={}; }
    addEventListener(name,fn) { this.listeners[name]=fn; }
    close() { this.readyState=2; }
    emit(name,data='') { this.listeners[name]({data,target:this}); }
}
w.EventSource=Events;
let alerts=0, reloads=0, requests=[], uploads=[], states=[], defaultState={boot:'old',state:'idle'}, rootPending;
w.alert=()=>++alerts;
$.get=()=>$.Deferred().resolve('OK').promise();
$.ajax=req=>{
    requests.push(req); const d=$.Deferred();
    if(req.url==='/api/esp-update') {
        assert.equal(req.cache,false); assert(req.timeout>0 && req.timeout<=4000);
        const state=states.length ? states.shift() : defaultState;
        if(state && state.errorStatus!==undefined) d.reject({status:state.errorStatus}); else d.resolve(state);
    } else if(req.url==='/') rootPending=d;
    else uploads.push({req,d});
    return d.promise();
};
w.eval(source.slice(0,source.indexOf('let intervalIdUpdateRoot')) +
    source.slice(source.indexOf('function closeModal()'),source.indexOf('function showWifiCreds(')));
w.espUpdateReload=()=>++reloads;
const file=new w.File([new Uint8Array(1261616)],'app.ota.bin');
function form() { const data=new w.FormData(); data.append('update',file); return data; }
function reset() {
    timers.clear(); w.espUpdateCurrent=null;
    now=0; alerts=reloads=0; requests=[]; uploads=[]; states=[]; defaultState={boot:'old',state:'idle'}; rootPending=null;
    w.connectEvents();
}
function start(params=form()) {
    w.modalConstructor('flashESP',params);
    assert.equal($('.modal-footer .btn-warning').length,1);
    $('.modal-footer .btn-warning').trigger('click');
    return uploads.at(-1);
}
function statusCount() { return requests.filter(r=>r.url==='/api/esp-update').length; }
function failed(key) { assert.equal($('#bar').text(),dict.md.esp.fu.errors[key]); assert.equal($('.modal-footer button').length,1); assert.equal(reloads,0); }
reset(); let upload=start();
assert.equal(requests[0].url,'/api/esp-update','get boot identity before upload');
assert.equal(upload.req.url,'/update?size=1261616');
assert.equal(upload.req.type,'POST'); assert.equal(upload.req.dataType,'json');
assert.equal(upload.req.contentType,false); assert.equal(upload.req.processData,false);
w.sourceEvents.emit('esp.fp','96.25'); assert.equal($('#prg')[0].style.width,'96.25%');
upload.d.resolve({result:'esp_updated'}); // No final SSE packet at all.
assert.equal($('#bar').text(),dict.md.esp.fu.ucr); assert.equal($('#prg')[0].style.width,'100%');
advance(4000); assert.equal(reloads,0,'old boot returning HTTP 200 is not a restart');
defaultState={boot:'new',state:'idle'}; advance(1500); assert.equal(reloads,1);
advance(100000); assert.equal(reloads,1);

// Connection disappears during reboot and the final HTTP response is lost too.
reset(); upload=start(); upload.d.reject({status:0});
assert.equal($('#bar').text(),dict.md.esp.fu.checking);
defaultState={errorStatus:0}; advance(5000); assert.equal(reloads,0);
defaultState={boot:'new',state:'idle'}; advance(1500); assert.equal(reloads,1);

// SSE lifecycle can initiate polling, but does not itself assert success.
reset(); upload=start(); w.sourceEvents.emit('esp.fi','restarting');
assert.equal($('#bar').text(),dict.md.esp.fu.checking);
defaultState={boot:'old',state:'restarting'}; advance(1000);
assert.equal($('#bar').text(),dict.md.esp.fu.ucr); assert.equal(reloads,0);
// Late progress must not replace the restart message.
w.sourceEvents.emit('esp.fp','96.25'); assert.equal($('#bar').text(),dict.md.esp.fu.ucr);
defaultState={boot:'new',state:'idle'}; advance(1500); assert.equal(reloads,1);
upload.d.reject({status:400}); assert.equal(reloads,1);

// Firmware validation fails even though all file bytes were sent.
reset(); upload=start(); w.sourceEvents.emit('esp.fp','100');
assert.equal($('#prg')[0].style.width,'99%'); assert.notEqual($('#bar').text(),dict.md.esp.fu.ucr);
upload.d.reject({status:400,responseJSON:{result:'ota_incomplete_or_invalid'}}); failed('ota_incomplete_or_invalid');
w.sourceEvents.emit('esp.fp','100'); w.sourceEvents.emit('esp.fi','restarting'); advance(100000);
failed('ota_incomplete_or_invalid'); assert.equal(statusCount(),1,'failed upload must not poll for success');
reset(); upload=start(); upload.d.resolve({result:'ota_write_failed'}); failed('ota_write_failed');
reset(); upload=start(); upload.d.resolve(null); failed('invalid_response');
reset(); upload=start(); upload.d.reject({status:0});
defaultState={boot:'old',state:'failed',error:'upload_aborted'}; advance(1000); failed('upload_aborted');
reset(); states=[{errorStatus:0}]; start(); assert.equal(uploads.length,0); failed('status_unavailable');

reset(); states=[null]; start(); assert.equal(uploads.length,0); failed('invalid_response');
reset(); upload=start(); upload.d.resolve({result:'esp_updated'}); defaultState=null; advance(4000); assert.equal(reloads,0);
defaultState={boot:'new',state:'idle'}; advance(1500); assert.equal(reloads,1);

// No infinite spinner or old delayed callback overwriting a later attempt.
reset(); upload=start(); upload.d.reject({status:0}); defaultState={errorStatus:0};
advance(92000); failed('reconnect_timeout'); const count=statusCount(); advance(100000); assert.equal(statusCount(),count);
defaultState={boot:'old',state:'idle'}; const retry=start();
upload.d.resolve({result:'esp_updated'}); assert.notEqual($('#bar').text(),dict.md.esp.fu.ucr);
retry.d.resolve({result:'esp_updated'}); defaultState={boot:'new',state:'idle'}; advance(1000); assert.equal(reloads,1);

// Rebooting SSE connection is retried slowly without the old 3-second alert.
reset(); upload=start(); upload.d.resolve({result:'esp_updated'}); defaultState={errorStatus:0};
const before=requests.length;
for(let i=0;i<20;i++) { w.sourceEvents.readyState=2; w.sourceEvents.emit('error'); advance(1500); }
assert.equal(alerts,0); assert.equal(reloads,0); assert(requests.length-before<25);
defaultState={boot:'new',state:'idle'}; advance(2000); assert.equal(reloads,1);

// Stock GitHub updater and custom URLs use the same completion path.
const url='https://firmware.test/releases/download/2.1.0/app.bin?release=stable&x=1';
reset(); upload=start({link:url,ver:'2.1.0'});
assert.equal(new URL(upload.req.url,w.location.href).searchParams.get('url'),url);
assert.equal(upload.req.dataType,'text'); upload.d.resolve('esp_updated');
defaultState={boot:'new',state:'idle'}; advance(1000); assert.equal(reloads,1);
reset(); upload=start({}); assert(!new URL(upload.req.url,w.location.href).searchParams.has('url'));
upload.d.resolve('download_failed'); failed('download_failed');
reset(); upload=start({link:url}); upload.d.resolve('esp_updated');
defaultState={errorStatus:404}; advance(1000); assert(rootPending); assert.equal(reloads,0);
rootPending.resolve('<html>Stock CZC</html>'); assert.equal(reloads,1); advance(10000); assert.equal(reloads,1);
for(const lang of ['en','ru']) {
    const fu=JSON.parse(fs.readFileSync('src/websrc/json/'+lang+'.json','utf8')).md.esp.fu;
    for(const key of Object.keys(dict.md.esp.fu.errors)) assert.equal(typeof fu.errors[key],'string');
}
console.log('PASS ESP popup: 96.25% + lost SSE/HTTP, old/new boot distinction, exact BIN size, validation/error/retry, bounded reconnect, URL and stock-image fallback');
reset(); w.eval('retryCount=0; updateValues={};');
let snapshot;
w.dataReplace=values=>{snapshot=JSON.parse(JSON.stringify(values));};
w.updateTooltips=()=>{};
w.sourceEvents.emit('root_update','{"wifiRssi":-52,"uptime":123}');
assert.equal(snapshot,undefined);
w.sourceEvents.emit('root_update','finish');
assert.deepEqual(snapshot,{wifiRssi:-52,uptime:123});
for(const wait of [1000,2000,4000,8000,15000,15000]) {
    const previous=w.sourceEvents; previous.readyState=2; previous.emit('error');
    advance(wait-1); assert.equal(w.sourceEvents,previous);
    advance(1); assert.notEqual(w.sourceEvents,previous);
}
w.sourceEvents.emit('open');
const previous=w.sourceEvents; previous.readyState=2; previous.emit('error');
advance(1000); assert.notEqual(w.sourceEvents,previous);
console.log('PASS SSE browser: combined root snapshot and finish, bounded reconnect backoff, reset after successful open');
dom.window.close();
