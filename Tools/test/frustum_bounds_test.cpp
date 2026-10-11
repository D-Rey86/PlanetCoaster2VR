#include "../../Loader/src/frustum_bounds.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

using pc2vr::frustum::Cache;
using Vec=std::array<double,3>;
constexpr double kPi=3.14159265358979323846;
void Check(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
XrQuaternionf Multiply(XrQuaternionf a,XrQuaternionf b){return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
XrQuaternionf Euler(double y,double p,double r){y*=kPi/360;p*=kPi/360;r*=kPi/360;return Multiply(Multiply({0,static_cast<float>(std::sin(y)),0,static_cast<float>(std::cos(y))},{static_cast<float>(std::sin(p)),0,0,static_cast<float>(std::cos(p))}),{0,0,static_cast<float>(std::sin(r)),static_cast<float>(std::cos(r))});}
// Independent reference: quaternion -> axis/angle -> Rodrigues rotations.
Vec Rotate(XrQuaternionf q,Vec v,bool inverse=false){
    const double n=std::sqrt(double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w);
    const double x=q.x/n,y=q.y/n,z=q.z/n,w=q.w/n,axisLength=std::sqrt(x*x+y*y+z*z);
    if(axisLength<1e-15)return v;
    const Vec a{x/axisLength,y/axisLength,z/axisLength};
    const double angle=2*std::atan2(axisLength,w)*(inverse?-1:1),c=std::cos(angle),s=std::sin(angle);
    const double dot=a[0]*v[0]+a[1]*v[1]+a[2]*v[2];
    const Vec cross{a[1]*v[2]-a[2]*v[1],a[2]*v[0]-a[0]*v[2],a[0]*v[1]-a[1]*v[0]};
    return {v[0]*c+cross[0]*s+a[0]*dot*(1-c),v[1]*c+cross[1]*s+a[1]*dot*(1-c),v[2]*c+cross[2]*s+a[2]*dot*(1-c)};
}
bool Oracle(const XrFovf& f,XrQuaternionf eye,XrQuaternionf ref,const XrFovf& b,bool expectValid){
    bool allFront=true;
    for(int iy=0;iy<=30;++iy)for(int ix=0;ix<=30;++ix){
        const double x=std::tan(double(f.angleLeft))+(std::tan(double(f.angleRight))-std::tan(double(f.angleLeft)))*ix/30;
        const double y=std::tan(double(f.angleDown))+(std::tan(double(f.angleUp))-std::tan(double(f.angleDown)))*iy/30;
        const Vec v=Rotate(ref,Rotate(eye,{x,y,-1}),true);
        allFront=allFront && v[2]<0;
        if(expectValid){Check(v[2]<0,"valid bound contains a rear ray");
            const double ax=std::atan2(v[0],-v[2]),ay=std::atan2(v[1],-v[2]);
            const bool covered=ax>=b.angleLeft-3e-10 && ax<=b.angleRight+3e-10 && ay>=b.angleDown-3e-10 && ay<=b.angleUp+3e-10;
            if(!covered)std::fprintf(stderr,"ray %d %d angles %.10g %.10g bounds %.10g %.10g %.10g %.10g input %.10g %.10g %.10g %.10g eye %.10g %.10g %.10g %.10g ref %.10g %.10g %.10g %.10g\n",ix,iy,ax,ay,b.angleLeft,b.angleRight,b.angleUp,b.angleDown,f.angleLeft,f.angleRight,f.angleUp,f.angleDown,eye.x,eye.y,eye.z,eye.w,ref.x,ref.y,ref.z,ref.w);
            Check(covered,"independent dense ray escaped bounds");}
    }
    return allFront;
}
int main(){
    const XrQuaternionf identity{0,0,0,1};const XrFovf f{-0.785398163f,0.785398163f,0.785398163f,-0.785398163f};
    Cache cache;XrFovf out{};
    Check(cache.Get(f,identity,identity,out) && !std::memcmp(&f,&out,sizeof(f)),"parallel baseline must be bit exact");
    Check(cache.Get(f,Euler(10,0,0),identity,out),"10 degree cant");
    Check(std::fabs(out.angleUp*180/kPi-50.95243)<1e-4,"pinned independent Python oracle upper corner");
    Check(std::fabs(out.angleLeft*180/kPi+55)<1e-4,"pinned independent Python oracle left corner");
    Check(Oracle(f,Euler(10,0,0),identity,out,true),"cant grid");
    const XrFovf wide{-80.f*static_cast<float>(kPi/180),55.f*static_cast<float>(kPi/180),50.f*static_cast<float>(kPi/180),-50.f*static_cast<float>(kPi/180)};
    Check(!cache.Get(wide,Euler(20,0,0),identity,out),"rear-plane crossing must be rejected");
    Check(!cache.Get(f,{0,0,0,0},identity,out),"invalid quaternion rejected");
    const XrFovf densityFov[2]{f,f};const uint32_t sameWidth[2]{1100,1100},sameHeight[2]{1100,1100};
    float density{};Check(pc2vr::frustum::PixelDensity(densityFov,sameWidth,sameHeight,density) && density==550.f,"equal runtime sizes preserve baseline density");
    const uint32_t largerRight[2]{1100,1536};Check(pc2vr::frustum::PixelDensity(densityFov,largerRight,sameHeight,density) && density==768.f,"right-eye recommendation must be respected");
    const uint32_t tallerRight[2]{1100,1600};Check(pc2vr::frustum::PixelDensity(densityFov,sameWidth,tallerRight,density) && density==800.f,"right-eye vertical density");
    float horizontal{};Check(pc2vr::frustum::PixelDensity(densityFov,largerRight,tallerRight,density,&horizontal) && density==800.f && horizontal==768.f,"scene and panel respect both eyes with independent axis densities");
    const uint32_t invalidRight[2]{1100,0};Check(!pc2vr::frustum::PixelDensity(densityFov,invalidRight,tallerRight,density,&horizontal) && density==0 && horizontal==0,"invalid recommendations cannot leak a partial density");
    Check(pc2vr::frustum::CameraLimit(2.4f)==2.4f,"valid explicit camera ceiling preserved");
    Check(pc2vr::frustum::CameraLimit(0,false)==130.f*static_cast<float>(kPi)/180.f,"original ceiling defaults to130deg");
    Check(pc2vr::frustum::CameraLimit(0,true)==155.f*static_cast<float>(kPi)/180.f,"expanded ceiling defaults to155deg");
    Check(pc2vr::frustum::CameraLimit(2.4f,false)==2.4f,"explicit ceiling preserved in original mode");
    Check(pc2vr::frustum::CameraLimit(0)==pc2vr::frustum::CameraLimit(static_cast<float>(kPi)),"invalid camera ceilings use safe default");
    XrFovf invalid=f;invalid.angleLeft=std::numeric_limits<float>::quiet_NaN();Check(!cache.Get(invalid,identity,identity,out),"NaN rejected");
    invalid=f;invalid.angleRight=invalid.angleLeft;Check(!cache.Get(invalid,identity,identity,out),"empty FOV rejected");
    std::mt19937 random(20261008);std::uniform_real_distribution<double> angle(-35,35),head(-150,150),extent(25,78);
    int valid=0,rear=0;
    for(int i=0;i<1200;++i){
        XrFovf input{static_cast<float>(-extent(random)*kPi/180),static_cast<float>(extent(random)*kPi/180),static_cast<float>(extent(random)*kPi/180),static_cast<float>(-extent(random)*kPi/180)};
        XrQuaternionf ref=Euler(head(random),angle(random),head(random)),eye=Multiply(ref,Euler(angle(random),angle(random),angle(random)));
        const bool ok=cache.Get(input,eye,ref,out);Check(Oracle(input,eye,ref,out,ok)==ok,"bound validity must match independent ray-plane result");
        if(ok){++valid;const XrFovf prior=out;eye={-eye.x,-eye.y,-eye.z,-eye.w};Check(cache.Get(input,eye,ref,out),"quaternion sign");Check(!std::memcmp(&prior,&out,sizeof(out)),"quaternion sign leaves bounds unchanged");}else ++rear;
    }
    constexpr int iterations=200000;const auto start=std::chrono::steady_clock::now();
    const auto q=Euler(10,5,8);double result=0;
    for(int i=0;i<iterations;++i){cache.Get(f,q,identity,out);result+=out.angleUp;}
    const double cached=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/iterations;
    const auto varyingStart=std::chrono::steady_clock::now();
    for(int i=0;i<iterations;++i){cache.Get(f,Euler(10+(i%2)*.001,5,8),identity,out);result+=out.angleUp;}
    const double varying=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-varyingStart).count()/iterations;
    std::printf("PASS: parallel exact, cant corners, invalid poses, sign, %d front/%d rear randomized dense grids; cached %.3f us, changing %.3f us per eye (checksum %.5f)\n",valid,rear,cached,varying,result);
}
