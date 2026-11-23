#pragma once
namespace pc2vr::menu::detail {
// The native rider menu retains its model, controls and callbacks. Its measured
// bounds use the existing ride-panel geometry, atlas and asynchronous click path.
inline constexpr char kCameraAppend[]=R"(
(() => {
    const p=_CameraSettingsPanel.prototype;
    const mount=p.componentDidMount,update=p.componentDidUpdate,unmount=p.componentWillUnmount;
    function rpc(path){return new Promise((resolve,reject)=>{
        const r=new XMLHttpRequest();r.open('GET','/pc2vr/'+path+(path.includes('?')?'&':'?')+'r='+Date.now(),true);r.timeout=5000;
        r.onload=()=>{try{const v=JSON.parse(r.responseText);if(!v.ok)throw Error('rider menu');resolve(v);}catch(e){reject(e);}};
        r.onerror=r.ontimeout=reject;r.send();
    });}
    const types=new Set(['coaster','flatRide','flatRideNoGuest','guest','kart','staff']);
    p.componentDidMount=function(...args){
        const v=mount?.apply(this,args);
        this._pc2vrCamera=installVRRidePanel(this,rpc('camera-owner.json').then(v=>v.owner),rpc,{
            prefix:'camera',root:()=>this.base,parts:root=>Array.from(root.children),
            eligible:()=>types.has(this.state.type)
        });
        return v;
    };
    p.componentDidUpdate=function(...args){const v=update?.apply(this,args);this._pc2vrCamera?.update();return v;};
    p.componentWillUnmount=function(...args){this._pc2vrCamera?.dispose();return unmount?.apply(this,args);};
})();
)";
}
