const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),path=require('node:path');
const header=fs.readFileSync(path.join(__dirname,'../../Loader/src/ride_panel_bridge.h'),'utf8');
const source=header.split('R"(')[1].split(')";')[0];
const frames=new Map(),requests=[],events=[],observers=[];let next=0,root=null;
const signals=()=>({callbacks:new Set(),add(fn){this.callbacks.add(fn)},remove(fn){this.callbacks.delete(fn)}});
const native={state:{context:['rides',123],frozen:false,editModeActive:false},props:{managementVisible:false}};
let box={left:750,top:12,right:944,bottom:432,width:194,height:420};
const control={isConnected:true,dispatchEvent(e){events.push({type:e.type,x:e.clientX,y:e.clientY});}};
const rootNode={isConnected:true,getBoundingClientRect:()=>box,contains:n=>n===control};
const pane={isConnected:true};rootNode.querySelector=selector=>{assert.equal(selector,'.Panel_panel');return pane};
const ruler={isConnected:false,setAttribute(name,text){assert.equal(name,'style');assert.ok(text.includes('visibility:hidden'))},getBoundingClientRect:()=>({width:78.6666667})};
const windowEvents=new Map(),documentEvents=new Map();
const ctx={console,Set,Date,Math,JSON,Promise,
    Input:{onInputMethodChanged:signals()},
    window:{_pc2vrRidePopups:{supported:true,entries:new Map(),listeners:new Set()},innerWidth:944,innerHeight:944,getComputedStyle:node=>{assert.equal(node,pane);return {getPropertyValue:()=> '0rem 0rem 1.8rem 0px rgba(0, 0, 0, 0.4)'}},addEventListener:(n,f)=>windowEvents.set(n,f),removeEventListener:n=>windowEvents.delete(n)},
    document:{getElementById:id=>{assert.equal(id,'infoPanelRoot');return root},elementFromPoint:(x,y)=>{assert.ok(x>=750&&y>=12);return control},addEventListener:(n,f)=>documentEvents.set(n,f),removeEventListener:n=>documentEvents.delete(n),
        createElement:()=>ruler,body:{appendChild:n=>{n.isConnected=true;n.parentNode={removeChild:child=>{child.isConnected=false;child.parentNode=null}}}}},
    ResizeObserver:class {constructor(fn){this.fn=fn;this.nodes=new Set();observers.push(this)}observe(n){this.nodes.add(n)}unobserve(n){this.nodes.delete(n)}disconnect(){this.nodes.clear()}},
    requestAnimationFrame:fn=>{frames.set(++next,fn);return next},cancelAnimationFrame:id=>frames.delete(id),
    MouseEvent:class {constructor(type){this.type=type}initMouseEvent(type,bubbles,cancelable,view,detail,sx,sy,x,y){Object.assign(this,{type,bubbles,cancelable,clientX:x,clientY:y})}},
    XMLHttpRequest:class {open(method,url,async){assert.equal(method,'GET');assert.equal(async,true);this.url=url}send(){requests.push(this)}abort(){this.aborted=true}}
};
let allow=true;
const layouts=[];const rpc=url=>{if(url.startsWith('ride-validate.json?'))return Promise.resolve({ok:true,allow});layouts.push(url);return Promise.resolve({ok:true})};
vm.runInNewContext(source+'\nglobalThis.install=installVRRidePanel;',ctx);
const settle=async()=>{for(let i=0;i<10;i++)await Promise.resolve()};
const frame=async()=>{const callbacks=[...frames.values()];frames.clear();for(const fn of callbacks)fn();await settle()};
const reply=(request,data)=>{request.responseText=JSON.stringify(data);request.onload()};
(async()=>{
    const bridge=ctx.install(native,Promise.resolve(7),rpc);await frame();
    assert.equal(layouts.length,1);assert.ok(layouts[0].includes('&on=0'));
    root=rootNode;bridge.update();await frame();
    assert.ok(observers[0].nodes.has(root));assert.ok(observers[0].nodes.has(ruler));assert.equal(frames.size,0,'idle UI must not schedule a frame loop');
    assert.ok(layouts.at(-1).includes('&pl=1417') || layouts.at(-1).includes('&pl=1416'),'native rem shadow resolves from measured DOM length');
    assert.equal(requests.length,1,'one request waits for a physical press');
    const seq=Number(new URLSearchParams(layouts.at(-1).split('?')[1]).get('seq'));
    reply(requests[0],{ok:true,owner:7,seq,x:832,y:40});
    assert.deepEqual(events,[{type:'mousedown',x:832,y:40}]);
    await frame();assert.deepEqual(events.map(e=>e.type),['mousedown','mouseup','click']);
    assert.equal(requests.length,2,'subscribe again only after delivering press/release');await frame();
    assert.equal(frames.size,0);assert.equal(layouts.length,2,'unchanged layout sends no additional update');
    reply(requests[1],{ok:true,owner:7,seq:seq-1,x:832,y:40});await settle();
    assert.equal(events.length,3,'stale source coordinates cannot click a changed panel');assert.equal(requests.length,3);
    box={...box,bottom:252,height:240};observers[0].fn();await frame();
    assert.ok(layouts.at(-1).includes('&h=24000'),'ResizeObserver follows native tab content height');
    native.state.frozen=true;bridge.update();await frame();
    assert.equal(requests[2].aborted,true);assert.ok(layouts.at(-1).includes('&on=0'));
    reply(requests[2],{ok:true,owner:7,seq,x:832,y:40});assert.equal(events.length,3,'aborted frozen panel cannot accept late click');
    native.state.frozen=false;bridge.update();await frame();assert.equal(requests.length,4);
    const currentSeq=Number(new URLSearchParams(layouts.at(-1).split('?')[1]).get('seq'));allow=false;
    reply(requests[3],{ok:true,owner:7,seq:currentSeq,x:832,y:40});await frame();
    assert.deepEqual(events.slice(-3).map(e=>e.type),['mousedown','mouseup','mouseleave'],'new global modal cancels the deferred click and clears native down state');
    await frame();
    const popupControl={...control};
    const popup={isConnected:true,getBoundingClientRect:()=>({left:730,top:140,right:908,bottom:520,width:178,height:380}),contains:n=>n===popupControl};
    const popupEntry={owner:control,popup};
    const registry=ctx.window._pc2vrRidePopups;registry.entries.set(1,popupEntry);
    for(const fn of registry.listeners)fn();await frame();
    assert.ok(layouts.at(-1).includes('&x=73000')&&layouts.at(-1).includes('&h=50800')&&layouts.at(-1).includes('&rx=75000')&&layouts.at(-1).includes('&rw=19400'),'portal bounds extend capture while preserving root pixel scale and anchor');
    assert.ok(observers[0].nodes.has(popup));
    ctx.document.elementFromPoint=()=>popupControl;allow=true;
    const popupSeq=Number(new URLSearchParams(layouts.at(-1).split('?')[1]).get('seq'));
    reply(requests[4],{ok:true,owner:7,seq:popupSeq,x:740,y:500});await frame();await frame();
    assert.deepEqual(events.slice(-3).map(e=>e.type),['mousedown','mouseup','click'],'owned portal outside root is clickable');
    registry.entries.delete(1);for(const fn of registry.listeners)fn();await frame();
    assert.ok(!observers[0].nodes.has(popup));assert.ok(layouts.at(-1).includes('&h=24000'));
    bridge.dispose();await settle();assert.equal(requests[5].aborted,true);assert.equal(frames.size,0);assert.equal(ruler.isConnected,false);assert.equal(registry.listeners.size,0);
    assert.equal(ctx.Input.onInputMethodChanged.callbacks.size,0);assert.equal(windowEvents.size,0);assert.equal(documentEvents.size,0);
    // Reuse one mounted inspector as the game changes selection without closing it.
    native.state.context=[];
    const selection=ctx.install(native,Promise.resolve(8),rpc);await frame();
    assert.ok(layouts.at(-1).includes('&on=0'));
    for(const type of ['inspectedBuilding','shops','rides','inspectedPool','inspectedScenery','inspectedMultiSelection']){
        native.state.context=[type,123];native.state.editModeActive=type==='inspectedBuilding';
        selection.update();await frame();
        assert.ok(layouts.at(-1).includes('&on=1'),type+' inspector should use the left hand');
    }
    native.props.managementVisible=true;selection.update();await frame();
    assert.ok(layouts.at(-1).includes('&on=0'));native.props.managementVisible=false;
    native.state.context=[];selection.update();await frame();
    assert.ok(layouts.at(-1).includes('&on=0'));selection.dispose();await settle();
    // Execute the actual rider component wrapper and shared bridge together.
    let cameraMount=0,cameraUpdate=0,cameraUnmount=0;
    ctx._CameraSettingsPanel=class {
        constructor(){this.state={type:'flatRide'};this.base=rootNode;this.props={};}
        componentDidMount(){cameraMount++;return 'native-mounted'}
        componentDidUpdate(){cameraUpdate++}
        componentWillUnmount(){cameraUnmount++}
    };
    const cameraSource=fs.readFileSync(path.join(__dirname,'../../Loader/src/camera_panel_bridge.h'),'utf8').split('R"(')[1].split(')";')[0];
    vm.runInNewContext(cameraSource,ctx);ctx.document.elementFromPoint=()=>control;
    const camera=new ctx._CameraSettingsPanel();
    camera.base={...rootNode,getBoundingClientRect:()=>({left:750,top:0,right:944,bottom:944,width:194,height:944}),children:[rootNode]};
    assert.equal(camera.componentDidMount(),'native-mounted');
    const last=()=>requests.at(-1),answer=value=>reply(last(),value);
    assert.ok(last().url.includes('camera-owner.json'));answer({ok:true,owner:29});await frame();
    assert.ok(last().url.includes('camera-layout.json?owner=29'));const cameraSeq=Number(new URLSearchParams(last().url.split('?')[1]).get('seq'));
    assert.ok(last().url.includes('&y=1200&w=19400&h=24000'),'rider anchor uses actual visible Panel bounds, excluding stretched empty wrapper');
    answer({ok:true});await settle();assert.ok(last().url.includes('camera-click.json?owner=29'));
    const eventCount=events.length;
    answer({ok:true,owner:29,seq:cameraSeq,x:832,y:40});await frame();
    assert.ok(last().url.includes('camera-validate.json?owner=29'));answer({ok:true,allow:true});await settle();
    assert.deepEqual(events.slice(eventCount).map(e=>e.type),['mousedown','mouseup','click'],'rider controls use the same ordered native button edges');
    for(const type of ['coaster','flatRideNoGuest','guest','kart','staff']){camera.state.type=type;camera.componentDidUpdate();await frame();assert.ok(!last().url.includes('on=0'),'native rider types remain eligible');}
    const outstanding=last();camera.state.type='hidden';camera.componentDidUpdate();await frame();
    assert.equal(outstanding.aborted,true);assert.ok(last().url.includes('camera-layout.json') && last().url.includes('on=0'),'camera close cancels input and hides its layout');
    camera.componentWillUnmount();await settle();assert.equal(cameraMount,1);assert.equal(cameraUpdate,6);assert.equal(cameraUnmount,1);
    assert.equal(registry.listeners.size,0);assert.equal(ctx.Input.onInputMethodChanged.callbacks.size,0);assert.equal(frames.size,0);
    console.log('PASS actual floating-panel JS: event-driven layout, exact coordinates, ordered native mouse edges, stale/frozen cancellation and observer cleanup');
})().catch(e=>{console.error(e);process.exitCode=1});
