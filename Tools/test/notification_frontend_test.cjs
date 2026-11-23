const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const source=fs.readFileSync(path.join(__dirname,'../../Loader/src/notification_bridge.h'),'utf8').split('R"(')[1].split(')";')[0].replace(/^import .*$/gm,'');
const signal=()=>({listeners:new Set(),add(fn){assert.ok(!this.listeners.has(fn),'no duplicate native subscription');this.listeners.add(fn)},remove(fn){this.listeners.delete(fn)},emit(value){for(const fn of [...this.listeners])fn(value)}});
const frames=new Map(),timers=new Map(),actions=[];let next=0,now=1000,changes=0,nativeRenders=0,nativeMounts=0,nativeUnmounts=0,viewUpdates=0;
const h=(nodeName,attributes,...children)=>({nodeName,attributes:attributes||{},children:children.flat(Infinity).filter(c=>c!=null && typeof c!=='boolean')});
class Component{forceUpdate(){viewUpdates++;this.tree=this.render();this.componentDidUpdate?.()}}
class NativeButton{componentWillMount(){nativeMounts++}componentWillUnmount(){nativeUnmounts++}render(){nativeRenders++;return h('native-button',{data:'unchanged'})}forceUpdate(){this.tree=this.render()}}
const important=[],notices=[],queued=[];
const model={onImportantNotificationsChanged:signal(),onNotificationsChanged:signal(),onNotificationIsReadChanged:signal(),onDisplayQueueChanged:signal(),onBeginNewNotification:signal(),
 getNotifications:(importance,filter)=>(importance===0?important:notices).filter(item=>filter==0 || item.type==filter),getQueuedNotifications:()=>queued,
 canShowNotification:(item,filter,importance)=>(filter==0 || item.type==filter) && (importance===0?item.importance>0:item.importance===0),
 setupListeners(){throw Error('native model setup is owned by HUD')},play(){throw Error('native playback is owned by NotificationsModule')},pause(){throw Error('native playback is owned by NotificationsModule')}};
const ctx={preact:{h,Component},window:{},VRNotificationButton:NativeButton,VRNotificationList:'NativeNotificationList',VRNotificationTranslate:x=>'translated:'+x,
 VRDashboardButton:'Button',VRDashboardLiteral:x=>x,Engine:{sendEvent:(...args)=>actions.push(args)},NotificationsDataModel:model,NotificationFilter:{All:0},NotificationListImportance:{Important:0,Notices:1},NotificationImportance:{Alert:2},
 NOTIFICATION_FILTER_DATA:{0:{panelLabel:'All'},1:{panelLabel:'Rides'},2:{panelLabel:'Staff'}},Set,Map,Date:{now:()=>now},
 requestAnimationFrame:fn=>{frames.set(++next,fn);return next},cancelAnimationFrame:id=>frames.delete(id),setTimeout:(fn,ms)=>{timers.set(++next,{fn,at:now+ms});return next},clearTimeout:id=>timers.delete(id)};
