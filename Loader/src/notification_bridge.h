#pragma once
namespace pc2vr::menu::detail {
// Appended before the dashboard extension in the existing native HUD module.
// Native notification data, row actions and queue playback remain authoritative.
inline constexpr char kNotificationAppend[]=R"(
import {NOTIFICATION_FILTER_DATA, NotificationImportance} from '/js/project/data/NotificationsDataModel.js';
import {NotificationsListContent as VRNotificationList} from '/js/project/modules/notifications/NotificationsModule.js';
import {ManagementMenuNotificationButton as VRNotificationButton} from '/js/project/modules/managementMenu/ManagementMenuNotificationButton.js';
import {translate as VRNotificationTranslate} from '/js/common/core/Localisation.js';
(() => {
    const wrist=window._pc2vrWrist || (window._pc2vrWrist={selected:false,listeners:new Set()});
    const buttons=new Set(),p=VRNotificationButton.prototype,render=p.render,mount=p.componentWillMount,unmount=p.componentWillUnmount;
    p.componentWillMount=function(...args){buttons.add(this);return mount?.apply(this,args);};
    p.componentWillUnmount=function(...args){buttons.delete(this);return unmount?.apply(this,args);};
    p.render=function(...args){return wrist.notificationsHidden?preact.h('div',{style:{display:'none'}}):render.apply(this,args);};
    wrist.setNotificationsHidden=value=>{if(wrist.notificationsHidden===value)return;wrist.notificationsHidden=value;for(const button of buttons)button.forceUpdate();};
})();
function createVRNotifications(changed){
    const model=NotificationsDataModel,wrist=window._pc2vrWrist;
    const filters=Object.keys(NOTIFICATION_FILTER_DATA).filter(key=>key!=NotificationFilter.All);
    const s={enabled:false,park:false,wheel:false,busy:false,open:false,filter:NotificationFilter.All,sort:0,items:[],unread:0,card:null,expires:0,timer:0,raf:0,dirty:false,view:null,alive:true,lastShown:new Map()};
    const signals=[model.onImportantNotificationsChanged,model.onNotificationsChanged,model.onNotificationIsReadChanged,model.onDisplayQueueChanged];
    const list=filter=>(model.getNotifications(NotificationListImportance.Important,filter) || []).concat(model.getNotifications(NotificationListImportance.Notices,filter) || []);
    const matches=(item,filter)=>model.canShowNotification(item,filter,NotificationListImportance.Important) || model.canShowNotification(item,filter,NotificationListImportance.Notices);
    function snapshot(){
        s.items=list(s.filter);
        const all=list(NotificationFilter.All).concat(model.getQueuedNotifications() || []);
        s.unread=0;for(const item of all)if(!item.isRead)s.unread++;
        if(s.card){s.card.unread=0;for(const item of all)if(!item.isRead && matches(item,s.card.filter))s.card.unread++;}
    }
    function clearCard(){if(s.timer)clearTimeout(s.timer);s.timer=0;s.card=null;s.expires=0;}
    function queue(data=false){
        if(!s.alive)return;s.dirty=s.dirty || data;
        if(!s.raf)s.raf=requestAnimationFrame(()=>{s.raf=0;if(s.dirty){s.dirty=false;snapshot();}s.view?.forceUpdate();changed();});
    }
    const dataChanged=()=>queue(true);
    function incoming(item){
        if(!s.alive)return;
        queue(true);
        if(!item || !s.enabled || !s.park || s.wheel || s.busy)return;
        const filter=filters.find(key=>matches(item,key)) ?? NotificationFilter.All;
        const now=Date.now(),key=String(filter),alert=item.importance==NotificationImportance.Alert;
        if(s.card){
            if(s.card.filter==filter){s.card.item=item;return;}
            if(!alert || s.card.item.importance==NotificationImportance.Alert)return;
        }
        const previous=s.lastShown.get(key);
        if(previous!==undefined && now-previous<20000)return;
        clearCard();s.lastShown.set(key,now);s.card={filter,item,unread:0};s.expires=now+6000;
        s.timer=setTimeout(()=>{clearCard();queue();},6000);
    }
    function context(enabled,park,wheel,busy){
        if(!s.alive)return;
        const toggled=s.enabled!==enabled,different=toggled || s.park!==park || s.wheel!==wheel || s.busy!==busy;
        if(!different)return;
        s.enabled=enabled;s.park=park;s.wheel=wheel;s.busy=busy;
        if(toggled){
            for(const signal of signals)signal[enabled?'add':'remove'](dataChanged);
            model.onBeginNewNotification[enabled?'add':'remove'](incoming);
            if(enabled)snapshot();else{s.lastShown.clear();s.open=false;}
        }
        if(!enabled || !park || wheel || busy)clearCard();
        if(!wheel)s.open=false;
        // Schedule outside native HUD/InfoPanel lifecycle notifications. Never
        // reenter the HUD diff or remount its singleton data models.
        queue(toggled);
    }
    function toast(){return s.enabled && s.park && !s.wheel && !s.busy && !!s.card;}
    function inbox(){return s.enabled && s.park && s.wheel && s.open;}
    function toggle(){s.open=!s.open;clearCard();queue(true);}
    function cycle(){const all=[NotificationFilter.All,...filters],index=all.findIndex(key=>key==s.filter);s.filter=all[(index+1)%all.length];queue(true);}
    function dismiss(){clearCard();queue();}
    function interact(){
        const item=s.card?.item;if(!item?.interactButtonAvailable)return;
        // Same native action as ManagementMenuNotification.Notification.onSelect.
        Engine.sendEvent('Notification_Interact',item.context,item.locationId);dismiss();
    }
    function button(label,onSelect,icon){return preact.h(VRDashboardButton,{label:VRDashboardLiteral(label),onSelect,icon,focusable:false,modifiers:'centered',rootClassName:'pc2vr-notice-button'});}
    class View extends preact.Component {
        componentDidMount(){s.view=this;changed();}
        componentDidUpdate(){changed();}
        componentWillUnmount(){if(s.view===this)s.view=null;}
        render(){
            const show=s.enabled && s.park && (s.wheel || toast());
            if(!show)return preact.h('div',{id:'pc2vr-notifications',style:{display:'none'}});
            if(toast()){
                const item=s.card.item,category=NOTIFICATION_FILTER_DATA[s.card.filter];
                return preact.h('div',{id:'pc2vr-notifications',className:'pc2vr-notice-toast',style:{borderLeft:'3px solid '+(item.importance===NotificationImportance.Alert?'#f35070':'#59b8df'),padding:'.5rem',boxSizing:'border-box',color:'#ffffff'}},
                    preact.h('div',{style:{fontSize:'.8rem',marginBottom:'.3rem'}},category?VRNotificationTranslate(category.panelLabel):'Notifications'),
                    preact.h('div',{style:{whiteSpace:'normal',fontSize:'1rem',marginBottom:'.4rem'}},VRNotificationTranslate(item.headerLabel)),
                    preact.h('div',{style:{fontSize:'.75rem',marginBottom:'.4rem'}},s.card.unread+' unread in this category'),
                    preact.h('div',{style:{width:'100%'}},item.interactButtonAvailable?button('View',interact,'/img/icons/takemeto.svg'):null,button('Dismiss',dismiss)));
            }
            const category=NOTIFICATION_FILTER_DATA[s.filter];
            return preact.h('div',{id:'pc2vr-notifications'},
                button((s.open?'Close notifications':'Notifications')+' ('+s.unread+')',toggle,'/img/icons/notifications.svg'),
                s.open?preact.h('div',{className:'pc2vr-notice-inbox'},
                    button('Category: '+(category?VRNotificationTranslate(category.panelLabel):'All'),cycle),
                    preact.h(VRNotificationList,{items:s.items,filter:s.filter,sortMode:s.sort,muted:false,mutedLowPriority:false,mutedAlerts:false,mutedWarnings:false,onSortChanged:value=>{s.sort=value;queue();},buttons:null})):null);
        }
    }
    return {context,toast,inbox,View,
        hidden:()=>s.enabled && s.park,
        dispose(){s.alive=false;clearCard();if(s.raf)cancelAnimationFrame(s.raf);if(s.enabled){for(const signal of signals)signal.remove(dataChanged);model.onBeginNewNotification.remove(incoming);}s.lastShown.clear();wrist.setNotificationsHidden(false);}}
}
)";
}
