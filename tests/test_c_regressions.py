"""Compile and execute production C functions with an instrumented OBS adapter.
No OBS process, libraries, sockets or personal configuration are used.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("SR_SOURCE", ROOT / "source-record.c")).read_text()


def function(name, source=SOURCE):
    import re
    match = re.search(r"^(?:static )?(?:inline )?[^\n;{}]+\b" + name + r"\([^;]*?\)\n\{", source, re.M)
    if not match:
        raise ValueError(f"Function missing: {name}")
    start, pos, depth = match.start(), match.end(), 1
    # Production functions used below contain no braces in string literals.
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


def audio_function():
    try:
        try:
            busy = function("encoders_busy")
        except ValueError:
            busy = ""
        return busy + "\n" + function("update_audio_encoder")
    except ValueError:
        old = function("update_encoder")
        audio = "static void legacy_audio(struct source_record_filter_context *filter, obs_data_t *settings) { const char *enc_id;\n" + old[old.index("\tconst int audio_track ="):]
        return audio + "\nstatic void update_audio_encoder(struct source_record_filter_context *filter,obs_data_t *settings){legacy_audio(filter,settings);filter->owns_audio_output=filter->audio_track==0;}\n"


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_AUDIO_MIXES 6
#define AUDIO_OUTPUT_SUCCESS 0
#define AUDIO_FORMAT_FLOAT_PLANAR 1
#define LOG_INFO 0
#define blog(...) ((void)0)
typedef struct audio { bool closed; int id; } audio_t;
typedef struct settings { int track, bitrate; bool different; char id[32], json[80]; } obs_data_t;
typedef struct encoder { bool active; char id[32],name[64]; audio_t *media; int bitrate, track; } obs_encoder_t;
typedef struct output { obs_encoder_t *audio[6],*video; bool active; } obs_output_t;
enum output_kind {FILE_OUTPUT,STREAM_OUTPUT,REPLAY_OUTPUT,OUTPUT_COUNT};
enum output_phase {OUTPUT_IDLE,OUTPUT_QUEUED,OUTPUT_STARTING,OUTPUT_ACTIVE,OUTPUT_FAILED,OUTPUT_STOPPING};
struct output_state {volatile long phase;};
struct audio_output_info {const char *name; int speakers, samples_per_sec, format; void *input_param; void *input_callback;};
struct source_record_filter_context {
 void *source,*video_output; obs_encoder_t *audioEncoder[6]; int audio_track;
 audio_t *audio_output; bool owns_audio_output, audio_settings_pending, closing;
 obs_data_t *applied_audio_settings;
 obs_output_t *fileOutput, *replayOutput, *streamOutput;
 obs_encoder_t *encoder;struct output_state outputs[OUTPUT_COUNT];volatile long pending_stops;
};
struct dstr { char array[256]; };
static void dstr_init(struct dstr *s) {s->array[0]=0;}
#define dstr_printf(s,...) snprintf((s)->array,sizeof((s)->array),__VA_ARGS__)
static void dstr_free(struct dstr *s) {}
static bool obs_data_has_user_value(obs_data_t *s,const char *k) {return true;}
static bool obs_data_has_default_value(obs_data_t *s,const char *k) {return true;}
static bool os_atomic_load_bool(const volatile bool *v){return *v;}
static long os_atomic_load_long(const volatile long *v){return *v;}
static bool obs_output_active(obs_output_t *o){return o&&o->active;}
static audio_t global_audio={0,99};
static int closes, opens, fail_open, fail_encoder;
static const char *obs_source_get_name(void *s) {return "fixture";}
static obs_data_t *obs_data_create(void) {return calloc(1,sizeof(obs_data_t));}
static void obs_data_release(obs_data_t *s) {free(s);}
static bool obs_data_get_bool(obs_data_t *s,const char *k) {return s->different;}
static long long obs_data_get_int(obs_data_t *s,const char *k) {return !strcmp(k,"audio_track")?s->track:s->bitrate;}
static const char *obs_data_get_string(obs_data_t *s,const char *k) {return s->id;}
static void obs_data_set_int(obs_data_t *s,const char *k,long long n) {s->bitrate=(int)n;}
static const char *obs_data_get_json(obs_data_t *s) {snprintf(s->json,sizeof(s->json),"%d",s->bitrate);return s->json;}
static void copy_audio_user_values(obs_data_t *a,obs_data_t *b,const char *id) {}
static audio_t *obs_get_audio(void) {return &global_audio;}
static struct audio_output_info info={.speakers=2};
static const struct audio_output_info *audio_output_get_info(audio_t *a) {assert(!a->closed);return &info;}
static int audio_output_get_sample_rate(audio_t *a) {return 48000;}
static void *audio_input_callback;
static int audio_output_open(audio_t **a,struct audio_output_info *i) {
 if(fail_open) return -1; *a=calloc(1,sizeof(audio_t));(*a)->id=++opens;return 0;
}
static void audio_output_close(audio_t *a) {assert(a!=&global_audio);assert(!a->closed);a->closed=true;closes++;free(a);}
static bool obs_encoder_active(obs_encoder_t *e) {return e&&e->active;}
static const char *obs_encoder_get_id(obs_encoder_t *e) {return e->id;}
static obs_encoder_t *obs_audio_encoder_create(const char *id,const char *n,obs_data_t *s,int track,void *unused) {
 if(fail_encoder) return NULL; obs_encoder_t *e=calloc(1,sizeof(*e));strcpy(e->id,id);strcpy(e->name,n);e->bitrate=s->bitrate;e->track=track;return e;
}
static void obs_encoder_set_audio(obs_encoder_t *e,audio_t *a) {assert(!a->closed);e->media=a;}
static void obs_encoder_release(obs_encoder_t *e) {if(e){assert(!e->active);free(e);}}
static void obs_output_set_audio_encoder(obs_output_t *o,obs_encoder_t *e,int i) {o->audio[i]=e;}
static obs_encoder_t *obs_output_get_video_encoder(obs_output_t *o){return o->video;}
static void obs_output_set_video_encoder(obs_output_t *o,obs_encoder_t *e){o->video=e;}
enum obs_scale_type {OBS_SCALE_DISABLE};
static int video_updates,scale_updates;
static bool (*obs_encoder_set_frame_rate_divisor_func)(obs_encoder_t *,uint32_t);
static void (*obs_encoder_set_gpu_scale_type_func)(obs_encoder_t *,enum obs_scale_type);
static void obs_encoder_set_video(obs_encoder_t *e,void *v){video_updates++;}
static void obs_encoder_set_scaled_size(obs_encoder_t *e,uint32_t w,uint32_t h){scale_updates++;}

'''


class CRegressions(unittest.TestCase):
    def run_c(self, code):
        with tempfile.TemporaryDirectory(prefix="sr-c-test-") as d:
            source, binary = Path(d)/"test.c", Path(d)/"test"
            source.write_text(code)
            build = subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-g", "-O1",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread", str(source), "-o", str(binary)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30,
                                    env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1" if os.name != "nt" and __import__('sys').platform != 'darwin' else "detect_leaks=0"))
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_audio_ownership_configuration_and_failure(self):
        audio = audio_function()
        self.run_c(PRELUDE + audio + r'''
static void dispose(struct source_record_filter_context *c) {
 for(int i=0;i<6;i++) obs_encoder_release(c->audioEncoder[i]);
 if(c->owns_audio_output) audio_output_close(c->audio_output);
 obs_data_release(c->applied_audio_settings);
}
int main(void) {
 struct source_record_filter_context preflight={0};
 obs_data_t initial={.different=true,.track=-1,.bitrate=128};strcpy(initial.id,"aac");
 update_audio_encoder(&preflight,&initial);initial.track=0;update_audio_encoder(&preflight,&initial);
 assert(preflight.audio_output!=&global_audio);dispose(&preflight);
 int modes[]={0,-1,1,6};
 for(int a=0;a<4;a++)for(int b=0;b<4;b++) {
  struct source_record_filter_context c={0};
  obs_data_t s={.different=true,.track=modes[a],.bitrate=128};strcpy(s.id,"aac");
  update_audio_encoder(&c,&s);
  assert(c.audio_output==(modes[a]?&global_audio:c.audio_output));
  assert(c.owns_audio_output==(modes[a]==0));
  s.track=modes[b];update_audio_encoder(&c,&s);
  assert(c.audio_track==modes[b]);assert(c.owns_audio_output==(modes[b]==0));
  assert(c.audioEncoder[0]->media==c.audio_output);
  if(modes[b]) assert(c.audio_output==&global_audio);
  if(modes[b]==-1) {
   assert(c.audioEncoder[5]);
   for(int i=0;i<6;i++){char name[64];snprintf(name,sizeof(name),"fixture track %d",i+1);assert(!strcmp(c.audioEncoder[i]->name,name));}
  } else assert(!c.audioEncoder[1]);
  s.bitrate=256;strcpy(s.id,"opus");update_audio_encoder(&c,&s);
  assert(c.audioEncoder[0]->bitrate==256);assert(!strcmp(c.audioEncoder[0]->id,"opus"));
  c.audioEncoder[0]->active=true;s.track=modes[b]?0:-1;
  audio_t *before=c.audio_output;update_audio_encoder(&c,&s);
  assert(c.audio_output==before);assert(c.audio_settings_pending);
  c.audioEncoder[0]->active=false;update_audio_encoder(&c,&s);
  assert(c.audio_track==s.track);assert(!c.audio_settings_pending);dispose(&c);
 }
 struct source_record_filter_context c={0};
 obs_data_t s={.different=true,.track=-1,.bitrate=128};strcpy(s.id,"aac");
 update_audio_encoder(&c,&s);s.track=0;fail_open=1;update_audio_encoder(&c,&s);
 assert(c.audio_output==&global_audio&&c.audio_track==-1);fail_open=0;
 fail_encoder=1;update_audio_encoder(&c,&s);assert(c.audio_output==&global_audio&&c.audio_track==-1);
 fail_encoder=0;dispose(&c);assert(opens==closes);puts("audio: 16 transitions, codec/bitrate, active deferral, rollback PASS");
}
''')

    def test_atomic_file_reservation(self):
        self.run_c(r'''#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern char *mkdtemp(char *);
struct dstr { char *array; size_t len; };
static void dstr_ncat(struct dstr *s,const char *v,size_t n) {s->array=calloc(1,n+1);memcpy(s->array,v,n);s->len=n;}
static void dstr_printf(struct dstr *s,const char *f,...) {va_list a;va_start(a,f);char b[4096];vsnprintf(b,sizeof(b),f,a);va_end(a);free(s->array);s->array=strdup(b);s->len=strlen(b);}
static void dstr_free(struct dstr *s) {free(s->array);}
#define os_fopen fopen
static bool os_file_exists(const char *p) {return access(p,F_OK)==0;}
''' + function("dedup_path") + r'''
static char root[256];static struct dstr paths[8];
static void *reserve(void *arg) {size_t i=(size_t)arg;dstr_printf(&paths[i],"%s/same.mp4",root);dedup_path(&paths[i],"mp4");return NULL;}
int main(void) {
 strcpy(root,"/tmp/sr-path-XXXXXX");assert(mkdtemp(root));
 char existing[512];snprintf(existing,sizeof(existing),"%s/same.mp4",root);
 FILE *f=fopen(existing,"wb");fputs("KEEP",f);fclose(f);
 pthread_t threads[8];for(size_t i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,reserve,(void*)i));
 for(int i=0;i<8;i++)pthread_join(threads[i],NULL);
 for(int i=0;i<8;i++){assert(os_file_exists(paths[i].array));for(int j=0;j<i;j++)assert(strcmp(paths[i].array,paths[j].array));}
 for(int i=0;i<8;i++){unlink(paths[i].array);dstr_free(&paths[i]);}
 f=fopen(existing,"rb");char bytes[5]={0};fread(bytes,1,4,f);fclose(f);assert(!strcmp(bytes,"KEEP"));
 unlink(existing);rmdir(root);puts("paths: concurrent reservation and existing content PASS");
}
''')

    def test_output_retry_lifetime_and_cancellation(self):
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define LOG_WARNING 0
#define blog(...) ((void)0)
#define UNUSED_PARAMETER(x) (void)(x)
#include <string.h>
#define OBS_TASK_GRAPHICS 1
#include <pthread.h>
static pthread_mutex_t start_tasks_mutex = PTHREAD_MUTEX_INITIALIZER;
#define bzalloc(n) calloc(1,n)
#define bfree free
typedef void (*obs_task_t)(void *);
typedef struct source { int refs, showing; bool enabled; void *context; } obs_source_t;
typedef obs_source_t obs_weak_source_t;
typedef struct output {int refs,starts,stops;bool active,fail,async;void (*stop)(void *,void *);void *data;void (*start_signal)(void *,void *);void *start_data;} obs_output_t;
typedef obs_output_t signal_handler_t;
typedef void calldata_t;
enum output_kind {FILE_OUTPUT,STREAM_OUTPUT,REPLAY_OUTPUT,OUTPUT_COUNT};
struct start_output;
enum output_phase {OUTPUT_IDLE,OUTPUT_QUEUED,OUTPUT_STARTING,OUTPUT_ACTIVE,OUTPUT_FAILED,OUTPUT_STOPPING};
struct output_state {uint64_t next_attempt;unsigned failures;struct start_output *start;volatile long phase;obs_output_t *observed_output;bool started_event,stopped_event;};
struct source_record_filter_context {
 obs_source_t *source;obs_weak_source_t *weak_self;obs_output_t *fileOutput,*streamOutput,*replayOutput;
 bool closing,exiting,record,stream,replayBuffer,output_active;
 bool starting_file_output,starting_stream_output,starting_replay_output;
 long pending_stops;struct output_state outputs[OUTPUT_COUNT];
 bool video_reset;void *video_output;unsigned width,height;
 struct obs_video_info {unsigned fps_num,fps_den,base_width,base_height,output_width,output_height,output_format,colorspace,range;} cached_ovi;
};
struct start_output {
 struct source_record_filter_context *context;obs_source_t *source_ref;obs_output_t *output;
 enum output_kind kind;volatile bool cancelled,done;volatile long refs;bool started;
};
struct stop_output {
 struct source_record_filter_context *context;obs_output_t *output;obs_source_t *source_ref;
 volatile long refs;volatile bool released;bool connected,connecting;
};
static bool obs_get_video_info(struct obs_video_info *v){return false;}
static bool os_atomic_load_bool(const volatile bool *v){return *v;}
static void os_atomic_store_bool(volatile bool *v,bool b){*v=b;}
static bool os_atomic_set_bool(volatile bool *v,bool x){bool old=*v;*v=x;return old;}
static long os_atomic_load_long(volatile long *v){return *v;}
static void os_atomic_store_long(volatile long *v,long n){*v=n;}
static long os_atomic_inc_long(volatile long *v){return ++*v;}
static long os_atomic_dec_long(volatile long *v){return --*v;}
static uint64_t os_gettime_ns(void){return 100;}
static void *obs_obj_get_data(obs_source_t *s){return s->context;}
static void *calldata_ptr(void *cd,const char *key){return cd;}
static bool obs_source_enabled(obs_source_t *s){return s->enabled;}
static obs_source_t *obs_filter_get_parent(obs_source_t *s){return s;}
static void obs_source_inc_showing(obs_source_t *s){s->showing++;}
static void obs_source_dec_showing(obs_source_t *s){assert(s->showing>0);s->showing--;}
static obs_source_t *obs_source_get_ref(obs_source_t *s){s->refs++;return s;}
static obs_source_t *obs_weak_source_get_source(obs_weak_source_t *s){return obs_source_get_ref(s);}
static void obs_source_release(obs_source_t *s){assert(s->refs>0);s->refs--;}
static obs_output_t *obs_output_get_ref(obs_output_t *o){o->refs++;return o;}
static void obs_output_release(void *data){obs_output_t *o=data;assert(o->refs>0);o->refs--;}
static bool obs_output_active(obs_output_t *o){return o&&o->active;}
static const char *obs_output_get_id(obs_output_t *o){return "rtmp_output";}
static bool obs_output_start(obs_output_t *o){o->starts++;o->active=!o->fail&&!o->async;if(o->active&&o->start_signal)o->start_signal(o->start_data,o);return !o->fail;}
static void clear_reserved_path(obs_output_t *o,bool remove){}
static void release_output_task(void *o){obs_output_release(o);}
static signal_handler_t *obs_output_get_signal_handler(obs_output_t *o){return o;}
static void signal_handler_connect(signal_handler_t *o,const char *n,void (*fn)(void*,void*),void *data){if(!strcmp(n,"start")){o->start_signal=fn;o->start_data=data;}else{o->stop=fn;o->data=data;}}
static void signal_handler_disconnect(signal_handler_t *o,const char *n,void (*fn)(void*,void*),void *data){if(!strcmp(n,"start")){if(o->start_signal==fn&&o->start_data==data)o->start_signal=NULL;}else if(o->stop==fn&&o->data==data)o->stop=NULL;}
static void remove_filter(void *a,void *b){}
static void obs_output_force_stop(obs_output_t *o){assert(o->active);o->active=false;o->stops++;if(o->stop){void(*fn)(void*,void*)=o->stop;void *d=o->data;fn(d,NULL);fn(d,NULL);}}
struct job {obs_task_t fn;void *data;};static struct job ui[32],gfx[32],destroy[32];static int nu,ng,nd;
static void run_queued(obs_task_t f,void *d){ui[nu++]=(struct job){f,d};}
static void obs_queue_task(int kind,obs_task_t f,void *d,bool wait){gfx[ng++]=(struct job){f,d};}
static void run_queued_destroy(obs_task_t f,void *d){destroy[nd++]=(struct job){f,d};}
static void drain(struct job *q,int *n){int count=*n;*n=0;for(int i=0;i<count;i++)q[i].fn(q[i].data);}
'''
        names = ("get_output", "output_wanted", "output_can_start", "sync_output_active", "close_filter_showing", "note_start_result",
                 "start_output_unref", "consume_start_results", "start_output_task", "mark_output_event", "output_started", "output_stopped",
                 "unobserve_output", "observe_output", "consume_output_events", "cancel_start", "queue_start", "detach_output",
                 "stop_output_unref", "finish_stop_task", "release_output_stopped", "force_stop_output_task", "queue_force_stop",
                 "invalidate_reset_video")
        self.run_c(prelude + "\n".join(function(n) for n in names) + r'''
int main(void) {
 obs_source_t source={.refs=1,.enabled=true};obs_output_t old={.refs=1},replacement={.refs=1},stream={.refs=1,.active=true};
 struct source_record_filter_context c={.source=&source,.weak_self=&source,.record=true,.fileOutput=&old};source.context=&c;
 queue_start(&c,&old,FILE_OUTPUT);cancel_start(&c,&old);c.fileOutput=&replacement;
 queue_start(&c,&replacement,FILE_OUTPUT);drain(ui,&nu);assert(old.starts==0&&replacement.starts==1);
 consume_start_results(&c);consume_output_events(&c);sync_output_active(&c);drain(destroy,&nd);assert(source.showing==1&&c.output_active&&!c.outputs[0].start);
 for(int i=0;i<20;i++)note_start_result(&c,FILE_OUTPUT,&replacement,false);
 uint64_t attempt=c.outputs[0].next_attempt;note_start_result(&c,STREAM_OUTPUT,&stream,true);
 assert(c.outputs[0].next_attempt==attempt&&c.outputs[0].failures==5);
 c.streamOutput=&stream;replacement.active=false;c.outputs[0].phase=OUTPUT_FAILED;sync_output_active(&c);
 assert(c.output_active&&output_can_start(&c,FILE_OUTPUT,attempt));
 assert(!output_can_start(&c,STREAM_OUTPUT,attempt));
 replacement.active=true;queue_force_stop(&c,&replacement,true);c.fileOutput=NULL;
 sync_output_active(&c);assert(c.output_active&&c.pending_stops==1);
 drain(ui,&nu);drain(destroy,&nd);assert(c.pending_stops==0&&replacement.stops==1&&replacement.refs==0);
 stream.active=false;sync_output_active(&c);assert(!c.output_active&&source.showing==0);
 c.fileOutput=&old;queue_force_stop(&c,&old,false);c.fileOutput=NULL;drain(ui,&nu);drain(destroy,&nd);
 assert(old.stops==0&&old.refs==0&&source.refs==1);
 obs_output_t async={.refs=1,.async=true};c.streamOutput=&async;c.stream=true;
 queue_start(&c,&async,STREAM_OUTPUT);drain(ui,&nu);consume_start_results(&c);consume_output_events(&c);
 assert(c.outputs[STREAM_OUTPUT].phase==OUTPUT_STARTING);
 for(int i=0;i<30;i++)assert(!output_can_start(&c,STREAM_OUTPUT,UINT64_MAX));
 assert(async.starts==1);sync_output_active(&c);assert(c.output_active);
 async.stop(async.data,&async);consume_output_events(&c);sync_output_active(&c);
 assert(c.outputs[STREAM_OUTPUT].phase==OUTPUT_FAILED&&!c.output_active);
 assert(output_can_start(&c,STREAM_OUTPUT,UINT64_MAX));
 queue_force_stop(&c,&async,true);c.streamOutput=NULL;drain(ui,&nu);drain(destroy,&nd);
 assert(source.refs==1&&async.refs==0);
 c.video_reset=true;c.video_output=&old;c.width=c.height=1920;invalidate_reset_video(&c);
 assert(!c.video_reset&&!c.video_output&&!c.width&&!c.height);
 c.output_active=true;source.showing=1;close_filter_showing(&c,&source);
 assert(c.closing&&!c.output_active&&!source.showing);
 close_filter_showing(&c,&source);stream.active=true;sync_output_active(&c);assert(!source.showing);
 obs_output_t closed_output={.refs=1};c.fileOutput=&closed_output;queue_start(&c,&closed_output,FILE_OUTPUT);
 assert(!c.outputs[FILE_OUTPUT].start&&!nu&&source.refs==1&&closed_output.refs==1);
 puts("outputs: stale start cancellation, independent backoff, drainage, idempotent stop, reset PASS");
}
''')

    def test_websocket_filter_identity_and_replay_continuity(self):
        prelude = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define UNUSED_PARAMETER(x) (void)(x)
#define bfree free
#define DARRAY(t) struct {t *array;size_t num;}
#define da_push_back(a,p) do{(a).array=realloc((a).array,((a).num+1)*sizeof(*(a).array));(a).array[(a).num++]=*(p);}while(0)
#define da_free(a) free((a).array)
#define OUTPUT_MODE_NONE 0
static bool os_atomic_load_bool(const volatile bool *v){return *v;}
struct source_record_filter_context {bool output_active,restart,closing;};
typedef struct data {char filter[32],filename[64],replay[64],error[64];bool enabled;int visits;} obs_data_t;
typedef int config_t;
typedef struct source {const char *id,*name;int refs;bool enabled;struct source *children[4];int count;
 struct source_record_filter_context ctx;obs_data_t settings;} obs_source_t;
static obs_source_t *roots[2];
static const char *obs_source_get_unversioned_id(obs_source_t *s){return s->id;}
static const char *obs_source_get_name(obs_source_t *s){return s->name;}
static obs_source_t *obs_source_get_ref(obs_source_t *s){s->refs++;return s;}
static void obs_source_release(obs_source_t *s){if(s){assert(s->refs>0);s->refs--;}}
static void *obs_obj_get_data(obs_source_t *s){return &s->ctx;}
static void *calldata_ptr(void *cd,const char *key){return cd;}
static bool obs_source_enabled(obs_source_t *s){return s->enabled;}
static void obs_source_set_enabled(obs_source_t *s,bool e){s->enabled=e;}
static const char *obs_data_get_string(obs_data_t *d,const char *k){if(!d)return "";if(!strcmp(k,"filter"))return d->filter;if(!strcmp(k,"filename"))return d->filename;return "";}
static void obs_data_set_string(obs_data_t *d,const char *k,const char *v){if(d){if(!strcmp(k,"replay_filename_formatting"))strcpy(d->replay,v);else if(!strcmp(k,"error"))strcpy(d->error,v);}}
static void obs_data_set_bool(obs_data_t *d,const char *k,bool b){if(d)d->enabled=b;}
static obs_data_t *obs_data_create(void){static obs_data_t d;return &d;}
static void obs_data_release(obs_data_t *d){}
static obs_data_t *obs_source_get_settings(obs_source_t *s){return &s->settings;}
static void obs_source_update(obs_source_t *s,obs_data_t *d){s->settings.visits++;}
static config_t *obs_frontend_get_profile_config(void){return NULL;}
static const char *config_get_string(config_t *c,const char *a,const char *b){return "file";}
static char *os_generate_formatted_filename(const char *a,bool b,const char *c){char *p=malloc(strlen(c)+1);strcpy(p,c);return p;}
static obs_source_t *obs_source_create(const char *a,const char *b,obs_data_t *c,void *d){return NULL;}
static void obs_source_filter_add(obs_source_t *s,obs_source_t *f){}
static obs_source_t *obs_source_get_filter_by_name(obs_source_t *s,const char *n){for(int i=0;i<s->count;i++)if(!strcmp(s->children[i]->name,n))return obs_source_get_ref(s->children[i]);return NULL;}
static void obs_source_enum_filters(obs_source_t *s,void(*f)(obs_source_t*,obs_source_t*,void*),void *d){for(int i=0;i<s->count;i++)f(s,s->children[i],d);}
static void obs_enum_sources(bool(*f)(void*,obs_source_t*),void *d){for(int i=0;i<2;i++)f(d,roots[i]);}
static void obs_enum_scenes(bool(*f)(void*,obs_source_t*),void *d){f(d,roots[0]);} /* duplicated enumeration */
struct filter_list {DARRAY(obs_source_t*) items;const char *name;};
'''
        names = ("find_filter", "find_source_by_filter", "find_source", "apply_to_all_filters",
                 "get_source_record_filter", "start_replay_buffer_source")
        self.run_c(prelude + "\n".join(function(n) for n in names) + r'''
static bool stop_one(obs_source_t *filter,obs_data_t *request,obs_data_t *response){assert(!strcmp(filter->id,"source_record_filter"));filter->settings.visits++;return filter->name[0]!='C';}
int main(void){
 obs_source_t a={.id="source_record_filter",.name="A",.refs=1,.enabled=true,.ctx={.output_active=true}};
 obs_source_t b={.id="source_record_filter",.name="B",.refs=1,.enabled=true,.ctx={.output_active=true}};
 obs_source_t c={.id="source_record_filter",.name="C",.refs=1,.enabled=true};
 obs_source_t p={.id="input",.name="parent",.children={&a,&b},.count=2},q={.id="input",.name="other",.children={&c},.count=1};
 roots[0]=&p;roots[1]=&q;obs_data_t request={0},response={0};
 assert(!apply_to_all_filters(&request,&response,stop_one));
 assert(a.settings.visits==1&&b.settings.visits==1&&c.settings.visits==1);
 assert(a.refs==1&&b.refs==1&&c.refs==1);
 strcpy(request.filter,"A");assert(apply_to_all_filters(&request,&response,stop_one));
 assert(a.settings.visits==2&&b.settings.visits==1&&c.settings.visits==1);
 strcpy(request.filename,"replay_new");assert(start_replay_buffer_source(&p,&request,&response));
 assert(!a.ctx.restart&&!b.ctx.restart);assert(!strcmp(a.settings.replay,"replay_new"));
 request.filter[0]=0;assert(start_replay_buffer_source(&p,&request,&response));
 assert(!a.ctx.restart&&!b.ctx.restart);assert(!strcmp(b.settings.replay,"replay_new"));
 assert(a.refs==1&&b.refs==1&&c.refs==1);puts("API: exact fanout, deduplication, aggregate failure, named/unnamed replay continuity PASS");
}
''')

    def test_audio_source_reference_is_held_during_name_read(self):
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
typedef struct source {int refs;const char *name;} obs_source_t;
typedef struct weak {obs_source_t *source;} obs_weak_source_t;
static obs_source_t *obs_weak_source_get_source(obs_weak_source_t *w){if(!w||!w->source)return NULL;w->source->refs++;return w->source;}
static const char *obs_source_get_name(obs_source_t *s){assert(s->refs>0);return s->name;}
static void obs_source_release(obs_source_t *s){if(s){assert(s->refs>0);s->refs--;}}
''' + function("audio_source_matches") + r'''
int main(void){obs_source_t s={0,"audio"};obs_weak_source_t w={&s};
 assert(audio_source_matches(&w,"audio"));assert(!s.refs);
 assert(!audio_source_matches(&w,"different"));assert(!s.refs);
 w.source=NULL;assert(!audio_source_matches(&w,"audio"));puts("audio source reference PASS");}
''')

    def test_idle_audio_codec_and_bitrate_changes(self):
        self.run_c(PRELUDE + audio_function() + r'''
int main(void){struct source_record_filter_context c={0};obs_data_t s={.bitrate=96};strcpy(s.id,"aac");
 update_audio_encoder(&c,&s);s.bitrate=192;strcpy(s.id,"opus");update_audio_encoder(&c,&s);
 assert(!strcmp(c.audioEncoder[0]->id,"opus"));assert(c.audioEncoder[0]->bitrate==192);
 obs_encoder_release(c.audioEncoder[0]);audio_output_close(c.audio_output);obs_data_release(c.applied_audio_settings);
 assert(opens==closes);puts("idle codec and bitrate PASS");}
''')

    def test_starting_output_defers_audio_swap_until_idle(self):
        self.run_c(PRELUDE + audio_function() + function("update_video_encoder") + r'''
int main(void){
 int phases[]={OUTPUT_QUEUED,OUTPUT_STARTING,OUTPUT_ACTIVE};
 for(int i=0;i<3;i++){
  struct source_record_filter_context c={0};obs_data_t s={.bitrate=96};strcpy(s.id,"aac");
  update_audio_encoder(&c,&s);audio_t *old=c.audio_output;obs_encoder_t *encoder=c.audioEncoder[0];
  obs_encoder_t video={0};obs_output_t replay={0};c.replayOutput=&replay;
  c.encoder=&video;c.video_output=&c;video_updates=scale_updates=0;
  c.outputs[STREAM_OUTPUT].phase=phases[i];s.different=true;s.track=-1;s.bitrate=192;strcpy(s.id,"opus");
  update_audio_encoder(&c,&s);update_video_encoder(&c,&s);
  assert(!video_updates&&!scale_updates&&replay.video==&video);
  assert(c.audio_settings_pending&&c.audio_output==old&&c.audioEncoder[0]==encoder&&!strcmp(encoder->id,"aac"));
  c.outputs[STREAM_OUTPUT].phase=OUTPUT_FAILED;c.pending_stops=1;update_audio_encoder(&c,&s);
  assert(c.audio_output==old&&c.audioEncoder[0]==encoder);
  c.pending_stops=0;update_audio_encoder(&c,&s);update_video_encoder(&c,&s);
  assert(video_updates==1&&scale_updates==1);
  assert(!c.audio_settings_pending&&c.audio_output==&global_audio&&!strcmp(c.audioEncoder[0]->id,"opus"));
  for(int j=0;j<6;j++)obs_encoder_release(c.audioEncoder[j]);obs_data_release(c.applied_audio_settings);
 }
 assert(opens==closes);}
''')

    def test_audio_idle_callback_needs_no_published_device(self):
        callback = function("audio_input_callback")
        prefix = callback[:callback.index("\tobs_source_t *audio_source =")]
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define UNUSED_PARAMETER(x) (void)(x)
typedef void obs_source_t;
struct audio_output_data {int unused;};
struct source_record_filter_context {void *source,*audio_output;bool closing;};
static bool os_atomic_load_bool(const volatile bool *v){return *v;}
static int source_reads;
static bool obs_source_removed(void *p){source_reads++;return false;}
''' + prefix + "return false;\n}\n" + r'''
int main(void){struct source_record_filter_context c={0};uint64_t out=0;
 assert(audio_input_callback(&c,777,999,&out,0,NULL));assert(out==777&&source_reads==0);
 assert(audio_input_callback(&c,888,999,&out,1,NULL));assert(out==888&&source_reads==0);
 c.audio_output=&c;assert(!audio_input_callback(&c,0,999,&out,1,NULL));assert(source_reads==1);}
''')

    def test_frontend_registration_and_removal_are_balanced(self):
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#define OBS_TASK_UI 1
#define OBS_INVALID_HOTKEY_PAIR_ID 0
#define OBS_INVALID_HOTKEY_ID 0
#define bzalloc(n) calloc(1,n)
#define da_push_back(a,p) do{(a).value=*(p);(a).num++;}while(0)
typedef void (*obs_task_t)(void *);
typedef void obs_data_t;
typedef void obs_source_t;
typedef void obs_weak_source_t;
struct source_record_filter_context {
 void *source,*weak_self;int last_frontend_event,enableHotkey,pauseHotkeys,splitHotkey,chapterHotkey;
 bool frontend_callback_registered,exiting;
};
static struct {void *value;int num;} source_record_filters;
static pthread_mutex_t filters_mutex=PTHREAD_MUTEX_INITIALIZER;
static bool os_atomic_set_bool(volatile bool *v,bool n){bool old=*v;*v=n;return old;}
static void os_atomic_store_bool(volatile bool *v,bool n){*v=n;}
static bool os_atomic_load_bool(const volatile bool *v){return *v;}
static int registered,removed,connected,disconnected,waits,queued,weak_refs,initial_updates,deferred_updates;
static bool ui_thread;
static obs_task_t queued_task;
static void *queued_data;
static void frontend_event(int event,void *data){}
static void on_video_reset(void *data,void *cd){}
static void *obs_get_signal_handler(void){return NULL;}
static void signal_handler_connect(void *sh,const char *n,void(*fn)(void*,void*),void *d){connected++;}
static void signal_handler_disconnect(void *sh,const char *n,void(*fn)(void*,void*),void *d){disconnected++;}
static void obs_frontend_add_event_callback(void(*fn)(int,void*),void *d){registered++;}
static void obs_frontend_remove_event_callback(void(*fn)(int,void*),void *d){removed++;}
static void *obs_source_get_weak_source(void *s){weak_refs++;return s;}
static void obs_weak_source_addref(void *s){weak_refs++;}
static void obs_weak_source_release(void *s){weak_refs--;assert(weak_refs>=0);}
static void source_record_filter_update(void *d,void *settings){initial_updates++;}
static void obs_source_update(void *s,void *settings){assert(s==(void*)42);deferred_updates++;}
static bool obs_in_task_thread(int kind){return ui_thread;}
static void obs_queue_task(int kind,obs_task_t fn,void *v,bool wait){assert(!wait);queued++;queued_task=fn;queued_data=v;}
''' + "\n".join(function(n) for n in ("remove_frontend_cb_task", "remove_frontend_cb", "source_record_filter_create", "source_record_filter_load")) + r'''
int main(void){struct source_record_filter_context *c=source_record_filter_create(NULL,(void*)42);
 assert(c->frontend_callback_registered&&registered==1&&connected==1);
 source_record_filter_load(c,NULL);assert(initial_updates==1&&deferred_updates==1);
 remove_frontend_cb(c);remove_frontend_cb(c);assert(removed==0&&disconnected==1&&queued==1&&weak_refs==2);
 /* The UI is joining teardown: free the context before it drains its queue. */
 obs_weak_source_release(c->weak_self);free(c);assert(weak_refs==1);
 queued_task(queued_data);assert(removed==1&&weak_refs==0);
 c=source_record_filter_create(NULL,(void*)42);c->exiting=true;ui_thread=true;
 remove_frontend_cb(c);assert(registered==2&&connected==2&&removed==2&&disconnected==2&&queued==1);
 obs_weak_source_release(c->weak_self);free(c);assert(weak_refs==0);}
''')

    def test_frontend_callback_uses_weak_reference_after_context_teardown(self):
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
typedef void obs_source_t;
enum obs_frontend_event {
 OBS_FRONTEND_EVENT_STREAMING_STARTING, OBS_FRONTEND_EVENT_STREAMING_STARTED,
 OBS_FRONTEND_EVENT_STREAMING_STOPPING, OBS_FRONTEND_EVENT_STREAMING_STOPPED,
 OBS_FRONTEND_EVENT_RECORDING_STARTING, OBS_FRONTEND_EVENT_RECORDING_STARTED,
 OBS_FRONTEND_EVENT_RECORDING_STOPPING, OBS_FRONTEND_EVENT_RECORDING_STOPPED,
 OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED, OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED,
 OBS_FRONTEND_EVENT_RECORDING_PAUSED, OBS_FRONTEND_EVENT_RECORDING_UNPAUSED,
 OBS_FRONTEND_EVENT_EXIT, OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN,
 OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP
};
#define OUTPUT_COUNT 3
struct source_record_filter_context {void *source;bool closing,exiting,max_seconds_reached;int last_frontend_event;};
static struct source_record_filter_context context={.source=(void*)42};
static bool live;
static int upgrades,releases,updates,removals;
static void *obs_weak_source_get_source(void *w){assert(w==(void*)43);upgrades++;return live?context.source:NULL;}
static void *obs_obj_get_data(void *s){assert(s==context.source);return &context;}
static void obs_source_release(void *s){assert(s==context.source);releases++;}
static void obs_source_update(void *s,void *v){assert(s==context.source);updates++;}
static bool os_atomic_load_bool(const volatile bool *v){return *v;}
static void os_atomic_store_bool(volatile bool *v,bool n){*v=n;}
static void *get_output(struct source_record_filter_context *c,int i){return NULL;}
static void cancel_start(struct source_record_filter_context *c,void *o){}
static void remove_frontend_cb(struct source_record_filter_context *c){removals++;}
''' + function("frontend_event") + r'''
int main(void){
 frontend_event(OBS_FRONTEND_EVENT_RECORDING_STARTED,(void*)43);
 assert(upgrades==1&&releases==0&&updates==0);
 live=true;frontend_event(OBS_FRONTEND_EVENT_RECORDING_STARTED,(void*)43);
 assert(upgrades==2&&releases==1&&updates==1);
 context.closing=true;frontend_event(OBS_FRONTEND_EVENT_RECORDING_STARTED,(void*)43);
 assert(releases==2&&updates==1);
 frontend_event(OBS_FRONTEND_EVENT_EXIT,(void*)43);
 assert(releases==3&&updates==2&&removals==1&&context.exiting);
 live=false;frontend_event(OBS_FRONTEND_EVENT_EXIT,(void*)43);
 assert(releases==3&&updates==2&&removals==1);}
''')


if __name__ == "__main__":
    unittest.main()
