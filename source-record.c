#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>
#include "version.h"
#include "obs-websocket-api.h"
#include <errno.h>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#define OUTPUT_MODE_NONE 0
#define OUTPUT_MODE_ALWAYS 1
#define OUTPUT_MODE_STREAMING 2
#define OUTPUT_MODE_RECORDING 3
#define OUTPUT_MODE_STREAMING_OR_RECORDING 4
#define OUTPUT_MODE_VIRTUAL_CAMERA 5

#define BACKGROUND_CHANNEL 0
#define SOURCE_CHANNEL 1

enum output_kind { FILE_OUTPUT, STREAM_OUTPUT, REPLAY_OUTPUT, OUTPUT_COUNT };
struct start_output;
enum output_phase { OUTPUT_IDLE, OUTPUT_QUEUED, OUTPUT_STARTING, OUTPUT_ACTIVE, OUTPUT_FAILED, OUTPUT_STOPPING };
struct output_state {
	uint64_t next_attempt;
	unsigned failures;
	struct start_output *start;
	enum output_phase phase;
	obs_output_t *observed_output;
	bool started_event;
	bool stopped_event;
};

struct source_record_filter_context {
	obs_source_t *source;
	obs_weak_source_t *weak_self;
	video_t *video_output;
	audio_t *audio_output;
	bool owns_audio_output;
	bool audio_settings_pending;
	obs_data_t *applied_audio_settings;
	bool output_active;
	uint32_t width;
	uint32_t height;
	uint64_t last_frame_time_ns;
	obs_view_t *view;
	bool restart;
	obs_output_t *fileOutput;
	obs_output_t *streamOutput;
	obs_output_t *replayOutput;
	obs_encoder_t *encoder;
	obs_encoder_t *audioEncoder[MAX_AUDIO_MIXES];
	obs_service_t *service;
	bool record;
	bool stream;
	bool replayBuffer;
	obs_hotkey_pair_id enableHotkey;
	obs_hotkey_pair_id pauseHotkeys;
	obs_hotkey_id splitHotkey;
	obs_hotkey_id chapterHotkey;
	int audio_track;
	obs_weak_source_t *audio_source;
	bool closing;
	bool exiting;
	volatile bool frontend_callback_registered;
	long long replay_buffer_duration;
	struct vec4 backgroundColor;
	bool remove_after_record;
	long long record_max_seconds;
	int last_frontend_event;
	bool video_reset;
	struct obs_video_info cached_ovi;
	struct output_state outputs[OUTPUT_COUNT];
	volatile long pending_stops;
	bool max_seconds_reached;
	/* Last recording path/format/filename applied to the live fileOutput, so a
	 * change while recording can be detected and the output restarted. */
	struct dstr rec_path;
	struct dstr rec_format;
	struct dstr rec_filename;
};

DARRAY(obs_source_t *) source_record_filters;
/* Guards source_record_filters: pushed from create (may run on the websocket
 * thread), erased from destroy (destruction task thread), iterated in
 * get_properties (UI thread). Holding it during iteration also keeps the
 * struct alive, since destroy's erase blocks before its bfree. */
static pthread_mutex_t filters_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t start_tasks_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile long filename_session;

static void *vendor;

static void run_queued(obs_task_t task, void *param)
{
	if (obs_in_task_thread(OBS_TASK_UI) && obs_get_video()) {
		obs_queue_task(OBS_TASK_GRAPHICS, task, param, false);
	} else {
		obs_queue_task(OBS_TASK_UI, task, param, false);
	}
}

/* Output/encoder releases must go to libobs' dedicated destruction thread:
 * obs_output_destroy blocks in os_event_wait(stopping_event), and ffmpeg-mux
 * deactivation waits on its child process. Running that on the graphics task
 * queue freezes every video mix (all recordings stall at once) and hangs OBS
 * shutdown — the upstream #99 failure mode. */
static void run_queued_destroy(obs_task_t task, void *param)
{
	obs_queue_task(OBS_TASK_DESTROY, task, param, false);
}

static const char *source_record_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Source Record";
}

struct video_frame {
	uint8_t *data[MAX_AV_PLANES];
	uint32_t linesize[MAX_AV_PLANES];
};

static bool EncoderAvailable(const char *encoder)
{
	const char *val;
	int i = 0;

	while (obs_enum_encoder_types(i++, &val))
		if (strcmp(val, encoder) == 0)
			return true;

	return false;
}

/* B10: obs_source_enum_active_tree visits a nested composite (scene/group) AND
 * all of its leaves, so summing every node double-counts audio that already
 * lives in the composite's own submix. Skip nodes whose enumerated parent is a
 * composite other than the top-level source we started from. */
struct calc_min_ts_ctx {
	obs_source_t *top;
	uint64_t *min_ts;
};

struct mix_audio_ctx {
	obs_source_t *top;
	struct obs_source_audio *mixed_audio;
};

static inline bool in_nested_composite(obs_source_t *parent, obs_source_t *top)
{
	return parent && parent != top && (obs_source_get_output_flags(parent) & OBS_SOURCE_COMPOSITE) != 0;
}

static void calc_min_ts(obs_source_t *parent, obs_source_t *child, void *param)
{
	struct calc_min_ts_ctx *ctx = param;
	if (in_nested_composite(parent, ctx->top))
		return;
	if (!child || obs_source_audio_pending(child))
		return;
	const uint64_t ts = obs_source_get_audio_timestamp(child);
	if (!ts)
		return;
	if (!*ctx->min_ts || ts < *ctx->min_ts)
		*ctx->min_ts = ts;
}

static void mix_audio(obs_source_t *parent, obs_source_t *child, void *param)
{
	struct mix_audio_ctx *ctx = param;
	if (in_nested_composite(parent, ctx->top))
		return;
	if (!child || obs_source_audio_pending(child))
		return;
	const uint64_t ts = obs_source_get_audio_timestamp(child);
	if (!ts)
		return;
	struct obs_source_audio *mixed_audio = ctx->mixed_audio;
	const size_t pos = (size_t)ns_to_audio_frames(mixed_audio->samples_per_sec, ts - mixed_audio->timestamp);

	if (pos > AUDIO_OUTPUT_FRAMES)
		return;

	const size_t count = AUDIO_OUTPUT_FRAMES - pos;

	struct obs_source_audio_mix child_audio;
	obs_source_get_audio_mix(child, &child_audio);
	for (size_t ch = 0; ch < (size_t)mixed_audio->speakers; ch++) {
		float *out = ((float *)mixed_audio->data[ch]) + pos;
		float *in = child_audio.output[0].data[ch];
		if (!in)
			continue;
		for (size_t i = 0; i < count; i++) {
			out[i] += in[i];
		}
	}
}

static bool audio_input_callback(void *param, uint64_t start_ts_in, uint64_t end_ts_in, uint64_t *out_ts, uint32_t mixers,
				 struct audio_output_data *mixes)
{
	UNUSED_PARAMETER(end_ts_in);
	struct source_record_filter_context *filter = param;
	if (!mixers || !filter->audio_output || os_atomic_load_bool(&filter->closing) || obs_source_removed(filter->source)) {
		*out_ts = start_ts_in;
		return true;
	}

	obs_source_t *audio_source = NULL;
	bool release_audio = false;
	if (filter->audio_source) {
		audio_source = obs_weak_source_get_source(filter->audio_source);
		if (audio_source)
			release_audio = true;
	} else {
		audio_source = obs_filter_get_parent(filter->source);
	}
	if (!audio_source || obs_source_removed(audio_source)) {
		*out_ts = start_ts_in;
		if (release_audio)
			obs_source_release(audio_source);
		return true;
	}

	const uint32_t flags = obs_source_get_output_flags(audio_source);
	if ((flags & OBS_SOURCE_COMPOSITE) != 0) {
		uint64_t min_ts = 0;
		struct calc_min_ts_ctx cctx = {audio_source, &min_ts};
		obs_source_enum_active_tree(audio_source, calc_min_ts, &cctx);
		if (min_ts) {
			struct obs_source_audio mixed_audio = {0};
			for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
				mixed_audio.data[i] = (uint8_t *)mixes->data[i];
			}
			mixed_audio.timestamp = min_ts;
			mixed_audio.speakers = audio_output_get_channels(filter->audio_output);
			mixed_audio.samples_per_sec = audio_output_get_sample_rate(filter->audio_output);
			mixed_audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
			struct mix_audio_ctx mctx = {audio_source, &mixed_audio};
			obs_source_enum_active_tree(audio_source, mix_audio, &mctx);

			for (size_t mix_idx = 0; mix_idx < MAX_AUDIO_MIXES; mix_idx++) {
				if ((mixers & (1 << mix_idx)) == 0)
					continue;
				// clamp audio
				for (size_t ch = 0; ch < (size_t)mixed_audio.speakers; ch++) {
					float *mix_data = mixes[mix_idx].data[ch];
					float *mix_end = &mix_data[AUDIO_OUTPUT_FRAMES];

					while (mix_data < mix_end) {
						float val = *mix_data;
						val = (val > 1.0f) ? 1.0f : val;
						val = (val < -1.0f) ? -1.0f : val;
						*(mix_data++) = val;
					}
				}
			}
			*out_ts = min_ts;
		} else {
			*out_ts = start_ts_in;
		}
		if (release_audio)
			obs_source_release(audio_source);
		return true;
	}
	if ((flags & OBS_SOURCE_AUDIO) == 0) {
		*out_ts = start_ts_in;
		if (release_audio)
			obs_source_release(audio_source);
		return true;
	}

	const uint64_t source_ts = obs_source_get_audio_timestamp(audio_source);
	if (!source_ts) {
		*out_ts = start_ts_in;
		if (release_audio)
			obs_source_release(audio_source);
		return true;
	}

	if (obs_source_audio_pending(audio_source)) {
		/* B11: never return false -> the audio thread drops the whole tick,
		 * splicing 1024 samples the encoder can't compensate (click +
		 * cumulative A/V drift). Emit silence for this tick instead (the mix
		 * buffers are already zeroed by input_and_output). */
		*out_ts = start_ts_in;
		if (release_audio)
			obs_source_release(audio_source);
		return true;
	}

	struct obs_source_audio_mix audio;
	obs_source_get_audio_mix(audio_source, &audio);

	const size_t channels = audio_output_get_channels(filter->audio_output);
	for (size_t mix_idx = 0; mix_idx < MAX_AUDIO_MIXES; mix_idx++) {
		if ((mixers & (1 << mix_idx)) == 0)
			continue;
		for (size_t ch = 0; ch < channels; ch++) {
			float *out = mixes[mix_idx].data[ch];
			float *in = audio.output[0].data[ch];
			if (!in)
				continue;
			for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++) {
				out[i] += in[i];
				if (out[i] > 1.0f)
					out[i] = 1.0f;
				if (out[i] < -1.0f)
					out[i] = -1.0f;
			}
		}
	}

	*out_ts = source_ts;
	if (release_audio)
		obs_source_release(audio_source);

	return true;
}

static const char *GetFormatExt(const char *format)
{
	if (strcmp(format, "fragmented_mp4") == 0)
		return "mp4";
	if (strcmp(format, "hybrid_mp4") == 0)
		return "mp4";
	if (strcmp(format, "hybrid_mov") == 0)
		return "mov";
	if (strcmp(format, "fragmented_mov") == 0)
		return "mov";
	if (strcmp(format, "hls") == 0)
		return "m3u8";
	if (strcmp(format, "mpegts") == 0)
		return "ts";
	return format;
}

static void clear_reserved_path(obs_output_t *output, bool remove_empty)
{
	if (!output)
		return;
	obs_data_t *settings = obs_output_get_settings(output);
	const char *path = obs_data_get_string(settings, "sr_reserved_path");
	if (remove_empty && *path && os_get_file_size(path) == 0)
		os_unlink(path);
	obs_data_erase(settings, "sr_reserved_path");
	obs_data_release(settings);
}

static void release_output_task(void *data)
{
	obs_output_t *output = data;
	clear_reserved_path(output, true);
	obs_output_release(output);
}

static obs_output_t *get_output(struct source_record_filter_context *context, enum output_kind kind)
{
	return kind == FILE_OUTPUT ? context->fileOutput : kind == STREAM_OUTPUT ? context->streamOutput : context->replayOutput;
}

static bool output_wanted(struct source_record_filter_context *context, enum output_kind kind)
{
	return !os_atomic_load_bool(&context->closing) && obs_source_enabled(context->source) &&
		(kind == FILE_OUTPUT ? context->record : kind == STREAM_OUTPUT ? context->stream : context->replayBuffer);
}

static bool output_can_start(struct source_record_filter_context *context, enum output_kind kind, uint64_t now)
{
	pthread_mutex_lock(&start_tasks_mutex);
	bool starting = context->outputs[kind].start != NULL;
	pthread_mutex_unlock(&start_tasks_mutex);
	return output_wanted(context, kind) && !starting &&
		context->outputs[kind].phase != OUTPUT_STARTING && context->outputs[kind].phase != OUTPUT_ACTIVE &&
		!obs_output_active(get_output(context, kind)) && !os_atomic_load_long(&context->pending_stops) &&
		now >= context->outputs[kind].next_attempt;
}

static void sync_output_active(struct source_record_filter_context *context)
{
	pthread_mutex_lock(&start_tasks_mutex);
	if (os_atomic_load_bool(&context->closing)) {
		pthread_mutex_unlock(&start_tasks_mutex);
		return;
	}
	bool active = os_atomic_load_bool(&context->output_active) && os_atomic_load_long(&context->pending_stops) > 0;
	for (int i = 0; i < OUTPUT_COUNT; i++)
		active = active || context->outputs[i].phase == OUTPUT_STARTING || obs_output_active(get_output(context, i));
	if (active != os_atomic_load_bool(&context->output_active)) {
		obs_source_t *parent = obs_filter_get_parent(context->source);
		if (parent) {
			if (active)
				obs_source_inc_showing(parent);
			else
				obs_source_dec_showing(parent);
		}
		os_atomic_store_bool(&context->output_active, active);
	}
	pthread_mutex_unlock(&start_tasks_mutex);
}

static void close_filter_showing(struct source_record_filter_context *context, obs_source_t *parent)
{
	pthread_mutex_lock(&start_tasks_mutex);
	os_atomic_store_bool(&context->closing, true);
	if (os_atomic_set_bool(&context->output_active, false) && parent)
		obs_source_dec_showing(parent);
	pthread_mutex_unlock(&start_tasks_mutex);
}

struct start_output {
	struct source_record_filter_context *context;
	obs_source_t *source_ref;
	obs_output_t *output;
	enum output_kind kind;
	volatile bool cancelled;
	volatile bool done;
	volatile long refs;
	bool started;
};

static void note_start_result(struct source_record_filter_context *context, enum output_kind kind,
			      obs_output_t *output, bool started)
{
	struct output_state *state = &context->outputs[kind];
	if (started) {
		state->failures = 0;
		state->next_attempt = 0;
	} else {
		if (!state->failures)
			blog(LOG_WARNING, "[Source Record] output '%s' failed to start: %s",
			     obs_source_get_name(context->source), output ? obs_output_get_last_error(output) : "creation/path failed");
		unsigned shift = state->failures < 4 ? state->failures : 4;
		if (state->failures < 5)
			state->failures++;
		state->next_attempt = os_gettime_ns() + (1000000000ULL << shift);
	}
}

static void start_output_unref(struct start_output *so)
{
	if (os_atomic_dec_long(&so->refs) != 0)
		return;
	run_queued_destroy((obs_task_t)obs_output_release, so->output);
	obs_source_release(so->source_ref);
	bfree(so);
}

