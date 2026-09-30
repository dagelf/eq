/* Chromium DevTools pipe driver; no frontend test runtime or downloaded packages. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static int input,output,serial;
static pid_t browser;
static char *session;
static void cleanup(void) { if (browser>0) { kill(browser,SIGTERM); waitpid(browser,NULL,0); } }
static void fail(const char *why) { fprintf(stderr,"Browser test: %s\n",why); exit(1); }
static struct json_object *command(const char *method,struct json_object *params) {
    fprintf(stderr,"CDP %s\n",method);
    struct json_object *o=json_object_new_object(); int id=++serial;
    json_object_object_add(o,"id",json_object_new_int(id));
    json_object_object_add(o,"method",json_object_new_string(method));
    json_object_object_add(o,"params",params?params:json_object_new_object());
    if (session) json_object_object_add(o,"sessionId",json_object_new_string(session));
    const char *s=json_object_to_json_string(o); size_t len=strlen(s)+1,off=0;
    while (off<len) { ssize_t n=write(input,s+off,len-off); if (n<=0) fail("Cannot write command"); off+=n; }
    json_object_put(o);
    for (;;) {
        size_t size=0,cap=4096; char *buf=malloc(cap);
        for (;;) {
            struct pollfd p={.fd=output,.events=POLLIN};
            if (poll(&p,1,15000)<=0) fail("Timed out waiting for Chromium");
            char ch; if (read(output,&ch,1)!=1) fail("Chromium closed the pipe");
            if (size+1>=cap) { cap*=2; buf=realloc(buf,cap); if (!buf || cap>16000000) fail("Response too large"); }
            buf[size++]=ch; if (!ch) break;
        }
        o=json_tokener_parse(buf); free(buf);
        struct json_object *response_id,*error;
        if (o && json_object_object_get_ex(o,"id",&response_id) && json_object_get_int(response_id)==id) {
            if (json_object_object_get_ex(o,"error",&error)) fail(json_object_to_json_string(error));
            return o;
        }
        if (o) json_object_put(o);
    }
}
static void evaluate(const char *expression) {
    struct json_object *p=json_object_new_object();
    json_object_object_add(p,"expression",json_object_new_string(expression));
    json_object_object_add(p,"awaitPromise",json_object_new_boolean(1));
    json_object_object_add(p,"returnByValue",json_object_new_boolean(1));
    struct json_object *o=command("Runtime.evaluate",p),*result,*error;
    json_object_object_get_ex(o,"result",&result);
    if (json_object_object_get_ex(result,"exceptionDetails",&error)) fail(json_object_to_json_string(error));
    json_object_put(o);
}
static void key(const char *name,int code) {
    const char *types[]={"keyDown","keyUp"};
    for (int i=0;i<2;i++) {
        struct json_object *p=json_object_new_object();
        json_object_object_add(p,"type",json_object_new_string(types[i]));
        json_object_object_add(p,"key",json_object_new_string(name));
        json_object_object_add(p,"code",json_object_new_string(name));
        json_object_object_add(p,"windowsVirtualKeyCode",json_object_new_int(code));
        if (code==13 && i==0) json_object_object_add(p,"text",json_object_new_string("\r"));
        json_object_put(command("Input.dispatchKeyEvent",p));
    }
}
int main(int argc,char **argv) {
    if (argc!=2) return 2;
    int to[2],from[2]; if (pipe2(to,O_CLOEXEC) || pipe2(from,O_CLOEXEC)) return 1;
    browser=fork();
    if (browser==0) {
        int a=fcntl(to[0],F_DUPFD,10),b=fcntl(from[1],F_DUPFD,10);
        dup2(a,3); dup2(b,4); close(a); close(b);
        setenv("DBUS_SESSION_BUS_ADDRESS","unix:path=/dev/null",1);
        char profile[1024]; snprintf(profile,sizeof(profile),"--user-data-dir=%s/chromium",argv[1]);
        const char *binary=getenv("CHROMIUM_BINARY");
        if (!binary) binary=access("/usr/lib/chromium/chromium",X_OK)==0?"/usr/lib/chromium/chromium":"chromium";
        execlp(binary,binary,"--headless","--disable-gpu","--disable-background-networking","--no-proxy-server","--password-store=basic","--no-first-run","--remote-debugging-pipe",profile,NULL); _exit(127);
    }
    if (browser<0) return 1;
    atexit(cleanup); close(to[0]); close(from[1]); input=to[1]; output=from[0];
    struct json_object *o=command("Target.createTarget",json_tokener_parse("{\"url\":\"about:blank\"}")),*result,*target;
    json_object_object_get_ex(o,"result",&result); json_object_object_get_ex(result,"targetId",&target);
    struct json_object *p=json_object_new_object();
    json_object_object_add(p,"targetId",json_object_get(target)); json_object_object_add(p,"flatten",json_object_new_boolean(1)); json_object_put(o);
    o=command("Target.attachToTarget",p); json_object_object_get_ex(o,"result",&result); json_object_object_get_ex(result,"sessionId",&target); session=strdup(json_object_get_string(target)); json_object_put(o);
    char navigate[128]; snprintf(navigate,sizeof(navigate),"{\"url\":\"http://127.0.0.1:%s\"}",getenv("EQWEB_PORT")?getenv("EQWEB_PORT"):"8932");
    json_object_put(command("Page.navigate",json_tokener_parse(navigate)));
    usleep(300000);
    evaluate("(async()=>{window.check=(v,m)=>{if(!v)throw Error(m)};window.waitFor=async(f)=>{for(let i=0;i<100;i++){if(await f())return;await new Promise(r=>setTimeout(r,50));}throw Error('condition timed out')};await waitFor(()=>document.querySelectorAll('#bands input[type=range]').length===10&&!document.querySelector('#editor').disabled);check(document.querySelectorAll('#editor input[aria-label]').length===22,'gain labels');document.querySelector('#band-0').focus()})()");
    key("ArrowUp",38);
    evaluate("(async()=>{await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.settings.gains[0]===0.1&&!s.pending});document.querySelector('#reset').focus()})()");
    key("Enter",13);
    evaluate("(async()=>{await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.settings.gains.every(v=>v===0)&&s.settings.preamp===0&&!s.pending});const o=document.querySelector('#output');o.value='test.other';o.dispatchEvent(new Event('change',{bubbles:true}));await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.settings.output==='test.other'&&s.live.connected&&!s.pending});await waitFor(()=>!document.querySelector('#default').disabled);document.querySelector('#default').focus()})()");
    key("Enter",13);
    evaluate("(async()=>{await waitFor(async()=> (await(await fetch('/api/state')).json()).live.default);await waitFor(()=>document.querySelector('#default').textContent==='EQ is the default sink');check(document.querySelector('#errors').textContent==='','UI errors')})()");
    evaluate("(async()=>{const text=document.querySelector('#eq-text');text.focus();text.value=JSON.stringify({gains:Array(10).fill(6),preamp:0,maintain:true,loudness_target:0});text.dispatchEvent(new Event('input',{bubbles:true}));await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.settings.gains.every(v=>v===6)&&s.settings.preamp===-6&&!s.pending});check(document.querySelector('#maintain').checked,'maintain checkbox');check(document.querySelector('#gain-summary').textContent.includes('Average +6.00 dB'),'average amplification');text.value='{broken';text.dispatchEvent(new Event('input',{bubbles:true}));check(text.getAttribute('aria-invalid')==='true','invalid text feedback');await new Promise(r=>setTimeout(r,100));check((await(await fetch('/api/state')).json()).settings.gains[0]===6,'invalid text changed audio');document.querySelector('#preset-name').value='Browser preset';document.querySelector('#preset-save').click();await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.library.presets['Browser preset']?.preamp===-6&&!s.library.pending});text.value='0 0 0 0 0 0 0 0 0 0 0';text.dispatchEvent(new Event('input',{bubbles:true}));await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.settings.gains.every(v=>v===0)&&s.settings.preamp===0&&!s.pending});await waitFor(()=>document.querySelector('#preset-select').options.length>1);document.querySelector('#preset-select').value='Browser preset';document.querySelector('#preset-load').click();await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.settings.gains[0]===6&&s.settings.preamp===-6&&!s.pending});check(JSON.parse(text.value).gains[0]===6,'preset text sync');check(!text.value.includes('\\n'),'jsonl values');document.querySelector('#maintain').checked=false;document.querySelector('#maintain').dispatchEvent(new Event('change'));document.querySelector('#reset').click();await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.settings.gains.every(v=>v===0)&&s.settings.preamp===0&&!s.settings.maintain&&!s.pending});const master=document.querySelector('#master');await waitFor(()=>!document.querySelector('#master-control').disabled);master.value=82;master.dispatchEvent(new Event('input',{bubbles:true}));await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.desktop.mixer.some(m=>m.name==='effect_input.eqweb'&&Math.abs(m.volume-82)<.1)});master.value=100;master.dispatchEvent(new Event('input',{bubbles:true}));await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.desktop.mixer.some(m=>m.name==='effect_input.eqweb'&&m.volume===100)});await waitFor(()=>document.querySelector('#now-title').textContent==='Song A');document.querySelector('#next').click();await waitFor(()=>document.querySelector('#now-title').textContent==='Song B');document.querySelector('#previous').click();await waitFor(()=>document.querySelector('#now-title').textContent==='Song A');const bar=document.querySelector('#player-progress');await waitFor(()=>!bar.disabled);const rect=bar.getBoundingClientRect();bar.dispatchEvent(new MouseEvent('click',{bubbles:true,clientX:rect.left+rect.width/2,clientY:rect.top+rect.height/2}));await waitFor(async()=>(await(await fetch('/api/state')).json()).desktop.selected_player.position===90);check(document.querySelector('#errors').textContent==='','new controls errors')})()");
    evaluate("(async()=>{await new Promise(r=>setTimeout(r,1000));const pos=async()=>(await(await fetch('/api/state')).json()).desktop.selected_player.position;const start=await pos();const fwd=document.querySelector('#seek-forward');check(fwd&&!fwd.disabled,'seek-forward enabled');fwd.click();await waitFor(async()=>Math.abs((await pos())-(start+20))<1);document.querySelector('#seek-back').click();await waitFor(async()=>Math.abs((await pos())-start)<1);check(document.querySelector('#maintain-load').checked,'maintain load default');check(document.querySelector('#output-log').checked,'output log default');const text=document.querySelector('#eq-text');text.value=JSON.stringify({gains:Array(10).fill(6),preamp:0});text.dispatchEvent(new Event('input',{bubbles:true}));await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.settings.gains[0]===6&&s.settings.preamp===0&&!s.pending});document.querySelector('#preset-name').value='Loud';document.querySelector('#preset-save').click();await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.library.presets.Loud&&!s.library.pending});document.querySelector('#reset').click();await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.settings.gains[0]===0&&!s.pending});await waitFor(()=>[...document.querySelector('#preset-select').options].some(o=>o.value==='Loud'));document.querySelector('#preset-select').value='Loud';document.querySelector('#preset-load').click();await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.settings.gains[0]===6&&s.settings.preamp===-6&&!s.pending});document.querySelector('#preset-quickbar').click();await waitFor(()=>document.querySelector('#quickbar [data-quickbar]'));document.querySelector('#reset').click();await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.settings.gains[0]===0&&!s.pending});document.querySelector('#quickbar [data-quickbar]').click();await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.settings.gains[0]===6&&!s.pending});const box=document.querySelector('#autoload');await waitFor(()=>box.options.length>=2);box.selectedIndex=0;const first=box.value;box.dispatchEvent(new WheelEvent('wheel',{deltaY:100,bubbles:true,cancelable:true}));await waitFor(()=>box.value!==first);check(document.querySelector('#errors').textContent==='','todo controls errors')})()");
    evaluate("(async()=>{\n const text=document.querySelector('#eq-text');\n const before=(await(await fetch('/api/state')).json()).settings;\n for(const bad of ['1,2,3,4,5,6,7,8,9,','1,2,,3,4,5,6,7,8,9']) {\n   text.value=bad; text.dispatchEvent(new Event('input',{bubbles:true}));\n   check(text.getAttribute('aria-invalid')==='true','incomplete CSV accepted');\n }\n await new Promise(r=>setTimeout(r,150));\n check(JSON.stringify((await(await fetch('/api/state')).json()).settings)===JSON.stringify(before),'incomplete CSV changed EQ');\n const originalFetch=window.fetch;\n let failures=1;\n window.fetch=async (...args)=>{\n   if(args[0]==='/api/eq' && failures-->0) {await new Promise(r=>setTimeout(r,200));throw TypeError('Simulated network failure');}\n   return originalFetch(...args);\n };\n const edit=value=>{text.value=JSON.stringify({gains:Array(10).fill(value),preamp:0,maintain:false});text.dispatchEvent(new Event('input',{bubbles:true}));};\n edit(2); await new Promise(r=>setTimeout(r,100)); edit(3);\n await new Promise(r=>setTimeout(r,250));\n check(JSON.parse(text.value).gains[0]===3,'failed request discarded newer edits');\n await waitFor(async()=>{const s=await(await originalFetch('/api/state')).json();return s.settings.gains.every(v=>v===3)&&!s.pending});\n window.fetch=originalFetch;\n let rejected=0;\n window.fetch=async (...args)=>args[0]==='/api/eq'?(rejected++,new Response(JSON.stringify({error:'Rejected for regression test'}),{status:400})):originalFetch(...args);\n edit(1); await new Promise(r=>setTimeout(r,1200));\n check(rejected===1,'permanent failure retried');\n check(JSON.parse(text.value).gains[0]===1,'rejected edits discarded');\n window.fetch=originalFetch; edit(0);\n await waitFor(async()=>{const s=await(await fetch('/api/state')).json();return s.settings.gains.every(v=>v===0)&&!s.pending});\n})()");
    evaluate("(async()=>{\n const state=async()=>(await(await fetch('/api/state')).json());\n const remember=document.querySelector('#remember-song');remember.checked=true;remember.dispatchEvent(new Event('change'));\n await waitFor(async()=>{const s=await state();return s.library.remember&&s.settings.gains[0]===2});\n const enabled=document.querySelector('#fallback-eq');enabled.checked=true;enabled.dispatchEvent(new Event('change'));\n const device=document.querySelector('#fallback-device');\n await waitFor(()=>!device.disabled&&[...device.options].some(o=>o.value==='test.output')&&[...device.options].some(o=>o.value==='test.other'));\n device.value='test.output';device.dispatchEvent(new Event('change'));\n await waitFor(async()=>(await state()).library.fallback_device==='test.output');\n const text=document.querySelector('#eq-text');text.value=JSON.stringify({gains:Array(10).fill(7),preamp:-2,maintain:false});text.dispatchEvent(new Event('input'));\n await waitFor(async()=>{const s=await state();return s.settings.gains[0]===7&&!s.pending});\n await waitFor(()=>!document.querySelector('#save-device-eq').disabled);document.querySelector('#save-device-eq').click();\n await waitFor(async()=>{const s=await state();return s.library.device_eq.gains[0]===7&&s.library.device_eq.preamp===-2&&!s.library.pending});\n document.querySelector('#next').click();\n await waitFor(async()=>{const s=await state();return s.desktop.selected_player.title==='Song B'&&s.settings.gains[0]===-3&&s.library.device_eq.gains[0]===7});\n enabled.checked=false;enabled.dispatchEvent(new Event('change'));\n await waitFor(async()=>{const s=await state();return s.settings.gains.every(v=>v===7)&&s.settings.preamp===-2});\n remember.checked=false;remember.dispatchEvent(new Event('change'));\n await waitFor(async()=>(await state()).library.remember===false);\n document.querySelector('#reset').click();\n await waitFor(async()=>{const s=await state();return s.library.device_eq.gains[0]===0&&s.settings.gains[0]===0&&!s.pending});\n check(document.querySelector('#device-eq-values').textContent.includes('Preamp: +0.00 dB'),'saved device EQ display');\n})()");
    json_object_put(command("Emulation.setDeviceMetricsOverride",json_tokener_parse("{\"width\":390,\"height\":844,\"deviceScaleFactor\":1,\"mobile\":true}")));
    evaluate("check(document.documentElement.scrollWidth<=innerWidth,'mobile horizontal overflow')");
    json_object_put(command("Emulation.setDeviceMetricsOverride",json_tokener_parse("{\"width\":1000,\"height\":1250,\"deviceScaleFactor\":1,\"mobile\":false}")));
    evaluate("window.scrollTo(0,0)");
    evaluate("check(document.querySelector('#band-0').getBoundingClientRect().height>=350,'taller bands');check(document.querySelector('#band-1').getBoundingClientRect().x-document.querySelector('#band-0').getBoundingClientRect().x<65,'closer bands')");
    o=command("Page.captureScreenshot",json_tokener_parse("{\"format\":\"png\"}")); json_object_object_get_ex(o,"result",&result); json_object_object_get_ex(result,"data",&target);
    char path[1024]; snprintf(path,sizeof(path),"%s/browser.png.base64",argv[1]); FILE *f=fopen(path,"w"); if (!f) return 1; fputs(json_object_get_string(target),f); fclose(f); json_object_put(o);
    puts("Browser: vertical layout, keyboard controls, live text, invalid input, compensation, presets, master volume, player controls, progress-click seek, relative seek, jsonl values, maintain-on-load, quickbar, autoload device EQ saving, cross-device fallback, request retries, and responsive layout passed.");
    return 0;
}
