#pragma once
namespace pc2vr::menu::detail {
// Appended to the verified native Browser module. Original tabs, catalogs,
// data providers and placement callbacks remain mounted and authoritative.
inline constexpr char kBuildAppend[]=R"(
import {Button as VRBuildButton} from '/js/project/components/Button.js';
import {stringLiteral as VRBuildLiteral} from '/js/common/util/LocalisationUtil.js';
(() => {
    const p=_Browser.prototype,render=p.render,mount=p.componentWillMount,update=p.componentDidUpdate,unmount=p.componentWillUnmount;
    const registry=window._pc2vrRidePopups || (window._pc2vrRidePopups={entries:new Map(),listeners:new Set()});
    const wrist=window._pc2vrWrist || (window._pc2vrWrist={selected:false,listeners:new Set()});
    wrist.buildListeners=wrist.buildListeners || new Set();wrist.dashboardListeners=wrist.dashboardListeners || new Set();
    const groups={rides:[1,2,3],scenery:[7,9],tools:[10,11,12]};
    const css=`
    .pc2vr-build .Browser_tabContainer{display:none;}
    .pc2vr-build-wheel{pointer-events:auto;}
    .pc2vr-build .Browser_viewStack{position:relative;bottom:auto;width:100%;height:100%;}
    .pc2vr-build .BrowserSection_root{position:absolute;bottom:0;left:0;right:0;max-height:100%;box-shadow:none;}
    .pc2vr-build .pc2vr-build-button{width:100%;height:100%;min-width:0;min-height:0;border:1px solid #7bc5e3;background-color:#08618b;}
    .pc2vr-build .pc2vr-build-button.disabled{background-color:#324957;}
    `;
    function make(component){
        const s={enabled:VRBuildInitiallyEnabled,alive:true,owner:0,seq:0,last:'',waiting:null,raf:0,root:null,anchor:null,catalogRoot:null,scrollSection:null,scrollList:null,observer:null,popups:[],catalog:false,group:null,expanded:false};
        const active=()=>s.alive && s.enabled && component.props.visible && component.props.mode==='Expanded' &&
            !component.props.hideTabBar && component.props.canOpenBrowser && (!component.props.tools || component.props.tools==='Hidden' || component.props.tools==='ModeIndicator');
        function rpc(path){return new Promise((resolve,reject)=>{const r=new XMLHttpRequest();r.open('GET','/pc2vr/'+path+(path.includes('?')?'&':'?')+'r='+Date.now(),true);r.timeout=5000;r.onload=()=>{try{const v=JSON.parse(r.responseText);if(!v.ok)throw Error('build bridge');resolve(v);}catch(e){reject(e)}};r.onerror=r.ontimeout=reject;r.send();});}
        function invalidate(){if(s.alive){component.forceUpdate();schedule();}}
        s.back=()=>{if(s.catalog){s.catalog=false;s.group=null;invalidate();}else if(s.group){s.group=null;invalidate();}else component.props.onCancel();};
        function mouse(node,type,x,y,scroll=0){const e=new MouseEvent(type);e.initMouseEvent(type,true,true,window,1,0,0,Math.round(x),Math.round(y),false,false,false,false,0,null);Object.defineProperties(e,{clientX:{value:Math.round(x)},clientY:{value:Math.round(y)}});if(scroll)Object.defineProperty(e,'deltaY',{value:scroll});node.dispatchEvent(e);}
        function wait(){
            if(!s.alive || !s.owner || s.waiting)return;
            const r=new XMLHttpRequest();s.waiting=r;r.open('GET','/pc2vr/build-click.json?owner='+s.owner+'&r='+Date.now(),true);
            r.onload=()=>{
              try {
                if(s.waiting!==r)return;s.waiting=null;
                let v;try{v=JSON.parse(r.responseText);if(!v.ok)throw Error('build click');}catch(_){fail();return;}
                if(!s.alive)return;
                if(s.enabled!==v.enabled){s.enabled=v.enabled;s.catalog=false;s.group=null;invalidate();}
                if(v.cancelled || !active() || v.owner!==s.owner || v.seq!==s.seq){wait();return;}
                const node=document.elementFromPoint(v.x,v.y);
                const dashboard=!!node && !!wrist.dashboard?.contains(node);
                if(!node || !(dashboard || s.root?.contains(node) || s.popups.some(p=>p.popup.contains(node)||p.layer?.contains(node)))){wait();return;}
                if(v.scroll){
                    if(dashboard){
                        rpc('build-validate.json?owner='+s.owner).then(state=>{if(state.allow && active() && v.seq===s.seq && node.isConnected)mouse(node,'wheel',v.x,v.y,v.scroll);wait();}).catch(fail);return;
                    }
                    const section=node.closest('.BrowserSection_root');
                    if(!s.catalog || !section || !s.catalogRoot?.contains(section)){wait();return;}
                    if(s.scrollSection!==section || !s.scrollList?.isConnected){s.scrollSection=section;s.scrollList=section.querySelector('.BrowserList_repeater');}
                    const list=s.scrollList;
                    if(!list){wait();return;}
                    rpc('build-validate.json?owner='+s.owner).then(state=>{
                        if(state.allow && active() && s.catalog && v.seq===s.seq && list.isConnected)mouse(list,'wheel',v.x,v.y,v.scroll);
                        wait();
                    }).catch(fail);return;
                }
                mouse(node,'mousedown',v.x,v.y);
                requestAnimationFrame(()=>{
                    if(!s.alive || !node.isConnected){schedule();wait();return;}
                    mouse(node,'mouseup',v.x,v.y);
                    rpc('build-validate.json?owner='+s.owner).then(state=>{
                        if(s.alive && node.isConnected)mouse(node,state.allow && active()?'click':'mouseleave',v.x,v.y);
                        schedule();wait();
                    }).catch(fail);
                });
              } catch(_) {fail();}
            };
            r.onerror=()=>{if(s.waiting===r){s.waiting=null;fail();}};r.send();
        }
        function fail(){s.enabled=false;invalidate();}
        function signalWheel(on,anchor,bottom){
            const old=wrist.build;
            if(on && old && old.bottom===bottom && ['left','top','right','bottom','width','height'].every(k=>old.anchor[k]===anchor[k]))return;
            if(!on && !old)return;
            wrist.build=on?{bottom,anchor:{left:anchor.left,top:anchor.top,right:anchor.right,bottom:anchor.bottom,width:anchor.width,height:anchor.height}}:null;
            for(const fn of wrist.buildListeners)fn();
        }
        function publish(on,box,anchor,catalog){
            if(!s.owner)return;
            const vw=Math.round(window.innerWidth*100),vh=Math.round(window.innerHeight*100);
            on=on && box.left>=0 && box.top>=0 && box.left<window.innerWidth && box.top<window.innerHeight;
            signalWheel(on,anchor,on?box.bottom:0);
            // One existing surface/atlas contains the wheel, catalog and dashboard.
            const dashboard=on?wrist.dashboardBox:null;
            if(dashboard)box={left:Math.min(box.left,dashboard.left),top:Math.min(box.top,dashboard.top),right:Math.max(box.right,dashboard.right),bottom:Math.max(box.bottom,dashboard.bottom)};
            const x=on?Math.floor(box.left*100):0,y=on?Math.floor(box.top*100):0;
            const right=on?Math.min(vw,Math.ceil(box.right*100)):0,bottom=on?Math.min(vh,Math.ceil(box.bottom*100)):0;
            const rx=on?Math.floor(anchor.left*100):0,ry=on?Math.floor(anchor.top*100):0,rw=on?Math.ceil(anchor.right*100)-rx:0;
            const cx=on && catalog?Math.max(x,Math.ceil(catalog.left*100)):0,cy=on && catalog?Math.max(y,Math.ceil(catalog.top*100)):0;
            const cw=on && catalog?Math.max(0,Math.min(right,Math.floor(catalog.right*100))-cx):0,ch=on && catalog?Math.max(0,Math.min(bottom,Math.floor(catalog.bottom*100))-cy):0;
            const text='&on='+(on?1:0)+'&x='+x+'&y='+y+'&w='+(right-x)+'&h='+(bottom-y)+'&vw='+vw+'&vh='+vh+'&pl=0&pt=0&pr=0&pb=0&rx='+rx+'&ry='+ry+'&rw='+rw+'&cx='+cx+'&cy='+cy+'&cw='+cw+'&ch='+ch;
            if(text===s.last)return;s.last=text;
            rpc('build-layout.json?owner='+s.owner+'&seq='+(++s.seq)+text).catch(fail);
        }
        function measure(){
            s.raf=0;if(!active()){publish(false);return;}
            if(!s.root?.isConnected || !s.anchor?.isConnected){
                s.observer?.disconnect();s.popups=[];s.root=component.base;s.anchor=s.root?.querySelector('.pc2vr-build-wheel');s.catalogRoot=s.root?.querySelector('#browserRoot');s.scrollSection=s.scrollList=null;
                if(s.root && s.anchor && typeof ResizeObserver==='function'){s.observer=new ResizeObserver(schedule);s.observer.observe(s.root);s.observer.observe(s.anchor);}
            }
            if(!s.root?.isConnected || !s.anchor?.isConnected){publish(false);return;}
            const next=[];for(const p of registry.entries.values())if(p.owner.isConnected && p.popup.isConnected && s.root.contains(p.owner))next.push(p);
            for(const p of s.popups)if(!next.includes(p))s.observer?.unobserve(p.popup);
            for(const p of next)if(!s.popups.includes(p))s.observer?.observe(p.popup);s.popups=next;
            const r=s.root.getBoundingClientRect(),anchor=s.anchor.getBoundingClientRect();
            const box={left:r.left,top:r.top,right:r.right,bottom:r.bottom};
            for(const p of s.popups){const b=p.popup.getBoundingClientRect();if(b.width>0 && b.height>0){box.left=Math.min(box.left,b.left);box.top=Math.min(box.top,b.top);box.right=Math.max(box.right,b.right);box.bottom=Math.max(box.bottom,b.bottom);}}
            publish(r.width>0 && r.height>0 && anchor.width>0,box,anchor,s.catalog?s.catalogRoot?.getBoundingClientRect():null);
        }
        function schedule(){if(s.alive && !s.raf)s.raf=requestAnimationFrame(measure);}
        s.schedule=schedule;
        s.dispose=()=>{s.alive=false;s.observer?.disconnect();if(s.raf)cancelAnimationFrame(s.raf);registry.listeners.delete(schedule);wrist.dashboardListeners.delete(schedule);window.removeEventListener('resize',invalidate);if(s.waiting){s.waiting.abort();s.waiting=null;}publish(false);};
        registry.listeners.add(schedule);window.addEventListener('resize',invalidate);
        wrist.dashboardListeners.add(schedule);
        rpc('build-owner.json').then(v=>{if(!s.alive)return;s.owner=v.owner;s.enabled=v.enabled;invalidate();wait();}).catch(fail);
        return s;
    }
    function state(component){return component._pc2vrBuild || (component._pc2vrBuild=make(component));}
    p.componentWillMount=function(...args){
        const v=mount?.apply(this,args),s=state(this),input=this.handleInput;
        this.handleInput=e=>{if(s.enabled && this.props.mode==='Expanded' && e.inputName===InputName.Cancel && e.button?.isPressed()){s.back();return true;}return input?.call(this,e);};return v;
    };
    p.render=function(...args){
        const tree=render.apply(this,args),s=state(this),props=this.props;
        if(!s.enabled)return tree;
        const expanded=props.mode==='Expanded';
        if(expanded && !s.expanded){s.catalog=false;s.group=null;}s.expanded=expanded;
        const active=expanded && props.visible && !props.hideTabBar && props.canOpenBrowser && (!props.tools || props.tools==='Hidden' || props.tools==='ModeIndicator');
        const d=window.innerHeight*.36,gap=window.innerWidth*.015,width=s.catalog?window.innerWidth*.98:d;
        // Keep the source below the native upper-left notification stack. Its
        // measured anchor preserves the wheel's existing physical VR placement.
        tree.attributes={...tree.attributes,className:tree.attributes.className+' pc2vr-build',style:{position:'fixed',left:'1vw',top:'20vh',width:width+'px',height:d+'px',display:active?'block':'none'}};
        const native=tree.children[0],stack=native.children[0];
        native.attributes={...native.attributes,style:{position:'absolute',left:(d+gap)+'px',top:0,bottom:'auto',width:Math.max(1,width-d-gap)+'px',height:d+'px',display:s.catalog?'block':'none'}};
        stack.attributes={...stack.attributes,visibleIndex:s.catalog?this.state.tabIndex:-1,focusable:s.catalog && props.focusable};
        // Preserve native catalog mode: tutorials can allow blueprints while hiding raw items.
        for(const section of stack.children)if(section?.attributes){section.attributes={...section.attributes,height:195,onClose:s.back};}
        const tabs=this._browserTabs;
        const leaf=id=>{const index=tabs.findIndex(t=>t.attributes.sectionID===id),a=tabs[index]?.attributes;return a?{id,label:a.label || VRBuildLiteral({8:'Paths',9:'Foliage',10:'Terrain',11:'Demolish',12:'Select'}[id] || 'Build'),icon:a.icon,disabled:a.disabled,choose:()=>{if(a.disabled)return;s.catalog=true;s.group=null;if(a.onButtonSelect)a.onButtonSelect();else this.onChangeTab(index);this.forceUpdate();s.schedule();}}:null;};
        const group=(id,label,icon)=>({id,label:VRBuildLiteral(label),icon,choose:()=>{s.group=id;s.catalog=false;this.forceUpdate();s.schedule();}});
        const items=(s.group?groups[s.group].map(leaf):[leaf(13),leaf(0),group('rides','Rides',tabs.find(t=>t.attributes.sectionID===1)?.attributes.icon),leaf(4),leaf(5),leaf(6),group('scenery','Scenery','img/icons/building.svg'),leaf(8),group('tools','Tools','img/icons/terrain.svg')]).filter(Boolean);
        const buttons=items.map((item,i)=>{
            const angle=-Math.PI/2+i*Math.PI*2/items.length;
            return preact.h('div',{key:item.id,style:{position:'absolute',left:(d*(.5+.35*Math.cos(angle)-.115))+'px',top:(d*(.5+.35*Math.sin(angle)-.075))+'px',width:(d*.23)+'px',height:(d*.15)+'px'}},preact.h(VRBuildButton,{rootClassName:'pc2vr-build-button',label:item.label,icon:item.icon,disabled:!!item.disabled,onSelect:item.choose,modifiers:'centered',focusable:!s.catalog,defaultFocus:i===0}));
        });
        buttons.push(preact.h('div',{style:{position:'absolute',left:'35%',top:'43%',width:'30%',height:'14%'}},preact.h(VRBuildButton,{rootClassName:'pc2vr-build-button',label:VRBuildLiteral(s.group||s.catalog?'Back':'Close'),onSelect:s.back,modifiers:'centered'})));
        tree.children.push(preact.h('style',null,css),preact.h('div',{key:'pc2vr-wheel',className:'pc2vr-build-wheel',style:{position:'absolute',left:0,top:0,width:d+'px',height:d+'px',boxSizing:'border-box',borderRadius:'50%',backgroundColor:'#092f45',border:'1px solid #59b8df'}},buttons));
        return tree;
    };
    p.componentDidUpdate=function(...args){const v=update?.apply(this,args);this._pc2vrBuild?.schedule();return v;};
    p.componentWillUnmount=function(...args){this._pc2vrBuild?.dispose();return unmount?.apply(this,args);};
})();
)";
// Native filter Layer is a portal, so register its measured content beside the
// already-supported dropdown registry. The native Constraint returns its single
// child directly; BrowserFilterPanel ignores rootClassName supplied by callers.
inline constexpr char kBuildLayerAppend[]=R"(
import {Constraint as VRBuildConstraint} from '/js/common/components/Constraint.js';
(() => {
    const registry=window._pc2vrRidePopups || (window._pc2vrRidePopups={entries:new Map(),listeners:new Set()});
    const p=Layer.prototype,mount=p.componentDidMount,update=p.componentDidUpdate,unmount=p.componentWillUnmount;
    const signal=()=>{for(const fn of registry.listeners)fn();};
    const constraintMount=VRBuildConstraint.prototype.componentDidMount;
    VRBuildConstraint.prototype.componentDidMount=function(...args){
        const owner=this.props.target?.base;
        if(this.props.query==='#filtersBtn' && owner?.classList.contains('BrowserSection_root')){
            const apply=this.applyConstraint;
            this.applyConstraint=(...values)=>{const v=apply.apply(this,values);signal();return v;};
        }
        return constraintMount?.apply(this,args);
    };
    function sync(layer){
        const constraint=layer.props.children?.[0]?.attributes;
        const owner=constraint?.target?.base;
        if(constraint?.query!=='#filtersBtn')return;
        if(!owner?.classList.contains('BrowserSection_root'))return;
        const child=layer._childElements[0],old=registry.entries.get(layer);
        if(old?.popup.isConnected)return;
        const popup=child?.classList?.contains('BrowserFilterPanel_root')?child:null;
        if(popup?.isConnected){registry.entries.set(layer,{owner,popup,layer:layer._layerElement});signal();}
    }
    p.componentDidMount=function(...args){const v=mount?.apply(this,args);sync(this);return v;};
    p.componentDidUpdate=function(...args){const v=update?.apply(this,args);sync(this);return v;};
    p.componentWillUnmount=function(...args){if(registry.entries.delete(this))signal();return unmount?.apply(this,args);};
})();
)";
}