/* Results stay in the ticket until consumed or cancelled. Graphics queues are
 * discarded by obs_reset_video, so they must not own refcounted payloads. */
static void consume_start_results(struct source_record_filter_context *context)
{
	for (int i = 0; i < OUTPUT_COUNT; i++) {
		pthread_mutex_lock(&start_tasks_mutex);
		struct start_output *so = context->outputs[i].start;
		if (so && os_atomic_load_bool(&so->done))
			context->outputs[i].start = NULL;
		else
			so = NULL;
		pthread_mutex_unlock(&start_tasks_mutex);
		if (!so)
			continue;
		if (!os_atomic_load_bool(&so->cancelled) && get_output(context, i) == so->output && !os_atomic_load_bool(&context->closing) &&
		    context->outputs[i].phase == OUTPUT_QUEUED) {
			context->outputs[i].phase = so->started ? OUTPUT_STARTING : OUTPUT_FAILED;
			if (!so->started)
				note_start_result(context, i, so->output, false);
		}
		start_output_unref(so);
	}
}

static void start_output_task(void *data)
{
	struct start_output *so = data;
	if (!os_atomic_load_bool(&so->cancelled))
		so->started = obs_output_start(so->output);
	if (so->kind == FILE_OUTPUT)
		clear_reserved_path(so->output, !so->started);
	os_atomic_set_bool(&so->done, true);
	start_output_unref(so); /* UI task */
}

static void mark_output_event(obs_weak_source_t *weak, calldata_t *cd, bool started)
{
	obs_source_t *source = obs_weak_source_get_source(weak);
	if (!source)
		return;
	struct source_record_filter_context *context = obs_obj_get_data(source);
	obs_output_t *output = calldata_ptr(cd, "output");
	pthread_mutex_lock(&start_tasks_mutex);
	for (int i = 0; context && i < OUTPUT_COUNT; i++) {
		struct output_state *state = &context->outputs[i];
		if (state->observed_output == output) {
			if (started)
				state->started_event = true;
			else
				state->stopped_event = true;
		}
	}
	pthread_mutex_unlock(&start_tasks_mutex);
	obs_source_release(source);
}

static void output_started(void *data, calldata_t *cd)
{
	mark_output_event(data, cd, true);
}

static void output_stopped(void *data, calldata_t *cd)
{
	mark_output_event(data, cd, false);
}

static void unobserve_output(struct source_record_filter_context *context, enum output_kind kind)
{
	pthread_mutex_lock(&start_tasks_mutex);
	obs_output_t *output = context->outputs[kind].observed_output;
	context->outputs[kind].observed_output = NULL;
	context->outputs[kind].started_event = false;
	context->outputs[kind].stopped_event = false;
	pthread_mutex_unlock(&start_tasks_mutex);
	if (output) {
		signal_handler_t *sh = obs_output_get_signal_handler(output);
		signal_handler_disconnect(sh, "start", output_started, context->weak_self);
		signal_handler_disconnect(sh, "stop", output_stopped, context->weak_self);
	}
}

static void observe_output(struct source_record_filter_context *context, enum output_kind kind, obs_output_t *output)
{
	unobserve_output(context, kind);
	pthread_mutex_lock(&start_tasks_mutex);
	context->outputs[kind].observed_output = output;
	pthread_mutex_unlock(&start_tasks_mutex);
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_connect(sh, "start", output_started, context->weak_self);
	signal_handler_connect(sh, "stop", output_stopped, context->weak_self);
}

static void consume_output_events(struct source_record_filter_context *context)
{
	for (int i = 0; i < OUTPUT_COUNT; i++) {
		pthread_mutex_lock(&start_tasks_mutex);
		bool started = context->outputs[i].started_event;
		bool stopped = context->outputs[i].stopped_event;
		context->outputs[i].started_event = context->outputs[i].stopped_event = false;
		pthread_mutex_unlock(&start_tasks_mutex);
		struct output_state *state = &context->outputs[i];
		if (started) {
			state->phase = OUTPUT_ACTIVE;
			note_start_result(context, i, get_output(context, i), true);
		}
		if (stopped && state->phase != OUTPUT_FAILED) {
			state->phase = output_wanted(context, i) ? OUTPUT_FAILED : OUTPUT_IDLE;
			if (state->phase == OUTPUT_FAILED)
				note_start_result(context, i, get_output(context, i), false);
		}
	}
}

static void cancel_start(struct source_record_filter_context *context, obs_output_t *output)
{
	for (int i = 0; i < OUTPUT_COUNT; i++) {
		pthread_mutex_lock(&start_tasks_mutex);
		struct start_output *so = context->outputs[i].start;
		if (so && so->output == output) {
			os_atomic_set_bool(&so->cancelled, true);
			context->outputs[i].start = NULL;
		} else {
			so = NULL;
		}
		pthread_mutex_unlock(&start_tasks_mutex);
		if (so)
			start_output_unref(so); /* context registry */
	}
}

static void queue_start(struct source_record_filter_context *context, obs_output_t *output, enum output_kind kind)
{
	if (!output) {
		note_start_result(context, kind, NULL, false);
		return;
	}
	pthread_mutex_lock(&start_tasks_mutex);
	if (context->outputs[kind].start || obs_output_active(output)) {
		pthread_mutex_unlock(&start_tasks_mutex);
		return;
	}
	struct start_output *so = bzalloc(sizeof(*so));
	so->context = context;
	so->source_ref = obs_source_get_ref(context->source);
	if (!so->source_ref) {
		pthread_mutex_unlock(&start_tasks_mutex);
		bfree(so);
		return;
	}
	so->refs = 2; /* context registry and UI task */
	so->output = obs_output_get_ref(output);
	so->kind = kind;
	context->outputs[kind].start = so;
	context->outputs[kind].phase = OUTPUT_QUEUED;
	pthread_mutex_unlock(&start_tasks_mutex);
	observe_output(context, kind, output);
	run_queued(start_output_task, so);
}

static void release_encoders(void *param)
{
	struct source_record_filter_context *context = param;
	if (context->source && obs_source_enabled(context->source) && (context->replayBuffer || context->record || context->stream))
		return;
	if (context->encoder && !obs_encoder_active(context->encoder)) {
		obs_encoder_release(context->encoder);
		context->encoder = NULL;
	}
	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (!context->audioEncoder[i] || obs_encoder_active(context->audioEncoder[i]))
			continue;
		obs_encoder_release(context->audioEncoder[i]);
		context->audioEncoder[i] = NULL;
	}
}

struct stop_output {
	struct source_record_filter_context *context;
	obs_output_t *output;
	obs_source_t *source_ref;
	volatile long refs;
	volatile bool released;
	bool connected;
};

static void stop_output_unref(struct stop_output *so)
{
	if (os_atomic_dec_long(&so->refs) == 0)
		bfree(so);
}

static void source_record_replay_saved(void *data, calldata_t *cd)
{
	struct source_record_filter_context *context = data;
	if (!context || !context->source)
		return;

	const char *path = calldata_string(cd, "path");
	char *path_fallback = NULL;
	if ((!path || !strlen(path)) && context->replayOutput) {
		proc_handler_t *ph = obs_output_get_proc_handler(context->replayOutput);
		if (ph) {
			calldata_t proc_cd;
			calldata_init(&proc_cd);
			if (proc_handler_call(ph, "get_last_replay", &proc_cd)) {
				const char *last_path = calldata_string(&proc_cd, "path");
				if (last_path && strlen(last_path))
					path_fallback = bstrdup(last_path);
			}
			calldata_free(&proc_cd);
		}
	}

	const char *emit_path = "";
	if (path && strlen(path)) {
		emit_path = path;
	} else if (path_fallback) {
		emit_path = path_fallback;
	}

	obs_data_t *event_data = obs_data_create();
	obs_data_set_string(event_data, "path", emit_path);
	obs_data_set_string(event_data, "filter", obs_source_get_name(context->source));

	obs_source_t *parent = obs_filter_get_parent(context->source);
	if (parent) {
		obs_data_set_string(event_data, "source", obs_source_get_name(parent));
	}

	obs_websocket_vendor_emit_event(vendor, "replay_buffer_saved", event_data);
	obs_data_release(event_data);
	bfree(path_fallback);
}

static void finish_stop_task(void *data)
{
	struct stop_output *so = data;
	if (!os_atomic_load_bool(&so->context->exiting))
		release_output_task(so->output);
	os_atomic_dec_long(&so->context->pending_stops);
	obs_source_release(so->source_ref);
	stop_output_unref(so);
}

void release_output_stopped(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct stop_output *so = data;
	if (os_atomic_set_bool(&so->released, true))
		return;
	os_atomic_inc_long(&so->refs); /* destruction task */
	if (so->connected) {
		signal_handler_t *sh = obs_output_get_signal_handler(so->output);
		signal_handler_disconnect(sh, "stop", release_output_stopped, so);
		stop_output_unref(so); /* signal connection */
	}
	run_queued_destroy(finish_stop_task, so);
}

static void force_stop_output_task(void *data)
{
	struct stop_output *so = data;
	signal_handler_t *sh = obs_output_get_signal_handler(so->output);
	/* N11: the "stop" signal is only emitted for an output that was actually
	 * capturing. Force-stopping an inactive output (start failed, or it died
	 * on its own) never fires "stop", so release_output_stopped would never
	 * run -> the output, its encoder refs and this struct leak. Only wait for
	 * the signal when the output is active; otherwise clean up directly. */
	if (sh && obs_output_active(so->output)) {
		so->connected = true;
		os_atomic_inc_long(&so->refs);
		signal_handler_connect(sh, "stop", release_output_stopped, data);
		if (obs_output_active(so->output) && !os_atomic_load_bool(&so->released))
			obs_output_force_stop(so->output);
		else
			release_output_stopped(data, NULL);
	} else {
		/* #99 root cause: obs_output_force_stop on an output with no data
		 * flowing (start failed, or it already stopped) resets the output's
		 * stopping_event; ffmpeg_mux_stop only sets flags and relies on the
		 * data path to deactivate, so with no packets nothing ever signals
		 * the event again. The obs_output_destroy triggered by our release
		 * then blocks forever in os_event_wait() — on the graphics thread,
		 * which freezes every recording in OBS. An inactive output has
		 * nothing to stop: skip the stop, go straight to cleanup. */
		release_output_stopped(data, NULL);
	}
	stop_output_unref(so); /* queued task */
}

static void ensure_directory(char *path)
{
#ifdef _WIN32
	char *backslash = strrchr(path, '\\');
	if (backslash)
		*backslash = '/';
#endif

	char *slash = strrchr(path, '/');
	if (slash) {
		*slash = 0;
		os_mkdirs(path);
		*slash = '/';
	}

#ifdef _WIN32
	if (backslash)
		*backslash = '\\';
#endif
}

/* Reserve the initial path on disk before an asynchronous start. The muxer
 * replaces only our empty placeholder. This also coordinates other processes
 * using exclusive creation; split files get a per-session namespace below. */
static bool dedup_path(struct dstr *path, const char *ext)
{
	size_t elen = ext ? strlen(ext) : 0;
	size_t base_len = path->len;
	if (elen && base_len > elen + 1 && path->array[base_len - elen - 1] == '.' &&
	    strcmp(path->array + base_len - elen, ext) == 0)
		base_len -= elen + 1;
	struct dstr base = {0};
	dstr_ncat(&base, path->array, base_len);
	for (int n = 0; n < 10000; n++) {
		if (n && elen)
			dstr_printf(path, "%s (%d).%s", base.array, n, ext);
		else if (n)
			dstr_printf(path, "%s (%d)", base.array, n);
		FILE *reservation = os_fopen(path->array, "wbx");
		if (reservation) {
			fclose(reservation);
			dstr_free(&base);
			return true;
		}
		if (errno != EEXIST)
			break;
	}
	dstr_free(&base);
	return false;
}

static void remove_filter(void *data, calldata_t *calldata)
{
	struct source_record_filter_context *filter = data;
	/* N4: disconnect from the output that actually emitted "stop" (from the
	 * signal's calldata) rather than always filter->fileOutput, which may be
	 * NULL or the newly recreated output by the time this runs. */
	obs_output_t *output = calldata_ptr(calldata, "output");
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_disconnect(sh, "stop", remove_filter, filter);
	obs_source_t *source = obs_filter_get_parent(filter->source);
	if (!source && filter->view) {
		source = obs_view_get_source(filter->view, SOURCE_CHANNEL);
		obs_source_release(source);
	}
	obs_source_filter_remove(source, filter->source);
}

static void detach_output(struct source_record_filter_context *context, obs_output_t *output)
{
	for (int i = 0; i < OUTPUT_COUNT; i++) {
		if (get_output(context, i) != output)
			continue;
		unobserve_output(context, i);
		context->outputs[i].phase = OUTPUT_STOPPING;
	}
	cancel_start(context, output);
}

static void stop_output_sync(struct source_record_filter_context *context, obs_output_t *output)
{
	if (!output)
		return;
	detach_output(context, output);
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	if (sh)
		signal_handler_disconnect(sh, "stop", remove_filter, context);
	if (obs_output_active(output))
		obs_output_force_stop(output);
}

/* Asynchronously force-stop an output. Holds a strong ref to the filter source
 * (B2) so the context can't be freed while the stop is in flight. For internal
 * restarts (resize, replay-duration change) pass disconnect_remove=true so the
 * output's "stop" does not tear the whole filter down via remove_filter (N4);
 * genuine stops keep that connection so remove_after_record still works. */
static bool queue_force_stop(struct source_record_filter_context *context, obs_output_t *output, bool disconnect_remove)
{
	if (!output)
		return false;
	obs_source_t *source_ref = obs_source_get_ref(context->source);
	if (!source_ref)
		return false;
	detach_output(context, output);
	os_atomic_inc_long(&context->pending_stops);
	if (disconnect_remove) {
		signal_handler_t *sh = obs_output_get_signal_handler(output);
		if (sh)
			signal_handler_disconnect(sh, "stop", remove_filter, context);
	}
	struct stop_output *so = bzalloc(sizeof(struct stop_output));
	so->refs = 1;
	so->output = output;
	so->context = context;
	so->source_ref = source_ref;
	run_queued(force_stop_output_task, so);
	return true;
}

static const char *get_encoder_id(obs_data_t *settings)
{
	const char *enc_id = obs_data_get_string(settings, "encoder");
	if (strlen(enc_id) == 0 || strcmp(enc_id, "x264") == 0 || strcmp(enc_id, "x264_lowcpu") == 0) {
		enc_id = "obs_x264";
	} else if (strcmp(enc_id, "qsv") == 0) {
		enc_id = "obs_qsv11_v2";
	} else if (strcmp(enc_id, "qsv_av1") == 0) {
		enc_id = "obs_qsv11_av1";
	} else if (strcmp(enc_id, "amd") == 0) {
		enc_id = "h264_texture_amf";
	} else if (strcmp(enc_id, "amd_hevc") == 0) {
		enc_id = "h265_texture_amf";
	} else if (strcmp(enc_id, "amd_av1") == 0) {
		enc_id = "av1_texture_amf";
	} else if (strcmp(enc_id, "nvenc") == 0) {
		enc_id = EncoderAvailable("obs_nvenc_h264_tex") ? "obs_nvenc_h264_tex"
								: (EncoderAvailable("jim_nvenc") ? "jim_nvenc" : "ffmpeg_nvenc");
	} else if (strcmp(enc_id, "nvenc_hevc") == 0) {
		enc_id = EncoderAvailable("obs_nvenc_hevc_tex")
				 ? "obs_nvenc_hevc_tex"
				 : (EncoderAvailable("jim_hevc_nvenc") ? "jim_hevc_nvenc" : "ffmpeg_hevc_nvenc");
	} else if (strcmp(enc_id, "nvenc_av1") == 0) {
		enc_id = EncoderAvailable("obs_nvenc_av1_tex") ? "obs_nvenc_av1_tex" : "jim_av1_nvenc";
	} else if (strcmp(enc_id, "apple_h264") == 0) {
		enc_id = "com.apple.videotoolbox.videoencoder.ave.avc";
	} else if (strcmp(enc_id, "apple_hevc") == 0) {
		enc_id = "com.apple.videotoolbox.videoencoder.ave.hevc";
	}
	/* Guard against saved settings referencing an encoder that isn't
	 * available (e.g. "ffmpeg_output", or a GPU encoder from another
	 * machine): fall back to x264 rather than creating a placeholder
	 * encoder that fails to initialize and gets retried every frame. */
	if (!EncoderAvailable(enc_id))
		enc_id = "obs_x264";
	return enc_id;
}

