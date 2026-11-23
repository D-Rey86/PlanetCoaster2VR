#pragma once

namespace pc2vr::menu::detail {
// Appended only to the verified native InfoPanelModule source. Native lifecycle
// and focus/context events supply state; no frame callback, DOM scan or polling.
inline constexpr char kInfoAppend[]=R"(
(() => {
    let requestId=0,sequence=0,last;
    const nonce=Date.now()+'-'+Math.random();
    function rpc(path){
        return new Promise((resolve,reject)=>{
            const r=new XMLHttpRequest();
            r.open('GET','/pc2vr/'+path+(path.includes('?')?'&':'?')+'r='+nonce+'-'+(++requestId),true);
            r.timeout=5000;
            r.onload=()=>{try{const v=JSON.parse(r.responseText);if(!v.ok)throw Error('selection context');resolve(v);}catch(e){reject(e);}};
            r.onerror=r.ontimeout=()=>reject(Error('selection context'));r.send();
        });
    }
    const owner=rpc('selection-owner.json').then(v=>v.owner);
    function notify(visible,safe){
        const flags=(visible?1:0)|(safe?2:0);
        if(flags===last)return;
        last=flags;const seq=++sequence;
        owner.then(id=>rpc('selection-state.json?owner='+id+'&seq='+seq+'&visible='+(visible?1:0)+'&safe='+(safe?1:0))).catch(()=>{});
    }
    const p=InfoPanelModule.prototype;
    const mount=p.componentDidMount,update=p.componentDidUpdate,unmount=p.componentWillUnmount;
    p.componentDidMount=function(...args){
        const v=mount?.apply(this,args);
        if(typeof installVRRidePanel==='function')this._pc2vrRide=installVRRidePanel(this,owner,rpc);
        this._pc2vrSelectionChanged=()=>{
            const visible=!!this.state?.context?.length;
            const wrist=window._pc2vrWrist;
            if(wrist && wrist.selected!==visible){wrist.selected=visible;for(const fn of wrist.listeners)fn();}
            const safe=visible && !this.state.frozen && !this.state.editModeActive && !this.props.managementVisible &&
                this._controlContext?.isEnabled() && Input.isHighestPriorityInputContext('infoPanel',InputName.Select) &&
                !Input.isTextInputOccurring();
            notify(visible,!!safe);
            this._pc2vrRide?.update();
        };
        Input.onInputContextChanged.add(this._pc2vrSelectionChanged);
        Focus.onChange.add(this._pc2vrSelectionChanged);
        this._pc2vrSelectionChanged();return v;
    };
    p.componentDidUpdate=function(...args){const v=update?.apply(this,args);this._pc2vrSelectionChanged?.();return v;};
    p.componentWillUnmount=function(...args){
        const wrist=window._pc2vrWrist;
        if(wrist?.selected){wrist.selected=false;for(const fn of wrist.listeners)fn();}
        this._pc2vrRide?.dispose();
        Input.onInputContextChanged.remove(this._pc2vrSelectionChanged);
        Focus.onChange.remove(this._pc2vrSelectionChanged);
        try{return unmount?.apply(this,args);}finally{notify(false,false);}
    };
})();
)";

// The native quit/DLC/etc. dialog is a separate app with a separate Input/Focus
// registry. Its real component lifecycle must gate the world-selection handoff.
inline constexpr char kDialogAppend[]=R"(
(() => {
    let requestId=0,sequence=0,last;
    const nonce=Date.now()+'-'+Math.random();
    function rpc(path){
        return new Promise((resolve,reject)=>{
            const r=new XMLHttpRequest();
            r.open('GET','/pc2vr/'+path+(path.includes('?')?'&':'?')+'r='+nonce+'-'+(++requestId),true);
            r.timeout=5000;
            r.onload=()=>{try{const v=JSON.parse(r.responseText);if(!v.ok)throw Error('dialog context');resolve(v);}catch(e){reject(e);}};
            r.onerror=r.ontimeout=()=>reject(Error('dialog context'));r.send();
        });
    }
    const owner=rpc('dialog-owner.json').then(v=>v.owner);
    function notify(visible){
        visible=visible===true;
        if(visible===last)return;
        last=visible;const seq=++sequence;
        owner.then(id=>rpc('dialog-state.json?owner='+id+'&seq='+seq+'&visible='+(visible?1:0)+'&safe=0')).catch(()=>{});
    }
    const p=_DialogApp.prototype;
    const mount=p.componentDidMount,update=p.componentDidUpdate,unmount=p.componentWillUnmount;
    p.componentDidMount=function(...args){const v=mount?.apply(this,args);notify(this.state.visible);return v;};
    p.componentDidUpdate=function(...args){const v=update?.apply(this,args);notify(this.state.visible);return v;};
    p.componentWillUnmount=function(...args){try{return unmount?.apply(this,args);}finally{notify(true);}};
})();
)";
}
