#pragma once
namespace pc2vr::menu::detail {
// Own code appended beside the verified native component. Resize/lifecycle
// callbacks measure a cached node. One pending XHR sleeps until an actual click.
inline constexpr char kRideAppend[]=R"(
function installVRRidePanel(component,owner,rpc,options={}){
    let alive=true,root=null,pane=null,ruler=null,shadow=null,observer=null,raf=0,sequence=0,last='',active=false,waiting=null,measured=null,popups=[],parts=[];
    const registry=window._pc2vrRidePopups || (window._pc2vrRidePopups={entries:new Map(),listeners:new Set()});
    const prefix=options.prefix || 'ride';
    // InfoPanelModule mounts infoPanelRoot for every nonempty selection context,
    // including the object inspector that remains visible during building edits.
    const eligible=()=>alive && (options.eligible?options.eligible():component.state?.context?.length>0 &&
        !component.state.frozen && !component.props.managementVisible);
    function mouse(node,type,x,y){
        const e=new MouseEvent(type);
        e.initMouseEvent(type,true,true,window,1,0,0,Math.round(x),Math.round(y),false,false,false,false,0,null);
        // Shipping1.52 leaves initMouseEvent coordinates at zero. Own values
        // survive real DOM dispatch (measured), including coordinate-based UI.
        Object.defineProperties(e,{clientX:{value:Math.round(x)},clientY:{value:Math.round(y)}});
        node.dispatchEvent(e);
    }
    function wait(){
        if(!alive || !active || waiting)return;
        owner.then(id=>{
            if(!alive || !active || waiting)return;
            const r=new XMLHttpRequest();waiting=r;
            r.open('GET','/pc2vr/'+prefix+'-click.json?owner='+id+'&r='+Date.now(),true);
            r.onload=()=>{
                if(waiting!==r)return;waiting=null;
                let v;try{v=JSON.parse(r.responseText);}catch(_){publish(false);return;}
                if(!v.ok){publish(false);return;}
                if(!alive || !eligible() || !root?.isConnected)return;
                if(v.cancelled){wait();return;}
                if(v.owner!==id || v.seq!==sequence){wait();return;}
                const box=measured;
                if(!box){wait();return;}
                if(v.x<box.left || v.x>=box.right || v.y<box.top || v.y>=box.bottom){wait();return;}
                const node=document.elementFromPoint(v.x,v.y);
                if(!node || !(root.contains(node) || popups.some(p=>p.popup.contains(node) || p.layer?.contains(node)))){wait();return;}
                mouse(node,'mousedown',v.x,v.y);
                // Native ButtonBehaviour sets its down state asynchronously.
                requestAnimationFrame(()=>{
                    if(!alive || !node.isConnected){schedule();wait();return;}
                    mouse(node,'mouseup',v.x,v.y);
                    // Global DialogApp belongs to a different native UI app.
                    // Validate after the down-state frame before committing a click.
                    rpc(prefix+'-validate.json?owner='+id).then(state=>{
                        if(alive && node.isConnected){
                            mouse(node,state.allow && eligible()?'click':'mouseleave',v.x,v.y);
                        }
                        schedule();wait();
                    }).catch(()=>{if(alive && node.isConnected)mouse(node,'mouseleave',v.x,v.y);publish(false);});
                });
            };
            r.onerror=()=>{if(waiting===r){waiting=null;publish(false);}};
            r.send();
        }).catch(()=>{});
    }
    function paintPadding(){
        if(!pane?.isConnected){pane=root?.querySelector('.Panel_panel');shadow=null;}
        if(!pane)return null;
        if(!shadow){
            const text=window.getComputedStyle(pane).getPropertyValue('box-shadow');
            const lengths=text.match(/-?(?:\d+\.?\d*|\.\d+)(?:px|rem)/g);
            // Unsupported/theme-changed paint retains the native front panel.
            if(!lengths || lengths.length!==4 || text.includes('inset'))return null;
            shadow=lengths;
        }
        let rem=0;
        if(shadow.some(n=>n.endsWith('rem'))){
            if(!ruler?.isConnected){
                ruler=document.createElement('div');
                ruler.setAttribute('style','position:absolute;left:0;top:0;width:10rem;height:0;visibility:hidden;pointer-events:none;overflow:hidden;');
                document.body.appendChild(ruler);observer?.observe(ruler);
            }
            rem=ruler.getBoundingClientRect().width*.1;
            if(!(rem>0))return null;
        }
        const n=shadow.map(v=>parseFloat(v)*(v.endsWith('rem')?rem:1));
        const radius=Math.max(0,n[2]+n[3]);
        // Measured1.52 paint envelope; filter support is added by ComposeEye.
        return [Math.max(0,radius-n[0]),Math.max(0,radius-n[1]),Math.max(0,radius+n[0]),Math.max(0,radius+n[1])];
    }
    function publish(on,box,padding,rootBox){
        const vw=Math.round(window.innerWidth*100),vh=Math.round(window.innerHeight*100);
        on=on && box.left>=0 && box.top>=0 && box.left<window.innerWidth && box.top<window.innerHeight;
        const x=on?Math.max(0,Math.floor(box.left*100)):0,y=on?Math.max(0,Math.floor(box.top*100)):0;
        const right=on?Math.min(vw,Math.ceil(box.right*100)):0,bottom=on?Math.min(vh,Math.ceil(box.bottom*100)):0;
        const pl=on?Math.min(x,Math.ceil(padding[0]*100)):0,pt=on?Math.min(y,Math.ceil(padding[1]*100)):0;
        const pr=on?Math.min(vw-right,Math.ceil(padding[2]*100)):0,pb=on?Math.min(vh-bottom,Math.ceil(padding[3]*100)):0;
        const rx=on?Math.floor(rootBox.left*100):0,ry=on?Math.floor(rootBox.top*100):0,rw=on?Math.ceil(rootBox.right*100)-rx:0;
        const rh=on?Math.min(vh,Math.ceil(rootBox.bottom*100))-ry:0;
        const text='&on='+(on?1:0)+'&x='+x+'&y='+y+'&w='+(right-x)+'&h='+(bottom-y)+'&vw='+vw+'&vh='+vh+'&pl='+pl+'&pt='+pt+'&pr='+pr+'&pb='+pb+'&rx='+rx+'&ry='+ry+'&rw='+rw+'&cx=0&cy=0&cw=0&ch=0&rh='+rh;
        active=on;
        if(!on && waiting){const r=waiting;waiting=null;r.abort();}
        if(text===last){if(on)wait();return;}
        last=text;const seq=++sequence;
        owner.then(id=>rpc(prefix+'-layout.json?owner='+id+'&seq='+seq+text)).then(()=>{if(on)wait();}).catch(()=>{});
    }
    function measure(){
        raf=0;
        if(!eligible() || !registry.supported){publish(false);return;}
        if(!root?.isConnected){
            observer?.disconnect();popups=[];parts=[];root=options.root?options.root():document.getElementById('infoPanelRoot');pane=null;shadow=null;
            if(root && typeof ResizeObserver==='function'){
                observer=new ResizeObserver(schedule);observer.observe(root);
                if(ruler?.isConnected)observer.observe(ruler);
            }
        }
        const next=[];
        for(const p of registry.entries.values())if(p.owner.isConnected && p.popup.isConnected && root?.contains(p.owner))next.push(p);
        for(const p of popups)if(!next.includes(p))observer?.unobserve(p.popup);
        for(const p of next)if(!popups.includes(p))observer?.observe(p.popup);
        popups=next;
        let rootBox=root?.getBoundingClientRect();
        if(root && options.parts){
            // CameraSettings' two direct native Panel children hold the actual
            // paint; their flex wrapper stretches over the whole screen height.
            const nextParts=options.parts(root);
            for(const node of parts)if(!nextParts.includes(node))observer?.unobserve(node);
            for(const node of nextParts)if(!parts.includes(node))observer?.observe(node);
            parts=nextParts;rootBox=null;
            for(const node of parts){const r=node.getBoundingClientRect();if(!(r.width>0 && r.height>0))continue;
                if(!rootBox)rootBox={left:r.left,top:r.top,right:r.right,bottom:r.bottom};
                else{rootBox.left=Math.min(rootBox.left,r.left);rootBox.top=Math.min(rootBox.top,r.top);rootBox.right=Math.max(rootBox.right,r.right);rootBox.bottom=Math.max(rootBox.bottom,r.bottom);}
            }
            if(rootBox){rootBox.width=rootBox.right-rootBox.left;rootBox.height=rootBox.bottom-rootBox.top;}
        }
        const box=rootBox?{left:rootBox.left,top:rootBox.top,right:rootBox.right,bottom:rootBox.bottom}:null;
        if(box)for(const p of popups){
            const r=p.popup.getBoundingClientRect();if(!(r.width>0 && r.height>0))continue;
            box.left=Math.min(box.left,r.left);box.top=Math.min(box.top,r.top);box.right=Math.max(box.right,r.right);box.bottom=Math.max(box.bottom,r.bottom);
        }
        const padding=rootBox && rootBox.width>0 ? paintPadding():null;
        measured=box;
        publish(!!rootBox && rootBox.width>0 && rootBox.height>0 && !!padding,box,padding,rootBox);
    }
    function schedule(){if(alive && !raf)raf=requestAnimationFrame(measure);}
    function transition(e){if(root && root.contains(e.target))schedule();}
    window.addEventListener('resize',schedule);
    document.addEventListener('transitionend',transition);
    registry.listeners.add(schedule);
    Input.onInputMethodChanged.add(schedule);
    schedule();
    return {
        update(){if(!eligible())publish(false);schedule();},
        dispose(){
            alive=false;observer?.disconnect();if(raf)cancelAnimationFrame(raf);
            registry.listeners.delete(schedule);
            window.removeEventListener('resize',schedule);document.removeEventListener('transitionend',transition);
            if(ruler?.parentNode)ruler.parentNode.removeChild(ruler);
            Input.onInputMethodChanged.remove(schedule);publish(false);
        }
    };
}
)";
}