static void (*obs_encoder_set_gpu_scale_type_func)(obs_encoder_t *encoder, enum obs_scale_type gpu_scale_type) = NULL;

static bool (*obs_encoder_set_frame_rate_divisor_func)(obs_encoder_t *, uint32_t) = NULL;

static void update_video_encoder(struct source_record_filter_context *filter, obs_data_t *settings)
{
	/* B5: never release/recreate an active encoder here. obs_encoder_video()
	 * compares against the fps_override wrapper, so with frame_rate_divisor>=2
	 * it never equals filter->video_output and the old code recreated a live
	 * encoder -> double-encoding. When active, media already equals
	 * video_output (it is never swapped under an active encoder), so only
	 * (re-)point and reconfigure while idle. */
	if (!obs_encoder_active(filter->encoder)) {
		obs_encoder_set_video(filter->encoder, filter->video_output);
		uint32_t divisor = (uint32_t)obs_data_get_int(settings, "frame_rate_divisor");
		if (obs_encoder_set_frame_rate_divisor_func)
			obs_encoder_set_frame_rate_divisor_func(filter->encoder, divisor > 1 ? divisor : 1);
		bool scale = obs_data_get_bool(settings, "scale");
		if (scale) {
			uint32_t width = (uint32_t)obs_data_get_int(settings, "width");
			uint32_t height = (uint32_t)obs_data_get_int(settings, "height");
			if (width > 0 && height > 0)
				obs_encoder_set_scaled_size(filter->encoder, width, height);
			else
				obs_encoder_set_scaled_size(filter->encoder, 0, 0);
			if (obs_encoder_set_gpu_scale_type_func)
				obs_encoder_set_gpu_scale_type_func(
					filter->encoder, (enum obs_scale_type)obs_data_get_int(settings, "scale_type"));
		} else {
			obs_encoder_set_scaled_size(filter->encoder, 0, 0);
		}
	}
	if (filter->fileOutput && obs_output_get_video_encoder(filter->fileOutput) != filter->encoder)
		obs_output_set_video_encoder(filter->fileOutput, filter->encoder);
	if (filter->streamOutput && obs_output_get_video_encoder(filter->streamOutput) != filter->encoder)
		obs_output_set_video_encoder(filter->streamOutput, filter->encoder);
	if (filter->replayOutput && obs_output_get_video_encoder(filter->replayOutput) != filter->encoder)
		obs_output_set_video_encoder(filter->replayOutput, filter->encoder);
}

/* True if the recording path, format or filename pattern differs from what the
 * live fileOutput was started with (the cache is refreshed by start_file_output
 * when the output is restarted). */
static bool record_settings_changed(struct source_record_filter_context *filter, obs_data_t *settings)
{
	const char *path = obs_data_get_string(settings, "path");
	const char *format = obs_data_get_string(settings, "rec_format");
	const char *filename = obs_data_get_string(settings, "filename_formatting");
	return dstr_cmp(&filter->rec_path, path) != 0 || dstr_cmp(&filter->rec_format, format) != 0 ||
	       dstr_cmp(&filter->rec_filename, filename) != 0;
}

static void start_file_output(struct source_record_filter_context *filter, obs_data_t *settings)
{
	obs_data_t *s = obs_data_create();
	const char *format = obs_data_get_string(settings, "rec_format");
	/* Remember what this output is being started with, so a later path/format/
	 * filename change while recording can be detected (see update). */
	dstr_copy(&filter->rec_path, obs_data_get_string(settings, "path"));
	dstr_copy(&filter->rec_format, format);
	dstr_copy(&filter->rec_filename, obs_data_get_string(settings, "filename_formatting"));
	/* Honour the profile's "no spaces in filenames" preference for both the
	 * first file and the muxer's split files. The muxer defaults allow_spaces
	 * to false, so without this every split file got its spaces turned into
	 * underscores while the first file (generated here) kept them. */
	config_t *config = obs_frontend_get_profile_config();
	const char *out_mode = config ? config_get_string(config, "Output", "Mode") : NULL;
	const bool adv_out = out_mode && (strcmp(out_mode, "Advanced") == 0 || strcmp(out_mode, "advanced") == 0);
	const bool without_space =
		config && config_get_bool(config, adv_out ? "AdvOut" : "SimpleOutput", "FileNameWithoutSpace");
	struct dstr filename_format = {0};
	dstr_copy(&filename_format, obs_data_get_string(settings, "filename_formatting"));
	if (obs_data_get_bool(settings, "split_file")) {
		dstr_catf(&filename_format, " [sr-%llu-%ld]", (unsigned long long)os_gettime_ns(),
			  os_atomic_inc_long(&filename_session));
	}
	clear_reserved_path(filter->fileOutput, true);
	char *filename = os_generate_formatted_filename(GetFormatExt(format), !without_space, filename_format.array);
	struct dstr path = {0};
	dstr_printf(&path, "%s/%s", obs_data_get_string(settings, "path"), filename);
	bfree(filename);
	ensure_directory(path.array);
	if (!dedup_path(&path, GetFormatExt(format))) {
		note_start_result(filter, FILE_OUTPUT, NULL, false);
		dstr_free(&path);
		dstr_free(&filename_format);
		obs_data_release(s);
		return;
	}
	obs_data_set_string(s, "path", path.array);
	obs_data_set_string(s, "sr_reserved_path", path.array);
	dstr_free(&path);
	obs_data_set_string(s, "directory", obs_data_get_string(settings, "path"));
	obs_data_set_string(s, "format", filename_format.array);
	dstr_free(&filename_format);
	obs_data_set_string(s, "extension", GetFormatExt(format));
	obs_data_set_bool(s, "allow_spaces", !without_space);
	obs_data_set_bool(s, "split_file", obs_data_get_bool(settings, "split_file"));
	obs_data_set_int(s, "max_size_mb", obs_data_get_int(settings, "max_size_mb"));
	obs_data_set_int(s, "max_time_sec", obs_data_get_int(settings, "max_time_sec"));

	const char *output_id = "ffmpeg_muxer";
	if (strcmp(format, "hybrid_mp4") == 0) {
		output_id = "mp4_output";
	} else if (strcmp(format, "hybrid_mov") == 0) {
		output_id = "mov_output";
	}
	if (!filter->fileOutput || strcmp(obs_output_get_id(filter->fileOutput), output_id) != 0) {
		unobserve_output(filter, FILE_OUTPUT);
		obs_output_release(filter->fileOutput);
		filter->fileOutput = obs_output_create(output_id, obs_source_get_name(filter->source), s, NULL);
		if (filter->fileOutput && filter->remove_after_record) {
			signal_handler_t *sh = obs_output_get_signal_handler(filter->fileOutput);
			signal_handler_connect(sh, "stop", remove_filter, filter);
		}
	} else {
		obs_output_update(filter->fileOutput, s);
	}
	if (!filter->fileOutput) {
		const char *reserved = obs_data_get_string(s, "sr_reserved_path");
		if (*reserved && os_get_file_size(reserved) == 0)
			os_unlink(reserved);
		obs_data_release(s);
		note_start_result(filter, FILE_OUTPUT, NULL, false);
		return;
	}
	obs_data_release(s);
	if (filter->encoder) {
		update_video_encoder(filter, settings);
		obs_output_set_video_encoder(filter->fileOutput, filter->encoder);
	}
	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (!filter->audioEncoder[i])
			continue;

		obs_encoder_set_audio(filter->audioEncoder[i], filter->audio_output);
		obs_output_set_audio_encoder(filter->fileOutput, filter->audioEncoder[i], i);
	}

	queue_start(filter, filter->fileOutput, FILE_OUTPUT);
}

#define FTL_PROTOCOL "ftl"
#define RTMP_PROTOCOL "rtmp"

static void start_stream_output(struct source_record_filter_context *filter, obs_data_t *settings)
{
	const char *server = obs_data_get_string(settings, "server");
	bool whip = strstr(server, "whip") != NULL;
	const char *service_id = whip ? "whip_custom" : "rtmp_custom";
	/* Build the service settings in a throwaway object so the stream key /
	 * bearer_token are never written back into the filter's persisted
	 * settings (which get serialized into the scene collection in clear).
	 * This also refreshes bearer_token from the current key on update, so
	 * rotating a WHIP key no longer leaves the service on a stale token. */
	obs_data_t *service_settings = obs_data_create();
	obs_data_set_string(service_settings, "server", server);
	obs_data_set_string(service_settings, "key", obs_data_get_string(settings, "key"));
	if (whip)
		obs_data_set_string(service_settings, "bearer_token", obs_data_get_string(settings, "key"));
	/* sanitize scene collections contaminated by earlier versions */
	obs_data_erase(settings, "bearer_token");

	/* N5: obs_service_update can't change a service's type. If the required
	 * type changed (RTMP<->WHIP), release the old service and recreate so a
	 * protocol switch actually connects. Defer the release: the outgoing
	 * streamOutput references the service by raw pointer until it stops. */
	if (filter->service && strcmp(obs_service_get_id(filter->service), service_id) != 0) {
		run_queued((obs_task_t)obs_service_release, filter->service);
		filter->service = NULL;
	}
	if (!filter->service) {
		filter->service = obs_service_create(service_id, obs_source_get_name(filter->source), service_settings, NULL);
	} else {
		obs_service_update(filter->service, service_settings);
	}
	obs_data_release(service_settings);
	obs_service_apply_encoder_settings(filter->service, settings, NULL);

	const char *type = NULL;
#ifdef _WIN32
	void *handle = os_dlopen("obs");
#else
	void *handle = dlopen(NULL, RTLD_LAZY);
#endif
	if (handle) {
		const char *(*type_func)(obs_service_t *) =
			(const char *(*)(obs_service_t *))os_dlsym(handle, "obs_service_get_output_type");
		if (!type_func)
			type_func = (const char *(*)(obs_service_t *))os_dlsym(handle, "obs_service_get_preferred_output_type");
		if (type_func) {
			type = type_func(filter->service);
		}
		if (!type) {
			const char *url = NULL;
			const char *(*url_func)(obs_service_t *) =
				(const char *(*)(obs_service_t *))os_dlsym(handle, "obs_service_get_url");
			if (url_func) {
				url = url_func(filter->service);
			} else {
				const char *(*info_func)(obs_service_t *, uint32_t) =
					(const char *(*)(obs_service_t *, uint32_t))os_dlsym(handle,
											     "obs_service_get_connect_info");
				if (info_func)
					url = info_func(filter->service, 0); // OBS_SERVICE_CONNECT_INFO_SERVER_URL
			}
			type = "rtmp_output";
			if (url != NULL && strncmp(url, "ftl", 3) == 0) {
				type = "ftl_output";
			} else if (url != NULL && strncmp(url, "rtmp", 4) != 0) {
				type = "ffmpeg_mpegts_muxer";
			}
		}
		os_dlclose(handle);
	} else {
		type = "rtmp_output";
	}

	if (!filter->streamOutput) {
		filter->streamOutput = obs_output_create(type, obs_source_get_name(filter->source), settings, NULL);
	} else {
		obs_output_update(filter->streamOutput, settings);
	}
	obs_output_set_service(filter->streamOutput, filter->service);

	if (filter->encoder) {
		update_video_encoder(filter, settings);
		obs_output_set_video_encoder(filter->streamOutput, filter->encoder);
	}

	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (!filter->audioEncoder[i])
			continue;
		obs_encoder_set_audio(filter->audioEncoder[i], filter->audio_output);
		obs_output_set_audio_encoder(filter->streamOutput, filter->audioEncoder[i], i);
	}

	queue_start(filter, filter->streamOutput, STREAM_OUTPUT);
}

static void start_replay_output(struct source_record_filter_context *filter, obs_data_t *settings)
{
	obs_data_t *s = obs_data_create();

	obs_data_set_string(s, "directory", obs_data_get_string(settings, "path"));
	obs_data_set_string(s, "format", obs_data_get_string(settings, "replay_filename_formatting"));
	obs_data_set_string(s, "extension", GetFormatExt(obs_data_get_string(settings, "rec_format")));
	obs_data_set_bool(s, "allow_spaces", true);
	filter->replay_buffer_duration = obs_data_get_int(settings, "replay_duration");
	obs_data_set_int(s, "max_time_sec", filter->replay_buffer_duration);
	obs_data_set_int(s, "max_size_mb", 10000);
	if (!filter->replayOutput) {
		obs_data_t *hotkeys = obs_data_get_obj(settings, "replay_hotkeys");
		struct dstr name;
		obs_source_t *parent = obs_filter_get_parent(filter->source);
		if (parent) {
			dstr_init_copy(&name, obs_source_get_name(parent));
			dstr_cat(&name, " - ");
			dstr_cat(&name, obs_source_get_name(filter->source));
		} else {
			dstr_init_copy(&name, obs_source_get_name(filter->source));
		}

		filter->replayOutput = obs_output_create("replay_buffer", name.array, s, hotkeys);
		signal_handler_t *sh = obs_output_get_signal_handler(filter->replayOutput);
		if (sh)
			signal_handler_connect(sh, "saved", source_record_replay_saved, filter);
		if (filter->remove_after_record) {
			sh = obs_output_get_signal_handler(filter->replayOutput);
			signal_handler_connect(sh, "stop", remove_filter, filter);
		}
		dstr_free(&name);
		obs_data_release(hotkeys);
	} else {
		obs_output_update(filter->replayOutput, s);
	}
	obs_data_release(s);
	if (filter->encoder) {
		update_video_encoder(filter, settings);
	}
	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (!filter->audioEncoder[i])
			continue;
		obs_encoder_set_audio(filter->audioEncoder[i], filter->audio_output);

		if (obs_output_get_audio_encoder(filter->replayOutput, i) != filter->audioEncoder[i])
			obs_output_set_audio_encoder(filter->replayOutput, filter->audioEncoder[i], i);
	}

	queue_start(filter, filter->replayOutput, REPLAY_OUTPUT);
}

static void copy_defaults(obs_data_t *from, obs_data_t *to)
{
	for (obs_data_item_t *default_item = obs_data_first(from); default_item != NULL; obs_data_item_next(&default_item)) {
		if (!obs_data_item_has_default_value(default_item))
			continue;
		enum obs_data_type item_type = obs_data_item_gettype(default_item);
		const char *name = obs_data_item_get_name(default_item);
		if (item_type == OBS_DATA_STRING) {
			obs_data_set_default_string(to, name, obs_data_item_get_default_string(default_item));
		} else if (item_type == OBS_DATA_NUMBER) {
			enum obs_data_number_type num_type = obs_data_item_numtype(default_item);
			if (num_type == OBS_DATA_NUM_INT) {
				obs_data_set_default_int(to, name, obs_data_item_get_default_int(default_item));
			} else if (num_type == OBS_DATA_NUM_DOUBLE) {
				obs_data_set_default_double(to, name, obs_data_item_get_default_double(default_item));
			}
		} else if (item_type == OBS_DATA_BOOLEAN) {
			obs_data_set_default_bool(to, name, obs_data_item_get_default_bool(default_item));
		}
	}
}

