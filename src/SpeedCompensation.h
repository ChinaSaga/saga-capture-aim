#pragma once
#include "AimLock.h"
#include "CoefficientMove.h"

namespace speed_compensation {
using Point = coefficient_move::Point;

// One continuous 1..100 strength for catch-up, command-rate feed-forward and
// bounded nominal image lead. Steady tracking speed gives a steady offset;
// neither elapsed tracking time nor accumulated error extends that distance.
// Command rate is not calibrated world velocity.
class Controller {
public:
    void reset() {
        ready_=false; previous_={}; axes_[0]={}; axes_[1]={};
        sawDispatch_=false; lastDispatchTime_=0; dispatchPeriod_=0;
        leaders_[0]={};leaders_[1]={};lead_={};feed_={};lastTotals_={};
    }
    Point leadOffset() const {return lead_;}
    Point feedForward() const {return feed_;}
    bool leading() const {return lead_.x!=0 || lead_.y!=0 || feed_.x!=0 || feed_.y!=0;}

    Point update(int amplitude, Point error, const aimlock::Memory& person,
                 bool sameTarget, double timestampMs, double inferenceMs,
                 std::uint64_t dispatched, Point commandTotals={}) {
        const Point previousLead=lead_;
        lead_={};feed_={};
        amplitude = std::clamp(amplitude, 0, 100);
        if (!amplitude || person.count <= 0 || person.count > 4 ||
            !coefficient_move::finite(error.x) || !coefficient_move::finite(error.y) ||
            !coefficient_move::finite(timestampMs) || !coefficient_move::finite(inferenceMs) || inferenceMs < 0 ||
            !coefficient_move::finite(commandTotals.x) || !coefficient_move::finite(commandTotals.y)) {
            reset(); return {};
        }
        for (int i=0; i<person.count; ++i) {
            const auto& part=person.part[i];
            if (!coefficient_move::finite(part.dx) || !coefficient_move::finite(part.dy) ||
                !coefficient_move::finite(part.w) || !coefficient_move::finite(part.h) || part.w<=0 || part.h<=0) {
                reset(); return {};
            }
        }
        if (!ready_ || !sameTarget || amplitude != amplitude_ ||
            timestampMs < lastTime_ || timestampMs-lastTime_ > 125) {
            reset(); ready_=true; previous_=person; lastTime_=timestampMs;
            amplitude_=amplitude; lastDispatch_=dispatched; lastTotals_=commandTotals; return {};
        }
        const double dt=(timestampMs-lastTime_)*.001;
        if (dt < .0001) return {}; // No duplicate-frame integration.
        int current=-1, old=-1;
        double largest=-1;
        for (int i=0;i<person.count;++i) for (int j=0;j<previous_.count;++j)
            if (person.part[i].lockType==previous_.part[j].lockType &&
                person.part[i].w*person.part[i].h>largest) {
                current=i;old=j;largest=person.part[i].w*person.part[i].h;
            }
        Point delta;
        bool discontinuity=false;
        if (current>=0) {
            delta={person.part[current].dx-previous_.part[old].dx,
                   person.part[current].dy-previous_.part[old].dy};
            const double jump=(std::max)(96.,4*(std::max)(person.part[current].w,previous_.part[old].w));
            if (std::hypot(delta.x,delta.y)>jump) {current=-1;discontinuity=true;}
        }
        // Trajectory batches can span multiple inference frames. Use recent
        // consumer progress, not a per-frame ACK requirement, and retain its
        // measured cadence for the per-command compensation distance.
        if (dispatched!=lastDispatch_) {
            const double interval=sawDispatch_ ? (timestampMs-lastDispatchTime_)*.001 : dt;
            dispatchPeriod_=sawDispatch_ ? .75*dispatchPeriod_+.25*interval : interval;
            lastDispatchTime_=timestampMs;sawDispatch_=true;
        }
        const Point commandRate{(commandTotals.x-lastTotals_.x)/dt,(commandTotals.y-lastTotals_.y)/dt};
        previous_=person;lastTime_=timestampMs;lastDispatch_=dispatched;lastTotals_=commandTotals;
        const double period=(std::max)({dt,dispatchPeriod_,std::clamp(inferenceMs*.001,0.,.125)});
        // Slow tracking can legitimately have several zero-count commands due
        // to rounding or center gating. Do not retire a valid estimate between
        // those sparse steps, while still clearing a blocked consumer promptly.
        const double grace=std::clamp(4*period,.064,.08);
        if (discontinuity || (sawDispatch_ && (timestampMs-lastDispatchTime_)*.001>grace)) {
            axes_[0]={};axes_[1]={};leaders_[0]={};leaders_[1]={};
            sawDispatch_=false;dispatchPeriod_=0;return {};
        }
        const double horizon=std::clamp(2*period,.008,.08);
        const double commandInterval=std::clamp((std::max)(dt,dispatchPeriod_),.0001,.08);
        const double strength=amplitude/100.;
        // A confirmed person's head-only/body-only handover has no shared
        // displacement sample. Keep an existing command-based lead, but do
        // not bootstrap one by subtracting different parts' lock points.
        if (current>=0 || leaders_[0].active)
            advance(leaders_[0],error.x,delta.x,commandRate.x,dt,horizon,sawDispatch_);
        if (current>=0 || leaders_[1].active)
            advance(leaders_[1],error.y,delta.y,commandRate.y,dt,horizon,sawDispatch_);
        // A fixed time horizon maps a steady tracking rate to a steady image
        // offset. No error-sign reset: the observed point may stay behind us.
        lead_={leaders_[0].active?leaders_[0].rate*.04*strength:0,
               leaders_[1].active?leaders_[1].rate*.04*strength:0};
        feed_={leaders_[0].active?leaders_[0].rate*commandInterval*strength:0,
               leaders_[1].active?leaders_[1].rate*commandInterval*strength:0};
        limit(lead_,amplitude);limit(feed_,amplitude);
        if (current<0 || !sawDispatch_) {axes_[0]={};axes_[1]={};return {};}
        // Catch up to the same predicted point used by the base movement.
        // Crossing the observed point must not reverse the boost while the
        // predicted point is still ahead. Reserve the feed-forward share of
        // tracking speed to avoid counting the same velocity twice.
        Point result{step(axes_[0],error.x+lead_.x,delta.x+lead_.x-previousLead.x,dt,horizon,commandInterval,amplitude)*(1-strength),
                     step(axes_[1],error.y+lead_.y,delta.y+lead_.y-previousLead.y,dt,horizon,commandInterval,amplitude)*(1-strength)};
        if (!coefficient_move::finite(result.x) || !coefficient_move::finite(result.y)) {reset();return {};}
        const double length=std::hypot(result.x,result.y);
        if (length>amplitude) {result.x*=amplitude/length;result.y*=amplitude/length;}
        return result;
    }

private:
    struct Leader {double rate=0,trend=0,duration=0,travel=0;int direction=0,samples=0;bool active=false;};
    static void limit(Point& p,double maximum) {
        const double length=std::hypot(p.x,p.y);
        if (length>maximum) {p.x*=maximum/length;p.y*=maximum/length;}
    }
    static void advance(Leader& state,double error,double delta,double commandRate,
                        double dt,double horizon,bool consumed) {
        const double visibleRate=delta/dt;
        state.trend+=(visibleRate-state.trend)*(1-std::exp(-dt/.035));
        const double sample=consumed?commandRate:visibleRate;
        state.rate+=(std::clamp(sample,-10000.,10000.)-state.rate)*(1-std::exp(-dt/.16));
        if (!state.active) {
            const double basis=consumed?state.rate:visibleRate;
            const int direction=basis>5?1:basis<-5?-1:0;
            // Startup requires actual persistent tracking, not just the first
            // pull toward a stationary off-center box. With no command yet,
            // consistent visible motion can bootstrap from inside the dead zone.
            const bool persistent=consumed ? error*direction>0 &&
                std::abs(state.trend)<=(std::max)(30.,std::abs(state.rate)*.35) &&
                std::abs(error)+direction*state.trend*horizon>.75 : std::abs(visibleRate)>20;
            if (!direction || direction!=state.direction) {
                state.duration=0;state.samples=0;state.travel=0;state.direction=direction;
            }
            if (persistent && direction) {
                state.duration+=dt;++state.samples;state.travel+=std::abs(delta);
                const double confirmation=consumed?(std::max)(.04,2*horizon):.024;
                if (state.samples>=3 && state.duration>=confirmation && (consumed || state.travel>=3)) state.active=true;
            } else state.duration=(std::max)(0.,state.duration-dt*.5);
        }
        // Net dispatch tends to zero when a target stops. Using total commands
        // (not the extra compensation alone) avoids a self-sustaining lead bias.
        if (std::abs(state.rate)<5 && std::abs(state.trend)<20) {
            state.rate=0;state.active=false;
        }
    }
    struct Axis {
        int direction=0, samples=0;
        double duration=0, velocity=0, sinceChange=1, cooldown=0;
    };
    static double step(Axis& state,double error,double delta,double dt,double horizon,double commandInterval,int amplitude) {
        constexpr double deadband=.75;
        const double distance=std::abs(error);
        const int direction=error>0?1:-1;
        state.sinceChange+=dt;
        state.cooldown=(std::max)(0.,state.cooldown-dt);
        if (distance<=deadband) {
            state.velocity=0;state.samples=0;state.duration=0;return 0;
        }
        if (state.direction!=direction) {
            const double cooldown=state.direction && state.sinceChange<.1 ? .12 : state.cooldown;
            state={direction,1,0,0,0,cooldown};return 0; // Remove old-direction compensation immediately.
        }
        state.samples=(std::min)(state.samples+1,1000);
        state.duration=(std::min)(state.duration+dt,1.);
        const double projected=distance+direction*delta/dt*horizon;
        double desired=0;
        // At least three same-direction observations, and enough elapsed time.
        // Fast convergence needs no extra push even when the remaining distance is large.
        if (state.cooldown==0 && state.samples>=3 && state.duration>=(std::max)(.016,horizon) && projected>deadband) {
            const double lag=projected-deadband;
            desired=(amplitude/.01)*lag/(lag+8.);
        }
        // Slow attack / fast release. Values represent pixel-equivalent speed;
        // use the measured consumer cadence when forming a mouse command.
        const double tau=desired>state.velocity ? .08+2*horizon : (std::max)(.008,horizon*.5);
        state.velocity+=(desired-state.velocity)*(1-std::exp(-dt/tau));
        // Do not coast toward a point that this trend already predicts crossing.
        if (projected<=deadband) state.velocity=0;
        return direction*(std::min)(double(amplitude),state.velocity*commandInterval);
    }
    bool ready_=false;
    int amplitude_=0;
    double lastTime_=0;
    std::uint64_t lastDispatch_=0;
    bool sawDispatch_=false;
    double lastDispatchTime_=0,dispatchPeriod_=0;
    aimlock::Memory previous_;
    Axis axes_[2];
    Leader leaders_[2];
    Point lead_,feed_,lastTotals_;
};

// Add AFTER the old radial cap: adding distance before that cap cannot speed up
// far-target movement. Disabled compensation is bit-for-bit the original output.
inline Point calculate(Point error,double factor,Point compensation,Point lead={},Point feed={}) {
    if (!coefficient_move::finite(lead.x) || !coefficient_move::finite(lead.y) ||
        !coefficient_move::finite(feed.x) || !coefficient_move::finite(feed.y)) return {};
    const auto base=coefficient_move::calculate({error.x+lead.x,error.y+lead.y},factor);
    if (!coefficient_move::finite(error.x) || !coefficient_move::finite(error.y)) return {};
    if (!coefficient_move::finite(factor) || factor<=0 ||
        !coefficient_move::finite(compensation.x) || !coefficient_move::finite(compensation.y)) return base;
    const double gain=(std::min)(factor,10.);
    return {std::round(base.x+compensation.x*gain+feed.x),std::round(base.y+compensation.y*gain+feed.y)};
}
}
