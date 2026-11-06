#include "../src/SpeedCompensation.h"
#include <cassert>
#include <deque>
#include <iostream>
#include <limits>
#include <source_location>

using speed_compensation::Controller;
using speed_compensation::Point;
static aimlock::Memory person(double x,double y=0,bool head=true,bool body=true,double offset=0) {
    aimlock::Memory p;
    if(head)p.part[p.count++]={x-10,y-15,20,20,x,y,0,0,.95f,1};
    if(body)p.part[p.count++]={x-25,y-15,50,100,x,y+offset,0,0,.95f,2};
    return p;
}
static void zero(Point p,std::source_location at=std::source_location::current()) {
    if(p.x!=0 || p.y!=0)std::cerr<<"Expected zero at "<<at.line()<<": "<<p.x<<", "<<p.y<<'\n';
    assert(p.x==0 && p.y==0);
}
struct Metrics { double movingError=0,stoppedError=0,maxStep=0,stoppedBoost=0; };
static Metrics simulate(int amplitude,double hz,double delay,double cameraGain) {
    Controller c; Metrics m; double target=80,camera=0,time=0;
    int movingCount=0,stoppedCount=0; std::uint64_t dispatch=0;
    std::deque<std::pair<double,double>> pending;
    for(int i=0;time<5.5;++i) {
        const double dt=(1./hz)*(i%5==0?1.15:i%5==1?.85:1.);
        time+=dt;
        const double speed=time<.5?0:time<1.5?250:time<2.5?800:time<3.5?100:time<4.5?-500:0;
        target+=speed*dt;
        while(!pending.empty() && pending.front().first<=time) {
            camera+=pending.front().second*cameraGain; pending.pop_front();
        }
        const double error=std::round(target-camera);
        auto p=person(error,0,i%70<50,i%70<30 || i%70>=40,50);
        const auto extra=c.update(amplitude,{error,0},p,i!=0,time*1000,3,dispatch);
        assert(std::abs(extra.y)<1e-8 && std::hypot(extra.x,extra.y)<=amplitude+1e-8);
        const auto move=speed_compensation::calculate({error,0},.3,extra);
        if(move.x || move.y) {++dispatch;pending.emplace_back(time+delay,move.x);}
        m.maxStep=(std::max)(m.maxStep,std::abs(move.x));
        if(time>.8 && time<4.5) {m.movingError+=std::abs(error);++movingCount;}
        if(time>5.1) {m.stoppedError+=std::abs(error);m.stoppedBoost+=std::abs(extra.x);++stoppedCount;}
    }
    m.movingError/=movingCount;m.stoppedError/=stoppedCount;m.stoppedBoost/=stoppedCount;
    return m;
}
struct LeadMetrics {double fast=0,slow=0,reversed=0,stopped=0,maxCommand=0;};
static LeadMetrics simulateLead(int amplitude,double hz,double delay,double gain,double consumePeriod=0) {
    Controller c;LeadMetrics m;double target=80,camera=0,time=0,total=0,nextConsume=0;
    std::uint64_t dispatch=0;int counts[4]{};
    std::deque<std::pair<double,double>> pending;
    for(int i=0;time<7;++i) {
        const double dt=(1./hz)*(i%5==0?1.15:i%5==1?.85:1.);time+=dt;
        const double speed=time<.5?0:time<2?600:time<3.5?200:time<5?-400:0;
        target+=speed*dt;
        while(!pending.empty() && pending.front().first<=time) {
            camera+=pending.front().second*gain;pending.pop_front();
        }
        const double error=std::round(target-camera);
        auto p=person(error,0,i%70<50,i%70<30 || i%70>=40,50);
        const auto extra=c.update(amplitude,{error,0},p,i!=0,time*1000,3,dispatch,{total,0});
        const auto lead=c.leadOffset(),feed=c.feedForward();
        assert(std::hypot(lead.x,lead.y)<=100.00001 && std::hypot(feed.x,feed.y)<=100.00001);
        assert(lead.y==0 && feed.y==0);
        auto move=speed_compensation::calculate({error,0},.3,extra,lead,feed);
        if(std::abs(error)<=7.5 && !c.leading())move={};
        if(time>=nextConsume && (move.x || move.y)) {
            ++dispatch;total+=move.x;pending.emplace_back(time+delay,move.x);nextConsume=time+consumePeriod;
        }
        m.maxCommand=(std::max)(m.maxCommand,std::abs(move.x));
        if(time>1.4 && time<2) {m.fast+=error;++counts[0];}
        if(time>2.9 && time<3.5) {m.slow+=error;++counts[1];}
        if(time>4.4 && time<5) {m.reversed-=error;++counts[2];}
        if(time>6.5) {m.stopped+=std::abs(error);++counts[3];}
    }
    m.fast/=counts[0];m.slow/=counts[1];m.reversed/=counts[2];m.stopped/=counts[3];return m;
}
int main() {
    std::cout<<std::unitbuf;
    Controller c;Point extra;
    zero(c.update(20,{30,0},person(30),false,1000,3,0));
    zero(c.update(20,{30,0},person(30),true,1010,3,1));
    zero(c.update(20,{30,0},person(30),true,1020,3,2));
    extra=c.update(20,{30,0},person(30),true,1030,3,3);
    assert(extra.x>0 && extra.x<3);
    double early=extra.x;
    for(int i=4;i<60;++i) extra=c.update(20,{30,0},person(30,0,i<20 || i>=30,i<40 || i>=50,65),true,1000+i*10.,3,i);
    assert(extra.x>early && extra.x<20 && extra.y==0);
    const auto boosted=speed_compensation::calculate({300,0},.618,extra);
    assert(boosted.x>coefficient_move::calculate({300,0},.618).x); // Actually breaks the old far-distance cap.
    zero(c.update(20,{-10,0},person(-10),true,1600,3,60)); // No old-direction shove.
    zero(c.update(20,{.5,.5},person(.5,.5),true,1610,3,61)); // Near-center noise.
    zero(c.update(20,{10,0},person(10),true,1610,3,62)); // Duplicate sample.
    zero(c.update(20,{10,0},person(10),true,1900,3,63)); // Stale sample.
    zero(c.update(20,{10,0},person(10),false,1910,3,64)); // Different person.
    zero(c.update(20,{10,0},{},true,1920,3,65)); // Lost target.
    zero(c.update(20,{10,0},person(10),true,1930,3,65));
    for(int i=1;i<50;++i) zero(c.update(20,{30,0},person(30),true,1930+i*10.,3,65)); // Consumer blocked.
    for(int i=0;i<50;++i) zero(c.update(0,{30,0},person(30),true,3000+i*10.,3,i));
    zero(c.update(20,{10,0},person(10),true,std::numeric_limits<double>::quiet_NaN(),3,100));
    zero(c.update(20,{10,0},person(10),true,4000,std::numeric_limits<double>::infinity(),100));
    // 250 Hz inference with one trajectory batch consumed every 12 ms.
    c.reset();
    for(int i=0;i<120;++i) {
        extra=c.update(20,{30,0},person(30),i!=0,4000+i*4.,2,i/3);
        if(i>90)assert(extra.x>10 && extra.x<=20);
    }
    for(int i=1;i<=30;++i) {
        extra=c.update(20,{30,0},person(30),true,4476+i*4.,2,39);
        if(i>=25)zero(extra); // A stalled consumer retires compensation rather than storing a burst.
    }
    for(double x:{-300.,-20.,0.,20.,300.}) {
        const auto a=speed_compensation::calculate({x,5},.618,{}), b=coefficient_move::calculate({x,5},.618);
        assert(a.x==b.x && a.y==b.y);
    }
    // A stationary target already converging quickly must not acquire a boost.
    c.reset();
    for(int i=0;i<8;++i) {const double e=100*std::pow(.6,i);zero(c.update(30,{e,0},person(e),i!=0,5000+i*10.,3,i));}
    // Compare measured closed-loop errors including command latency, mouse gain,
    // variable frame intervals, head/body dropout, speed changes and stopping.
    for(double hz:{60.,120.,240.})for(double delay:{0.,.01,.02})for(double gain:{.5,1.,1.4}) {
        const auto base=simulate(0,hz,delay,gain), improved=simulate(15,hz,delay,gain);
        std::cout<<"hz="<<hz<<" delay="<<delay<<" gain="<<gain<<" error="<<base.movingError<<" -> "<<improved.movingError<<" stopped="<<base.stoppedError<<" -> "<<improved.stoppedError<<'\n';
        if(base.stoppedError<=2) {
            assert(improved.movingError<base.movingError);
            assert(improved.stoppedError<=2);
        } else {
            assert(improved.movingError<=base.movingError);
            assert(improved.stoppedBoost<.1); // Existing unstable base gain must not be amplified.
        }
        assert(improved.maxStep<=35);
    }
    std::cout<<"PASS bounded compensation, no frame/consumer windup, aligned part handover, reset, and 27 delayed closed-loop cases\n";
    for(double hz:{60.,120.,240.})for(double gain:{.5,1.,1.4}) {
        const auto m=simulateLead(100,hz,.01,gain);
        std::cout<<"LEAD hz="<<hz<<" gain="<<gain<<" fast="<<m.fast<<" slow="<<m.slow<<" reversed="<<m.reversed<<" stopped="<<m.stopped<<" max="<<m.maxCommand<<'\n';
        assert(m.fast<-5 && m.slow<-1 && m.reversed<-3);
        assert(m.stopped<=8 && m.maxCommand<70);
    }
    const auto asynchronous=simulateLead(100,240,.01,1,.012);
    std::cout<<"ASYNC fast="<<asynchronous.fast<<" slow="<<asynchronous.slow<<" reversed="<<asynchronous.reversed<<" stopped="<<asynchronous.stopped<<'\n';
    assert(asynchronous.fast<-5 && asynchronous.slow<-1 && asynchronous.reversed<-3 && asynchronous.stopped<=8);
    const auto middle=simulateLead(75,120,.01,1),full=simulateLead(100,120,.01,1);
    assert(middle.fast>full.fast && full.fast<-5); // Sliding upward progressively extends lead.
    c.reset();
    for(int i=0;i<100;++i)c.update(100,{20,0},person(20),i!=0,10000+i*10.,3,i,{i*6.,0});
    const double established=c.leadOffset().x;
    assert(established>20 && c.leading());
    c.update(100,{-20,0},person(-20,0,true,false),true,11000,3,100,{600,0});
    c.update(100,{-20,0},person(-20,0,false,true,65),true,11010,3,101,{606,0});
    assert(c.leadOffset().x>20 && std::abs(c.leadOffset().x-established)<2 && c.leadOffset().y==0);
    auto forward=speed_compensation::calculate({-20,0},.3,{},c.leadOffset(),c.feedForward());
    assert(forward.x>0); // Crosshair is already ahead, and still follows motion to the right.
    c.update(100,{20,0},{},true,11020,3,102,{612,0});
    zero(c.leadOffset());zero(c.feedForward());assert(!c.leading());
    c.update(100,{20,0},person(20),false,11030,3,103,{618,0});
    zero(c.leadOffset());zero(c.feedForward());
    // Visible motion inside the dead zone can start a lead before the first command.
    c.reset();for(int i=0;i<10;++i)c.update(100,{i*1.,0},person(i*1.),i!=0,12000+i*4.,2,0,{});
    assert(c.leading() && c.leadOffset().x>0);
    // Acquiring a target off to the left must not label that initial pull as
    // sustained leftward motion while the observed error is rapidly closing.
    c.reset();
    for(int i=0;i<40;++i) {
        const double e=-300+i*6.;c.update(100,{e,0},person(e),i!=0,13000+i*10.,3,i,{-i*5.,0});
        assert(!c.leading());
    }
    std::cout<<"PASS sustained lead, slowdown, reversal, asynchronous dispatch, part-only handover and loss cleanup\n";
    // Every enabled value participates in prediction, including 1 and the old
    // 49/50/51 boundary. A steady observed tracking rate must not accumulate
    // extra lead merely because tracking continues for another eight seconds.
    double previousDistance=0,distances[101]{};
    for(int amplitude=1;amplitude<=100;++amplitude) {
        c.reset();Point atTwoSeconds;
        for(int i=0;i<=1000;++i) {
            const auto boost=c.update(amplitude,{20,0},person(20),i!=0,20000+i*10.,3,i,{i*6.,0});
            assert(std::hypot(boost.x,boost.y)<=amplitude+1e-8);
            if(i==200)atTwoSeconds=c.leadOffset();
        }
        const auto steady=c.leadOffset();
        assert(c.leading() && steady.x>previousDistance && steady.y==0);
        assert(std::abs(steady.x-atTwoSeconds.x)<.001);
        distances[amplitude]=steady.x;previousDistance=steady.x;
        // Halving the sustained tracking speed reduces, rather than stores,
        // the previous prediction distance at every slider value.
        for(int i=1;i<=200;++i)c.update(amplitude,{20,0},person(20),true,30000+i*10.,3,1000+i,{6000+i*3.,0});
        assert(c.leadOffset().x>steady.x*.49 && c.leadOffset().x<steady.x*.51);
        c.update(amplitude,{20,0},{},true,32010,3,1201,{6603,0});
        zero(c.leadOffset());zero(c.feedForward());assert(!c.leading());
    }
    assert(std::abs((distances[51]-distances[50])-(distances[50]-distances[49]))<1e-8);
    // Closed-loop output, not just the prediction offset: leading is already
    // possible below 50, and small changes around 50 do not switch modes.
    for(double hz:{60.,120.,240.}) {
        double previousError=1e9;
        for(int amplitude:{1,15,25,49,50,51,75,100}) {
            const auto m=simulateLead(amplitude,hz,.01,1);
            std::cout<<"UNIFIED hz="<<hz<<" amplitude="<<amplitude<<" fast="<<m.fast
                <<" slow="<<m.slow<<" reversed="<<m.reversed<<" stopped="<<m.stopped<<'\n';
            assert(m.fast<previousError && m.stopped<=8 && m.maxCommand<70);
            if(amplitude>=49)assert(m.fast<0 && m.slow<0 && m.reversed<0);
            previousError=m.fast;
        }
    }
    assert(simulateLead(25,240,.01,1).fast<0);
    std::cout<<"PASS all 100 strengths, stable-distance prediction, continuous 49/50/51 behavior, low-strength leading and slowdown\n";
    return 0;
}