static void set_encoder_defaults(obs_data_t *settings)
{
	obs_data_t *enc_defaults = obs_encoder_defaults(get_encoder_id(settings));
	if (enc_defaults) {
		copy_defaults(enc_defaults, settings);
		obs_data_release(enc_defaults);
	}
	const char *enc_id = obs_data_get_string(settings, "audio_encoder");
	if (!enc_id || !strlen(enc_id))
		enc_id = "ffmpeg_aac";
	enc_defaults = obs_encoder_defaults(enc_id);
	if (enc_defaults) {
		if (obs_data_has_default_value(enc_defaults, "bitrate")) {
			obs_data_set_default_int(settings, "audio_bitrate", obs_data_get_default_int(enc_defaults, "bitrate"));
			obs_data_unset_default_value(enc_defaults, "bitrate");
		}
		copy_defaults(enc_defaults, settings);
		obs_data_release(enc_defaults);
	}
}

/* N12: copy the user-set audio-encoder properties beyond bitrate (samplerate,
 * "allow he-aac", ...) into the encoder settings. The UI exposes them but only
 * "bitrate" was passed through, so they were shown yet never applied. */
static void copy_audio_user_values(obs_data_t *from, obs_data_t *to, const char *enc_id)
{
	obs_data_t *defaults = obs_encoder_defaults(enc_id);
	if (!defaults)
		return;
	for (obs_data_item_t *item = obs_data_first(defaults); item != NULL; obs_data_item_next(&item)) {
		const char *name = obs_data_item_get_name(item);
		if (strcmp(name, "bitrate") == 0 || !obs_data_has_user_value(from, name))
			continue;
		enum obs_data_type t = obs_data_item_gettype(item);
		if (t == OBS_DATA_STRING) {
			obs_data_set_string(to, name, obs_data_get_string(from, name));
		} else if (t == OBS_DATA_NUMBER) {
			if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT)
				obs_data_set_int(to, name, obs_data_get_int(from, name));
			else
				obs_data_set_double(to, name, obs_data_get_double(from, name));
		} else if (t == OBS_DATA_BOOLEAN) {
			obs_data_set_bool(to, name, obs_data_get_bool(from, name));
		}
	}
	obs_data_release(defaults);
}

static void update_audio_encoder(struct source_record_filter_context *filter, obs_data_t *settings)
{
	const int audio_track = obs_data_get_bool(settings, "different_audio") ? (int)obs_data_get_int(settings, "audio_track") : 0;
	if (audio_track < -1 || audio_track > MAX_AUDIO_MIXES)
		return;
	const char *enc_id = obs_data_get_string(settings, "audio_encoder");
	if (!*enc_id)
		enc_id = "ffmpeg_aac";
	obs_data_t *audio_settings = obs_data_create();
	obs_data_set_int(audio_settings, "bitrate", obs_data_get_int(settings, "audio_bitrate"));
	copy_audio_user_values(settings, audio_settings, enc_id);
	bool changed = !filter->audioEncoder[0] || filter->audio_track != audio_track ||
		strcmp(obs_encoder_get_id(filter->audioEncoder[0]), enc_id) != 0 ||
		!filter->applied_audio_settings ||
		strcmp(obs_data_get_json(filter->applied_audio_settings), obs_data_get_json(audio_settings)) != 0;
	if (!changed || os_atomic_load_bool(&filter->closing)) {
		if (!changed)
			filter->audio_settings_pending = false;
		obs_data_release(audio_settings);
		return;
	}
	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (filter->audioEncoder[i] && obs_encoder_active(filter->audioEncoder[i])) {
			if (!filter->audio_settings_pending)
				blog(LOG_INFO, "[Source Record] audio changes pending until all outputs stop");
			filter->audio_settings_pending = true;
			obs_data_release(audio_settings);
			return;
		}
	}

	const bool own_audio = audio_track == 0;
	audio_t *audio = filter->audio_output;
	if (!audio || own_audio != filter->owns_audio_output) {
		if (own_audio) {
			struct audio_output_info oi = {0};
			oi.name = obs_source_get_name(filter->source);
			oi.speakers = audio_output_get_info(obs_get_audio())->speakers;
			oi.samples_per_sec = audio_output_get_sample_rate(obs_get_audio());
			oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
			oi.input_param = filter;
			oi.input_callback = audio_input_callback;
			audio = NULL;
			if (audio_output_open(&audio, &oi) != AUDIO_OUTPUT_SUCCESS) {
				obs_data_release(audio_settings);
				return;
			}
		} else {
			audio = obs_get_audio();
		}
	}
	obs_encoder_t *encoders[MAX_AUDIO_MIXES] = {0};
	const int count = audio_track == -1 ? MAX_AUDIO_MIXES : 1;
	for (int i = 0; i < count; i++) {
		struct dstr track_name = {0};
		const char *name = obs_source_get_name(filter->source);
		if (audio_track == -1) {
			dstr_printf(&track_name, "%s track %d", name, i + 1);
			name = track_name.array;
		}
		encoders[i] = obs_audio_encoder_create(enc_id, name, audio_settings,
						     audio_track > 0 ? audio_track - 1 : i, NULL);
		dstr_free(&track_name);
		if (!encoders[i]) {
			for (int j = 0; j < i; j++)
				obs_encoder_release(encoders[j]);
			if (own_audio && audio != filter->audio_output)
				audio_output_close(audio);
			obs_data_release(audio_settings);
			return;
		}
		obs_encoder_set_audio(encoders[i], audio);
	}
	/* Detach idle encoders before closing their borrowed media pointer. */
	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (filter->fileOutput)
			obs_output_set_audio_encoder(filter->fileOutput, encoders[i], i);
		if (filter->replayOutput)
			obs_output_set_audio_encoder(filter->replayOutput, encoders[i], i);
		if (filter->streamOutput)
			obs_output_set_audio_encoder(filter->streamOutput, encoders[i], i);
		obs_encoder_release(filter->audioEncoder[i]);
		filter->audioEncoder[i] = encoders[i];
	}
	if (filter->owns_audio_output && filter->audio_output != audio)
		audio_output_close(filter->audio_output);
	filter->audio_output = audio;
	filter->owns_audio_output = own_audio;
	filter->audio_track = audio_track;
	filter->audio_settings_pending = false;
	obs_data_release(filter->applied_audio_settings);
	filter->applied_audio_settings = audio_settings;
}

static void update_encoder(struct source_record_filter_context *filter, obs_data_t *settings)
{
	const char *enc_id = get_encoder_id(settings);
	const bool need_new_encoder = !filter->encoder || strcmp(obs_encoder_get_id(filter->encoder), enc_id) != 0;
	if (need_new_encoder && filter->encoder && obs_encoder_active(filter->encoder)) {
		/* Fix: an active encoder is never released here. Releasing an
		 * in-use encoder caused crashes; it is swapped once idle. */
	} else if (need_new_encoder) {
		obs_encoder_release(filter->encoder);
		filter->encoder = NULL;
		set_encoder_defaults(settings);
		filter->encoder = obs_video_encoder_create(enc_id, obs_source_get_name(filter->source), settings, NULL);

		obs_encoder_set_video(filter->encoder, filter->video_output);
		uint32_t divisor = (uint32_t)obs_data_get_int(settings, "frame_rate_divisor");
		if (divisor > 1 && obs_encoder_set_frame_rate_divisor_func)
			obs_encoder_set_frame_rate_divisor_func(filter->encoder, divisor);
		bool scale = obs_data_get_bool(settings, "scale");
		if (scale) {
			uint32_t width = (uint32_t)obs_data_get_int(settings, "width");
			uint32_t height = (uint32_t)obs_data_get_int(settings, "height");
			if (width > 0 && height > 0) {
				obs_encoder_set_scaled_size(filter->encoder, width, height);
			} else {
				obs_encoder_set_scaled_size(filter->encoder, 0, 0);
			}
			if (obs_encoder_set_gpu_scale_type_func)
				obs_encoder_set_gpu_scale_type_func(filter->encoder,
								    (enum obs_scale_type)obs_data_get_int(settings, "scale_type"));
		} else {
			obs_encoder_set_scaled_size(filter->encoder, 0, 0);
		}
		if (filter->fileOutput && obs_output_get_video_encoder(filter->fileOutput) != filter->encoder)
			obs_output_set_video_encoder(filter->fileOutput, filter->encoder);
		if (filter->streamOutput && obs_output_get_video_encoder(filter->streamOutput) != filter->encoder)
			obs_output_set_video_encoder(filter->streamOutput, filter->encoder);
		if (filter->replayOutput && obs_output_get_video_encoder(filter->replayOutput) != filter->encoder)
			obs_output_set_video_encoder(filter->replayOutput, filter->encoder);
	} else if (!obs_encoder_active(filter->encoder)) {
		/* Fix: re-point the encoder at the current video_output in case
		 * the view was recreated after a parent size change. */
		obs_encoder_set_video(filter->encoder, filter->video_output);
		obs_encoder_update(filter->encoder, settings);
	}
	update_audio_encoder(filter, settings);
}

/* B13: persist the replay-buffer hotkey bindings into the filter settings
 * before the replay output (which owns them) is destroyed, so bindings set
 * during the session survive a restart/disable/remove instead of being lost. */
static void save_replay_hotkeys(struct source_record_filter_context *context)
{
	if (!context->replayOutput)
		return;
	obs_data_t *hotkeys = obs_hotkeys_save_output(context->replayOutput);
	obs_data_t *settings = obs_source_get_settings(context->source);
	obs_data_set_obj(settings, "replay_hotkeys", hotkeys);
	obs_data_release(settings);
	obs_data_release(hotkeys);
}

/* Tolerant "WxH" parser: accepts x, X, *, or the UTF-8 multiplication sign
 * U+00D7 as separator, with optional surrounding spaces. Returns false if it
 * cannot read two numbers, so the caller can preserve the user's typed text. */
static bool parse_resolution(const char *res, uint32_t *width, uint32_t *height)
{
	if (!res || !*res)
		return false;
	char *end = NULL;
	unsigned long w = strtoul(res, &end, 10);
	if (end == res)
		return false;
	while (*end == ' ' || *end == '\t')
		end++;
	if (*end == 'x' || *end == 'X' || *end == '*') {
		end++;
	} else if ((unsigned char)end[0] == 0xC3 && (unsigned char)end[1] == 0x97) {
		end += 2; /* '×' (U+00D7) in UTF-8 */
	} else {
		return false;
	}
	while (*end == ' ' || *end == '\t')
		end++;
	const char *hstart = end;
	unsigned long h = strtoul(hstart, &end, 10);
	if (end == hstart)
		return false;
	*width = (uint32_t)w;
	*height = (uint32_t)h;
	return true;
}

static void invalidate_reset_video(struct source_record_filter_context *context)
{
	if (!os_atomic_load_bool(&context->video_reset) && context->video_output) {
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi) &&
		    (ovi.fps_num != context->cached_ovi.fps_num || ovi.fps_den != context->cached_ovi.fps_den ||
		     ovi.base_width != context->cached_ovi.base_width || ovi.base_height != context->cached_ovi.base_height ||
		     ovi.output_width != context->cached_ovi.output_width ||
		     ovi.output_height != context->cached_ovi.output_height ||
		     ovi.output_format != context->cached_ovi.output_format ||
		     ovi.colorspace != context->cached_ovi.colorspace || ovi.range != context->cached_ovi.range))
			os_atomic_set_bool(&context->video_reset, true);
	}
	if (os_atomic_set_bool(&context->video_reset, false)) {
		context->video_output = NULL;
		context->width = 0;
		context->height = 0;
	}
}

static bool audio_source_matches(obs_weak_source_t *weak, const char *name)
{
	obs_source_t *source = obs_weak_source_get_source(weak);
	bool matches = source && strcmp(name, obs_source_get_name(source)) == 0;
	obs_source_release(source);
	return matches;
}

