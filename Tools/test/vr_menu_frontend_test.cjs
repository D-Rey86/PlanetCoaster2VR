const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync(require('node:path').join(__dirname, '../../Loader/ui/VRMenu.js'), 'utf8');
const requests = [], intervals = new Map(), timeouts = new Map();
let actionReply=null, rejectAction=false, rejectState=false;
let id = 0;
const server = {ok:true, Resolution:100, ExpandedCoverage:0, Foveated:1, UIWidth:55, UIDistance:150, UIHeight:-5,
    UIHorizontal:0, UIFollowHmd:0, nvidia:false, cheeky:null, renderWidth:2096, renderHeight:2096};
class Component {
    constructor(props={}) {this.props=props;this._dirty=1;}
    setState(update) {Object.assign(this.state, update);this._dirty=0;this.renders=(this.renders||0)+1;}
}
class XHR {
    open(method, path) {this.path=path;assert.equal(method,'GET');}
    send() {
        requests.push(this.path);
        let result={ok:true};
        if (this.path.includes('state.json')) result=rejectState?{ok:false,error:'Connection lost'}:{...server};
        if (this.path.includes('set.json')) {
            const query=new URLSearchParams(this.path.split('?')[1]);
            server[query.get('key')]=Number(query.get('value'));
            const nativeNames={CheekyCenterMode:['CenterMode',1],CheekyRoundness:['Roundness',100],
                CheekyTransition:['TransitionWidth',100],CheekyAlignedHeight:['AlignedHeightOffset',100],
                CheekyManualHeight:['HeightOffset',100],CheekyBorder:['AlignmentBorder',1],
                CheekyGazeSmoothing:['GazeSmoothingMs',1],CheekyGazeHold:['GazeHoldMs',1]};
            const key=query.get('key'), mapping=nativeNames[key];
            if(mapping && server.cheeky) server.cheeky.settings[mapping[0]]=Number(query.get('value'))/mapping[1];
        }
        if(this.path.includes('recalibrate.json')) {
            actionReply=()=>{this.responseText=JSON.stringify(rejectAction?{ok:false,error:'Recalibration rejected'}:{ok:true});this.onload();};
            return;
        }
        this.responseText=JSON.stringify(result);this.onload();
    }
}
const context={preact:{Component,h:(type,props,...children)=>({type,props,children})},
    stringLiteral:value=>`[STRING_LITERAL:Value=|${value}|]`,
    OptionsMenuBasePanel:'panel',OptionsMenuBasePanelSection:'section',
    OptionsMenuListItemSlider:'slider',OptionsMenuListItemOnOffStepper:'toggle',Button:'button',
    XMLHttpRequest:XHR,clearInterval:value=>intervals.delete(value),clearTimeout:value=>timeouts.delete(value),
    setInterval:callback=>{intervals.set(++id,callback);return id;},
    setTimeout:callback=>{timeouts.set(++id,callback);return id;}};
vm.runInNewContext(source.replace(/^import .*;\r?\n/gm,'').replace(/^export /gm,'')+
    '\nglobalThis.api={VRSettingsPanel,installVRMenu,trackingStatus};',context);
