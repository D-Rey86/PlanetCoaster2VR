const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),path=require('node:path');
const header=fs.readFileSync(path.join(__dirname,'../../Loader/src/build_panel_bridge.h'),'utf8');
const source=header.split('R"(')[2].split(')";')[0].replace(/^import .*$/gm,'');
const node=(name)=>({isConnected:true,classList:{contains:x=>x===name}});
class Layer{constructor(owner,query,popup){this.props={children:[{attributes:{target:{base:owner},query}}]};this._childElements=[popup];this._layerElement=node('Layer_root');}}
let applied=0;
class Constraint{constructor(target,query){this.props={target:{base:target},query};this.applyConstraint=()=>++applied;}componentDidMount(){}}
const ctx={Layer,VRBuildConstraint:Constraint,window:{},Map,Set};vm.runInNewContext(source,ctx);
const registry=ctx.window._pc2vrRidePopups;let signals=0;registry.listeners.add(()=>signals++);
const owner=node('BrowserSection_root'),popup=node('BrowserFilterPanel_root');
const layer=new Layer(owner,'#filtersBtn',popup);layer.componentDidMount();
assert.equal(registry.entries.get(layer).popup,popup,'native filter root is registered despite ignored caller rootClassName');
assert.equal(registry.entries.get(layer).owner,owner);assert.equal(signals,1);
layer.componentDidUpdate();assert.equal(signals,1,'connected popup identity remains cached');
const other=new Layer(owner,'#other',popup);other.componentDidMount();assert.equal(registry.entries.has(other),false,'unrelated portals keep native ownership');
layer.componentWillUnmount();assert.equal(registry.entries.has(layer),false);assert.equal(signals,2);
const constraint=new Constraint(owner,'#filtersBtn');constraint.componentDidMount();assert.equal(constraint.applyConstraint(),1);assert.equal(applied,1);assert.equal(signals,3,'native deferred positioning signals a fresh measurement after applying styles');
const unrelated=new Constraint(owner,'#other');unrelated.componentDidMount();unrelated.applyConstraint();assert.equal(applied,2);assert.equal(signals,3,'other native constraints keep their original callback');
console.log('PASS native filter portal root/owner registration, cached identity, unrelated-layer isolation and unmount cleanup');