static void source_record_filter_update(void *data, obs_data_t *settings)
{
	struct source_record_filter_context *filter = data;
	invalidate_reset_video(filter);
	obs_source_t *parent = obs_filter_get_parent(filter->source);
	if (obs_obj_is_private(parent)) {
		os_atomic_store_bool(&filter->closing, true);
		return;
	}
	/* B3: an explicit settings update should retry a failed start immediately */
	for (int i = 0; i < OUTPUT_COUNT; i++) {
		filter->outputs[i].next_attempt = 0;
		filter->outputs[i].failures = 0;
	}
	if (obs_data_get_bool(settings, "scale")) {
		const char *res = obs_data_get_string(settings, "resolution");
		uint32_t width = 0, height = 0;
		if (parse_resolution(res, &width, &height) && width > 0 && height > 0) {
			obs_data_set_int(settings, "width", width);
			obs_data_set_int(settings, "height", height);
		} else {
			/* Only rewrite "resolution" from an already-parsed
			 * width/height (legacy config migration). If they are 0
			 * (new filter, or unparseable text), keep the user's typed
			 * value so it can be corrected, instead of clobbering it to
			 * "0x0" and silently disabling scaling under a ticked box. */
			int w = (int)obs_data_get_int(settings, "width");
			int h = (int)obs_data_get_int(settings, "height");
			if (w > 0 && h > 0) {
				struct dstr str;
				dstr_init(&str);
				dstr_printf(&str, "%dx%d", w, h);
				obs_data_set_string(settings, "resolution", str.array);
				dstr_free(&str);
			} else if (res && *res) {
				blog(LOG_WARNING, "[Source Record] could not parse scale resolution '%s'", res);
			}
		}
	}
	filter->remove_after_record = obs_data_get_bool(settings, "remove_after_record");
	const long long new_max_seconds = obs_data_get_int(settings, "record_max_seconds");
	if (new_max_seconds != filter->record_max_seconds)
		filter->max_seconds_reached = false; /* N6: settings change re-arms */
	filter->record_max_seconds = new_max_seconds;
	const long long record_mode = obs_data_get_int(settings, "record_mode");
	const long long stream_mode = obs_data_get_int(settings, "stream_mode");
	const bool replay_buffer = obs_data_get_bool(settings, "replay_buffer") && !os_atomic_load_bool(&filter->closing);
	if (!os_atomic_load_bool(&filter->closing) && (record_mode != OUTPUT_MODE_NONE || stream_mode != OUTPUT_MODE_NONE || replay_buffer)) {
		if (filter->video_output)
			update_encoder(filter, settings);
		else
			update_audio_encoder(filter, settings);
	}
	bool record = false;
	if (os_atomic_load_bool(&filter->closing)) {
	} else if (record_mode == OUTPUT_MODE_ALWAYS) {
		record = true;
	} else if (record_mode == OUTPUT_MODE_RECORDING) {
		record = obs_frontend_recording_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPING &&
			 filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPED;
	} else if (record_mode == OUTPUT_MODE_STREAMING) {
		record = obs_frontend_streaming_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPING &&
			 filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPED;
	} else if (record_mode == OUTPUT_MODE_STREAMING_OR_RECORDING) {
		record = (obs_frontend_streaming_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPING &&
			  filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPED) ||
			 (obs_frontend_recording_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPING &&
			  filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPED);
	} else if (record_mode == OUTPUT_MODE_VIRTUAL_CAMERA) {
		record = obs_frontend_virtualcam_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED;
	}
	/* N6: once record_max_seconds was hit this session, stay stopped until a
	 * *_STOPPED event or a settings change clears the latch. */
	if (filter->max_seconds_reached)
		record = false;

	if (parent && filter->view && (record || replay_buffer)) {
		obs_source_t *view_source = obs_view_get_source(filter->view, SOURCE_CHANNEL);
		if (view_source != parent)
			obs_view_set_source(filter->view, SOURCE_CHANNEL, parent);
		obs_source_release(view_source);
	}

	if (parent && filter->view && !os_atomic_load_bool(&filter->closing)) {
		obs_source_t *background_source = obs_view_get_source(filter->view, BACKGROUND_CHANNEL);
		if (!background_source) {
			background_source = obs_source_create_private("color_source", "Source Record Background", NULL);
			obs_view_set_source(filter->view, BACKGROUND_CHANNEL, background_source);
		}
		obs_data_t *css = obs_source_get_settings(background_source);
		if (obs_data_get_int(css, "color") != obs_data_get_int(settings, "backgroundColor") ||
		    obs_data_get_int(css, "width") != obs_source_get_width(parent) ||
		    obs_data_get_int(css, "height") != obs_source_get_height(parent)) {
			obs_data_set_int(css, "color", obs_data_get_int(settings, "backgroundColor"));
			obs_data_set_int(css, "width", obs_source_get_width(parent));
			obs_data_set_int(css, "height", obs_source_get_height(parent));
			obs_source_update(background_source, css);
		}
		obs_data_release(css);
		obs_source_release(background_source);
	}

	if (record != filter->record) {
		if (record) {
			/* The graphics tick starts this output once prior stops drain. */
		} else if (filter->fileOutput) {
			if (os_atomic_load_bool(&filter->closing)) {
				stop_output_sync(filter, filter->fileOutput);
			} else {
				if (queue_force_stop(filter, filter->fileOutput, false))
					filter->fileOutput = NULL;
			}
		}
		filter->record = record;
	} else if (record && filter->fileOutput && obs_source_enabled(filter->source) && !os_atomic_load_bool(&filter->closing) &&
		   record_settings_changed(filter, settings)) {
		/* Apply a path/format/filename change to an already-running recording.
		 * A live ffmpeg muxer can't change its file mid-write, so restart splits
		 * the recording: the current file finalizes and a new one starts with the
		 * new settings. Only fires while recording; an idle filter picks up new
		 * settings on its next start. Mirrors the replay-duration restart below. */
		blog(LOG_INFO,
		     "[Source Record] recording path/format changed while active; restarting output (splits the file)");
		if (queue_force_stop(filter, filter->fileOutput, true))
			filter->fileOutput = NULL;

	}

	if (record && filter->fileOutput && filter->last_frontend_event == OBS_FRONTEND_EVENT_RECORDING_PAUSED &&
	    !obs_output_paused(filter->fileOutput)) {
		obs_output_pause(filter->fileOutput, true);
		filter->last_frontend_event = -1;
	} else if (record && filter->fileOutput && filter->last_frontend_event == OBS_FRONTEND_EVENT_RECORDING_UNPAUSED &&
		   obs_output_paused(filter->fileOutput)) {
		obs_output_pause(filter->fileOutput, false);
		filter->last_frontend_event = -1;
	}

	if (replay_buffer != filter->replayBuffer) {
		if (replay_buffer) {
			/* The graphics tick starts this output once prior stops drain. */
		} else if (filter->replayOutput) {
			obs_data_t *hotkeys = obs_hotkeys_save_output(filter->replayOutput);
			obs_data_set_obj(settings, "replay_hotkeys", hotkeys);
			obs_data_release(hotkeys);
			if (os_atomic_load_bool(&filter->closing)) {
				stop_output_sync(filter, filter->replayOutput);
			} else {
				if (queue_force_stop(filter, filter->replayOutput, false))
					filter->replayOutput = NULL;
			}
		}

		filter->replayBuffer = replay_buffer;
	} else if (replay_buffer && filter->replayOutput && obs_source_enabled(filter->source)) {
		if (filter->replay_buffer_duration != obs_data_get_int(settings, "replay_duration")) {
			obs_data_t *hotkeys = obs_hotkeys_save_output(filter->replayOutput);
			obs_data_set_obj(settings, "replay_hotkeys", hotkeys);
			obs_data_release(hotkeys);
			if (queue_force_stop(filter, filter->replayOutput, true))
				filter->replayOutput = NULL;

		}
		if (filter->replayOutput) {
			obs_data_t *replay_settings = obs_output_get_settings(filter->replayOutput);
			obs_data_set_string(replay_settings, "format", obs_data_get_string(settings, "replay_filename_formatting"));
			obs_data_release(replay_settings);
		}
	}

	bool stream = false;
	if (os_atomic_load_bool(&filter->closing)) {
	} else if (stream_mode == OUTPUT_MODE_ALWAYS) {
		stream = true;
	} else if (stream_mode == OUTPUT_MODE_RECORDING) {
		stream = obs_frontend_recording_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPING &&
			 filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPED;
	} else if (stream_mode == OUTPUT_MODE_STREAMING) {
		stream = obs_frontend_streaming_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPING &&
			 filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPED;
	} else if (stream_mode == OUTPUT_MODE_STREAMING_OR_RECORDING) {
		stream = (obs_frontend_streaming_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPING &&
			  filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPED) ||
			 (obs_frontend_recording_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPING &&
			  filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPED);
	} else if (stream_mode == OUTPUT_MODE_VIRTUAL_CAMERA) {
		stream = obs_frontend_virtualcam_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED;
	}

	if (parent && filter->view && stream) {
		obs_source_t *view_source = obs_view_get_source(filter->view, SOURCE_CHANNEL);
		if (view_source != parent)
			obs_view_set_source(filter->view, SOURCE_CHANNEL, parent);
		obs_source_release(view_source);
	}

	if (stream != filter->stream) {
		if (stream) {
			/* The graphics tick starts this output once prior stops drain. */
		} else if (filter->streamOutput) {
			if (os_atomic_load_bool(&filter->closing)) {
				stop_output_sync(filter, filter->streamOutput);
			} else {
				if (queue_force_stop(filter, filter->streamOutput, false))
					filter->streamOutput = NULL;
			}
		}
		filter->stream = stream;
	}

	if (!replay_buffer && !record && !stream) {
		if (filter->encoder && !obs_encoder_active(filter->encoder)) {
			obs_encoder_release(filter->encoder);
			filter->encoder = NULL;
		}
		for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
			if (!filter->audioEncoder[i] || obs_encoder_active(filter->audioEncoder[i]))
				continue;
			obs_encoder_release(filter->audioEncoder[i]);
			filter->audioEncoder[i] = NULL;
		}
	}

	vec4_from_rgba(&filter->backgroundColor, (uint32_t)obs_data_get_int(settings, "backgroundColor"));

	if (obs_data_get_bool(settings, "different_audio")) {
		const char *source_name = obs_data_get_string(settings, "audio_source");
		if (!strlen(source_name)) {
			if (filter->audio_source) {
				obs_weak_source_release(filter->audio_source);
				filter->audio_source = NULL;
			}
		} else {
			if (!audio_source_matches(filter->audio_source, source_name)) {
				if (filter->audio_source) {
					obs_weak_source_release(filter->audio_source);
					filter->audio_source = NULL;
				}
				obs_source_t *source = obs_get_source_by_name(source_name);
				if (source) {
					filter->audio_source = obs_source_get_weak_source(source);
					obs_source_release(source);
				}
			}
		}

	} else if (filter->audio_source) {
		obs_weak_source_release(filter->audio_source);
		filter->audio_source = NULL;
	}
}

static void source_record_filter_save(void *data, obs_data_t *settings)
{
	struct source_record_filter_context *filter = data;
	if (filter->replayOutput) {
		obs_data_t *hotkeys = obs_hotkeys_save_output(filter->replayOutput);
		obs_data_set_obj(settings, "replay_hotkeys", hotkeys);
		obs_data_release(hotkeys);
	}
}

static void source_record_filter_defaults(obs_data_t *settings)
{
	config_t *config = obs_frontend_get_profile_config();

	const char *mode = config_get_string(config, "Output", "Mode");
	if (!mode)
		mode = "Simple";
	const char *type = config_get_string(config, "AdvOut", "RecType");
	if (!type)
		type = "Standard";
	/* type != "Standard" (either case) selects the FFmpeg custom path,
	 * otherwise the standard recording path. The original || made this
	 * tautologically true, so RecType=Standard wrongly used FFFilePath. */
	const char *adv_path = strcmp(type, "Standard") != 0 && strcmp(type, "standard") != 0
				       ? config_get_string(config, "AdvOut", "FFFilePath")
				       : config_get_string(config, "AdvOut", "RecFilePath");
	bool adv_out = strcmp(mode, "Advanced") == 0 || strcmp(mode, "advanced") == 0;
	const char *rec_path = adv_out ? adv_path : config_get_string(config, "SimpleOutput", "FilePath");

	obs_data_set_default_string(settings, "path", rec_path);
	const char *format = config_get_string(config, "Output", "FilenameFormatting");
	obs_data_set_default_string(settings, "filename_formatting", format);
	obs_data_set_default_string(settings, "replay_filename_formatting", format);
	obs_data_set_default_string(settings, "rec_format",
				    config_get_string(config, adv_out ? "AdvOut" : "SimpleOutput", "RecFormat2"));

	obs_data_set_default_int(settings, "backgroundColor", 0);

	const char *enc_id;
	if (adv_out) {
		enc_id = config_get_string(config, "AdvOut", "RecEncoder");
		if (strcmp(enc_id, "none") == 0 || strcmp(enc_id, "None") == 0)
			enc_id = config_get_string(config, "AdvOut", "Encoder");
		else if (strcmp(enc_id, "jim_nvenc") == 0 || strcmp(enc_id, "obs_nvenc_h264_tex") == 0)
			enc_id = "nvenc";

	} else {
		const char *quality = config_get_string(config, "SimpleOutput", "RecQuality");
		if (strcmp(quality, "Stream") == 0 || strcmp(quality, "stream") == 0) {
			enc_id = config_get_string(config, "SimpleOutput", "StreamEncoder");
		} else if (strcmp(quality, "Lossless") == 0 || strcmp(quality, "lossless") == 0) {
			/* "ffmpeg_output" is an OUTPUT id, not an encoder; the plugin
			 * records via ffmpeg_muxer and cannot reproduce the frontend's
			 * lossless pipeline. Fall back to the configured stream encoder. */
			enc_id = config_get_string(config, "SimpleOutput", "StreamEncoder");
		} else {
			enc_id = config_get_string(config, "SimpleOutput", "RecEncoder");
		}
	}
	obs_data_set_default_string(settings, "encoder", enc_id);

	obs_data_set_default_string(settings, "audio_encoder", "ffmpeg_aac");

	set_encoder_defaults(settings);
	obs_data_set_default_int(settings, "replay_duration", 5);
	obs_data_set_default_int(settings, "max_size_mb", 2048);
	obs_data_set_default_int(settings, "max_time_sec", 15 * 60);
}

static void source_record_filter_filter_remove(void *data, obs_source_t *parent);

static void frontend_event(enum obs_frontend_event event, void *data)
{
	struct source_record_filter_context *context = data;
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING || event == OBS_FRONTEND_EVENT_STREAMING_STARTED ||
	    event == OBS_FRONTEND_EVENT_STREAMING_STOPPING || event == OBS_FRONTEND_EVENT_STREAMING_STOPPED ||
	    event == OBS_FRONTEND_EVENT_RECORDING_STARTING || event == OBS_FRONTEND_EVENT_RECORDING_STARTED ||
	    event == OBS_FRONTEND_EVENT_RECORDING_STOPPING || event == OBS_FRONTEND_EVENT_RECORDING_STOPPED ||
	    event == OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED || event == OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED ||
	    event == OBS_FRONTEND_EVENT_RECORDING_PAUSED || event == OBS_FRONTEND_EVENT_RECORDING_UNPAUSED) {
		/* N9: don't act on (or queue update_task for) a closing filter */
		if (os_atomic_load_bool(&context->closing))
			return;
		context->last_frontend_event = (int)event;
		if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED || event == OBS_FRONTEND_EVENT_STREAMING_STOPPED ||
		    event == OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED)
			context->max_seconds_reached = false; /* N6: session ended, re-arm */

		obs_source_update(context->source, NULL);
	} else if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		os_atomic_store_bool(&context->closing, true);
		for (int i = 0; i < OUTPUT_COUNT; i++)
			cancel_start(context, get_output(context, i));
		os_atomic_store_bool(&context->exiting, true);
		obs_source_update(context->source, NULL);
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
		os_atomic_store_bool(&context->closing, true);
		for (int i = 0; i < OUTPUT_COUNT; i++)
			cancel_start(context, get_output(context, i));
		obs_source_update(context->source, NULL);
	}
}

/* B1: the core "video_reset" signal (OBS 31.1+) fires from obs_reset_video,
 * which frees every private-view mix. Only flag it here (wrong thread to touch
 * pointers); the graphics-thread tick drops the dangling video_output. */
static void on_video_reset(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct source_record_filter_context *context = data;
	os_atomic_set_bool(&context->video_reset, true);
}

/* N9: the frontend callback vector has no lock; removing our callback from the
 * destruction thread / an output-stop thread while the UI dispatches an event
 * is a data race. Do the removal on the UI thread instead. */
static void remove_frontend_cb_task(void *data)
{
	struct source_record_filter_context *context = data;
	signal_handler_disconnect(obs_get_signal_handler(), "video_reset", on_video_reset, context);
	obs_frontend_remove_event_callback(frontend_event, context);
}

static void remove_frontend_cb(struct source_record_filter_context *context)
{
	if (!os_atomic_set_bool(&context->frontend_callback_registered, false))
		return;
	/* wait=true rendezvouses so no in-flight frontend_event uses a freed
	 * context; skip the hop while exiting (UI may be joining the destruction
	 * thread during obs_shutdown -> deadlock) or if already on the UI thread. */
	if (os_atomic_load_bool(&context->exiting) || obs_in_task_thread(OBS_TASK_UI))
		remove_frontend_cb_task(context);
	else
		obs_queue_task(OBS_TASK_UI, remove_frontend_cb_task, context, true);
}

static void *source_record_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct source_record_filter_context *context = bzalloc(sizeof(struct source_record_filter_context));
	context->source = source;
	context->weak_self = obs_source_get_weak_source(source);

	pthread_mutex_lock(&filters_mutex);
	da_push_back(source_record_filters, &source);
	pthread_mutex_unlock(&filters_mutex);
	context->last_frontend_event = -1;
	context->enableHotkey = OBS_INVALID_HOTKEY_PAIR_ID;
	context->pauseHotkeys = OBS_INVALID_HOTKEY_PAIR_ID;
	context->splitHotkey = OBS_INVALID_HOTKEY_ID;
	context->chapterHotkey = OBS_INVALID_HOTKEY_ID;
	source_record_filter_update(context, settings);
	obs_frontend_add_event_callback(frontend_event, context);
	os_atomic_store_bool(&context->frontend_callback_registered, true);
	signal_handler_connect(obs_get_signal_handler(), "video_reset", on_video_reset, context);
	return context;
}