vm.runInNewContext(source,ctx);
const settle=()=>{let iterations=0;while(frames.size){assert.ok(++iterations<15,'notification/layout work settles without a frame poll');const work=[...frames.values()];frames.clear();work.forEach(fn=>fn())}};
const advance=ms=>{now+=ms;for(const [id,timer] of [...timers])if(timer.at<=now){timers.delete(id);timer.fn()}settle()};
const nodes=(tree,name)=>{const out=[];if(tree?.nodeName===name)out.push(tree);for(const child of tree?.children || [])if(typeof child==='object')out.push(...nodes(child,name));return out};
const labels=tree=>nodes(tree,'Button').map(n=>n.attributes.label);
const controller=ctx.createVRNotifications(()=>{changes++;ctx.window._pc2vrWrist.setNotificationsHidden(controller.hidden())});
const view=new controller.View();view.componentDidMount();view.forceUpdate();
const original=new NativeButton();original.componentWillMount();assert.equal(original.render().nodeName,'native-button');
controller.context(true,true,false,false);settle();assert.equal(original.tree.attributes.style.display,'none','only the original notification widget presentation is replaced');assert.equal(nativeMounts,1);assert.equal(model.onBeginNewNotification.listeners.size,1);assert.equal(controller.toast(),false);
const item=(id,type=1,importance=1)=>({notificationID:id,type,importance,isRead:false,headerLabel:'message-'+id,interactButtonAvailable:true,context:{native:id},locationId:99});
const first=item(1);important.push(first);model.onBeginNewNotification.emit(first);settle();assert.equal(controller.toast(),true);assert.equal(timers.size,1);
assert.ok(labels(view.tree).includes('View'));assert.ok(labels(view.tree).includes('Dismiss'));assert.ok(JSON.stringify(view.tree).includes('translated:message-1'),'uses full native localized message');
const firstDeadline=[...timers.values()][0].at;now+=1000;const second=item(2);important.push(second);model.onBeginNewNotification.emit(second);settle();assert.equal(timers.size,1);assert.equal([...timers.values()][0].at,firstDeadline,'repeated category does not extend a stream of cards');assert.ok(JSON.stringify(view.tree).includes('2 unread in this category'));
const staff=item(3,2);important.push(staff);model.onBeginNewNotification.emit(staff);settle();assert.ok(JSON.stringify(view.tree).includes('translated:message-2'),'another ordinary category does not stack or replace an active card');
const alert=item(4,2,2);important.push(alert);model.onBeginNewNotification.emit(alert);settle();assert.ok(JSON.stringify(view.tree).includes('translated:message-4'),'a new urgent category can replace an ordinary card');assert.equal(timers.size,1);
nodes(view.tree,'Button').find(n=>n.attributes.label==='View').attributes.onSelect();settle();assert.equal(actions.length,1);assert.equal(actions[0][0],'Notification_Interact');assert.equal(actions[0][1],alert.context,'retains native context object');assert.equal(actions[0][2],99);assert.equal(controller.toast(),false);assert.equal(alert.isRead,false,'card dismissal never fabricates native read state');
model.onBeginNewNotification.emit(item(5));settle();assert.equal(controller.toast(),false,'category cooldown suppresses repeated cards while retaining inbox data');advance(21000);const fresh=item(6);important.push(fresh);model.onBeginNewNotification.emit(fresh);settle();assert.equal(controller.toast(),true);advance(6000);assert.equal(controller.toast(),false,'card expires without a frame timer');assert.equal(timers.size,0);
controller.context(true,true,false,true);model.onBeginNewNotification.emit(item(7,2,2));settle();assert.equal(controller.toast(),false,'selected ride/build placement collapses incoming cards');
controller.context(true,true,true,false);settle();assert.ok(labels(view.tree)[0].startsWith('Notifications ('));nodes(view.tree,'Button')[0].attributes.onSelect();settle();assert.equal(controller.inbox(),true);
let list=nodes(view.tree,'NativeNotificationList')[0];assert.ok(list);assert.equal(list.attributes.items[0],first,'native row objects and actions are reused');assert.equal(list.attributes.items.length,important.length);
list.attributes.onSortChanged(1);settle();assert.equal(nodes(view.tree,'NativeNotificationList')[0].attributes.sortMode,1,'native sort control remains functional');
nodes(view.tree,'Button').find(n=>n.attributes.label.startsWith('Category:')).attributes.onSelect();settle();list=nodes(view.tree,'NativeNotificationList')[0];assert.equal(list.attributes.filter,'1');assert.ok(list.attributes.items.every(item=>item.type===1),'native category selection filters the inbox');
const previous=viewUpdates;first.isRead=true;model.onNotificationIsReadChanged.emit(first);model.onImportantNotificationsChanged.emit();settle();assert.ok(viewUpdates>previous);assert.ok(labels(view.tree)[0].includes(String(important.length-1)),'unread badge follows the native model');
controller.context(true,true,false,false);settle();assert.equal(controller.inbox(),false,'closing the wheel collapses the inbox');assert.equal(controller.toast(),false,'opening inbox does not replay an old card');
controller.context(false,true,false,false);settle();assert.equal(original.tree.nodeName,'native-button','disable restores native widget render');for(const key of ['onImportantNotificationsChanged','onNotificationsChanged','onNotificationIsReadChanged','onDisplayQueueChanged','onBeginNewNotification'])assert.equal(model[key].listeners.size,0);
const idle=changes,updates=viewUpdates;settle();assert.equal(changes,idle);assert.equal(viewUpdates,updates);assert.equal(frames.size,0);assert.equal(timers.size,0);
controller.context(true,true,false,false);settle();controller.dispose();view.componentWillUnmount();original.componentWillUnmount();assert.equal(nativeUnmounts,1);assert.equal(frames.size,0);assert.equal(timers.size,0);assert.equal(ctx.window._pc2vrWrist.notificationsHidden,false);for(const value of Object.values(model))if(value?.listeners)assert.equal(value.listeners.size,0);
assert.ok(nativeRenders>0);console.log('PASS notification native ownership/actions, unread counts, category cooldown/grouping, expiry, busy suppression, inbox filter/sort, disable restoration and idle/lifetime cleanup');
