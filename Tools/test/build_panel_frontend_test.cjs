const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),path=require('node:path');
const header=fs.readFileSync(path.join(__dirname,'../../Loader/src/build_panel_bridge.h'),'utf8');
const source=header.split('R"(')[1].split(')";')[0].replace(/^import .*$/gm,'');
const frames=new Map(),requests=[],events=[],observers=[];let next=0;
const h=(nodeName,attributes,...children)=>({nodeName,attributes:attributes||{},children:children.flat()});
const box=(left,top,width,height)=>({left,top,width,height,right:left+width,bottom:top+height});
let anchor={isConnected:true,getBoundingClientRect:()=>box(10,550,360,360)},catalogShown=false,listLookups=0;
const list={isConnected:true,dispatchEvent:e=>events.push([e.type,e.deltaY])};
const section={querySelector:q=>{assert.equal(q,'.BrowserList_repeater');listLookups++;return list}};
const catalogRoot={contains:n=>n===section,getBoundingClientRect:()=>box(385,550,605,360)};
const control={isConnected:true,closest:q=>{assert.equal(q,'.BrowserSection_root');return section},dispatchEvent:e=>events.push(e.type)};
const root={isConnected:true,querySelector:q=>q==='#browserRoot'?catalogRoot:anchor,contains:n=>n===control,getBoundingClientRect:()=>box(10,550,catalogShown?980:360,360)};
const changes=[];let cancel=0,nativeInput=0;
class Browser {
 constructor(){this.props={visible:true,mode:'Expanded',canOpenBrowser:true,tools:'Hidden',focusable:true,onCancel:()=>cancel++};this.state={tabIndex:-1};this.base=root;this.handleInput=()=>{nativeInput++;};}
 render(){this._browserTabs=[13,0,1,2,3,4,5,6,7,8,9,10,11,12].map(sectionID=>h('Tab',{sectionID,label:'native'+sectionID,icon:'icon'+sectionID,...(sectionID===8?{onButtonSelect:()=>changes.push('paths')}:{}),disabled:sectionID===10}));return h('div',{className:'Browser_container'},h('div',{id:'browserRoot'},h('ViewStack',{visibleIndex:this.state.tabIndex},h('Section',{showBlueprintsOnMount:true}),h('Section',{showBlueprintsOnMount:false}),h('Section',{})),h('div',{className:'Browser_tabContainer'},h('TabBar',{},this._browserTabs))));}
 onChangeTab(i){this.state.tabIndex=i;changes.push(i);}
 forceUpdate(){this.tree=this.render();catalogShown=!!this._pc2vrBuild?.catalog;this.componentDidUpdate();}
}
const ctx={_Browser:Browser,VRBuildInitiallyEnabled:true,VRBuildButton:'Button',VRBuildLiteral:x=>x,preact:{h},InputName:{Cancel:'cancel'},console,Set,Map,Promise,Math,Date,JSON,
 window:{innerWidth:1000,innerHeight:1000,addEventListener(){},removeEventListener(){}},document:{elementFromPoint:()=>control},
 requestAnimationFrame:fn=>{frames.set(++next,fn);return next},cancelAnimationFrame:id=>frames.delete(id),
 ResizeObserver:class{constructor(fn){this.fn=fn;this.nodes=new Set();observers.push(this)}observe(n){this.nodes.add(n)}unobserve(n){this.nodes.delete(n)}disconnect(){this.nodes.clear()}},
 MouseEvent:class{constructor(type){this.type=type}initMouseEvent(type){this.type=type}},
 XMLHttpRequest:class{open(method,url,async){assert.equal(method,'GET');assert.equal(async,true);this.url=url}send(){requests.push(this)}abort(){this.aborted=true}}
};
vm.runInNewContext(source,ctx);
const settle=async()=>{for(let i=0;i<10;i++)await Promise.resolve()};
const reply=async(r,data)=>{r.responseText=JSON.stringify(data);r.onload();await settle()};
const frame=async()=>{const f=[...frames.values()];frames.clear();for(const fn of f)fn();await settle()};
const find=(tree,predicate)=>predicate(tree)?tree:tree?.children?.map(t=>typeof t==='object'?find(t,predicate):null).find(Boolean);
const button=(b,label)=>find(b.tree,t=>t.nodeName==='Button'&&t.attributes.label===label);
const last=(part)=>requests.findLast(r=>r.url.includes(part));
(async()=>{
 const b=new Browser();b.componentWillMount();b.forceUpdate();
 await reply(last('build-owner'),{ok:true,owner:9,enabled:true});await frame();
 assert.ok(find(b.tree,t=>t.nodeName==='TabBar'),'original native tabs stay mounted to emit engine category changes');
 assert.equal(find(b.tree,t=>t.nodeName==='ViewStack').attributes.visibleIndex,-1);
 const nativeModes=find(b.tree,t=>t.nodeName==='ViewStack').children.map(t=>t.attributes.showBlueprintsOnMount);
 assert.deepEqual(nativeModes,[true,false,undefined],'preserve blueprint, custom and unspecified native catalog modes');
 assert.ok(last('build-layout').url.includes('&rw=36000'));
 await reply(last('build-layout'),{ok:true});await frame();assert.equal(frames.size,0,'no idle animation/layout loop');
 button(b,'Rides').attributes.onSelect();assert.ok(button(b,'native1'));
 button(b,'native1').attributes.onSelect();assert.equal(changes.at(-1),2);
 assert.equal(find(b.tree,t=>t.nodeName==='ViewStack').attributes.visibleIndex,2);
 assert.equal(find(b.tree,t=>t.nodeName==='Section').attributes.height,195);
 b.handleInput({inputName:'cancel',button:{isPressed:()=>true}});assert.equal(cancel,0);assert.equal(find(b.tree,t=>t.nodeName==='ViewStack').attributes.visibleIndex,-1);
 button(b,'native8').attributes.onSelect();assert.equal(changes.at(-1),'paths','tool selection uses exact native callback');
 b.props.tools='Placement';b.forceUpdate();assert.equal(b.tree.attributes.style.display,'none');
 b.props.tools='Hidden';b.forceUpdate();button(b,'Back').attributes.onSelect();
 button(b,'Tools').attributes.onSelect();assert.equal(button(b,'native10').attributes.disabled,true);button(b,'native10').attributes.onSelect();assert.equal(changes.at(-1),'paths');
 b.handleInput({inputName:'cancel',button:{isPressed:()=>true}});b.handleInput({inputName:'cancel',button:{isPressed:()=>true}});assert.equal(cancel,1);
 await frame();const layout=last('build-layout'),seq=Number(new URLSearchParams(layout.url.split('?')[1]).get('seq'));
 await reply(last('build-click'),{ok:true,owner:9,seq,x:150,y:600,enabled:true});assert.deepEqual(events,['mousedown']);
 await frame();assert.deepEqual(events,['mousedown','mouseup']);await reply(last('build-validate'),{ok:true,allow:true});assert.deepEqual(events,['mousedown','mouseup','click']);await frame();
 button(b,'native0').attributes.onSelect();await frame();
 const scrollLayout=last('build-layout'),scrollSeq=Number(new URLSearchParams(scrollLayout.url.split('?')[1]).get('seq'));
 assert.ok(scrollLayout.url.includes('&cx=38500&cy=55000&cw=60500&ch=36000'),'publish measured native catalog bounds');
 await reply(last('build-click'),{ok:true,owner:9,seq:scrollSeq,x:450,y:600,scroll:1,enabled:true});
 await reply(last('build-validate'),{ok:true,allow:true});assert.deepEqual(events.at(-1),['wheel',1],'native list receives wheel, with no mouse click');
 await reply(last('build-click'),{ok:true,owner:9,seq:scrollSeq,x:450,y:600,scroll:-1,enabled:true});
 await reply(last('build-validate'),{ok:true,allow:false});assert.equal(events.filter(e=>Array.isArray(e)).length,1,'modal/stale wheel is cancelled');
 assert.equal(listLookups,1,'cache the native list across thumbstick repeats');
 await reply(last('build-click'),{ok:true,cancelled:true,enabled:false});assert.equal(b.tree.attributes.className,'Browser_container','disable restores original tree');
 anchor.isConnected=false;anchor={isConnected:true,getBoundingClientRect:()=>box(10,550,360,360)};
 await reply(last('build-click'),{ok:true,cancelled:true,enabled:true});await frame();assert.ok(observers.at(-1).nodes.has(anchor),'reenable resolves replaced anchor');
 b.handleInput({inputName:'other'});assert.equal(nativeInput,1);
 b.componentWillUnmount();assert.equal(last('build-click').aborted,true);assert.equal(frames.size,0);assert.equal(ctx.window._pc2vrRidePopups.listeners.size,0);
 console.log('PASS actual build-wheel JS: native category/tool callbacks, mounted tabs, B hierarchy, placement hide, ordered validated click, enable recovery and lifetime cleanup');
})().catch(e=>{console.error(e);process.exitCode=1});