static void source_record_filter_destroy(void *data)
{
	struct source_record_filter_context *context = data;
	pthread_mutex_lock(&filters_mutex);
	da_erase_item(source_record_filters, &context->source);
	pthread_mutex_unlock(&filters_mutex);
	close_filter_showing(context, obs_filter_get_parent(context->source));
	remove_frontend_cb(context);

	stop_output_sync(context, context->fileOutput);
	stop_output_sync(context, context->streamOutput);
	save_replay_hotkeys(context);
	stop_output_sync(context, context->replayOutput);

	if (context->enableHotkey != OBS_INVALID_HOTKEY_PAIR_ID)
		obs_hotkey_pair_unregister(context->enableHotkey);

	if (context->pauseHotkeys != OBS_INVALID_HOTKEY_PAIR_ID)
		obs_hotkey_pair_unregister(context->pauseHotkeys);

	if (context->splitHotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(context->splitHotkey);

	if (context->chapterHotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(context->chapterHotkey);

	clear_reserved_path(context->fileOutput, true);
	obs_output_release(context->fileOutput);
	obs_output_release(context->streamOutput);
	obs_output_release(context->replayOutput);
	context->fileOutput = NULL;
	context->streamOutput = NULL;
	context->replayOutput = NULL;

	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		obs_encoder_release(context->audioEncoder[i]);
		context->audioEncoder[i] = NULL;
	}
	obs_encoder_release(context->encoder);
	context->encoder = NULL;

	obs_weak_source_release(context->audio_source);
	context->audio_source = NULL;

	if (context->owns_audio_output && context->audio_output)
		audio_output_close(context->audio_output);
	context->audio_output = NULL;
	obs_data_release(context->applied_audio_settings);

	obs_service_release(context->service);
	context->service = NULL;

	if (context->view) {
		obs_view_set_source(context->view, BACKGROUND_CHANNEL, NULL);
		obs_view_set_source(context->view, SOURCE_CHANNEL, NULL);
		if (context->video_output) {
			obs_view_remove(context->view);
			context->video_output = NULL;
		}
		obs_view_destroy(context->view);
		context->view = NULL;
	}

	obs_weak_source_release(context->weak_self);
	context->source = NULL;
	dstr_free(&context->rec_path);
	dstr_free(&context->rec_format);
	dstr_free(&context->rec_filename);
	bfree(context);
}

static bool source_record_enable_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct source_record_filter_context *context = data;
	if (!pressed)
		return false;

	if (obs_source_enabled(context->source))
		return false;

	obs_source_set_enabled(context->source, true);
	return true;
}

static bool source_record_disable_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct source_record_filter_context *context = data;
	if (!pressed)
		return false;
	if (!obs_source_enabled(context->source))
		return false;
	obs_source_set_enabled(context->source, false);
	return true;
}

static bool source_record_pause_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct source_record_filter_context *context = data;
	if (!pressed)
		return false;

	if (obs_output_paused(context->fileOutput))
		return false;

	obs_output_pause(context->fileOutput, true);
	return true;
}

static bool source_record_unpause_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct source_record_filter_context *context = data;
	if (!pressed)
		return false;
	if (!obs_output_paused(context->fileOutput))
		return false;

	obs_output_pause(context->fileOutput, false);
	return true;
}

static void source_record_split_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct source_record_filter_context *context = data;
	if (!context->fileOutput)
		return;
	proc_handler_t *ph = obs_output_get_proc_handler(context->fileOutput);
	struct calldata cd;
	calldata_init(&cd);
	proc_handler_call(ph, "split_file", &cd);
	calldata_free(&cd);
}

static void source_record_chapter_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct source_record_filter_context *context = data;
	if (!context->fileOutput)
		return;
	proc_handler_t *ph = obs_output_get_proc_handler(context->fileOutput);
	struct calldata cd;
	calldata_init(&cd);
	proc_handler_call(ph, "add_chapter", &cd);
	calldata_free(&cd);
}

static void source_record_filter_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct source_record_filter_context *context = data;
	if (os_atomic_load_bool(&context->closing))
		return;
	consume_start_results(context);
	consume_output_events(context);
	sync_output_active(context);
	if (!os_atomic_load_bool(&context->output_active) && !os_atomic_load_long(&context->pending_stops))
		release_encoders(context);

	obs_source_t *parent = obs_filter_get_parent(context->source);
	if (!parent)
		return;

	if (obs_obj_is_private(parent)) {
		os_atomic_store_bool(&context->closing, true);
		return;
	}

	if (context->enableHotkey == OBS_INVALID_HOTKEY_PAIR_ID)
		context->enableHotkey = obs_hotkey_pair_register_source(
			parent, "source_record.enable", obs_module_text("SourceRecordEnable"), "source_record.disable",
			obs_module_text("SourceRecordDisable"), source_record_enable_hotkey, source_record_disable_hotkey, context,
			context);

	if (context->pauseHotkeys == OBS_INVALID_HOTKEY_PAIR_ID)
		context->pauseHotkeys = obs_hotkey_pair_register_source(
			parent, "source_record.PauseRecording", obs_frontend_get_locale_string("Basic.Main.PauseRecording"),
			"source_record.UnpauseRecording", obs_frontend_get_locale_string("Basic.Main.UnpauseRecording"),
			source_record_pause_hotkey, source_record_unpause_hotkey, context, context);

	if (context->splitHotkey == OBS_INVALID_HOTKEY_ID)
		context->splitHotkey = obs_hotkey_register_source(parent, "source_record.SplitRecording",
								  obs_frontend_get_locale_string("Basic.Main.SplitFile"),
								  source_record_split_hotkey, context);

	if (context->chapterHotkey == OBS_INVALID_HOTKEY_ID)
		context->chapterHotkey = obs_hotkey_register_source(parent, "source_record.AddChapterMarker",
								    obs_frontend_get_locale_string("Basic.Main.AddChapterMarker"),
								    source_record_chapter_hotkey, context);

	uint32_t width = obs_source_get_width(parent);
	width += (width & 1);
	uint32_t height = obs_source_get_height(parent);
	height += (height & 1);

	/* B1: obs_reset_video frees every private-view mix, leaving video_output
	 * dangling. On OBS <=31.0 (no "video_reset" signal) detect it by comparing
	 * the global OVI; then drop the pointer so the branch below recreates the
	 * mix. Do NOT obs_view_remove — the mix is already gone. */

	invalidate_reset_video(context);
	/* Fix: never destroy the video_output while an encoder is still feeding
	 * from it. A size change on the parent (window capture / PipeWire on
	 * focus change) used to call obs_view_remove() immediately, freeing the
	 * video_output under a running encoder + replay buffer -> crash. */
	const bool size_changed = width && height && (context->width != width || context->height != height);

	if (width && height && !context->video_output) {
		struct obs_video_info ovi = {0};
		obs_get_video_info(&ovi);
		context->cached_ovi = ovi;

		ovi.base_width = width;
		ovi.base_height = height;
		ovi.output_width = width;
		ovi.output_height = height;

		if (!context->view)
			context->view = obs_view_create();

		context->video_output = obs_view_add2(context->view, &ovi);
		if (context->video_output) {
			context->width = width;
			context->height = height;
		}
	} else if (size_changed && context->video_output) {
		if (os_atomic_load_bool(&context->output_active)) {
			/* defer: let the restart branch stop the outputs first */
			os_atomic_store_bool(&context->restart, true);
		} else if (!context->encoder || !obs_encoder_active(context->encoder)) {
			/* outputs are down and the encoder is idle: safe to swap */
			struct obs_video_info ovi = {0};
			obs_get_video_info(&ovi);
			context->cached_ovi = ovi;

			ovi.base_width = width;
			ovi.base_height = height;
			ovi.output_width = width;
			ovi.output_height = height;

			obs_view_remove(context->view);
			context->video_output = obs_view_add2(context->view, &ovi);
			if (context->video_output) {
				context->width = width;
				context->height = height;
				if (context->encoder)
					obs_encoder_set_video(context->encoder, context->video_output);
			}
		}
		/* else: encoder still winding down, retry on the next tick */
	}

	if (os_atomic_load_bool(&context->restart) && os_atomic_load_bool(&context->output_active)) {
		if (context->fileOutput) {
			if (queue_force_stop(context, context->fileOutput, true))
				context->fileOutput = NULL;
		}
		if (context->streamOutput) {
			if (queue_force_stop(context, context->streamOutput, true))
				context->streamOutput = NULL;
		}
		if (context->replayOutput) {
			save_replay_hotkeys(context);
			if (queue_force_stop(context, context->replayOutput, true))
				context->replayOutput = NULL;
		}
		os_atomic_store_bool(&context->restart, false);
		sync_output_active(context);
	} else if (output_can_start(context, FILE_OUTPUT, os_gettime_ns()) ||
		   output_can_start(context, STREAM_OUTPUT, os_gettime_ns()) ||
		   output_can_start(context, REPLAY_OUTPUT, os_gettime_ns())) {
		if (!context->video_output || !width || !height)
			return;
		if (size_changed || os_atomic_load_long(&context->pending_stops))
			return;
		obs_data_t *s = obs_source_get_settings(context->source);
		if (!context->encoder || !obs_encoder_active(context->encoder))
			update_encoder(context, s);
		if (context->record || context->stream || context->replayBuffer) {
			obs_source_t *view_source = obs_view_get_source(context->view, SOURCE_CHANNEL);
			if (view_source != parent)
				obs_view_set_source(context->view, SOURCE_CHANNEL, parent);
			obs_source_release(view_source);
		}

		obs_source_t *background_source = obs_view_get_source(context->view, BACKGROUND_CHANNEL);
		if (!background_source) {
			background_source = obs_source_create_private("color_source", "Source Record Background", NULL);
			obs_view_set_source(context->view, BACKGROUND_CHANNEL, background_source);
		}
		obs_data_t *css = obs_source_get_settings(background_source);
		if (obs_data_get_int(css, "color") != obs_data_get_int(s, "backgroundColor") ||
		    obs_data_get_int(css, "width") != obs_source_get_width(parent) ||
		    obs_data_get_int(css, "height") != obs_source_get_height(parent)) {
			obs_data_set_int(css, "color", obs_data_get_int(s, "backgroundColor"));
			obs_data_set_int(css, "width", obs_source_get_width(parent));
			obs_data_set_int(css, "height", obs_source_get_height(parent));
			obs_source_update(background_source, css);
		}
		obs_data_release(css);
		obs_source_release(background_source);

		uint64_t now = os_gettime_ns();
		if (output_can_start(context, FILE_OUTPUT, now))
			start_file_output(context, s);
		if (output_can_start(context, STREAM_OUTPUT, now))
			start_stream_output(context, s);
		if (output_can_start(context, REPLAY_OUTPUT, now))
			start_replay_output(context, s);
		obs_data_release(s);
	} else if (!obs_source_enabled(context->source) &&
		   (context->fileOutput || context->streamOutput || context->replayOutput)) {
		if (context->fileOutput) {
			if (queue_force_stop(context, context->fileOutput, false))
				context->fileOutput = NULL;
		}
		if (context->streamOutput) {
			if (queue_force_stop(context, context->streamOutput, false))
				context->streamOutput = NULL;
		}
		if (context->replayOutput) {
			save_replay_hotkeys(context);
			if (queue_force_stop(context, context->replayOutput, false))
				context->replayOutput = NULL;
		}
		sync_output_active(context);
	}

	/* B14: when nothing is active and the encoder is idle, stop pinning the
	 * parent into the private view. It stayed assigned after every stop, so the
	 * parent (and its filter chain) kept rendering every frame and stayed
	 * "showing" forever -> perf drain + ghost source (#38/#40/#135/#141). The
	 * encoder-idle gate avoids blanking the last draining frames of a stop. */
	if ((!obs_source_enabled(context->source) || (!context->record && !context->stream && !context->replayBuffer)) && !os_atomic_load_bool(&context->output_active) &&
	    (!context->encoder || !obs_encoder_active(context->encoder)) && context->view) {
		obs_source_t *view_source = obs_view_get_source(context->view, SOURCE_CHANNEL);
		if (view_source) {
			obs_view_set_source(context->view, SOURCE_CHANNEL, NULL);
			obs_source_release(view_source);
		}
	}

	if (os_atomic_load_bool(&context->output_active) && context->fileOutput && context->record_max_seconds) {
		int totalFrames = obs_output_get_total_frames(context->fileOutput);
		video_t *video = obs_output_video(context->fileOutput);
		uint64_t frameTimeNs = video_output_get_frame_time(video);
		long long msecs = util_mul_div64(totalFrames, frameTimeNs, 1000000ULL);
		if (msecs >= context->record_max_seconds * 1000) {
			obs_data_t *s = obs_source_get_settings(context->source);
			const long long record_mode = obs_data_get_int(s, "record_mode");
			obs_data_release(s);
			if (record_mode == OUTPUT_MODE_ALWAYS) {
				/* websocket auto-stop contract: flip mode to NONE */
				obs_data_t *settings = obs_data_create();
				obs_data_set_int(settings, "record_mode", OUTPUT_MODE_NONE);
				obs_source_update(context->source, settings);
				obs_data_release(settings);
			} else {
				/* N6: session-bound modes must stop transiently, not
				 * persist record_mode=NONE into the scene collection.
				 * Latch instead; frontend *_STOPPED / a settings change
				 * re-arms it. */
				if (queue_force_stop(context, context->fileOutput, false))
					context->fileOutput = NULL;
				context->record = false;
				context->max_seconds_reached = true;
				if (!context->streamOutput && !context->replayOutput && os_atomic_load_bool(&context->output_active)) {
					sync_output_active(context);
				}
			}
		}
	}
}

static void all_properties_changed(obs_properties_t *props, obs_data_t *settings)
{
	obs_property_t *property = obs_properties_first(props);
	while (property) {
		if (obs_property_get_type(property) == OBS_PROPERTY_GROUP) {
			all_properties_changed(obs_property_group_content(property), settings);
		} else {
			obs_property_modified(property, settings);
		}
		obs_property_next(&property);
	}
}

static bool encoder_changed(void *data, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(property);
	obs_properties_remove_by_name(props, "encoder_group");
	obs_properties_remove_by_name(props, "audio_encoder_group");
	bool visible = obs_property_visible(obs_properties_get(props, "others"));
	obs_properties_remove_by_name(props, "others");
	obs_properties_remove_by_name(props, "plugin_info");
	set_encoder_defaults(settings);
	const char *enc_id = get_encoder_id(settings);
	obs_properties_t *enc_props = obs_get_encoder_properties(enc_id);
	if (enc_props) {
		all_properties_changed(enc_props, settings);
		obs_properties_add_group(props, "encoder_group", obs_encoder_get_display_name(enc_id), OBS_GROUP_NORMAL, enc_props);
	}

	enc_id = obs_data_get_string(settings, "audio_encoder");
	if (!enc_id || !strlen(enc_id))
		enc_id = "ffmpeg_aac";
	enc_props = obs_get_encoder_properties(enc_id);
	if (enc_props) {
		all_properties_changed(enc_props, settings);
		const char *name = obs_encoder_get_display_name(enc_id);
		if (!name || !strlen(name))
			name = obs_module_text("AudioEncoder");
		obs_property_t *b = obs_properties_get(enc_props, "bitrate");
		if (b && obs_property_get_type(b) == OBS_PROPERTY_INT) {
			obs_property_int_set_suffix(obs_properties_add_int(enc_props, "audio_bitrate", obs_property_description(b),
									   obs_property_int_min(b), obs_property_int_max(b),
									   obs_property_int_step(b)),
						    obs_property_int_suffix(b));
			obs_properties_remove_by_name(enc_props, "bitrate");
		} else if (b && obs_property_get_type(b) == OBS_PROPERTY_LIST &&
			   obs_property_list_format(b) == OBS_COMBO_FORMAT_INT) {
			/* Some encoders (CoreAudio AAC on macOS) expose "bitrate" as
			 * an int-list, not an int. Copying int_min/max/step off it
			 * yields a [0,0] spinner locked at 0. Rebuild it as a list. */
			obs_property_t *nb = obs_properties_add_list(enc_props, "audio_bitrate", obs_property_description(b),
								    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
			const size_t count = obs_property_list_item_count(b);
			for (size_t i = 0; i < count; i++)
				obs_property_list_add_int(nb, obs_property_list_item_name(b, i), obs_property_list_item_int(b, i));
			obs_properties_remove_by_name(enc_props, "bitrate");
		}
		obs_properties_add_group(props, "audio_encoder_group", name, OBS_GROUP_NORMAL, enc_props);
	}

	obs_property_t *p = obs_properties_add_text(props, "others", obs_module_text("OtherSourceRecords"), OBS_TEXT_INFO);
	obs_property_set_visible(p, visible);
	obs_properties_add_text(
		props, "plugin_info",
		"<a href=\"https://obsproject.com/forum/resources/source-record.1285/\">Source Record</a> (" PROJECT_VERSION
		") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
		OBS_TEXT_INFO);
	return true;
}

static bool list_add_audio_sources(void *data, obs_source_t *source)
{
	obs_property_t *p = data;
	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_COMPOSITE) != 0) {
		obs_property_list_add_string(p, obs_source_get_name(source), obs_source_get_name(source));
	} else if ((flags & OBS_SOURCE_AUDIO) != 0) {
		obs_property_list_add_string(p, obs_source_get_name(source), obs_source_get_name(source));
	}
	return true;
}

