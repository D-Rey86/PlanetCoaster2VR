#pragma once
namespace pc2vr::menu::detail {
// Original HUD components keep their data subscriptions and native callbacks.
// Only their layout/ownership changes. One sleeping click request, no UI poll.
inline constexpr char kDashboardAppend[]=R"(
import {Button as VRDashboardButton} from '/js/project/components/Button.js';
import {stringLiteral as VRDashboardLiteral} from '/js/common/util/LocalisationUtil.js';
import {StatusControls as VRDashboardControls} from '/js/project/modules/statusBar/components/StatusControls.js';
(() => {
    const wrist=window._pc2vrWrist || (window._pc2vrWrist={selected:false,listeners:new Set()});
    wrist.buildListeners=wrist.buildListeners || new Set();wrist.dashboardListeners=wrist.dashboardListeners || new Set();
    const popupRegistry=window._pc2vrRidePopups || (window._pc2vrRidePopups={entries:new Map(),listeners:new Set()});
    const layoutRules=[
        [["StatusBar_root"],[["position","relative"],["left","auto"],["right","auto"],["top","auto"],["bottom","auto"],["width","100%"],["height","auto"],["minHeight","0"],["flexDirection","column"],["background","none"],["border","0"],["boxShadow","none"],["padding","0"]]],
        [["StatusBar_pattern"],[["display","none"]]],
        [["StatusBar_borderSpacer"],[["display","none"]]],
        [["TimeStat_borderSpacer"],[["display","none"]]],
        [["StatusControls_borderSpacer"],[["display","none"]]],
        [["StatusControls_promptShim"],[["display","none"]]],
        [["StatusBar_left"],[["position","relative"],["width","100%"],["height","auto"],["flexDirection","column"]]],
        [["StatusBar_leftComponents"],[["position","relative"],["width","100%"],["height","auto"],["flexWrap","wrap"],["alignItems","center"]]],
        [["StatusBar_parkNameLabel"],[["width","100%"],["maxWidth","100%"],["height","auto"],["whiteSpace","nowrap"],["overflow","hidden"],["textOverflow","ellipsis"],["margin","0 0 .3rem"],["padding","0"]]],
        [["StatusBarStat_root"],[["margin","0 .5rem .3rem 0"]]],
        [["TimeStat_root"],[["position","relative"],["width","100%"],["height","auto"],["justifyContent","space-between"],["margin",".3rem 0"],["left","auto"],["right","auto"],["top","auto"],["bottom","auto"],["transform","none"],["minHeight","1.6rem"]]],
        [["StatusControls_root"],[["position","relative"],["width","auto"],["height","auto"],["justifyContent","center"]]],
        [["StatusControls_rightComponents"],[["position","relative"],["width","auto"],["height","auto"],["justifyContent","center"]]],
        [["StatusControls_content"],[["position","relative"],["height","auto"]]],
        [["StatusControls_options"],[["position","relative"],["height","auto"]]],
        [["StatusControls_content","optionMenu"],[["display","none"]]],
        [["pc2vr-dashboard-objectives"],[["width","100%"],["marginTop",".4rem"]]],
        [["ObjectivesModule_root"],[["position","relative"],["left","auto"],["right","auto"],["top","auto"],["bottom","auto"],["width","100%"],["maxWidth","100%"],["height","auto"],["margin","0"],["boxShadow","none"]]],
        [["ObjectivesContainer_root"],[["position","relative"],["left","auto"],["right","auto"],["top","auto"],["bottom","auto"],["width","100%"],["maxWidth","100%"],["height","auto"],["margin","0"],["boxShadow","none"]]],
        [["ObjectivesPinnedModule_root"],[["position","relative"],["left","auto"],["right","auto"],["top","auto"],["bottom","auto"],["width","100%"],["maxWidth","100%"],["height","auto"],["margin","0"],["boxShadow","none"]]],
        [["ObjectivesContainer_contentContainer"],[["position","relative"],["width","100%"],["height","auto"]]],
        [["ObjectivesContainer_content"],[["height","auto"],["maxHeight","30vh"]]],
        [["ObjectivesContainer_modeContent"],[["width","100%"]]],
        [["InputHintsModule_root"],[["display","none"]]]
        ,[["NotificationsList_content"],[["position","relative"],["width","100%"],["height","auto"]]]
        ,[["NotificationsList_contentItems"],[["position","relative"],["width","100%"],["height","auto"]]]
        ,[["NotificationsList_scrollPane"],[["width","100%"],["height","18vh"],["maxHeight","18vh"]]]
        ,[["NotificationsList_repeater"],[["width","100%"],["height","18vh"],["maxHeight","18vh"]]]
        ,[["pc2vr-notice-button"],[["width","100%"],["minWidth","0"],["marginTop",".3rem"]]]
    ];
    function make(component){
        const s={alive:true,enabled:false,baseVisible:false,toastBusy:false,owner:0,seq:0,last:'',root:null,parkRoot:null,scroll:null,scrollMode:'',observer:null,raf:0,waiting:null,popups:[],styled:new Map(),placement:new Map(),styleDirty:true,notices:null};
        // Gameface does not invalidate descendant selectors when an ancestor's
        // class changes. Apply display to the measured native roots themselves.
        // Resolve once; re-resolve only on destruction, root replacement or the
        // native conditional InputHints mount transition. No frame polling/scan.
        const chrome=[
            {selector:'.ManagementMenu_root',mode:'management'},
            {selector:'.HUD_promptsList',mode:'always'},
            {selector:'.Heatmaps_root',mode:'heatmaps'},
            {selector:'.HUD_innerContent .InputHintsModule_root',mode:'hints'},
            {selector:'.CenterPrompt_target',mode:'always'}
        ];
        let chromeRoot=null,managementClosed=true,heatmapsClosed=true,hintsPresent=true;
        function restoreChrome(){for(const entry of chrome)if(entry.saved){if(entry.node?.isConnected)entry.node.style.display=entry.original;entry.saved=false;}}
        s.chromeState=(management,heatmaps,hints)=>{
            managementClosed=management;heatmapsClosed=heatmaps;
            if(hintsPresent!==hints){
                hintsPresent=hints;const entry=chrome[3];
                if(entry.saved && entry.node?.isConnected)entry.node.style.display=entry.original;
                entry.node=null;entry.saved=false;entry.resolved=false;
            }
        };
        function syncChrome(){
            if(!s.enabled){restoreChrome();return;}
            const root=component.base;if(!root)return;
            if(chromeRoot!==root){restoreChrome();chromeRoot=root;for(const entry of chrome){entry.node=null;entry.resolved=false;}}
            for(const entry of chrome){
                if(entry.node && !entry.node.isConnected){entry.node=null;entry.saved=false;entry.resolved=false;}
                if(!entry.resolved){entry.node=entry.mode==='hints' && !hintsPresent?null:root.querySelector(entry.selector);entry.resolved=true;}
                const node=entry.node;if(!node)continue;
                const hidden=entry.mode==='management'?managementClosed:entry.mode==='heatmaps'?heatmapsClosed:true;
                if(hidden){if(!entry.saved){entry.original=node.style.display || '';entry.saved=true;}if(node.style.display!=='none')node.style.display='none';}
                else if(entry.saved){node.style.display=entry.original;entry.saved=false;}
            }
        }
        const active=()=>s.alive && s.enabled && component._pc2vrDashboardVisible;
        const rpc=path=>new Promise((resolve,reject)=>{const r=new XMLHttpRequest();r.open('GET','/pc2vr/'+path+(path.includes('?')?'&':'?')+'r='+Date.now(),true);r.timeout=5000;r.onload=()=>{try{const v=JSON.parse(r.responseText);if(!v.ok)throw Error('dashboard');resolve(v);}catch(e){reject(e)}};r.onerror=r.ontimeout=reject;r.send();});
        // Gameface ignores setAttribute('style', ...); use the same direct style
        // properties as its native renderer. Resolve on component/input changes,
        // retain original properties for disable/unmount, and never scan per frame.
        function restoreStyles(){for(const [node,entry] of s.styled)if(node.isConnected)for(const [name,value] of entry)node.style[name]=value.original;s.styled.clear();s.styleDirty=true;}
        function styleNative(){
            if(!s.styleDirty)return false;s.styleDirty=false;let changed=false;
            for(const [node] of s.styled)if(!node.isConnected)s.styled.delete(node);
            for(const node of s.root.querySelectorAll('div')){
                const classes=(node.className || '').split(/\s+/);
                for(const [required,properties] of layoutRules)if(required.every(c=>classes.includes(c))){
                    let entry=s.styled.get(node);if(!entry){entry=new Map();s.styled.set(node,entry);}
                    for(const [name,value] of properties){
                        let saved=entry.get(name);if(!saved){saved={original:node.style[name] || '',desired:null,applied:null};entry.set(name,saved);}
                        // Native getters normalize values (for example none ->
                        // an empty transform). Compare their actual result so a
                        // focus update cannot invalidate an unchanged click target.
                        if(saved.desired!==value || node.style[name]!==saved.applied){node.style[name]=value;saved.desired=value;saved.applied=node.style[name];changed=true;}
                    }
                }
            }
            return changed;
        }
        function refresh(){s.styleDirty=true;schedule();}
        function invalidate(){if(s.alive){component.forceUpdate();refresh();}}
        // Called inside native InfoPanel lifecycle/focus notifications. A nested
        // HUD diff remounts that panel and resets its layout sequence. Only our
        // cached wrapper needs to change visibility during this transition.
        function selectionVisibility(){
            s.notices?.context(s.enabled,s.baseVisible,!!wrist.build,!!wrist.selected || s.toastBusy);
            component._pc2vrDashboardVisible=s.baseVisible && (!!wrist.build || !!s.notices?.toast());
            if(s.enabled && s.root?.isConnected)s.root.style.display=component._pc2vrDashboardVisible?'block':'none';
            if(s.parkRoot?.isConnected)s.parkRoot.style.display=s.enabled && (!wrist.build || s.notices?.inbox())?'none':'block';
            schedule();
        }
        function fail(){s.enabled=false;restoreChrome();restoreStyles();invalidate();}
        function mouse(node,type,x,y,scroll=0){const e=new MouseEvent(type);e.initMouseEvent(type,true,true,window,1,0,0,Math.round(x),Math.round(y),false,false,false,false,0,null);Object.defineProperties(e,{clientX:{value:Math.round(x)},clientY:{value:Math.round(y)}});if(scroll)Object.defineProperty(e,'deltaY',{value:scroll});node.dispatchEvent(e);}
        function wait(){
            if(!s.alive || !s.owner || s.waiting)return;
            const r=new XMLHttpRequest();s.waiting=r;r.open('GET','/pc2vr/dashboard-click.json?owner='+s.owner+'&r='+Date.now(),true);
            r.onload=()=>{try{
                if(s.waiting!==r)return;s.waiting=null;const v=JSON.parse(r.responseText);if(!v.ok)throw Error('dashboard click');
                if(!s.alive)return;
                if(s.enabled!==v.enabled){s.enabled=v.enabled;invalidate();}
                if(v.cancelled || !active() || v.owner!==s.owner || v.seq!==s.seq){wait();return;}
                const node=document.elementFromPoint(v.x,v.y);
                if(!node || !(s.root?.contains(node) || s.popups.some(p=>p.popup.contains(node)||p.layer?.contains(node)))){wait();return;}
                if(v.scroll){
                    rpc('dashboard-validate.json?owner='+s.owner).then(state=>{if(state.allow && active() && v.seq===s.seq && node.isConnected)mouse(node,'wheel',v.x,v.y,v.scroll);wait();}).catch(fail);return;
                }
                mouse(node,'mousedown',v.x,v.y);
                requestAnimationFrame(()=>{
                    if(!s.alive || !node.isConnected){schedule();wait();return;}
                    mouse(node,'mouseup',v.x,v.y);
                    rpc('dashboard-validate.json?owner='+s.owner).then(state=>{if(s.alive && node.isConnected)mouse(node,state.allow && active()?'click':'mouseleave',v.x,v.y);refresh();wait();}).catch(fail);
                });
            }catch(_){fail();}};
            r.onerror=()=>{if(s.waiting===r){s.waiting=null;fail();}};r.send();
        }
        function publish(on,box,rootBox,scroll){
            if(!s.owner)return;
            const vw=Math.round(window.innerWidth*100),vh=Math.round(window.innerHeight*100);
            on=on && box.left>=0 && box.top>=0 && box.right<=window.innerWidth && box.bottom<=window.innerHeight;
            const combined=on && !!wrist.build,old=wrist.dashboardBox;
            if((combined && (!old || ['left','top','right','bottom'].some(k=>old[k]!==box[k]))) || (!combined && old)){
                wrist.dashboardBox=combined?{left:box.left,top:box.top,right:box.right,bottom:box.bottom}:null;
                for(const fn of wrist.dashboardListeners)fn();
            }
            const x=on?Math.floor(box.left*100):0,y=on?Math.floor(box.top*100):0,w=on?Math.ceil(box.right*100)-x:0,h=on?Math.ceil(box.bottom*100)-y:0;
            const rx=on?Math.floor(rootBox.left*100):0,ry=on?Math.floor(rootBox.top*100):0,rw=on?Math.ceil(rootBox.right*100)-rx:0;
            const cx=on && scroll?Math.max(x,Math.ceil(scroll.left*100)):0,cy=on && scroll?Math.max(y,Math.ceil(scroll.top*100)):0;
            const cw=on && scroll?Math.max(0,Math.min(x+w,Math.floor(scroll.right*100))-cx):0,ch=on && scroll?Math.max(0,Math.min(y+h,Math.floor(scroll.bottom*100))-cy):0;
            const rh=on?Math.ceil(rootBox.bottom*100)-ry:0;
            const text='&on='+(on?1:0)+'&x='+x+'&y='+y+'&w='+w+'&h='+h+'&vw='+vw+'&vh='+vh+'&pl=0&pt=0&pr=0&pb=0&rx='+rx+'&ry='+ry+'&rw='+rw+'&cx='+cx+'&cy='+cy+'&cw='+cw+'&ch='+ch+'&rh='+rh;
            if(text===s.last)return;s.last=text;rpc('dashboard-layout.json?owner='+s.owner+'&seq='+(++s.seq)+text).catch(fail);
        }
)" R"(
        function measure(){
            s.raf=0;syncChrome();
            if(!s.root?.isConnected){s.observer?.disconnect();s.popups=[];s.scroll=null;s.root=component.base?.querySelector('#pc2vr-dashboard');s.parkRoot=s.root?.querySelector('#pc2vr-dashboard-park');s.placement.clear();s.styleDirty=true;if(s.root && typeof ResizeObserver==='function'){s.observer=new ResizeObserver(schedule);s.observer.observe(s.root);}}
            if(s.parkRoot?.isConnected)s.parkRoot.style.display=s.enabled && (!wrist.build || s.notices?.inbox())?'none':'block';
            if(s.enabled && s.root)s.root.style.display=active()?'block':'none';
            if(!active()){if(!s.enabled && s.styled.size)restoreStyles();publish(false);return;}
            const anchor=wrist.build?.anchor,width=anchor?.width || Math.min(window.innerWidth*.42,window.innerHeight*.36);let moved=false;
            if(s.root)for(const [key,value] of [['left',anchor?anchor.left+'px':'1vw'],['top',anchor?(wrist.build.bottom+width*.05)+'px':'20vh'],['width',width+'px']]){
                const saved=s.placement.get(key);
                if(!saved || saved.value!==value || s.root.style[key]!==saved.applied){s.root.style[key]=value;s.placement.set(key,{value,applied:s.root.style[key]});moved=true;}
            }
            if(s.root && (styleNative() || moved)){publish(false);schedule();return;}
            const rootBox=s.root?.getBoundingClientRect();
            if(!rootBox || rootBox.width<=0 || rootBox.height<=0){publish(false);return;}
            const next=[];for(const p of popupRegistry.entries.values())if(p.owner.isConnected && p.popup.isConnected && s.root.contains(p.owner))next.push(p);
            for(const p of s.popups)if(!next.includes(p))s.observer?.unobserve(p.popup);for(const p of next)if(!s.popups.includes(p))s.observer?.observe(p.popup);s.popups=next;
            const box={left:rootBox.left,top:rootBox.top,right:rootBox.right,bottom:rootBox.bottom};
            for(const p of s.popups){const b=p.popup.getBoundingClientRect();if(b.width>0 && b.height>0){box.left=Math.min(box.left,b.left);box.top=Math.min(box.top,b.top);box.right=Math.max(box.right,b.right);box.bottom=Math.max(box.bottom,b.bottom);}}
            const scrollMode=s.notices?.inbox()?'inbox':'park';if(s.scrollMode!==scrollMode){s.scroll=null;s.scrollMode=scrollMode;}
            if(!s.scroll?.isConnected)s.scroll=s.root.querySelector(scrollMode==='inbox'?'.pc2vr-notice-inbox .ScrollPane_root':'#pc2vr-dashboard-park .ScrollPane_root') || (scrollMode==='inbox'?s.root.querySelector('.NotificationsList_repeater'):null);
            publish(true,box,rootBox,s.scroll?.getBoundingClientRect());
        }
        function schedule(){if(s.alive && !s.raf)s.raf=requestAnimationFrame(measure);}
        s.schedule=schedule;s.refresh=refresh;s.invalidate=invalidate;
        s.notices=createVRNotifications(()=>{wrist.setNotificationsHidden(s.notices.hidden());selectionVisibility();refresh();});
        // The wheel owns the combined surface's clicks. Its actual DOM hit test
        // also accepts this subtree and its portals, preserving overlap ordering.
        const input={contains:node=>active() && (s.root?.contains(node) || s.popups.some(p=>p.popup.contains(node)||p.layer?.contains(node)))};
        wrist.dashboard=input;
        s.dispose=()=>{s.notices.dispose();restoreChrome();restoreStyles();publish(false);s.alive=false;if(wrist.dashboard===input)wrist.dashboard=null;s.observer?.disconnect();if(s.raf)cancelAnimationFrame(s.raf);if(s.waiting){const r=s.waiting;s.waiting=null;r.abort();}window.removeEventListener('resize',invalidate);wrist.listeners.delete(selectionVisibility);wrist.buildListeners.delete(selectionVisibility);popupRegistry.listeners.delete(schedule);};
        window.addEventListener('resize',invalidate);wrist.listeners.add(selectionVisibility);popupRegistry.listeners.add(schedule);
        wrist.buildListeners.add(selectionVisibility);
        rpc('dashboard-owner.json').then(v=>{if(!s.alive)return;s.owner=v.owner;s.enabled=v.enabled;invalidate();wait();}).catch(fail);
        return s;
    }
    const p=_HUD.prototype,render=p.render,willMount=p.componentWillMount,mount=p.componentDidMount,update=p.componentDidUpdate,unmount=p.componentWillUnmount;
    // These native modules register singleton actions on mount. Keep the same
    // parent from their first render, including while configuration is disabled.
    p.componentWillMount=function(...args){this._pc2vrDashboard=make(this);return willMount?.apply(this,args);};
    p.componentDidMount=function(...args){const v=mount?.apply(this,args);this._pc2vrDashboard?.schedule();return v;};
    p.render=function(props,state){
        const tree=render.call(this,props,state),s=this._pc2vrDashboard;
        this._pc2vrDashboardVisible=false;
        if(!s || !tree?.children)return tree;
        const statusIndex=tree.children.findIndex(n=>n?.nodeName===StatusBar),objectiveIndex=tree.children.findIndex(n=>n?.nodeName===ObjectivesModule);
        if(statusIndex<0 || objectiveIndex<0)return tree;
        const status=tree.children[statusIndex],objectives=tree.children[objectiveIndex];
        const visible=s.enabled && state.statusBarVisible && !state.gameMenuVisible && !state.fanfareVisible && !state.blueprintCreationVisible &&
            !this.isManagementVisible() && !state.parkExtensionActive && !state.sequencerVisible && !state.helpVisible && !state.helpCategory &&
            (!state.toolsPanelMode || state.toolsPanelMode==='Hidden' || state.toolsPanelMode==='ModeIndicator') && !state.notificationsTarget && !state.challengesVisible;
        s.baseVisible=!!visible;s.toastBusy=!!state.toolsPanelMode && state.toolsPanelMode!=='Hidden';
        s.notices.context(s.enabled,s.baseVisible,!!wrist.build,!!wrist.selected || s.toastBusy);
        this._pc2vrDashboardVisible=!!visible && (!!wrist.build || s.notices.toast());
        status.attributes={...status.attributes,pc2vrDashboard:s.enabled};
        objectives.attributes={...objectives.attributes,pc2vrDashboard:s.enabled,...(s.enabled?{menuFocusable:false,onMenuFocused:()=>{},onClose:(...args)=>{this.onCloseObjectives(...args);if(wrist.build)this.setMenuFocus(HUDMenu.Browser);}}:{})};
        if(s.enabled)for(const child of tree.children)if(child?.nodeName===ScreenBG)child.attributes={...child.attributes,darkenBackground:false};
        const anchor=wrist.build?.anchor,width=anchor?.width || Math.min(window.innerWidth*.42,window.innerHeight*.36);
        // This added control is operated by the laser. Native keyboard/gamepad
        // focus must remain with the park, selected ride, or opened native menu.
        const button=preact.h('div',{style:{display:s.enabled?'block':'none'}},preact.h(VRDashboardButton,{rootClassName:'pc2vr-dashboard-objectives',focusable:false,label:VRDashboardLiteral(state.objectivesVisible?'Close objectives':'Objectives'),disabled:!state.objectivesAvailable,onSelect:()=>{this.onSelectObjectives();if(wrist.build)this.setMenuFocus(HUDMenu.Browser);}}));
        tree.children[statusIndex]=preact.h('div',{key:'pc2vr-dashboard',id:'pc2vr-dashboard',className:s.enabled?'pc2vr-dashboard':'',style:s.enabled?{position:'absolute',left:anchor?anchor.left+'px':'1vw',top:anchor?(wrist.build.bottom+width*.05)+'px':'20vh',width:width+'px',display:this._pc2vrDashboardVisible?'block':'none',pointerEvents:'auto',boxSizing:'border-box',backgroundColor:'#092f45',border:'1px solid #59b8df',borderRadius:'.5rem',padding:'.5rem'}:{position:'static'}},preact.h('div',{id:'pc2vr-dashboard-park',style:{display:s.enabled && (!wrist.build || s.notices.inbox())?'none':'block'}},status,button,objectives),preact.h(s.notices.View,{}));
        // Gameface's Preact diff expects a dense normalized child list.
        tree.children.splice(objectiveIndex,1);
        s.chromeState(state.focusedMenu!==HUDMenu.ManagementMenu,state.heatmapState===0,!state.objectivesVisible && !state.objectivesPinned);
        return tree;
    };
    p.componentDidUpdate=function(...args){const v=update?.apply(this,args);this._pc2vrDashboard?.refresh();return v;};
    p.componentWillUnmount=function(...args){this._pc2vrDashboard?.dispose();return unmount?.apply(this,args);};
    const statusRender=StatusBar.prototype.render;
    StatusBar.prototype.render=function(...args){const tree=statusRender.apply(this,args);if(this.props.pc2vrDashboard){for(const child of tree.children || [])if(child?.nodeName===VRDashboardControls)child.attributes={...child.attributes,showUndo:false,showRedo:false,showCameraMode:false,showToD:false,showWeatherButton:false,needOptionMenuButton:false};}return tree;};
    const objectivesRender=ObjectivesModule.prototype.render;
    ObjectivesModule.prototype.render=function(...args){const tree=objectivesRender.apply(this,args);if(this.props.pc2vrDashboard){for(let i=0;i<(tree.children?.length || 0);++i){const child=tree.children[i];if(child?.attributes?.query==='#objectivesTarget' && child.children?.[0])tree.children[i]=child.children[0];}}return tree;};
})();
)";
}
