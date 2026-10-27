"""Reservation rollback when libobs cannot allocate an output.

Executes the complete production start_file_output and reservation helpers with
an OBS adapter that fails output creation after the real file was reserved.
No OBS, sockets or installed libraries are used.
"""
import unittest

import test_c_regressions as regression


class OutputCreationTests(unittest.TestCase):
    def test_null_output_creation_removes_reservation_and_records_retry(self):
        adapter = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
extern char *mkdtemp(char *);
#define MAX_AUDIO_MIXES 6
#define FILE_OUTPUT 0
#define bfree free
struct dstr {char *array;size_t len;};
static void dstr_copy(struct dstr *s,const char *v) {
 free(s->array);s->len=strlen(v);s->array=malloc(s->len+1);memcpy(s->array,v,s->len+1);
}
static void dstr_ncat(struct dstr *s,const char *v,size_t n) {
 assert(!s->array);s->array=calloc(1,n+1);memcpy(s->array,v,n);s->len=n;
}
static void dstr_printf(struct dstr *s,const char *f,...) {
 va_list a;va_start(a,f);char b[4096];vsnprintf(b,sizeof(b),f,a);va_end(a);dstr_copy(s,b);
}
static void dstr_catf(struct dstr *s,const char *f,...) {
 va_list a;va_start(a,f);char b[4096];vsnprintf(b,sizeof(b),f,a);va_end(a);
 size_t n=strlen(b);s->array=realloc(s->array,s->len+n+1);memcpy(s->array+s->len,b,n+1);s->len+=n;
}
static void dstr_free(struct dstr *s) {free(s->array);s->array=NULL;s->len=0;}
typedef struct settings {char path[1024],reserved[1024];} obs_data_t;
typedef struct output {int unused;} obs_output_t;
typedef int obs_encoder_t;
typedef int signal_handler_t;
typedef int config_t;
struct source_record_filter_context {
 void *source,*audio_output;struct dstr rec_path,rec_format,rec_filename;
 obs_output_t *fileOutput;obs_encoder_t *encoder,*audioEncoder[MAX_AUDIO_MIXES];bool remove_after_record;
};
static volatile long filename_session;
static int allocations,creates,retries,queued;
static char reserved_path[1024];
static const char *obs_data_get_string(obs_data_t *s,const char *k) {
 if(!strcmp(k,"path"))return s->path;
 if(!strcmp(k,"sr_reserved_path"))return s->reserved;
 if(!strcmp(k,"rec_format"))return "mp4";
 if(!strcmp(k,"filename_formatting"))return "capture";
 return "";
}
static bool obs_data_get_bool(obs_data_t *s,const char *k) {return false;}
static long long obs_data_get_int(obs_data_t *s,const char *k) {return 0;}
static obs_data_t *obs_data_create(void) {allocations++;return calloc(1,sizeof(obs_data_t));}
static void obs_data_release(obs_data_t *s) {assert(s);allocations--;free(s);}
static void obs_data_set_string(obs_data_t *s,const char *k,const char *v) {
 if(!strcmp(k,"path"))snprintf(s->path,sizeof(s->path),"%s",v);
 if(!strcmp(k,"sr_reserved_path"))snprintf(s->reserved,sizeof(s->reserved),"%s",v);
}
static void obs_data_set_bool(obs_data_t *s,const char *k,bool v) {}
static void obs_data_set_int(obs_data_t *s,const char *k,long long v) {}
static config_t *obs_frontend_get_profile_config(void) {return NULL;}
static const char *config_get_string(config_t *c,const char *a,const char *b) {return "Simple";}
static bool config_get_bool(config_t *c,const char *a,const char *b) {return false;}
static const char *obs_source_get_name(void *s) {return "fixture";}
static uint64_t os_gettime_ns(void) {return 1;}
static long os_atomic_inc_long(volatile long *v) {return ++*v;}
static char *os_generate_formatted_filename(const char *ext,bool space,const char *format) {
 char *p=malloc(12);memcpy(p,"capture.mp4",12);return p;
}
#define os_fopen fopen
#define os_unlink unlink
static int os_mkdirs(const char *p) {struct stat st;return stat(p,&st)?mkdir(p,0700):0;}
static int64_t os_get_file_size(const char *p) {struct stat st;return stat(p,&st)?-1:st.st_size;}
static void clear_reserved_path(obs_output_t *o,bool remove) {assert(!o);}
static void unobserve_output(struct source_record_filter_context *c,int kind) {assert(kind==FILE_OUTPUT);}
static void obs_output_release(obs_output_t *o) {assert(!o);}
static const char *obs_output_get_id(obs_output_t *o) {return "ffmpeg_muxer";}
static obs_output_t *obs_output_create(const char *id,const char *name,obs_data_t *s,void *hotkeys) {
 assert(!strcmp(id,"ffmpeg_muxer"));assert(*s->reserved);
 assert(os_get_file_size(s->reserved)==0); /* Failure occurs after exclusive reservation. */
 if(creates)assert(!strcmp(reserved_path,s->reserved));
 snprintf(reserved_path,sizeof(reserved_path),"%s",s->reserved);creates++;return NULL;
}
static signal_handler_t *obs_output_get_signal_handler(obs_output_t *o) {assert(!o);return NULL;}
static void signal_handler_connect(signal_handler_t *s,const char *n,void(*f)(void*,void*),void *d) {assert(!s);}
static void remove_filter(void *d,void *cd) {}
static void obs_output_update(obs_output_t *o,obs_data_t *s) {assert(false);}
static void note_start_result(struct source_record_filter_context *c,int kind,obs_output_t *o,bool started) {
 assert(kind==FILE_OUTPUT&&!o&&!started);retries++;
}
static void update_video_encoder(struct source_record_filter_context *c,obs_data_t *s) {assert(false);}
static void obs_output_set_video_encoder(obs_output_t *o,obs_encoder_t *e) {assert(false);}
static void obs_encoder_set_audio(obs_encoder_t *e,void *a) {assert(false);}
static void obs_output_set_audio_encoder(obs_output_t *o,obs_encoder_t *e,int i) {assert(false);}
static void queue_start(struct source_record_filter_context *c,obs_output_t *o,int kind) {queued++;}
'''
        code = adapter + "\n".join(regression.function(name) for name in (
            "GetFormatExt", "ensure_directory", "dedup_path", "start_file_output")) + r'''
int main(void) {
 char directory[]="/tmp/sr-output-create-XXXXXX";assert(mkdtemp(directory));
 obs_data_t settings={0};snprintf(settings.path,sizeof(settings.path),"%s",directory);
 struct source_record_filter_context context={.remove_after_record=true};
 for(int attempt=1;attempt<=2;attempt++) {
  start_file_output(&context,&settings);
  assert(!context.fileOutput);assert(creates==attempt&&retries==attempt);
  assert(queued==0);assert(access(reserved_path,F_OK)==-1&&errno==ENOENT);assert(allocations==0);
 }
 dstr_free(&context.rec_path);dstr_free(&context.rec_format);dstr_free(&context.rec_filename);
 assert(!rmdir(directory));puts("NULL output: reservation rollback, retry, no queued start PASS");
}
'''
        regression.CRegressions().run_c(code)


if __name__ == "__main__":
    unittest.main()