static bool source_record_split_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct source_record_filter_context *context = data;
	if (!context->fileOutput)
		return false;
	proc_handler_t *ph = obs_output_get_proc_handler(context->fileOutput);
	struct calldata cd;
	calldata_init(&cd);
	proc_handler_call(ph, "split_file", &cd);
	calldata_free(&cd);
	return true;
}

bool output_exists(const char *id)
{
	const char *output_id;
	size_t i = 0;
	while (obs_enum_output_types(i++, &output_id)) {
		if (strcmp(id, output_id) == 0)
			return true;
	}
	return false;
}

static obs_properties_t *source_record_filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	if (data && ((struct source_record_filter_context *)data)->audio_settings_pending)
		obs_properties_add_text(props, "audio_pending", obs_module_text("AudioChangesPending"), OBS_TEXT_INFO);

	obs_properties_t *record = obs_properties_create();

	obs_property_t *p = obs_properties_add_list(record, "record_mode", obs_module_text("RecordMode"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(p, obs_module_text("None"), OUTPUT_MODE_NONE);
	obs_property_list_add_int(p, obs_module_text("Always"), OUTPUT_MODE_ALWAYS);
	obs_property_list_add_int(p, obs_module_text("Streaming"), OUTPUT_MODE_STREAMING);
	obs_property_list_add_int(p, obs_module_text("Recording"), OUTPUT_MODE_RECORDING);
	obs_property_list_add_int(p, obs_module_text("StreamingOrRecording"), OUTPUT_MODE_STREAMING_OR_RECORDING);
	obs_property_list_add_int(p, obs_module_text("VirtualCamera"), OUTPUT_MODE_VIRTUAL_CAMERA);

	obs_properties_add_path(record, "path", obs_module_text("Path"), OBS_PATH_DIRECTORY, NULL, NULL);
	obs_properties_add_text(record, "filename_formatting", obs_module_text("FilenameFormatting"), OBS_TEXT_DEFAULT);
	p = obs_properties_add_list(record, "rec_format", obs_module_text("RecFormat"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "flv", "flv");
	obs_property_list_add_string(p, "mp4", "mp4");
	if (output_exists("mp4_output"))
		obs_property_list_add_string(p, "hybrid_mp4", "hybrid_mp4");
	if (output_exists("mov_output"))
		obs_property_list_add_string(p, "hybrid_mov", "hybrid_mov");
	obs_property_list_add_string(p, "fragmented_mp4", "fragmented_mp4");
	obs_property_list_add_string(p, "fragmented_mov", "fragmented_mov");
	obs_property_list_add_string(p, "mov", "mov");
	obs_property_list_add_string(p, "mkv", "mkv");
	obs_property_list_add_string(p, "ts", "ts");
	obs_property_list_add_string(p, "m3u8", "m3u8");

	obs_properties_t *split_file = obs_properties_create();
	p = obs_properties_add_int(split_file, "max_time_sec",
				   obs_frontend_get_locale_string("Basic.Settings.Output.SplitFile.Time"), 0, 31536000, 1);
	obs_property_int_set_suffix(p, " s");
	p = obs_properties_add_int(split_file, "max_size_mb",
				   obs_frontend_get_locale_string("Basic.Settings.Output.SplitFile.Size"), 0, 1073741824, 1);
	obs_property_int_set_suffix(p, " MB");
	obs_properties_add_button(split_file, "split_file_now", obs_frontend_get_locale_string("Basic.Main.SplitFile"),
				 source_record_split_button);
	obs_properties_add_group(record, "split_file", obs_frontend_get_locale_string("Basic.Settings.Output.EnableSplitFile"),
				 OBS_GROUP_CHECKABLE, split_file);

	obs_properties_add_int(record, "record_max_seconds", obs_module_text("MaxSeconds"), 0, 31536000, 1);

	obs_properties_add_group(props, "record", obs_module_text("Record"), OBS_GROUP_NORMAL, record);

	obs_properties_t *replay = obs_properties_create();

	p = obs_properties_add_int(replay, "replay_duration", obs_module_text("Duration"), 1, 10000, 1);
	obs_property_int_set_suffix(p, "s");

	obs_properties_add_text(replay, "replay_filename_formatting", obs_module_text("FilenameFormatting"), OBS_TEXT_DEFAULT);

	obs_properties_add_group(props, "replay_buffer", obs_module_text("ReplayBuffer"), OBS_GROUP_CHECKABLE, replay);

	obs_properties_t *stream = obs_properties_create();

	p = obs_properties_add_list(stream, "stream_mode", obs_module_text("StreamMode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(p, obs_module_text("None"), OUTPUT_MODE_NONE);
	obs_property_list_add_int(p, obs_module_text("Always"), OUTPUT_MODE_ALWAYS);
	obs_property_list_add_int(p, obs_module_text("Streaming"), OUTPUT_MODE_STREAMING);
	obs_property_list_add_int(p, obs_module_text("Recording"), OUTPUT_MODE_RECORDING);
	obs_property_list_add_int(p, obs_module_text("StreamingOrRecording"), OUTPUT_MODE_STREAMING_OR_RECORDING);
	obs_property_list_add_int(p, obs_module_text("VirtualCamera"), OUTPUT_MODE_VIRTUAL_CAMERA);

	obs_properties_add_text(stream, "server", obs_module_text("Server"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(stream, "key", obs_module_text("Key"), OBS_TEXT_PASSWORD);

	obs_properties_add_group(props, "stream", obs_module_text("Stream"), OBS_GROUP_NORMAL, stream);

	obs_properties_t *background = obs_properties_create();

	obs_properties_add_color(background, "backgroundColor", obs_module_text("BackgroundColor"));

	obs_properties_add_group(props, "background", obs_module_text("Background"), OBS_GROUP_NORMAL, background);

	obs_properties_t *audio = obs_properties_create();

	p = obs_properties_add_list(audio, "audio_track", obs_module_text("AudioTrack"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("None"), 0);
	obs_property_list_add_int(p, obs_module_text("All"), -1);
	const char *track = obs_module_text("Track");
	for (int i = 1; i <= MAX_AUDIO_MIXES; i++) {
		char buffer[64];
		snprintf(buffer, 64, "%s %i", track, i);
		obs_property_list_add_int(p, buffer, i);
	}

	p = obs_properties_add_list(audio, "audio_source", obs_module_text("Source"), OBS_COMBO_TYPE_EDITABLE,
				    OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(list_add_audio_sources, p);
	obs_enum_scenes(list_add_audio_sources, p);

	obs_properties_add_group(props, "different_audio", obs_module_text("DifferentAudio"), OBS_GROUP_CHECKABLE, audio);

	obs_properties_t *scale = obs_properties_create();
	p = obs_properties_add_list(scale, "resolution", obs_module_text("Resolution"), OBS_COMBO_TYPE_EDITABLE,
				    OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p, "640x480", "640x480");
	obs_property_list_add_string(p, "800x600", "800x600");
	obs_property_list_add_string(p, "1280x720", "1280x720");
	obs_property_list_add_string(p, "1920x1080", "1920x1080");
	obs_property_list_add_string(p, "2560x1440", "2560x1440");

	if (obs_encoder_set_gpu_scale_type_func) {
		p = obs_properties_add_list(scale, "scale_type", obs_module_text("ScaleType"), OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p, obs_frontend_get_locale_string("Basic.Settings.Video.DownscaleFilter.Bilinear"),
					  OBS_SCALE_BILINEAR);
		obs_property_list_add_int(p, obs_frontend_get_locale_string("Basic.Settings.Video.DownscaleFilter.Area"),
					  OBS_SCALE_AREA);
		obs_property_list_add_int(p, obs_frontend_get_locale_string("Basic.Settings.Video.DownscaleFilter.Bicubic"),
					  OBS_SCALE_BICUBIC);
		obs_property_list_add_int(p, obs_frontend_get_locale_string("Basic.Settings.Video.DownscaleFilter.Lanczos"),
					  OBS_SCALE_LANCZOS);
	}

	obs_properties_add_group(props, "scale", obs_module_text("Scale"), OBS_GROUP_CHECKABLE, scale);

	if (obs_encoder_set_frame_rate_divisor_func) {
		p = obs_properties_add_list(props, "frame_rate_divisor", obs_module_text("Framerate"), OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_INT);
		struct obs_video_info ovi;
		obs_get_video_info(&ovi);
		float fps = ovi.fps_den > 0 ? (float)ovi.fps_num / (float)ovi.fps_den : 0.0f;
		struct dstr str;
		dstr_init(&str);
		dstr_printf(&str, "%.2f fps", fps);
		obs_property_list_add_int(p, str.array, 0);
		for (int i = 2; i <= 10; i++) {
			dstr_printf(&str, "%.2f fps (/%d)", fps / (float)i, i);
			obs_property_list_add_int(p, str.array, i);
		}
		dstr_free(&str);
	}

	p = obs_properties_add_list(props, "encoder", obs_module_text("VideoEncoder"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_t *audio_encoder = obs_properties_add_list(props, "audio_encoder", obs_module_text("AudioEncoder"),
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p, obs_module_text("Software"), "x264");
	if (EncoderAvailable("obs_qsv11"))
		obs_property_list_add_string(p, obs_module_text("QSV.H264"), "qsv");
	if (EncoderAvailable("obs_qsv11_av1"))
		obs_property_list_add_string(p, obs_module_text("QSV.AV1"), "qsv_av1");
	if (EncoderAvailable("ffmpeg_nvenc") || EncoderAvailable("jim_nvenc") || EncoderAvailable("obs_nvenc_h264_tex"))
		obs_property_list_add_string(p, obs_module_text("NVENC.H264"), "nvenc");
	if (EncoderAvailable("jim_av1_nvenc") || EncoderAvailable("obs_nvenc_av1_tex"))
		obs_property_list_add_string(p, obs_module_text("NVENC.AV1"), "nvenc_av1");
	if (EncoderAvailable("h265_texture_amf"))
		obs_property_list_add_string(p, obs_module_text("AMD.HEVC"), "amd_hevc");
	if (EncoderAvailable("ffmpeg_hevc_nvenc") || EncoderAvailable("obs_nvenc_hevc_tex"))
		obs_property_list_add_string(p, obs_module_text("NVENC.HEVC"), "nvenc_hevc");
	if (EncoderAvailable("h264_texture_amf"))
		obs_property_list_add_string(p, obs_module_text("AMD.H264"), "amd");
	if (EncoderAvailable("av1_texture_amf"))
		obs_property_list_add_string(p, obs_module_text("AMD.AV1"), "amd_av1");
	if (EncoderAvailable("com.apple.videotoolbox.videoencoder.ave.avc"))
		obs_property_list_add_string(p, obs_module_text("Apple.H264"), "apple_h264");
	if (EncoderAvailable("com.apple.videotoolbox.videoencoder.ave.hevc"))
		obs_property_list_add_string(p, obs_module_text("Apple.HEVC"), "apple_hevc");

	const char *enc_id = NULL;
	size_t i = 0;
	while (obs_enum_encoder_types(i++, &enc_id)) {
		const uint32_t caps = obs_get_encoder_caps(enc_id);
		if ((caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)) != 0)
			continue;
		const char *name = obs_encoder_get_display_name(enc_id);
		if (obs_get_encoder_type(enc_id) == OBS_ENCODER_VIDEO) {
			obs_property_list_add_string(p, name, enc_id);
		} else {
			obs_property_list_add_string(audio_encoder, name, enc_id);
		}
	}
	obs_property_set_modified_callback2(p, encoder_changed, data);
	obs_property_set_modified_callback2(audio_encoder, encoder_changed, data);

	obs_properties_add_group(props, "encoder_group", obs_module_text("VideoEncoder"), OBS_GROUP_NORMAL,
				 obs_properties_create());
	obs_properties_add_group(props, "audio_encoder_group", obs_module_text("AudioEncoder"), OBS_GROUP_NORMAL,
				 obs_properties_create());

	p = obs_properties_add_text(props, "others", obs_module_text("OtherSourceRecords"), OBS_TEXT_INFO);
	if (data) {
		struct source_record_filter_context *context = data;
		struct dstr sources_text;
		dstr_init(&sources_text);
		pthread_mutex_lock(&filters_mutex);
		for (size_t i = 0; i < source_record_filters.num; i++) {
			if (source_record_filters.array[i] == context->source)
				continue;
			if (sources_text.len)
				dstr_cat(&sources_text, "\n");
			obs_source_t *parent = obs_filter_get_parent(source_record_filters.array[i]);
			if (parent) {
				dstr_cat(&sources_text, obs_source_get_name(parent));
				dstr_cat(&sources_text, " - ");
			}
			dstr_cat(&sources_text, obs_source_get_name(source_record_filters.array[i]));
		}
		pthread_mutex_unlock(&filters_mutex);
		if (sources_text.len > 0) {
			obs_data_t *settings = obs_source_get_settings(context->source);
			obs_data_set_string(settings, "others", sources_text.array);
			obs_data_release(settings);
			obs_property_set_visible(p, true);
		} else {
			obs_property_set_visible(p, false);
		}
		dstr_free(&sources_text);
	} else {
		obs_property_set_visible(p, false);
	}

	obs_properties_add_text(
		props, "plugin_info",
		"<a href=\"https://obsproject.com/forum/resources/source-record.1285/\">Source Record</a> (" PROJECT_VERSION
		") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
		OBS_TEXT_INFO);
	return props;
}

static void source_record_filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct source_record_filter_context *context = data;
	obs_source_skip_video_filter(context->source);
}

static void source_record_filter_filter_remove(void *data, obs_source_t *parent)
{
	struct source_record_filter_context *context = data;
	close_filter_showing(context, parent);
	stop_output_sync(context, context->fileOutput);
	stop_output_sync(context, context->streamOutput);
	save_replay_hotkeys(context);
	stop_output_sync(context, context->replayOutput);
	remove_frontend_cb(context);
}

struct obs_source_info source_record_filter_info = {
	.id = "source_record_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = source_record_filter_get_name,
	.create = source_record_filter_create,
	.destroy = source_record_filter_destroy,
	.update = source_record_filter_update,
	.load = source_record_filter_update,
	.save = source_record_filter_save,
	.get_defaults = source_record_filter_defaults,
	.video_tick = source_record_filter_tick,
	.get_properties = source_record_filter_properties,
	.filter_remove = source_record_filter_filter_remove,
	.video_render = source_record_filter_render,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("source-record", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Source Record Filter";
}

static void find_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	const char *id = obs_source_get_unversioned_id(child);
	if (strcmp(id, "source_record_filter") != 0)
		return;
	obs_source_t **filter = param;
	/* N3: take the ref inside the callback (the enum holds filter_mutex here),
	 * keeping the last match to mirror the original reverse-iteration result. */
	obs_source_t *ref = obs_source_get_ref(child);
	if (ref) {
		obs_source_release(*filter);
		*filter = ref;
	}
}

struct filter_list {
	DARRAY(obs_source_t *) items;
	const char *name;
};

static void find_source_by_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct filter_list *list = param;
	if (strcmp(obs_source_get_unversioned_id(child), "source_record_filter") != 0 ||
	    (*list->name && strcmp(obs_source_get_name(child), list->name) != 0))
		return;
	for (size_t i = 0; i < list->items.num; i++)
		if (list->items.array[i] == child)
			return;
	obs_source_t *ref = obs_source_get_ref(child);
	if (ref)
		da_push_back(list->items, &ref);
}

static bool find_source(void *data, obs_source_t *source)
{
	obs_source_enum_filters(source, find_source_by_filter, data);
	return true;
}

static bool apply_to_all_filters(obs_data_t *request, obs_data_t *response,
				 bool (*operation)(obs_source_t *, obs_data_t *, obs_data_t *))
{
	struct filter_list list = {0};
	list.name = obs_data_get_string(request, "filter");
	obs_enum_sources(find_source, &list);
	obs_enum_scenes(find_source, &list);
	bool success = list.items.num > 0;
	if (!success)
		obs_data_set_string(response, "error", "no source record filter found");
	for (size_t i = 0; i < list.items.num; i++) {
		success = operation(list.items.array[i], request, response) && success;
		obs_source_release(list.items.array[i]);
	}
	da_free(list.items);
	return success;
}

obs_source_t *get_source_record_filter(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data, bool create)
{
	const char *filter_name = obs_data_get_string(request_data, "filter");
	/* Global operations already hold the exact filter identity. */
	if (strcmp(obs_source_get_unversioned_id(source), "source_record_filter") == 0) {
		struct source_record_filter_context *context = obs_obj_get_data(source);
		if (!context || os_atomic_load_bool(&context->closing)) {
			obs_data_set_string(response_data, "error", "filter is closing");
			return NULL;
		}
		if (create && !obs_source_enabled(source))
			obs_source_set_enabled(source, true);
		return obs_source_get_ref(source);
	}
	obs_source_t *filter = NULL;
	config_t *config = obs_frontend_get_profile_config();
	if (strlen(filter_name)) {
		filter = obs_source_get_filter_by_name(source, filter_name);
		if (!filter) {
			if (response_data)
				obs_data_set_string(response_data, "error", "filter not found");
			return NULL;
		}
		if (strcmp(obs_source_get_unversioned_id(filter), "source_record_filter") != 0) {
			if (response_data)
				obs_data_set_string(response_data, "error", "filter is not source record filter");
			obs_source_release(filter);
			return NULL;
		}
	} else {
		obs_source_enum_filters(source, find_filter, &filter);
		if (!filter) {
			if (!create) {
				if (response_data)
					obs_data_set_string(response_data, "error", "failed to find filter");
				return NULL;
			}

			const char *filename = obs_data_get_string(request_data, "filename");
			if (!strlen(filename)) {
				filename = config_get_string(config, "Output", "FilenameFormatting");
			}
			obs_data_t *settings = obs_data_create();
			obs_data_set_bool(settings, "remove_after_record", true);
			char *filter_name = os_generate_formatted_filename(NULL, true, filename);
			filter = obs_source_get_filter_by_name(source, filter_name);
			if (!filter) {
				filter = obs_source_create("source_record_filter", filter_name, settings, NULL);
			} else if (strcmp(obs_source_get_unversioned_id(filter), "source_record_filter") != 0) {
				if (response_data)
					obs_data_set_string(response_data, "error", "filter is not source record filter");
				obs_source_release(filter);
				bfree(filter_name);
				obs_data_release(settings);
				return NULL;
			}
			bfree(filter_name);
			obs_data_release(settings);
			if (!filter) {
				if (response_data)
					obs_data_set_string(response_data, "error", "failed to create filter");
				return NULL;
			}
			obs_source_filter_add(source, filter);
		}
	}
	/* Only (re-)enable the filter for requests that actually start an
	 * output. Stop/pause/split/chapter requests pass create=false and must
	 * not silently re-enable a filter the user disabled -> a latched
	 * stream/record would resume on a "stop" call. */
	if (create && !obs_source_enabled(filter))
		obs_source_set_enabled(filter, true);
	return filter;
}

static bool start_record_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, true);
	if (!filter)
		return false;
	obs_data_t *settings = obs_source_get_settings(filter);
	const char *filename = obs_data_get_string(request_data, "filename");
	struct source_record_filter_context *context = obs_obj_get_data(filter);
	if (context && os_atomic_load_bool(&context->output_active)) {
		if (strlen(filename)) {
			if (strstr(filename, "%") || strcmp(filename, obs_data_get_string(settings, "filename_formatting")) != 0) {
				os_atomic_store_bool(&context->restart, true);
			}
		} else if (strstr(obs_data_get_string(settings, "filename_formatting"), "%")) {
			os_atomic_store_bool(&context->restart, true);
		}
	}

	if (strlen(filename))
		obs_data_set_string(settings, "filename_formatting", filename);
	if (obs_data_has_user_value(request_data, "max_seconds"))
		obs_data_set_int(settings, "record_max_seconds", obs_data_get_int(request_data, "max_seconds"));
	obs_data_set_int(settings, "record_mode", OUTPUT_MODE_ALWAYS);

	obs_source_update(filter, settings);
	obs_data_release(settings);

	obs_source_release(filter);
	return true;
}

static bool pause_record_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;

	struct source_record_filter_context *context = obs_obj_get_data(filter);
	if (!context || !context->fileOutput) {
		if (response_data)
			obs_data_set_string(response_data, "error", "record output is not active");
		obs_source_release(filter);
		return false;
	}
	obs_output_pause(context->fileOutput, true);
	obs_source_release(filter);
	return true;
}

static bool unpause_record_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;

	struct source_record_filter_context *context = obs_obj_get_data(filter);
	if (!context || !context->fileOutput) {
		if (response_data)
			obs_data_set_string(response_data, "error", "record output is not active");
		obs_source_release(filter);
		return false;
	}
	obs_output_pause(context->fileOutput, false);
	obs_source_release(filter);
	return true;
}

static bool split_record_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;

	struct source_record_filter_context *context = obs_obj_get_data(filter);
	if (!context || !context->fileOutput) {
		if (response_data)
			obs_data_set_string(response_data, "error", "record output is not active");
		obs_source_release(filter);
		return false;
	}
	proc_handler_t *ph = obs_output_get_proc_handler(context->fileOutput);
	struct calldata cd;
	calldata_init(&cd);
	if (!proc_handler_call(ph, "split_file", &cd)) {
		calldata_free(&cd);
		if (response_data)
			obs_data_set_string(response_data, "error", "split_file call failed");
		obs_source_release(filter);
		return false;
	}
	calldata_free(&cd);
	obs_source_release(filter);
	return true;
}

static bool add_chapter_record_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;

	struct source_record_filter_context *context = obs_obj_get_data(filter);
	if (!context || !context->fileOutput) {
		if (response_data)
			obs_data_set_string(response_data, "error", "record output is not active");
		obs_source_release(filter);
		return false;
	}
	proc_handler_t *ph = obs_output_get_proc_handler(context->fileOutput);
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "chapter_name", obs_data_get_string(request_data, "chapter_name"));
	if (!proc_handler_call(ph, "add_chapter", &cd)) {
		calldata_free(&cd);
		if (response_data)
			obs_data_set_string(response_data, "error", "add_chapter call failed");
		obs_source_release(filter);
		return false;
	}
	calldata_free(&cd);
	obs_source_release(filter);
	return true;
}

