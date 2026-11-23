const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),path=require('node:path');
const source=[...fs.readFileSync(path.join(__dirname,'../../Loader/src/dashboard_bridge.h'),'utf8').matchAll(/R"\(([\s\S]*?)\)"/g)].map(m=>m[1]).join('').replace(/^import .*$/gm,'');
const buildSource=fs.readFileSync(path.join(__dirname,'../../Loader/src/build_panel_bridge.h'),'utf8').split('R"(')[1].split(')";')[0].replace(/^import .*$/gm,'');
const notificationSource=fs.readFileSync(path.join(__dirname,'../../Loader/src/notification_bridge.h'),'utf8').split('R"(')[1].split(')";')[0].replace(/^import .*$/gm,'');
const h=(nodeName,attributes,...children)=>({nodeName,attributes:attributes||{},children:children.flat(Infinity).filter(n=>n!=null && typeof n!=='boolean')});
const chromeNodes=new Map(['.ManagementMenu_root','.HUD_promptsList','.Heatmaps_root','.HUD_innerContent .InputHintsModule_root','.CenterPrompt_target'].map(selector=>[selector,{isConnected:true,style:{display:''}}]));let chromeQueries=0;
const requests=[],frames=new Map(),events=[],observers=[];let next=0,selected=0,nativeMount=0,nativeUpdate=0,nativeUnmount=0;
const box=(left,top,width,height)=>({left,top,width,height,right:left+width,bottom:top+height});
const control={isConnected:true,dispatchEvent:e=>events.push(e.deltaY?['wheel',e.deltaY]:e.type)};
let styleQueries=0;
const styledNode={className:'TimeStat_root',isConnected:true,style:new Proxy({transform:'translate(-50%, 0%)',color:'white'},{set(o,key,value){o[key]=key==='transform' && value==='none'?'':value;return true}})};
const root={isConnected:true,style:{},contains:n=>n===control,querySelector:()=>null,querySelectorAll:()=>{styleQueries++;return [styledNode]},getBoundingClientRect:()=>box(parseFloat(root.style.left)||0,parseFloat(root.style.top)||0,360,160)};
const wheelRoot={isConnected:true,contains:()=>false,querySelector:q=>q==='.pc2vr-build-wheel'?wheelAnchor:null,getBoundingClientRect:()=>box(10,50,360,360)};
const wheelAnchor={isConnected:true,getBoundingClientRect:()=>box(10,50,360,360)};
class Browser{
 constructor(){this.base=wheelRoot;this.state={tabIndex:0};this.props={visible:true,mode:'Minimised',canOpenBrowser:true,tools:'Hidden',onCancel:()=>{this.props.mode='Minimised';this.forceUpdate()}};this.handleInput=()=>{};}
 render(){this._browserTabs=[13,0,1,2,3,4,5,6,7,8,9,10,11,12].map(sectionID=>h('Tab',{sectionID,label:String(sectionID)}));return h('div',{className:'Browser_container'},h('div',{},h('ViewStack',{},h('Section',{}))));}
 forceUpdate(){this.tree=this.render();this.componentDidUpdate();}
}
class StatusBar{render(){return h('div',{},h('stats',{onSelect:'native'}),h('controls',{native:true}));}}
class Objectives{render(){return h('div',{},false,h('constraint',{query:'#objectivesTarget'},h('native-objective',{progress:73})));}}
class HUD{
 constructor(){this.base={querySelector:q=>{if(q==='#pc2vr-dashboard')return root;chromeQueries++;assert.ok(chromeNodes.has(q));return chromeNodes.get(q)}};this.state={statusBarVisible:true,toolsPanelMode:'Hidden',browserMode:'Minimised',objectivesAvailable:true,heatmapState:0};this.props={};this.onSelectObjectives=()=>{selected++;this.state.objectivesVisible=!this.state.objectivesVisible;this.state.focusedMenu='Objectives';this.forceUpdate()};this.onCloseObjectives=()=>{this.state.objectivesVisible=false;this.forceUpdate()};}
 render(){return h('div',{className:'HUD_root'},h(StatusBar,{nativeStats:true}),h(Objectives,{onAvailableChange:'native',menuVisible:this.state.objectivesVisible}),h('ScreenBG',{darkenBackground:this.state.objectivesVisible}),h('other',{native:true}));}
 componentWillMount(){assert.ok(this._pc2vrDashboard,'dashboard ownership exists before native mount subscriptions can render');this.forceUpdate();}
 componentDidMount(){nativeMount++;}componentDidUpdate(){nativeUpdate++;}componentWillUnmount(){nativeUnmount++;}
 isManagementVisible(){return !!this.state.management;}
 forceUpdate(){this.tree=this.render(this.props,this.state);root.style={...this.tree.children.find(n=>n?.attributes.id==='pc2vr-dashboard').attributes.style};this.componentDidUpdate();}
 setMenuFocus(focus){this.state.focusedMenu=focus;this.forceUpdate();}
}
const signal=()=>({add(){},remove(){}});
const ctx={_HUD:HUD,_Browser:Browser,VRBuildInitiallyEnabled:true,VRBuildButton:'BuildButton',VRBuildLiteral:x=>x,InputName:{Cancel:'cancel'},HUDMenu:{Browser:'Browser',ManagementMenu:'ManagementMenu'},StatusBar,ObjectivesModule:Objectives,ScreenBG:'ScreenBG',VRDashboardControls:'controls',VRDashboardButton:'Button',VRDashboardLiteral:x=>x,preact:{h,Component:class{}},console,Set,Map,Promise,Math,Date,JSON,
 NotificationsDataModel:{onImportantNotificationsChanged:signal(),onNotificationsChanged:signal(),onNotificationIsReadChanged:signal(),onDisplayQueueChanged:signal(),onBeginNewNotification:signal(),getNotifications:()=>[],getQueuedNotifications:()=>[]},NotificationFilter:{All:0},NotificationListImportance:{Important:0,Notices:1},NOTIFICATION_FILTER_DATA:{},VRNotificationButton:class{render(){}},
 window:{innerWidth:1000,innerHeight:1000,addEventListener(){},removeEventListener(){}},document:{elementFromPoint:()=>control},
 requestAnimationFrame:fn=>{frames.set(++next,fn);return next},cancelAnimationFrame:id=>frames.delete(id),
 ResizeObserver:class{constructor(fn){this.fn=fn;observers.push(this)}observe(){}unobserve(){}disconnect(){this.disconnected=true}},
 MouseEvent:class{constructor(type){this.type=type}initMouseEvent(type){this.type=type}},
 XMLHttpRequest:class{open(method,url,async){assert.equal(method,'GET');assert.equal(async,true);this.url=url}send(){requests.push(this)}abort(){this.aborted=true}}
};
vm.runInNewContext(notificationSource,ctx);
vm.runInNewContext(source,ctx);
vm.runInNewContext(buildSource,ctx);
const settle=async()=>{for(let i=0;i<10;i++)await Promise.resolve()};
const frame=async()=>{const work=[...frames.values()];frames.clear();for(const fn of work)fn();await settle()};
const last=op=>requests.filter(r=>r.url.includes(op+'.json')).at(-1);
const reply=async(r,value)=>{r.responseText=JSON.stringify(value);r.onload();await settle()};
const dashboard=hud=>hud.tree.children.find(n=>n?.attributes.id==='pc2vr-dashboard');
const park=hud=>dashboard(hud).children[0];
(async()=>{
 const hud=new HUD();hud.componentWillMount();hud.forceUpdate();assert.equal(dashboard(hud).attributes.className,'','unconfirmed capability keeps native styling');hud.componentDidMount();
 const initialChildren=dashboard(hud).children.map(n=>n.nodeName);
 await reply(last('dashboard-owner'),{ok:true,owner:17,enabled:true});await frame();await frame();assert.equal(root.style.display,'none','park info is absent while the LB wheel is closed');assert.equal(ctx.window._pc2vrWrist.dashboardBox,undefined);
 assert.equal(hud.tree.attributes.className,'HUD_root','no ancestor CSS gate');
 for(const node of chromeNodes.values())assert.equal(node.style.display,'none','native passive chrome hides directly');
 const queriesAfterMount=chromeQueries;
 hud.state.focusedMenu='ManagementMenu';hud.forceUpdate();await frame();assert.equal(chromeNodes.get('.ManagementMenu_root').style.display,'','explicit management focus restores native styling');hud.state.focusedMenu=undefined;hud.forceUpdate();await frame();assert.equal(chromeNodes.get('.ManagementMenu_root').style.display,'none');
 hud.state.heatmapState=2;hud.forceUpdate();await frame();assert.equal(chromeNodes.get('.Heatmaps_root').style.display,'','open heatmaps retain native controls');hud.state.heatmapState=1;hud.forceUpdate();await frame();assert.equal(chromeNodes.get('.Heatmaps_root').style.display,'','pinned heatmaps retain native controls');hud.state.heatmapState=0;hud.forceUpdate();await frame();assert.equal(chromeNodes.get('.Heatmaps_root').style.display,'none');
 assert.equal(chromeQueries,queriesAfterMount,'focus/visibility changes reuse stable native roots');
 const oldChrome=chromeNodes.get('.HUD_promptsList');oldChrome.isConnected=false;const replacement={isConnected:true,style:{display:'flex'}};chromeNodes.set('.HUD_promptsList',replacement);hud.forceUpdate();await frame();assert.equal(replacement.style.display,'none');assert.equal(chromeQueries,queriesAfterMount+1,'only a destroyed root is resolved again');
 const browser=new Browser();browser.componentWillMount();browser.forceUpdate();await reply(last('build-owner'),{ok:true,owner:9,enabled:true});await frame();browser.props.mode='Expanded';browser.forceUpdate();hud.state.browserMode='Expanded';hud.forceUpdate();for(let i=0;i<6;i++)await frame();
 assert.equal(nativeMount,1);assert.equal(styledNode.style.transform,'','native centered clock transform reset and normalized');assert.equal(root.style.display,'block');assert.equal(root.style.left,'10px');assert.equal(root.style.top,'428px','dashboard is below the measured wheel plus its spacing');
 const union=new URLSearchParams(last('build-layout').url.split('?')[1]);assert.equal(union.get('y'),'5000');assert.equal(union.get('h'),'53800','one existing build surface contains both wheel and dashboard');assert.equal(union.get('rw'),'36000','wheel anchor width does not grow with combined content');
 const d=park(hud);assert.ok(d);assert.equal(dashboard(hud).attributes.style.pointerEvents,'auto','interactive wrapper overrides the HUD pointer pass-through');assert.equal(d.children[0].nodeName,StatusBar);assert.equal(d.children[0].attributes.nativeStats,true);assert.equal(d.children[1].children[0].attributes.focusable,false,'laser-only added button cannot retain native focus while hidden and intercept ride Back');
 const stableLayout=last('dashboard-layout');hud.forceUpdate();await frame();assert.equal(last('dashboard-layout'),stableLayout,'normalized native styles do not hide an unchanged surface during focus updates');
 const dense=n=>{for(const child of n.children || []){assert.ok(child!=null && typeof child!=='boolean','native Preact child list stays normalized');if(typeof child==='object')dense(child)}};dense(hud.tree);
 assert.equal(d.children[2].nodeName,Objectives);assert.equal(d.children[2].attributes.onAvailableChange,'native');assert.equal(hud.tree.children.filter(n=>n?.nodeName===Objectives).length,0,'no duplicate objective subscriptions');
 d.children[1].children[0].attributes.onSelect();assert.equal(selected,1);assert.equal(park(hud).children[1].children[0].attributes.label,'Close objectives');
 assert.equal(hud.state.focusedMenu,'Browser','native objective callback returns gamepad focus to the still-open wheel');assert.equal(d.children[2].attributes.menuFocusable,false);
 const nativeStatus=new StatusBar();nativeStatus.props={};assert.equal(nativeStatus.render().children[1].attributes.native,true);
 nativeStatus.props.pc2vrDashboard=true;assert.equal(nativeStatus.render().children[1].attributes.showUndo,false);
 const obj=new Objectives();obj.props={};assert.equal(obj.render().children[0].nodeName,'constraint');obj.props.pc2vrDashboard=true;assert.equal(obj.render().children[0].nodeName,'native-objective','unwrap native objective constraint only inside wrist');
 await frame();const layout=last('dashboard-layout'),q=new URLSearchParams(layout.url.split('?')[1]),seq=Number(q.get('seq'));assert.equal(q.get('w'),'36000');assert.equal(q.get('h'),'16000');
 await reply(last('dashboard-click'),{ok:true,enabled:true,owner:17,seq,x:100,y:450});assert.deepEqual(events,['mousedown']);await frame();assert.deepEqual(events,['mousedown','mouseup']);await reply(last('dashboard-validate'),{ok:true,allow:true});assert.equal(events.at(-1),'click');
 const buildSeq=Number(new URLSearchParams(last('build-layout').url.split('?')[1]).get('seq'));const delivered=events.length;
 await reply(last('build-click'),{ok:true,enabled:true,owner:9,seq:buildSeq,x:100,y:450});await frame();await reply(last('build-validate'),{ok:true,allow:true});assert.deepEqual(events.slice(delivered),['mousedown','mouseup','click'],'actual combined owner routes dashboard clicks using its DOM hit');
 await reply(last('build-click'),{ok:true,enabled:true,owner:9,seq:buildSeq,x:100,y:450,scroll:1});await reply(last('build-validate'),{ok:true,allow:true});assert.deepEqual(events.at(-1),['wheel',1],'combined owner sends dashboard wheel input without a catalog lookup');
 for(const state of [{management:true},{gameMenuVisible:true},{toolsPanelMode:'Placement'}]){
  const before={...hud.state};Object.assign(hud.state,state);hud.forceUpdate();assert.equal(dashboard(hud).attributes.style.display,'none');hud.state=before;
 }
 hud.forceUpdate();const updates=nativeUpdate;ctx.window._pc2vrWrist.selected=true;for(const fn of ctx.window._pc2vrWrist.listeners)fn();assert.equal(root.style.display,'block','open wheel keeps dashboard priority over the selected ride');assert.equal(nativeUpdate,updates,'selection lifecycle does not reenter native HUD diff and remount the ride panel');ctx.window._pc2vrWrist.selected=false;for(const fn of ctx.window._pc2vrWrist.listeners)fn();assert.equal(root.style.display,'block');assert.equal(nativeUpdate,updates,'selection close does not rebuild native components');
 hud.forceUpdate();await frame();const recent=Number(new URLSearchParams(last('dashboard-layout').url.split('?')[1]).get('seq'));
 await reply(last('dashboard-click'),{ok:true,enabled:true,owner:17,seq:recent,x:100,y:450});await frame();await reply(last('dashboard-validate'),{ok:true,allow:false});assert.equal(events.at(-1),'mouseleave','modal veto cancels committed click');
 const popupOwner={isConnected:true},popup={isConnected:true,getBoundingClientRect:()=>box(400,50,100,600)};wheelRoot.contains=n=>n===popupOwner;ctx.window._pc2vrRidePopups.entries.set('filter',{owner:popupOwner,popup});for(const fn of ctx.window._pc2vrRidePopups.listeners)fn();for(let i=0;i<6;i++)await frame();assert.equal(root.style.top,'668px','dashboard moves below measured catalog popups so pointer ownership cannot overlap');ctx.window._pc2vrRidePopups.entries.delete('filter');for(const fn of ctx.window._pc2vrRidePopups.listeners)fn();for(let i=0;i<6;i++)await frame();assert.equal(root.style.top,'428px');
 browser.handleInput({inputName:'cancel',button:{isPressed:()=>true}});for(let i=0;i<4;i++)await frame();assert.equal(root.style.display,'none');assert.equal(ctx.window._pc2vrWrist.dashboardBox,null);assert.equal(ctx.window._pc2vrWrist.build,null,'native B closes both wheel and dashboard');
 browser.props.mode='Expanded';browser.forceUpdate();for(let i=0;i<6;i++)await frame();assert.equal(root.style.display,'block');assert.equal(nativeMount,1,'reopening wheel retains singleton objective model');
 await reply(last('dashboard-click'),{ok:true,enabled:false,cancelled:true});assert.equal(dashboard(hud).attributes.className,'','configuration off restores native layout');
 assert.equal(hud.tree.attributes.className,'HUD_root');await frame();for(const [selector,node] of chromeNodes)assert.equal(node.style.display,selector==='.HUD_promptsList'?'flex':'','configuration off restores each original display value');
 assert.deepEqual(dashboard(hud).children.map(n=>n.nodeName),initialChildren,'native modules retain the same parent and child slots across enable/disable');
 await frame();await frame();await frame();assert.equal(styledNode.style.transform,'translate(-50%, 0%)','configuration off restores original native layout');assert.equal(styledNode.style.color,'white','unrelated native inline styles retained');const count=requests.length,queries=styleQueries;await frame();await frame();assert.equal(requests.length,count,'no frame polling when layout unchanged');assert.equal(styleQueries,queries,'no idle descendant scans');
 browser.componentWillUnmount();hud.componentWillUnmount();assert.equal(nativeUnmount,1);assert.ok(nativeUpdate>0);assert.equal(ctx.window._pc2vrWrist.listeners.size,0);assert.equal(ctx.window._pc2vrWrist.buildListeners.size,0);assert.equal(ctx.window._pc2vrWrist.dashboardListeners.size,0);assert.equal(ctx.window._pc2vrRidePopups.listeners.size,0);assert.ok(observers.every(o=>o.disconnected));
 console.log('PASS dashboard: native data/callback ownership, objective expansion/constraint, priority, exact layout, ordered clicks/modal veto, config fallback and lifecycle cleanup');
})().catch(e=>{console.error(e);process.exitCode=1});
