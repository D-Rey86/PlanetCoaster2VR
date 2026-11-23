const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'../../Loader/src/ride_popup_bridge.h'),'utf8').split('R"(')[1].split(')";')[0];
class DropDownSkin {}
const frames=[],window={};let popup=null,calls=0;
vm.runInNewContext(source,{window,Map,Set,DropDownSkin,Focus:{getFocusableBase:()=>popup},delayForLayoutComplete:fn=>frames.push(fn)});
const registry=window._pc2vrRidePopups;registry.listeners.add(()=>++calls);
const native=new DropDownSkin();native.base={isConnected:true};native.props={menuOpen:false};native.componentDidMount();assert.equal(calls,0);
native.props={menuOpen:true};native.componentDidUpdate();assert.equal(frames.length,1);
const layer={classList:{contains:cls=>cls==='Layer_root'}};popup={isConnected:true,parentElement:layer};
frames.shift()();frames.shift()();assert.equal(calls,1);assert.equal(registry.entries.get(native).popup,popup);assert.equal(registry.entries.get(native).layer,layer);
native.componentDidUpdate();assert.equal(calls,1,'unchanged native popup does no work');assert.equal(frames.length,0);
native.props={menuOpen:false};native.componentDidUpdate();assert.equal(calls,2);assert.equal(registry.entries.size,0);
popup=null;native.props={menuOpen:true};native.componentDidUpdate();native.componentWillUnmount();
popup={isConnected:true,parentElement:layer};frames.shift()();frames.shift()();assert.equal(registry.entries.size,0,'late native layout callback cannot resurrect unmounted popup');
console.log('PASS native dropdown lifecycle, portal ownership, cached identity, close and delayed unmount cancellation');
