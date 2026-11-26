// SPDX-License-Identifier: GPL-3.0-or-later
// SIMULATED tables of docs/drafts/LOW_DELAY_MODE.md: g++ -std=c++20 -O2 -Iinclude -Itests/remoteplay/shim -Itests/remoteplay \
//   tools/remoteplay_latency_bench/lowdelay_sim_table.cpp -o /tmp/ld && /tmp/ld <predictMs> <burstPairs>
#include "FgScheduleSim.h"
#include <iostream>
#include <cstdlib>
int main(int argc,char**argv){
  namespace F=fgsim;
  const double pm=argc>1?atof(argv[1]):3.0; const double burst=argc>2?atof(argv[2]):8.0; const double secs=120;
  auto def=[]{F::Fixes f;f.admit=1;f.gridEarly=true;f.robustGuard=2;return f;};
  std::printf("predictMs=%.1f burst=%.0f pairs, seed 7, %.0f s per run\n",pm,burst,secs);
  std::printf("%-16s %-8s %7s %7s %8s %7s %7s %6s %7s %6s %6s %7s\n","set","run","realMs","P95","onGrid1","shown","longMs","gen/s","pred/s","held/s","sw/min","predPr");
  for(const auto& c:F::costSets()){
    const auto base=F::simulate(c,F::Source::Card,F::Config{"d",F::parts(false,false,false,false),false,false,def()},7,secs);
    auto pr=[&](const char* n,const F::Result& r){std::printf("%-16s %-8s %7.2f %7.2f %8.2f %7.2f %7.1f %6.2f %7.2f %6.2f %6.2f %7.3f\n",c.name,n,r.realMeanMs,r.realP95Ms,r.grid.onGrid1msPct(),r.grid.shownPct(),r.grid.longestMs,r.genShownPerS,r.predShownPerS,r.predHeldShownPerS,r.switchesPerMin,r.predictedPairShare);};
    pr("P14",base);
    for(double u:{0.0,0.05,0.10,0.20,0.30,0.50}){
      auto f=def();f.lowDelay=true;f.unusableShare=u;f.unusableBurst=burst;f.predictMs=pm;
      const auto r=F::simulate(c,F::Source::Card,F::Config{"l",F::parts(false,false,false,false),false,false,f},7,secs);
      char n[16];std::snprintf(n,16,"LD u%02d",int(u*100+0.5));pr(n,r);
    }
  }
}
