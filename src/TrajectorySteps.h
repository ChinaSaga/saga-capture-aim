#pragma once
#include <cmath>

namespace trajectory_steps {
// Round cumulative endpoints, then subtract counts already sent. This preserves
// the final integer displacement even for sub-count and negative curve segments.
template<class Send,class Pause>
void emit(float x,float y,const float* curve,int groups,Send send,Pause pause) {
    int sentX=0,sentY=0;
    for(int k=0;k<groups;++k) {
        const int index=groups==3?(k==0?4:k==1?7:10):(k+1)*10/groups;
        const int endX=int(std::round(index==10?x:curve[index*2]-curve[0]));
        const int endY=int(std::round(index==10?y:curve[index*2+1]-curve[1]));
        const int dx=endX-sentX,dy=endY-sentY;
        if(dx||dy) {send(dx,dy);if(k+1<groups)pause();}
        sentX=endX;sentY=endY;
    }
}
}
