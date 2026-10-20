/* Hardware-free failure tests for the real blank implementation.
 * Build: clang -O0 -Wall -Wextra tests/blank_failure_test.c -o /tmp/brightctl-blank-failure-test -framework CoreGraphics -framework CoreFoundation -framework IOKit -ldl
 * Run: /tmp/brightctl-blank-failure-test
 * CoreGraphics, DisplayServices, preferences and dynamic loading below are all
 * replaced. Real CoreFoundation is used only for in-memory value objects.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

static int fake_open(const char *p, int flags, ...) { (void)p; (void)flags; return 91; }
static int fake_close(int fd) { (void)fd; return 0; }
static int fake_flock(int fd, int flags) { (void)fd; (void)flags; return 0; }
static int fake_fstat(int fd, struct stat *st) { (void)fd; memset(st,0,sizeof *st); st->st_mode=S_IFREG|0600; st->st_uid=getuid(); return 0; }
#define fstat fake_fstat
#define open fake_open
#define close fake_close
#define flock fake_flock

static CFPropertyListRef pref;
static float level;
static bool alc, in_mirror_set, external_present;
static CGDirectDisplayID mirror, staged_mirror;
static int mutations, mirror_calls, origin_calls, reads, fails, pref_writes;
static int signal_during_zero;
static int test_signal;
static bool fail_sync, fail_mirror, fail_zero, fail_restore, fail_alc;
static bool stale_dark_read, stale_restore_read, disconnect_on_commit;
static bool topology_removes_mirror;
static CGPoint origin;

static CFPropertyListRef fake_copy(CFStringRef k, CFStringRef d) {
    (void)k; (void)d; return pref ? CFRetain(pref) : NULL;
}
static void fake_pref_set(CFStringRef k, CFPropertyListRef v, CFStringRef d) {
    (void)k; (void)d; pref_writes++; if (pref) CFRelease(pref); pref = v ? CFRetain(v) : NULL;
}
static Boolean fake_sync(CFStringRef d) { (void)d; return !fail_sync; }
static CGError fake_online(uint32_t max, CGDirectDisplayID *out, uint32_t *n) {
    *n = external_present ? 2 : 1;
    if (max && out) out[0] = 1;
    if (max > 1 && out && external_present) out[1] = 2;
    return kCGErrorSuccess;
}
static boolean_t fake_builtin(CGDirectDisplayID d) { return d == 1; }
static boolean_t fake_asleep(CGDirectDisplayID d) { (void)d; return false; }
static CGDirectDisplayID fake_main(void) { return 2; }
static CGDirectDisplayID fake_mirrors(CGDirectDisplayID d) { (void)d; return mirror; }
static boolean_t fake_in_set(CGDirectDisplayID d) { (void)d; return in_mirror_set; }
static CGRect fake_bounds(CGDirectDisplayID d) { (void)d; return (CGRect){origin, {1920,1080}}; }
static CGError fake_begin(CGDisplayConfigRef *c) {
    *c = (CGDisplayConfigRef)(uintptr_t)1; staged_mirror = mirror; return kCGErrorSuccess;
}
static CGError fake_mirror(CGDisplayConfigRef c, CGDirectDisplayID d, CGDirectDisplayID target) {
    (void)c; (void)d; mirror_calls++;
    if (fail_mirror) return kCGErrorFailure;
    staged_mirror = target; return kCGErrorSuccess;
}
static CGError fake_origin(CGDisplayConfigRef c, CGDirectDisplayID d, int32_t x, int32_t y) {
    (void)c; (void)d; origin_calls++; origin = (CGPoint){x,y}; return kCGErrorSuccess;
}
static CGError fake_complete(CGDisplayConfigRef c, CGConfigureOption opt) {
    (void)c; if (opt != kCGConfigureForSession) abort();
    mutations++; mirror = staged_mirror; in_mirror_set = mirror != 0;
    if (mirror) origin = (CGPoint){0,0};
    if (disconnect_on_commit) {
        external_present = false;
        if (topology_removes_mirror) { mirror = 0; in_mirror_set = false; }
    }
    return kCGErrorSuccess;
}
static CGError fake_cancel(CGDisplayConfigRef c) { (void)c; return kCGErrorSuccess; }
static void *fake_dlopen(const char *p, int f) { (void)p; (void)f; return NULL; }
static void *fake_dlsym(void *h, const char *p) { (void)h; (void)p; return NULL; }

#define CFPreferencesCopyAppValue fake_copy
#define CFPreferencesSetAppValue fake_pref_set
#define CFPreferencesAppSynchronize fake_sync
#define CGGetOnlineDisplayList fake_online
#define CGGetActiveDisplayList fake_online
#define CGDisplayIsBuiltin fake_builtin
#define CGDisplayIsAsleep fake_asleep
#define CGMainDisplayID fake_main
#define CGDisplayMirrorsDisplay fake_mirrors
#define CGDisplayIsInMirrorSet fake_in_set
#define CGDisplayBounds fake_bounds
#define CGBeginDisplayConfiguration fake_begin
#define CGConfigureDisplayMirrorOfDisplay fake_mirror
#define CGConfigureDisplayOrigin fake_origin
#define CGCompleteDisplayConfiguration fake_complete
#define CGCancelDisplayConfiguration fake_cancel
#define dlopen fake_dlopen
#define dlsym fake_dlsym
#include "../brightctl_core.c"

static int fake_get(CGDirectDisplayID d, float *v) {
    (void)d; reads++;
    if (stale_dark_read && level == 0 && reads == 2) *v = .73f;
    else if (stale_restore_read && level > 0) *v = .06f;
    else *v = level;
    return 0;
}
static int fake_set(CGDirectDisplayID d, float v) {
    (void)d; mutations++;
    if ((v == 0 && fail_zero) || (v > 0 && fail_restore)) return -1;
    level = v;
    if (v == 0 && signal_during_zero) raise(signal_during_zero);
    return 0;
}
static bool fake_has(CGDirectDisplayID d) { (void)d; return true; }
static int fake_get_alc(CGDirectDisplayID d, bool *v) { (void)d; *v = alc; return 0; }
static int fake_set_alc(CGDirectDisplayID d, bool v) {
    (void)d; mutations++; if (fail_alc) return -1; alc = v; return 0;
}
static void reset(void) {
    if (pref) CFRelease(pref); pref = NULL;
    level=.73f; alc=true; external_present=true; mirror=0; in_mirror_set=false;
    staged_mirror=0; mutations=mirror_calls=origin_calls=reads=pref_writes=0;
    signal_during_zero=0; test_signal=SIGTERM;
    fail_sync=fail_mirror=fail_zero=fail_restore=fail_alc=false;
    stale_dark_read=stale_restore_read=disconnect_on_commit=topology_removes_mirror=false;
    origin=(CGPoint){-1920,120};
    g_inited=true; g_ds_get=fake_get; g_ds_set=fake_set;
    g_has_alc=fake_has; g_get_alc=fake_get_alc; g_set_alc=fake_set_alc;
}
#define CHECK(c, message) do { if (!(c)) { printf("FAIL %s: %s\n", name, message); fails++; } } while (0)

static void roundtrip(void) {
    const char *name="roundtrip"; reset();
    CHECK(bctl_builtin_blank(true),"blank succeeds");
    CHECK(level==0 && !alc && mirror==2,"dark state");
    CHECK(bctl_builtin_is_blank(),"query recognizes dark online panel");
    CHECK(pref!=NULL,"query preserves record");
    CHECK(bctl_builtin_blank(false),"restore succeeds");
    CHECK(fabsf(level-.73f)<.001f && alc && mirror==0,"original state restored");
    CHECK(origin.x==-1920 && origin.y==120,"original position restored");
    CHECK(pref==NULL,"record cleared after restore");
}
static void repeated(void) {
    const char *name="repeated"; reset();
    CHECK(bctl_builtin_blank(true),"first blank");
    CHECK(bctl_builtin_blank(true),"second blank");
    CHECK(bctl_builtin_blank(false),"restore");
    CHECK(fabsf(level-.73f)<.001f && alc,"snapshot was not overwritten by zero");
}
static void preexisting_source(void) {
    const char *name="preexisting-source"; reset(); in_mirror_set=true; mirror=0;
    CHECK(bctl_builtin_blank(true),"blank original mirror source");
    CHECK(bctl_builtin_blank(false),"restore source");
    CHECK(mirror_calls==0,"existing mirror graph never mutated");
    CHECK(in_mirror_set,"source remains in original mirror set");
}
static void no_external(void) {
    const char *name="no-external"; reset(); external_present=false;
    CHECK(!bctl_builtin_blank(true),"reject blank");
    CHECK(mutations==0 && pref==NULL,"no mutation or snapshot");
}
static void persistence_failure(void) {
    const char *name="persistence-failure"; reset(); fail_sync=true;
    CHECK(!bctl_builtin_blank(true),"reject unsynchronized snapshot");
    CHECK(mutations==0,"do not touch hardware before durable snapshot");
}
static void mirror_failure(void) {
    const char *name="mirror-failure"; reset(); fail_mirror=true;
    CHECK(!bctl_builtin_blank(true),"report mirror failure");
    CHECK(fabsf(level-.73f)<.001f && alc,"rollback original brightness and ALC");
}
static void zero_failure(void) {
    const char *name="zero-failure"; reset(); fail_zero=true;
    CHECK(!bctl_builtin_blank(true),"report dim failure");
    CHECK(fabsf(level-.73f)<.001f && alc && mirror==0,"rollback all changed state");
}
static void failed_restore_preserves(void) {
    const char *name="failed-restore"; reset();
    CHECK(bctl_builtin_blank(true),"setup"); fail_restore=true;
    CHECK(!bctl_builtin_blank(false),"report restore failure");
    CHECK(pref!=NULL,"failed recovery retains snapshot");
    CHECK(mirror==0 && alc,"other recovery steps still run");
}
static void stale_restore(void) {
    const char *name="stale-restore"; reset();
    CHECK(bctl_builtin_blank(true),"setup"); stale_restore_read=true;
    CHECK(!bctl_builtin_blank(false),"stale 0.06 readback cannot verify target 0.73");
    CHECK(pref!=NULL,"do not discard snapshot before target readback");
}
static void stale_dark(void) {
    const char *name="stale-dark"; reset(); stale_dark_read=true;
    CHECK(bctl_builtin_blank(true),"one stale read must settle with bounded retry");
    CHECK(level==0 && pref!=NULL,"eventually dark, snapshot retained");
}
static void disconnect_race(void) {
    const char *name="disconnect-race"; reset(); disconnect_on_commit=true;
    CHECK(!bctl_builtin_blank(true),"external removed during commit must cancel blank");
    CHECK(level>=.1f,"internal must end visibly restored");
}
static void lost_mirror_position(void) {
    const char *name="lost-mirror-position"; reset();
    CHECK(bctl_builtin_blank(true),"setup");
    mirror=0; in_mirror_set=false; origin=(CGPoint){0,0}; external_present=false;
    CHECK(bctl_builtin_blank(false),"recover after OS already removed mirror");
    CHECK(origin.x==-1920 && origin.y==120,"restore position even if mirror already matches");
}
static int fake_probe(bctl_display *out, int max) {
    if (out && max) *out=(bctl_display){.display_id=1,.is_builtin=true,.channel=BCTL_CHANNEL_DS,.percent=73};
    return 1;
}
static unsigned int fake_sleep(unsigned int n) { (void)n; raise(test_signal); return 0; }
#define bctl_probe fake_probe
#define sleep fake_sleep
#define main tested_cli_main
#include "../brightctl2.c"
#undef main
#undef sleep
#undef bctl_probe

static void pure_query(void) {
    const char *name="pure-query"; reset(); CHECK(bctl_builtin_blank(true),"setup");
    int writes=pref_writes, mods=mutations;
    for(int i=0;i<20;i++) { CHECK(bctl_builtin_has_restore_state(),"record remains"); CHECK(bctl_builtin_is_blank(),"state remains dark"); }
    CHECK(writes==pref_writes && mods==mutations,"queries neither write preferences nor mutate display");
}
static void alc_failure(void) {
    const char *name="alc-failure"; reset(); fail_alc=true;
    CHECK(!bctl_builtin_blank(true),"ALC failure reported");
    CHECK(fabsf(level-.73f)<.001f && mirror==0,"brightness and graph rollback despite ALC error");
    CHECK(pref!=NULL,"incomplete ALC rollback retains record");
    fail_alc=false; CHECK(bctl_builtin_blank(false),"later retry completes restore");
    CHECK(alc && pref==NULL,"original ALC recovered");
}
static void preexisting_sink(void) {
    const char *name="preexisting-sink"; reset(); in_mirror_set=true; mirror=2;
    CHECK(bctl_builtin_blank(true),"blank existing sink"); CHECK(bctl_builtin_blank(false),"restore sink");
    CHECK(mirror_calls==0 && mirror==2 && in_mirror_set,"preserve original mirror sink graph");
}
static void signal_cli(int sig, bool during_zero) {
    const char *name="cli-signal"; reset(); blank_stop=0; test_signal=sig;
    if(during_zero) signal_during_zero=sig;
    char *argv[]={"test-cli","--lang","en","blank","on",NULL};
    CHECK(tested_cli_main(5,argv)==0,"CLI exits successfully after signal");
    CHECK(blank_stop==1,"signal only sets stop flag");
    CHECK(fabsf(level-.73f)<.001f && alc && mirror==0 && pref==NULL,"normal CLI flow restores all original state");
}
int main(void) {
    roundtrip(); repeated(); preexisting_source(); no_external(); persistence_failure();
    mirror_failure(); zero_failure(); failed_restore_preserves(); stale_restore();
    stale_dark(); disconnect_race(); lost_mirror_position();
    pure_query(); alc_failure(); preexisting_sink();
    signal_cli(SIGINT,false); signal_cli(SIGTERM,false); signal_cli(SIGTERM,true);
    if (pref) CFRelease(pref);
    printf("18 hardware-free scenarios; %d failed assertions\n", fails);
    return fails ? 1 : 0;
}