const {VRSettingsPanel,installVRMenu,trackingStatus}=context.api;
const settle=async()=>{for(let i=0;i<12;++i)await Promise.resolve();};
(async()=>{
    let mounted=0;
    class Native extends Component {
        componentWillMount(){++mounted;this._panelData=[{}];this._tabs=[{}];this._panels=[{}];}
    }
    installVRMenu(Native,context.preact,'tab');installVRMenu(Native,context.preact,'tab');
    const native=new Native({mode:7});native.componentWillMount();
    assert.equal(mounted,1);assert.equal(native._panels.length,2);
    assert.equal(native._panels[1].props.panelIndex,1);assert.equal(native._panels[1].props.mode,7);
    const panel=new VRSettingsPanel({hidden:true});panel.componentDidMount();await settle();
    assert.equal(requests.length,1);assert.equal(intervals.size,0,'hidden tab must not poll');
    assert.equal(panel.state.available,false,'AMD/missing dependency must disable Cheeky controls');
    const findControl=(node,key)=>node && typeof node==='object' && node.props?.key===key ? node :
        (Array.isArray(node) ? node : node?.children || []).map(child=>findControl(child,key)).find(Boolean);
    for(const key of ['CheekyCenterMode','CheekyRoundness','CheekyTransition','CheekyAlignedHeight','CheekyBorder','CheekyGazeSmoothing','CheekyGazeHold']) {
        const control=findControl(panel.render(panel.props,panel.state),key);
        assert.ok(control,`recommended control ${key} exists`);
        assert.equal(control.props.disabled,true,`${key} disabled without Cheeky`);
    }
    const beforeUnavailableAction=requests.length;panel.recalibrate();await settle();
    assert.equal(requests.length,beforeUnavailableAction,'unavailable recalibration sends nothing');
    const findFoveated=node=>node && typeof node==='object' && node.props?.key==='Foveated' ? node :
        (Array.isArray(node) ? node : node?.children || []).map(findFoveated).find(Boolean);
    const foveated=findFoveated(panel.render(panel.props,panel.state));
    const findCoverage=node=>node && typeof node==='object' && node.props?.key==='ExpandedCoverage' ? node :
        (Array.isArray(node) ? node : node?.children || []).map(findCoverage).find(Boolean);
    const coverage=findCoverage(panel.render(panel.props,panel.state));
    assert.ok(coverage,'native menu exposes expanded coverage');
    assert.equal(coverage.props.toggled,false,'original behavior is default');
    assert.equal(coverage.props.disabled,false,'coverage works without NVIDIA/Cheeky');
    coverage.props.onChange(true);await settle();assert.equal(server.ExpandedCoverage,1);
    panel.refresh();await settle();assert.equal(panel.state.values.ExpandedCoverage,1);
    panel.toggle('ExpandedCoverage','test','test').props.onChange(false);await settle();
    assert.equal(server.ExpandedCoverage,0);assert.equal(server.Resolution,100);
    assert.ok(foveated,'native menu exposes foveated rendering');
    assert.equal(foveated.props.disabled,false,'VRS toggle is independent of optional NVIDIA Cheeky');
    assert.equal(foveated.props.toggled,true,'toggle reads current mod setting');
    foveated.props.onChange(false);await settle();assert.equal(server.Foveated,0,'toggle saves immediately');
    panel.refresh();await settle();assert.equal(panel.state.values.Foveated,0,'poll retains applied setting');
    const disabled=panel.toggle('CheekyEnabled','test','test',true);assert.equal(disabled.props.disabled,true);
    assert.match(disabled.props.label,/^\[STRING_LITERAL:Value=\|/,'native components require localization expressions');
    assert.match(panel.slider('UIWidth','test',20,120,1,' degrees','test').props.formatter(55),/^\[STRING_LITERAL:Value=\|/);
    panel.props={hidden:false};panel.componentDidUpdate({hidden:true});await settle();
    assert.equal(intervals.size,1,'one polling timer for visible tab');
    const renderCount=panel.renders;panel.refresh();await settle();
    assert.equal(panel.renders,renderCount,'unchanged state must not rebuild native controls');
    assert.equal(panel._dirty,0,'shipping Preact render flag must stay numeric');
    panel.change('UIWidth',50);panel.refresh();await settle();
    assert.equal(panel.state.values.UIWidth,50,'poll must not overwrite a pending slider value');
    panel.change('UIWidth',45);panel.flush();await settle();
    assert.equal(server.UIWidth,45);assert.equal(requests.filter(x=>x.includes('key=UIWidth')).length,1,
        'slider changes coalesce to the latest value');
    panel.change('UIFollowHmd',1,true);await settle();assert.equal(server.UIFollowHmd,1);
    server.nvidia=true;
    server.cheeky={ready:true,settings:{Enabled:true,CenterMode:0,AutoStereoAlignment:true,
        AlignedHeightOffset:-.15,HeightOffset:.25,Roundness:.4,TransitionWidth:.08,
        AlignmentBorder:false,GazeSmoothingMs:35,GazeHoldMs:500},
        eye_calibration:{status:'Active (shared mono source)'},
        gaze:{layer:true,abi:true,status_flags:108,using_gaze:true,ambiguous:false}};
    const beforeRead=requests.filter(x=>x.includes('set.json')).length;
    panel.refresh();await settle();assert.equal(panel.state.available,true);
    assert.equal(panel.state.values.CheekyAlignedHeight,-15);assert.equal(panel.state.values.CheekyManualHeight,25);
    assert.equal(panel.state.values.CheekyRoundness,40);assert.equal(panel.state.values.CheekyTransition,8);
    assert.equal(panel.state.values.CheekyGazeSmoothing,35);assert.equal(panel.state.values.CheekyGazeHold,500);
    assert.equal(requests.filter(x=>x.includes('set.json')).length,beforeRead,'reading does not overwrite user settings');
    const baseline=JSON.parse(JSON.stringify(server.cheeky));
    const status=(settings={},gaze={})=>trackingStatus({...baseline,settings:{...baseline.settings,CenterMode:1,...settings},gaze:{...baseline.gaze,...gaze}});
    assert.match(trackingStatus(null),/unavailable/i);
    assert.match(trackingStatus(baseline),/fixed center selected/,'fixed mode never claims live gaze');
    assert.match(status(),/Active;/);
    assert.match(status({CenterMode:2}),/Simulated/);
    assert.match(status({Enabled:false}),/foveated DLSS is off/);
    assert.match(status({}, {layer:false}),/adapter unavailable/);
    assert.match(status({}, {abi:false}),/adapter unavailable/);
    assert.match(status({}, {status_flags:104}),/Not provided/);
    assert.match(status({}, {status_flags:236}),/layout unsupported/);
    assert.match(status({}, {status_flags:100}),/not focused/);
    assert.match(status({}, {status_flags:76}),/No valid signal/);
    assert.match(status({}, {status_flags:44}),/eye mapping/);
    assert.match(status({}, {using_gaze:false}),/fresh sample/);
    assert.match(status({}, {ambiguous:true}),/eye mapping/);
    server.cheeky.settings.CenterMode=1;panel.refresh();await settle();
    assert.match(panel.state.tracking,/Active;/);
    rejectState=true;panel.refresh();await settle();
    assert.match(panel.state.tracking,/connection lost/,'failed poll cannot leave an Active status displayed');
    rejectState=false;panel.refresh();await settle();assert.match(panel.state.tracking,/Active;/);
    const rendersBeforeStatus=panel.renders;
    server.cheeky.gaze.using_gaze=false;panel.refresh();await settle();
    assert.ok(panel.renders>rendersBeforeStatus,'status changes update even when settings are unchanged');
    assert.doesNotMatch(panel.state.tracking,/Active;/);
    server.cheeky.settings.CenterMode=2;panel.refresh();await settle();
    assert.equal(findControl(panel.render(panel.props,panel.state),'CheekyCenterMode').props.toggled,false,
        'simulated mode is not displayed as real eye tracking');
    assert.equal(server.cheeky.settings.CenterMode,2,'reading simulated mode preserves it');
    findControl(panel.render(panel.props,panel.state),'CheekyCenterMode').props.onChange(true);await settle();
    assert.equal(server.cheeky.settings.CenterMode,1,'explicit eye-tracking selection replaces simulated mode');
    server.cheeky.settings.AutoStereoAlignment=false;panel.refresh();await settle();
    const manualHeight=findControl(panel.render(panel.props,panel.state),'CheekyManualHeight');
    assert.ok(manualHeight,'manual alignment edits its own height');
    manualHeight.props.onChange(-25);panel.flush();await settle();
    assert.equal(server.cheeky.settings.HeightOffset,-.25);assert.equal(server.cheeky.settings.AlignedHeightOffset,-.15);
    server.cheeky.settings.AutoStereoAlignment=true;panel.refresh();await settle();
    assert.ok(findControl(panel.render(panel.props,panel.state),'CheekyAlignedHeight'));
    panel.change('CheekyAlignedHeight',-5);panel.recalibrate();panel.recalibrate();await settle();
    assert.equal(server.cheeky.settings.AlignedHeightOffset,-.05,'pending geometry applies before recalibration');
    assert.equal(panel.state.recalibrating,true);
    assert.equal(requests.filter(x=>x.includes('recalibrate.json')).length,1,'double activation sends only one action');
    assert.ok(requests.findIndex(x=>x.includes('key=CheekyAlignedHeight'))<requests.findIndex(x=>x.includes('recalibrate.json')));
    actionReply();await settle();assert.equal(panel.state.recalibrating,false);assert.match(panel.state.message,/Markers/);
    rejectAction=true;panel.recalibrate();await settle();actionReply();await settle();
    assert.equal(panel.state.recalibrating,false);assert.match(panel.state.message,/rejected/);
    assert.equal(intervals.size,1,'controls/status add no polling timer');
    panel.props={hidden:true};panel.componentDidUpdate({hidden:false});assert.equal(intervals.size,0);
    panel.change('UIDistance',180);panel.componentWillUnmount();await settle();
    assert.equal(server.UIDistance,180,'closing the menu must flush the final pending edit');
    assert.equal(intervals.size,0);assert.equal(timeouts.size,0,'no timer survives unmount');
    console.log('PASS: native tab insertion, capability gating, signed placement, truthful gaze status, serialized recalibration, coalesced edits and hidden-menu inactivity');
})().catch(error=>{console.error(error);process.exitCode=1;});
