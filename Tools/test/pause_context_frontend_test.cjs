// Execute the actual appended pause lifecycle observer, without Gameface or UI.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const source=fs.readFileSync('Loader/src/vr_menu.cpp','utf8');
const script=source.split('constexpr char kPauseAppend[] = R"(')[1].split(')";')[0];
const requests=[],calls=[];
class Request {
    open(method,url,async){assert.equal(method,'GET');assert.equal(async,true);this.url=url;}
    send(){requests.push(this);}
    respond(value){this.responseText=JSON.stringify(value);this.onload();}
}
class GameMenuModule {
    constructor(){this.props={visible:false};}
    componentDidMount(){calls.push('mount');return 11;}
    componentWillReceiveProps(props){calls.push(['update',props.visible]);return 12;}
    componentWillUnmount(){calls.push('unmount');return 13;}
}
vm.runInNewContext(script,{XMLHttpRequest:Request,GameMenuModule,Date,Math,Promise});
async function flush(){await new Promise(resolve=>setImmediate(resolve));}
(async()=>{
    assert.match(requests[0].url,/^\/pc2vr\/pause-owner\.json\?r=/);
    const menu=new GameMenuModule();assert.equal(menu.componentDidMount(),11);
    assert.equal(menu.componentWillReceiveProps({visible:true}),12);
    assert.equal(menu.componentWillUnmount(),13);
    assert.equal(requests.length,1,'updates must wait for native owner');
    requests[0].respond({ok:true,owner:7});await flush();
    assert.equal(requests.length,4);
    const states=requests.slice(1).map(r=>new URL(r.url,'https://local.invalid').searchParams);
    assert.deepEqual(states.map(p=>p.get('visible')),['0','1','0']);
    assert.deepEqual(states.map(p=>p.get('seq')),['1','2','3']);
    assert.ok(states.every(p=>p.get('owner')==='7'));
    assert.deepEqual(calls,['mount',['update',true],'unmount']);
    for(const request of requests.slice(1))request.respond({ok:true});await flush();
    console.log('PASS pause mount/update/unmount, original lifecycle preservation and ordered native owner requests');
})().catch(error=>{console.error(error);process.exitCode=1;});
