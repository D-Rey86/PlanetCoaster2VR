#pragma once
namespace pc2vr::menu::detail {
// The native Layer portals dropdown contents to body. Its existing Focus key
// supplies the real popup element; lifecycle callbacks own its cached reference.
inline constexpr char kRidePopupAppend[]=R"(
(() => {
    const registry=window._pc2vrRidePopups || (window._pc2vrRidePopups={entries:new Map(),listeners:new Set()});
    registry.supported=true;
    const signal=()=>{for(const fn of registry.listeners)fn();};
    const p=DropDownSkin.prototype;
    const mount=p.componentDidMount,update=p.componentDidUpdate,unmount=p.componentWillUnmount;
    function sync(component){
        const old=registry.entries.get(component);
        const owner=component.base;
        const popup=component.props.menuOpen ? Focus.getFocusableBase(component._scrollPaneFK):null;
        if(owner?.isConnected && popup?.isConnected){
            if(old?.owner===owner && old.popup===popup)return;
            let layer=popup.parentElement;
            while(layer && !layer.classList.contains('Layer_root'))layer=layer.parentElement;
            registry.entries.set(component,{owner,popup,layer});signal();
        }else if(registry.entries.delete(component))signal();
    }
    function changed(component){
        sync(component);
        if(component.props.menuOpen && !registry.entries.has(component)){
            const generation=component._pc2vrPopupGeneration=(component._pc2vrPopupGeneration || 0)+1;
            // Same layout completion seam used by native DropDownSkin.
            delayForLayoutComplete(()=>delayForLayoutComplete(()=>{
                if(component._pc2vrPopupGeneration===generation)sync(component);
            }));
        }
    }
    p.componentDidMount=function(...args){const v=mount?.apply(this,args);changed(this);return v;};
    p.componentDidUpdate=function(...args){const v=update?.apply(this,args);changed(this);return v;};
    p.componentWillUnmount=function(...args){
        ++this._pc2vrPopupGeneration;
        if(registry.entries.delete(this))signal();
        return unmount?.apply(this,args);
    };
})();
)";
}