static bool stop_record_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "record_mode", OUTPUT_MODE_NONE);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	obs_source_release(filter);
	return true;
}

static void websocket_start_record(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		if (obs_data_get_bool(request_data, "stop_existing"))
			stop_record_source(source, request_data, NULL);
		success = start_record_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		success = apply_to_all_filters(request_data, response_data, start_record_source);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_pause_record(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = pause_record_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		success = apply_to_all_filters(request_data, response_data, pause_record_source);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_unpause_record(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = unpause_record_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		success = apply_to_all_filters(request_data, response_data, unpause_record_source);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_split_record(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = split_record_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		success = apply_to_all_filters(request_data, response_data, split_record_source);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_add_chapter_record(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = add_chapter_record_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		success = apply_to_all_filters(request_data, response_data, add_chapter_record_source);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_stop_record(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = stop_record_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		success = apply_to_all_filters(request_data, response_data, stop_record_source);
	}
	obs_data_set_bool(response_data, "success", success);
}

static bool start_replay_buffer_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, true);
	if (!filter)
		return false;
	obs_data_t *settings = obs_source_get_settings(filter);
	const char *filename = obs_data_get_string(request_data, "filename");

	/* The replay buffer uses replay_filename_formatting, not
	 * filename_formatting. Writing the record key here did nothing for
	 * replays and clobbered the record path; and the old restart-detection
	 * needlessly force-stopped all outputs (flushing the buffer). The
	 * replay format is applied hot (source_record_filter_update pushes it to
	 * the live output and the muxer reads it at save time), so no restart. */
	if (strlen(filename))
		obs_data_set_string(settings, "replay_filename_formatting", filename);

	obs_data_set_bool(settings, "replay_buffer", true);

	obs_source_update(filter, settings);
	obs_data_release(settings);

	obs_source_release(filter);
	return true;
}

static bool stop_replay_buffer_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "replay_buffer", false);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	obs_source_release(filter);
	return true;
}

static bool save_replay_buffer_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;
	struct source_record_filter_context *context = obs_obj_get_data(filter);
	if (!context->replayOutput) {
		blog(LOG_WARNING, "[Source Record] replay_buffer_save requested but replay output is not active");
		obs_source_release(filter);
		return false;
	}

	proc_handler_t *ph = obs_output_get_proc_handler(context->replayOutput);
	calldata_t cd = {0};
	bool success = proc_handler_call(ph, "save", &cd);
	calldata_free(&cd);
	if (!success)
		blog(LOG_WARNING, "[Source Record] replay_buffer_save proc call failed");
	else
		blog(LOG_INFO, "[Source Record] replay_buffer_save requested successfully");
	obs_source_release(filter);
	return success;
}

static void websocket_start_replay_buffer(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		if (obs_data_get_bool(request_data, "stop_existing"))
			stop_replay_buffer_source(source, request_data, NULL);
		success = start_replay_buffer_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		success = apply_to_all_filters(request_data, response_data, start_replay_buffer_source);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_stop_replay_buffer(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = stop_replay_buffer_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		success = apply_to_all_filters(request_data, response_data, stop_replay_buffer_source);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_save_replay_buffer(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = save_replay_buffer_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		success = apply_to_all_filters(request_data, response_data, save_replay_buffer_source);
	}
	if (!success)
		blog(LOG_WARNING, "[Source Record] websocket replay_buffer_save finished with failures");
	else
		blog(LOG_INFO, "[Source Record] websocket replay_buffer_save finished successfully");
	obs_data_set_bool(response_data, "success", success);
}

static bool start_stream_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, true);
	if (!filter)
		return false;
	obs_data_t *settings = obs_source_get_settings(filter);

	const char *server = obs_data_get_string(request_data, "server");
	const char *key = obs_data_get_string(request_data, "key");

	/* N8: rotating server/key on a live stream must restart the output. On the
	 * common no-'filter' path restart was never set, so the new credentials
	 * were silently ignored and the stream stayed on the old ones. */
	struct source_record_filter_context *context = obs_obj_get_data(filter);
	if (context && os_atomic_load_bool(&context->output_active) &&
	    ((server && strlen(server) && strcmp(server, obs_data_get_string(settings, "server")) != 0) ||
	     (key && strlen(key) && strcmp(key, obs_data_get_string(settings, "key")) != 0)))
		os_atomic_store_bool(&context->restart, true);

	if (server && strlen(server))
		obs_data_set_string(settings, "server", server);
	if (key && strlen(key))
		obs_data_set_string(settings, "key", key);

	obs_data_set_int(settings, "stream_mode", OUTPUT_MODE_ALWAYS);

	obs_source_update(filter, settings);
	obs_data_release(settings);

	obs_source_release(filter);
	return true;
}

static bool stop_stream_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "stream_mode", OUTPUT_MODE_NONE);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	obs_source_release(filter);
	return true;
}

static void websocket_start_stream(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		if (obs_data_get_bool(request_data, "stop_existing"))
			stop_stream_source(source, request_data, NULL);
		success = start_stream_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		success = apply_to_all_filters(request_data, response_data, start_stream_source);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_stop_stream(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = stop_stream_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		success = apply_to_all_filters(request_data, response_data, stop_stream_source);
	}
	obs_data_set_bool(response_data, "success", success);
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Source Record] loaded version %s", PROJECT_VERSION);
	obs_register_source(&source_record_filter_info);

	da_init(source_record_filters);

	vendor = obs_websocket_register_vendor("source-record");
	obs_websocket_vendor_register_request(vendor, "record_start", websocket_start_record, NULL);
	obs_websocket_vendor_register_request(vendor, "record_pause", websocket_pause_record, NULL);
	obs_websocket_vendor_register_request(vendor, "record_unpause", websocket_unpause_record, NULL);
	obs_websocket_vendor_register_request(vendor, "record_split", websocket_split_record, NULL);
	obs_websocket_vendor_register_request(vendor, "record_add_chapter", websocket_add_chapter_record, NULL);
	obs_websocket_vendor_register_request(vendor, "record_stop", websocket_stop_record, NULL);
	obs_websocket_vendor_register_request(vendor, "replay_buffer_start", websocket_start_replay_buffer, NULL);
	obs_websocket_vendor_register_request(vendor, "replay_buffer_stop", websocket_stop_replay_buffer, NULL);
	obs_websocket_vendor_register_request(vendor, "replay_buffer_save", websocket_save_replay_buffer, NULL);
	obs_websocket_vendor_register_request(vendor, "stream_start", websocket_start_stream, NULL);
	obs_websocket_vendor_register_request(vendor, "stream_stop", websocket_stop_stream, NULL);

	return true;
}

void obs_module_post_load(void)
{
#ifdef _WIN32
	void *handle = os_dlopen("obs");
#else
	void *handle = dlopen(NULL, RTLD_LAZY);
#endif
	if (handle) {
		obs_encoder_set_frame_rate_divisor_func =
			(bool (*)(obs_encoder_t *, uint32_t))os_dlsym(handle, "obs_encoder_set_frame_rate_divisor");
		obs_encoder_set_gpu_scale_type_func =
			(void (*)(obs_encoder_t *, enum obs_scale_type))os_dlsym(handle, "obs_encoder_set_gpu_scale_type");
		os_dlclose(handle);
	}
}

void obs_module_unload(void)
{
	da_free(source_record_filters);
}

const char *obs_module_name(void)
{
	return obs_module_text("SourceRecord");
}
