// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Hardware-free tests include production transition helpers and rollback.
// Fake dependencies below record driver effects; no GPU libraries are loaded.

#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cassert>
#include <cstring>
#include <algorithm>
#include <cstdarg>
namespace clock_transition_fixture {
constexpr int VF_NUM_POINTS=128, MAX_GPU_FANS=4;
using gc_u32=unsigned int;
enum LockMode:int {LOCK_MODE_NONE=0,LOCK_MODE_FLATTEN=1,LOCK_MODE_HARD=2};
enum {NVML_SUCCESS=0,NVML_CLOCK_GRAPHICS=0,NVML_CLOCK_SM=1,NVML_CLOCK_MEM=2,
      NVML_CLOCK_ID_CURRENT=0,NVML_TEMPERATURE_GPU=0,
      NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW=0,
      XBAR_PINNED_SYS_ENTRY_INDEX=1,XBAR_PINNED_VIDEO_ENTRY_INDEX=2};
struct Point {unsigned int freq_kHz,volt_uV;};
struct DesiredSettings {
 bool hasLock{},resetOcBeforeApply{},hasGpuOffset{};
 LockMode lockMode{}; unsigned int lockMHz{}; int gpuOffsetMHz{};
 bool hasCurvePoint[VF_NUM_POINTS]{};
 unsigned int curvePointMHz[VF_NUM_POINTS]{};
 bool curvePointFromGpuOffset[VF_NUM_POINTS]{};
};
static bool service_request_replaces_lock_domain(const DesiredSettings* d) {
 return d->hasLock||d->hasGpuOffset||d->resetOcBeforeApply;
}
static void lb_log(const char*,...){}
static void debug_log(const char*,...){}
static void set_last_apply_phase(const char*){}
struct nvmlUtilization_t {unsigned int gpu,memory;};
struct Api {
 int (*setGpuLockedClocks)(int,unsigned int,unsigned int){};
 int (*resetGpuLockedClocks)(int){};
 int (*getClock)(int,int,int,unsigned int*){};
 int (*getTemperature)(int,int,unsigned int*){};
 int (*getUtilization)(int,nvmlUtilization_t*){};
 int (*getPowerUsage)(int,unsigned int*){};
 int (*setPowerLimit)(int,unsigned int){};
 int (*setDefaultFanSpeed)(int,unsigned int){};
 int (*setFanControlPolicy)(int,unsigned int,unsigned int){};
 int (*setFanSpeed)(int,unsigned int,unsigned int){};
 int (*getFanControlPolicy)(int,unsigned int,unsigned int*){};
};
struct LinuxGpuState {
 unsigned int retainedTransitionCeilingMHz{};
 Api nvml{}; int nvmlDevice{}; Point curve[VF_NUM_POINTS]{};
 int freqOffsets[VF_NUM_POINTS]{};
 int powerLimitCurrentmW{},powerLimitDefaultmW{};
};
static unsigned int cap=0, resets=0;
static bool refuseArm=false;
static int setCap(int,unsigned int,unsigned int hi){if(refuseArm)return 1;cap=hi;return 0;}
static int resetCap(int){cap=0;++resets;return 0;}
enum ServiceMutationDomain : gc_u32 {
    SERVICE_MUTATION_DOMAIN_RESET_BASELINE = 1u << 0,
    SERVICE_MUTATION_DOMAIN_GPU_OFFSET = 1u << 1,
    SERVICE_MUTATION_DOMAIN_MEM_OFFSET = 1u << 2,
    SERVICE_MUTATION_DOMAIN_POWER = 1u << 3,
    SERVICE_MUTATION_DOMAIN_VF_CURVE = 1u << 4,
    SERVICE_MUTATION_DOMAIN_LOCK = 1u << 5,
    SERVICE_MUTATION_DOMAIN_FAN = 1u << 6,
    SERVICE_MUTATION_DOMAIN_XBAR = 1u << 7,
    SERVICE_MUTATION_DOMAIN_SYS_CLK = 1u << 8,
    SERVICE_MUTATION_DOMAIN_VIDEO_CLK = 1u << 9,
};
struct LinuxHardwareSnapshot {
    bool valid;
    gc_u32 availableMutationDomains;
    bool gpuOffsetValid;
    bool memOffsetValid;
    bool powerValid;
    bool curveValid;
    bool fanValid;
    bool xbarValid;
    bool sysClkValid;
    bool videoClkValid;
    int gpuOffsetMHz;
    // Display/actual MHz (NVML effective MHz halved at capture, as on Windows).
    int memOffsetMHz;
    unsigned int powerLimitmW;
    int xbarOffsetKhz;
    int xbarMsvddOffsetUv;
    int sysClkOffsetKhz;
    int videoClkOffsetKhz;
    int curveOffsets[VF_NUM_POINTS];
    bool curveMask[VF_NUM_POINTS];
    unsigned int fanCount;
    unsigned int fanPolicy[MAX_GPU_FANS];
    // The *intended* duty per fan, not the measured one.  Rollback has to
    // restore what the driver was told to hold; restoring a measured 0% from a
    // stopped fan would strand the GPU at a duty nobody asked for.
    unsigned int fanTargetPercent[MAX_GPU_FANS];
    bool fanTargetKnown[MAX_GPU_FANS];
};

#include "apply_clock_ceiling_policy.h"
#include "linux_transaction.h"
#include "linux_apply_ceiling.h"
template<class... A> static bool nvml_set_clock_offset(A...){return true;}
static int nvml_mem_effective_mhz_from_display_mhz(int x){return x*2;}
template<class... A> static bool linux_xbar_write_owned(A...){return true;}
template<class... A> static bool linux_xbar_write_entry(A...){return true;}
template<class... A> static bool linux_read_power_limit_pair(A...){return true;}
template<class... A> static bool nvml_read_fan_intent(A...){return true;}
template<class... A> static int nvml_read_fan_measured(A...){return 0;}
template<class... A> static bool fan_manual_write_confirmed(A...){return true;}
static void gc_strlcpy(char* dst,size_t n,const char* s){if(dst&&n)std::snprintf(dst,n,"%s",s);}
static void linux_backend_refresh(LinuxGpuState*){}
static bool apply_curve_offsets_verified(LinuxGpuState*,const int*,const bool*,int){return true;}
#include "linux_backend_rollback.h"
struct ProbeContext {LinuxGpuState* gpu; DesiredSettings* d; LinuxHardwareSnapshot* s;};
static bool probeStep(void* raw,unsigned int phase){
 auto* p=(ProbeContext*)raw;
 if(phase!=LINUX_MUTATION_LOCK_CEILING)return false;
 return linux_apply_arm_transition_ceiling(p->gpu,p->d,p->d);
}
static bool probeRollback(void* raw,unsigned int phases){
 auto* p=(ProbeContext*)raw; char err[128]{};
 return linux_backend_restore_snapshot(p->gpu,p->s,phases,err,sizeof(err));
}
#include "vf_offset_range_policy.h"
static VfOffsetRange vf_offset_range_current(){return vf_offset_range_fallback();}
static int clamp_freq_delta_khz(int x){return std::clamp(x,-1000000,1000000);}
static int gpu_offset_component_mhz_for_point(int ci,int offset,int excluded){return ci<excluded?0:offset;}
#include "gpu_backend_apply_targets.h"

using VFCurvePoint = Point;
using DWORD = unsigned long;
struct App {
 bool isServiceProcess=true,usingBackgroundService=false,loaded=false;
 int numVisible=0,numPopulated=0;
 Point curve[VF_NUM_POINTS]{}; int freqOffsets[VF_NUM_POINTS]{};
 int gpuClockOffsetkHz{}; LockMode lockMode{};
 unsigned int appliedLockFreq{},lockedFreq{},transitionClockCapMHz{}; bool transitionClockCapActive{};
 int readback{};
};
static App g_app;
static Api g_nvml_api;
static bool nvml_ensure_ready(){return true;}
static bool nvml_set_gpu_locked_clocks(unsigned int lo,unsigned int hi,char*,size_t){return setCap(0,lo,hi)==0;}
static bool nvml_reset_gpu_locked_clocks(char*,size_t){return resetCap(0)==0;}
static const char* lock_mode_name(LockMode){return "fixture";}
static unsigned int displayed_curve_mhz(unsigned int khz){return khz/1000;}
static unsigned int curve_point_verify_tolerance_mhz(int){return 8;}
static void set_message(char* dst,size_t size,const char* format,...){
 va_list ap;va_start(ap,format);std::vsnprintf(dst,size,format,ap);va_end(ap);
}
static void set_curve_target_mismatch_detail(int,unsigned int,unsigned int,bool,char* dst,size_t size){
 std::snprintf(dst,size,"mismatch");
}
static void apply_clock_witness_set_clamp(unsigned int,bool){}
static void apply_clock_witness_finish_transition(){}
static void apply_clock_witness_record_at_arming(const char*){}
#include "gpu_backend_apply_ceiling.h"
#include "gpu_backend_apply_verify.h"
#include "clock_reset_policy.h"
struct ServiceResponse {struct {bool loaded;} snapshot;};
static bool service_client_get_ready_state(ServiceResponse*,int,const char*,char*,size_t){return false;}
static void apply_ready_service_envelope_to_app(ServiceResponse*){}
static void Sleep(DWORD){}
static void apply_clock_witness_poll(const char*){}
struct Sample {bool curveOk,offsetsOk;unsigned int freq;};
static Sample samples[3]{};
static int sampleIndex=0;
static bool nvapi_read_curve(){
 const auto sample=samples[sampleIndex];
 if(sample.curveOk){g_app.curve[0].freq_kHz=sample.freq;g_app.numPopulated=sample.freq?1:0;}
 return sample.curveOk;
}
static bool nvapi_read_offsets(){
 const auto sample=samples[sampleIndex++];
 if(sample.offsetsOk)g_app.freqOffsets[0]=(int)(sample.freq/1000);
 return sample.offsetsOk;
}
static void rebuild_visible_map(){g_app.numVisible=g_app.numPopulated;}
static void detect_locked_tail_from_curve(){}
#include "gpu_backend_snapshot.h"

static bool failScalar=false,failCurve=false;
static int scalarWrites=0,curveWrites=0;
static ApplyRecoveryResult rollback_to_safe_defaults(){
 auto r=reset_core_clock_controls(false,true,
   [](){++scalarWrites;return !failScalar;},
   [](){++curveWrites;return !failCurve;});
 if(apply_recovery_permits_release(r)){resetCap(0);r.restrictionReleased=true;}
 return r;
}
#define ARRAY_COUNT(x) (sizeof(x)/sizeof((x)[0]))
static void StringCchCopyA(char* dst,size_t n,const char* s){std::snprintf(dst,n,"%s",s);}
static void invalidate_scalar_readbacks(int*){}
static void refresh_global_state(char*,size_t){}
#include "gpu_backend_apply_failure.h"
#undef ARRAY_COUNT
#define CHECK(condition) do { if(!(condition)){std::fprintf(stderr,"clock transition check failed at %d: %s\n",__LINE__,#condition);return __LINE__;} } while(0)

static int run(){
 LinuxGpuState g{};g.nvml.setGpuLockedClocks=setCap;g.nvml.resetGpuLockedClocks=resetCap;
 g.curve[0].freq_kHz=3600000;
 DesiredSettings old{},next{};old.hasLock=next.hasLock=true;
 old.lockMode=next.lockMode=LOCK_MODE_HARD;old.lockMHz=2500;next.lockMHz=3000;next.resetOcBeforeApply=true;
 LinuxHardwareSnapshot snapshot{};snapshot.valid=snapshot.curveValid=true;
 // Real arm + final-lock calls, both directions and every mode pair, repeated.
 for(int repeat=0;repeat<3;++repeat)for(int from=0;from<3;++from)for(int to=0;to<3;++to){
   old.lockMode=(LockMode)from;old.lockMHz=repeat==1?3000:2500;
   next.lockMode=(LockMode)to;next.lockMHz=repeat==1?2500:3000;
   next.hasLock=to!=0;g.freqOffsets[0]=from==0?-200000:0;
   linux_apply_ceiling_reset_state();linux_apply_ceiling_note_outgoing(&old);
   g_linuxOutgoingCeilingMHz=linux_outgoing_ceiling_mhz(&g,&old);
   auto planned=linux_apply_clock_ceiling_plan(&g,&next,&old);
   CHECK(planned.required);CHECK(linux_apply_arm_transition_ceiling(&g,&next,&old));
   CHECK(cap==planned.ceilingMHz);
   if(from==LOCK_MODE_HARD)CHECK(cap<=old.lockMHz);
   CHECK(linux_snapshot_curve_needs_a_pin(&snapshot)==(from==LOCK_MODE_HARD));
   CHECK(linux_apply_reset_baseline_locked_clocks(&g,&next));CHECK(cap==planned.ceilingMHz);
   CHECK(linux_apply_write_final_lock(&g,&next));
   CHECK(cap==(to==LOCK_MODE_HARD?next.lockMHz:0));
 }
 old.lockMode=next.lockMode=LOCK_MODE_HARD;old.lockMHz=2500;next.lockMHz=3000;next.hasLock=true;
 // Rejected arm through the real transaction and real rollback: no old-pin reset.
 cap=2500;resets=0;refuseArm=true;linux_apply_ceiling_reset_state();
 linux_apply_ceiling_note_outgoing(&old);g_linuxOutgoingCeilingMHz=2500;
 ProbeContext context{&g,&next,&snapshot};
 auto failed=linux_execute_transaction(LINUX_MUTATION_LOCK_CEILING,probeStep,probeRollback,&context);
 CHECK(!failed.success&&failed.rollbackAttempted);CHECK(resets==0&&cap==2500);
 refuseArm=false;
 // Re-establish low protection BEFORE restoring a previous HARD curve after a
 // higher final pin, release, or failed baseline. Never unlock on rollback.
 for(unsigned int mask:{(unsigned int)LINUX_MUTATION_LOCK_CEILING,
       (unsigned int)LINUX_MUTATION_RESET_BASELINE,
       (unsigned int)(LINUX_MUTATION_CURVE|LINUX_MUTATION_LOCK)}){
   cap=3000;resets=0;g_linuxOutgoingCeilingMHz=2500;
   char err[128]{};CHECK(!linux_backend_restore_snapshot(&g,&snapshot,mask,err,sizeof(err)));
   CHECK(cap==2500&&resets==0);
 }
 linux_apply_ceiling_reset_state();
 CHECK(linux_outgoing_ceiling_mhz(&g,nullptr)==2500);
 CHECK(linux_outgoing_state_holds_clocks_down(&g,nullptr));
 CHECK(linux_apply_write_final_lock(&g,&next));
 CHECK(g.retainedTransitionCeilingMHz==0);
 // Exact explicit targets override selective policy for every lock mode.
 for(int mode=0;mode<3;++mode){
   DesiredSettings d{};d.hasGpuOffset=true;d.gpuOffsetMHz=300;
   d.hasCurvePoint[1]=true;d.curvePointMHz[1]=2500;
   bool populated[VF_NUM_POINTS]{},tail[VF_NUM_POINTS]{},mask[VF_NUM_POINTS]{},explicitMask[VF_NUM_POINTS]{};
   int offsets[VF_NUM_POINTS]{},freq[VF_NUM_POINTS]{},target[VF_NUM_POINTS]{};
   for(int i=0;i<4;++i){populated[i]=true;freq[i]=2400000;tail[i]=i>=2;}
   explicitMask[1]=true;
   CHECK(apply_build_curve_targets(&d,true,false,mode!=0,(LockMode)mode,2,2800,true,1,0,0,
     populated,offsets,freq,tail,target,mask));
   CHECK(target[1]==100000);CHECK(target[0]==0);
   for(int i=0;i<4;++i)g_app.curve[i].freq_kHz=i>=2?2800000:2500000;
   char detail[128]{};g_app.curve[1].freq_kHz=2700000;
   CHECK(!apply_verify_curve_targets(&d,explicitMask,target,true,true,mode!=0,(LockMode)mode,tail,2800,detail,sizeof(detail)));
   g_app.curve[1].freq_kHz=2500000;
   CHECK(apply_verify_curve_targets(&d,explicitMask,target,true,true,mode!=0,(LockMode)mode,tail,2800,detail,sizeof(detail)));
   g_app.curve[1].freq_kHz=0;
   CHECK(!apply_verify_curve_targets(&d,explicitMask,target,true,true,mode!=0,(LockMode)mode,tail,2800,detail,sizeof(detail)));
   // Same delta intent remains stable when the sampled base changes.
   freq[1]+=30000;
   CHECK(apply_build_curve_targets(&d,true,false,mode!=0,(LockMode)mode,2,2800,true,1,0,0,
     populated,offsets,freq,tail,target,mask));CHECK(target[1]==70000);
 }
 // User repro: excluded point 69 has offset zero, but its derived 2295 MHz
 // preview becomes 2407 MHz after the driver reshapes the curve. It is not an
 // explicit absolute target. Check real offsets on first/repeat applies.
 {
   DesiredSettings d{};d.hasCurvePoint[69]=true;d.curvePointMHz[69]=2295;
   bool explicitMask[VF_NUM_POINTS]{},tail[VF_NUM_POINTS]{};
   int target[VF_NUM_POINTS]{};char detail[128]{};
   for(int mode=0;mode<3;++mode)for(unsigned int mhz:{2295u,2407u,2392u,2422u}){
     g_app.curve[69].freq_kHz=mhz*1000;g_app.freqOffsets[69]=0;
     CHECK(apply_verify_curve_targets(&d,explicitMask,target,true,true,mode!=0,(LockMode)mode,tail,2957,detail,sizeof(detail)));
   }
   // A real extra offset must fail even when MHz happens to match the old
   // preview. A negative-offset refusal must also fail (zero is higher).
   g_app.curve[69].freq_kHz=2295000;g_app.freqOffsets[69]=100000;
   CHECK(!apply_verify_curve_targets(&d,explicitMask,target,true,true,false,LOCK_MODE_NONE,tail,0,detail,sizeof(detail)));
   target[69]=-100000;g_app.freqOffsets[69]=0;
   CHECK(!apply_verify_curve_targets(&d,explicitMask,target,true,true,false,LOCK_MODE_NONE,tail,0,detail,sizeof(detail)));
   // Explicit absolute targets retain their authority with matching offsets.
   explicitMask[69]=true;target[69]=0;g_app.curve[69].freq_kHz=2407000;
   CHECK(!apply_verify_curve_targets(&d,explicitMask,target,true,true,false,LOCK_MODE_NONE,tail,0,detail,sizeof(detail)));
   // HARD-tail readback remains diagnostic; FLATTEN must prove the target.
   tail[69]=true;
   CHECK(!apply_verify_curve_targets(&d,explicitMask,target,true,true,true,LOCK_MODE_FLATTEN,tail,2295,detail,sizeof(detail)));
   CHECK(apply_verify_curve_targets(&d,explicitMask,target,true,true,true,LOCK_MODE_HARD,tail,2295,detail,sizeof(detail)));
 }
 // 2026-09-17 under-load repro. Profile 4 stores point 70 as base 2322 MHz
 // with curve_semantics=base_plus_gpu_offset, so loading it reconstructs
 // 2322+475 = 2797 MHz. Under 99% load the driver reports the stock base one
 // whole VF bin higher (2352), so the same correct +475000 kHz offset reads
 // back as 2827 MHz. Holding that point to the reconstructed absolute failed
 // the apply, and the correction loop then rewrote identical offsets until an
 // unrelated watchdog tore the service down.
 {
   DesiredSettings d{};
   d.hasCurvePoint[70]=true;d.curvePointMHz[70]=2797;
   d.hasGpuOffset=true;d.gpuOffsetMHz=475;d.curvePointFromGpuOffset[70]=true;
   bool explicitMask[VF_NUM_POINTS]{},tail[VF_NUM_POINTS]{};
   int target[VF_NUM_POINTS]{};char detail[128]{};
   target[70]=475000;g_app.freqOffsets[70]=475000;
   // The load-shifted readback is accepted in BOTH routings: the offset is the
   // intent, and it verified exactly.
   for(int selective=0;selective<2;++selective){
     g_app.curve[70].freq_kHz=2827000;
     CHECK(apply_verify_curve_targets(&d,explicitMask,target,true,selective!=0,false,
       LOCK_MODE_NONE,tail,0,detail,sizeof(detail)));
     g_app.curve[70].freq_kHz=2797000;
     CHECK(apply_verify_curve_targets(&d,explicitMask,target,true,selective!=0,false,
       LOCK_MODE_NONE,tail,0,detail,sizeof(detail)));
   }
   // Offset authority is not a licence: an offset the driver pushed ABOVE what
   // was asked still fails, however plausible the MHz looks.
   g_app.freqOffsets[70]=505000;g_app.curve[70].freq_kHz=2797000;
   CHECK(!apply_verify_curve_targets(&d,explicitMask,target,true,true,false,
     LOCK_MODE_NONE,tail,0,detail,sizeof(detail)));
   // A genuinely user-typed absolute point keeps absolute authority: same
   // readback, same 30 MHz miss, still a failure.
   DesiredSettings typed{};
   typed.hasCurvePoint[70]=true;typed.curvePointMHz[70]=2797;
   bool typedMask[VF_NUM_POINTS]{};typedMask[70]=true;
   g_app.freqOffsets[70]=475000;g_app.curve[70].freq_kHz=2827000;
   CHECK(!apply_verify_curve_targets(&typed,typedMask,target,true,true,false,
     LOCK_MODE_NONE,tail,0,detail,sizeof(detail)));
   // Target building must not re-derive a reconstructed point from the live
   // base. The requested offset survives a base that moved under load; the old
   // absolute-minus-live-base rule produced 445000 for the shifted sample.
   bool populated[VF_NUM_POINTS]{},mask[VF_NUM_POINTS]{};
   int offsets[VF_NUM_POINTS]{},freq[VF_NUM_POINTS]{};
   populated[70]=true;
   for(int baseMHz:{2322,2352}){
     freq[70]=baseMHz*1000;offsets[70]=0;
     int built[VF_NUM_POINTS]{};
     CHECK(apply_build_curve_targets(&d,true,false,false,LOCK_MODE_NONE,0,0,true,70,0,0,
       populated,offsets,freq,tail,built,mask));
     CHECK(built[70]==475000);CHECK(mask[70]);
     // Negative control: the SAME request without provenance is the pre-fix
     // rule, and it tracks the shifted base instead of the asked-for offset.
     DesiredSettings typedSame=d;typedSame.curvePointFromGpuOffset[70]=false;
     int legacy[VF_NUM_POINTS]{};
     CHECK(apply_build_curve_targets(&typedSame,true,false,false,LOCK_MODE_NONE,0,0,true,70,0,0,
       populated,offsets,freq,tail,legacy,mask));
     CHECK(legacy[70]==2797000-baseMHz*1000);
   }
   // Per-point, not per-request: hand-editing one field of a loaded profile
   // leaves that point a real absolute target while its neighbours stay
   // projections. A single request-wide flag would have mis-verified point 71.
   {
     DesiredSettings mixed{};
     mixed.hasGpuOffset=true;mixed.gpuOffsetMHz=475;
     mixed.hasCurvePoint[70]=true;mixed.curvePointMHz[70]=2797;
     mixed.curvePointFromGpuOffset[70]=true;
     mixed.hasCurvePoint[71]=true;mixed.curvePointMHz[71]=2900;
     bool mixedMask[VF_NUM_POINTS]{};mixedMask[71]=true;
     int mixedTarget[VF_NUM_POINTS]{};char mixedDetail[128]{};
     bool mixedTail[VF_NUM_POINTS]{};
     mixedTarget[70]=475000;mixedTarget[71]=475000;
     g_app.freqOffsets[70]=475000;g_app.curve[70].freq_kHz=2827000;
     g_app.freqOffsets[71]=475000;g_app.curve[71].freq_kHz=2900000;
     CHECK(apply_verify_curve_targets(&mixed,mixedMask,mixedTarget,true,true,false,
       LOCK_MODE_NONE,mixedTail,0,mixedDetail,sizeof(mixedDetail)));
     // The typed neighbour still fails on its own absolute readback.
     g_app.curve[71].freq_kHz=2930000;
     CHECK(!apply_verify_curve_targets(&mixed,mixedMask,mixedTarget,true,true,false,
       LOCK_MODE_NONE,mixedTail,0,mixedDetail,sizeof(mixedDetail)));
   }
 }
 // Real reset sequencing: VF-global must never invoke the scalar helper.
 for(int vfGlobal=0;vfGlobal<2;++vfGlobal)for(int scalarFails=0;scalarFails<2;++scalarFails)
 for(int curveFails=0;curveFails<2;++curveFails){
   int scalar=0,curve=0;
   auto recovered=reset_core_clock_controls(vfGlobal,true,
     [&](){++scalar;return !scalarFails;},[&](){++curve;return !curveFails;});
   CHECK(scalar==(vfGlobal?0:1));CHECK(curve==((!vfGlobal&&scalarFails)?0:1));
   CHECK(apply_recovery_permits_release(recovered)==((vfGlobal||!scalarFails)&&!curveFails));
 }

 // Same population but newer sample must replace the first complete sample.
 g_app=App{};samples[0]={true,true,2500000};samples[1]={true,true,2600000};samples[2]={true,true,2700000};
 sampleIndex=0;bool offsetsOk=false;
 CHECK(read_live_curve_snapshot_settled(3,0,&offsetsOk));
 CHECK(offsetsOk&&g_app.curve[0].freq_kHz==2700000&&g_app.freqOffsets[0]==2700);
 // Latest read failure cannot silently reuse older success as verification.
 for(int failure=0;failure<3;++failure){
   samples[2]={failure!=0,failure!=1,failure==2?0u:2700000u};sampleIndex=0;
   CHECK(!read_live_curve_snapshot_settled(3,0,&offsetsOk));CHECK(!offsetsOk);
 }
 CHECK(vf_offset_zero_readback_is_benign(100000,0,400000,500000));
 CHECK(!vf_offset_zero_readback_is_benign(-100000,0,400000,500000));
 CHECK(!vf_offset_zero_readback_is_benign(100000,0,2500000,500000));
 CHECK(!vf_offset_zero_readback_is_benign(100000,0,0,500000));
 CHECK(!apply_recovery_permits_release(ApplyRecoveryResult{}));
 CHECK(!apply_clock_witness_counts_toward_verdict(true,false,true));
 CHECK(apply_clock_witness_counts_toward_verdict(true,false,false));
 g_nvml_api=g.nvml;g_app.lockMode=LOCK_MODE_HARD;g_app.appliedLockFreq=2500;
 // A guard's destructor never releases an abandoned/partly written state.
 cap=2500;resets=0;
 {ApplyClockCeilingGuard guard(&next);CHECK(guard.arm()==APPLY_CEILING_ARM_INSTALLED);}
 CHECK(cap==2500&&resets==0&&g_app.transitionClockCapActive);
 for(int scalarFails=0;scalarFails<2;++scalarFails)for(int curveFails=0;curveFails<2;++curveFails){
   failScalar=scalarFails;failCurve=curveFails;scalarWrites=curveWrites=0;resets=0;
   {ApplyClockCeilingGuard guard(&next);CHECK(guard.arm()==APPLY_CEILING_ARM_INSTALLED);
    char result[256]{};CHECK(!apply_recover_clock_failure(guard,"injected partial write",result,sizeof(result)));}
   CHECK(scalarWrites==1);CHECK(curveWrites==(scalarFails?0:1));
   CHECK(resets==(!scalarFails&&!curveFails?1:0));
 }

 // Late failure after final release must acquire protection again before reset.
 failScalar=false;failCurve=true;scalarWrites=curveWrites=0;resets=0;
 {ApplyClockCeilingGuard guard(&next);CHECK(guard.arm()==APPLY_CEILING_ARM_INSTALLED);
  resetCap(0);guard.adopt("final release");CHECK(!guard.armed&&cap==0);
  char result[256]{};CHECK(!apply_recover_clock_failure(guard,"late fan failure",result,sizeof(result)));
  CHECK(cap==2500&&scalarWrites==1&&curveWrites==1);}
 CHECK(g_app.transitionClockCapActive&&g_app.transitionClockCapMHz==2500);
 // Even a failed final setter may have installed a higher cap before failure.
 {ApplyClockCeilingGuard guard(&next);CHECK(guard.arm()==APPLY_CEILING_ARM_INSTALLED);
  cap=3000;char result[256]{};
  CHECK(!apply_recover_clock_failure(guard,"partial final pin write",result,sizeof(result)));
  CHECK(cap==2500);}
 // The next apply uses a retained cap even when intent/curve markers changed.
 g_app.lockMode=LOCK_MODE_NONE;g_app.curve[0].freq_kHz=3600000;
 CHECK(apply_outgoing_ceiling_mhz()==2500);
 for(const auto range:{vf_offset_range_from_probe(true,0,500000),
                      vf_offset_range_from_probe(true,-400000,250000),
                      vf_offset_range_from_probe(true,INT_MIN,INT_MAX),
                      vf_offset_range_from_probe(true,0,0),vf_offset_range_fallback()}){
   CHECK(vf_offset_range_permits_khz(range,vf_offset_range_flatten_floor_khz(range)));
   CHECK(vf_offset_range_supports_flatten(range)==(range.minKHz<0));
   CHECK(vf_offset_range_hard_limit_khz(range)>0);
 }
 CHECK(!vf_offset_range_permits_khz(vf_offset_range_from_probe(true,-400000,250000),400000));
 return 0;
}
#undef CHECK
} // namespace clock_transition_fixture
int run_clock_transition_tests(){return clock_transition_fixture::run();}
