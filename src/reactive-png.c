/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Reactive PNG: an image source that lights up, and optionally bounces, while
 * the streamer talks.
 *
 * Per-frame cost is one sprite draw with OBS's stock DrawMultiply technique:
 * no shader to compile, no render target, no per-frame allocation.
 *
 * Threads:
 *   audio thread     on_audio() only. It touches the two atomics below and
 *                    nothing else, so it can never hold up OBS's audio.
 *   graphics thread  rp_tick(), rp_render() and rp_update(). OBS defers
 *                    update() for video sources onto the tick, so all other
 *                    state is shared between them without locks.
 */

#include <obs-module.h>
#include <graphics/image-file.h>

#include <math.h>
#include <stdatomic.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("reactive-png", "en-US")

#define S_FILE "file"
#define S_AUDIO "audio_source"
#define S_THRESHOLD "threshold_db"
#define S_HOLD "hold_ms"
#define S_IDLE "idle_brightness"
#define S_TALK "talk_brightness"
#define S_FADE "fade_ms"
#define S_BOUNCE "bounce"
#define S_BOUNCE_HEIGHT "bounce_height"
#define S_BOUNCE_TIME "bounce_ms"

#define T_(s) obs_module_text(s)

/* How often to look for an audio source that is not available yet. Sources can
 * load after us at startup, and the user can delete and re-add them. */
#define AUDIO_RETRY_SECONDS 1.0f

struct reactive_png {
	obs_source_t *source;

	/* Shared with the audio thread. */
	_Atomic float threshold;  /* mean-square level, not dB */
	atomic_uint voice_blocks; /* audio blocks seen above the threshold */

	char *file;
	gs_image_file4_t image;

	char *audio_uuid;
	obs_weak_source_t *audio;
	float audio_retry_left;
	size_t channels;

	float hold_seconds;
	float idle_multiplier;
	float talk_multiplier;
	float fade_rate; /* 0..1 glow change per second */
	float hop_rate;  /* 0..1 hop progress per second */

	/* Headroom above the image that the hop moves into. It is part of the
	 * source's size so a hop is never clipped when filters render the
	 * source to a texture of exactly that size. */
	uint32_t bounce_px;

	unsigned voice_seen;
	float hold_left;
	bool was_talking;
	float glow; /* 0 idle .. 1 fully lit */
	float hop;  /* 0 on the ground, otherwise progress through the hop */

	/* Values rp_render() draws with, worked out once per tick. */
	float multiplier;
	float image_y;
};

/* Sliders read as perceived brightness, the shader multiplies linear light. */
static inline float percent_to_multiplier(long long percent)
{
	return powf((float)percent / 100.0f, 2.2f);
}

/* Runs for every block of audio (about 47 times a second), after the mic's
 * filters, so a noise gate or suppressor on the mic also cleans up triggering.
 * Comparing mean squares against a precomputed threshold keeps log, sqrt and
 * division out of this path. */
static void on_audio(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	UNUSED_PARAMETER(source);

	struct reactive_png *ctx = param;
	if (muted)
		return;

	float loudest = 0.0f;
	for (size_t ch = 0; ch < ctx->channels; ch++) {
		const float *samples = (const float *)audio->data[ch];
		if (!samples)
			continue;

		float sum = 0.0f;
		for (uint32_t i = 0; i < audio->frames; i++)
			sum += samples[i] * samples[i];
		if (sum > loudest)
			loudest = sum;
	}

	const float threshold = atomic_load_explicit(&ctx->threshold, memory_order_relaxed);
	if (loudest > threshold * (float)audio->frames)
		atomic_fetch_add_explicit(&ctx->voice_blocks, 1, memory_order_relaxed);
}

static void audio_detach(struct reactive_png *ctx)
{
	if (!ctx->audio)
		return;

	obs_source_t *source = obs_weak_source_get_source(ctx->audio);
	if (source) {
		obs_source_remove_audio_capture_callback(source, on_audio, ctx);
		obs_source_release(source);
	}
	obs_weak_source_release(ctx->audio);
	ctx->audio = NULL;
}

static void audio_attach(struct reactive_png *ctx)
{
	obs_source_t *source = obs_get_source_by_uuid(ctx->audio_uuid);
	if (!source)
		return;

	obs_source_add_audio_capture_callback(source, on_audio, ctx);
	ctx->audio = obs_source_get_weak_source(source);
	obs_source_release(source);
}

static void audio_maintain(struct reactive_png *ctx, float seconds)
{
	ctx->audio_retry_left -= seconds;
	if (ctx->audio_retry_left > 0.0f)
		return;
	ctx->audio_retry_left = AUDIO_RETRY_SECONDS;

	if (ctx->audio && obs_weak_source_expired(ctx->audio))
		audio_detach(ctx);
	if (!ctx->audio && ctx->audio_uuid && *ctx->audio_uuid)
		audio_attach(ctx);
}

static void load_image(struct reactive_png *ctx, const char *file)
{
	obs_enter_graphics();
	gs_image_file4_free(&ctx->image);
	obs_leave_graphics();

	bfree(ctx->file);
	ctx->file = bstrdup(file);
	if (!*file)
		return;

	/* The _file4 variant matters: the plain gs_image_file_init() leaves
	 * alpha straight, which leaves halos on edges with the premultiplied
	 * blend in rp_render(). */
	gs_image_file4_init(&ctx->image, file, GS_IMAGE_ALPHA_PREMULTIPLY);

	obs_enter_graphics();
	gs_image_file4_init_texture(&ctx->image);
	obs_leave_graphics();

	if (!ctx->image.image3.image2.image.loaded)
		blog(LOG_WARNING, "[reactive-png] failed to load '%s'", file);
}

static void pose_update(struct reactive_png *ctx)
{
	const float height = (float)ctx->bounce_px;
	const float arc = 4.0f * ctx->hop * (1.0f - ctx->hop);

	ctx->multiplier = ctx->idle_multiplier + (ctx->talk_multiplier - ctx->idle_multiplier) * ctx->glow;
	ctx->image_y = height - height * arc;
}

static const char *rp_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("ReactivePNG");
}

static void rp_update(void *data, obs_data_t *settings)
{
	struct reactive_png *ctx = data;

	/* update() fires on every slider move, so decode only on a new path. */
	const char *file = obs_data_get_string(settings, S_FILE);
	if (!ctx->file || strcmp(ctx->file, file) != 0)
		load_image(ctx, file);

	const char *uuid = obs_data_get_string(settings, S_AUDIO);
	if (!ctx->audio_uuid || strcmp(ctx->audio_uuid, uuid) != 0) {
		audio_detach(ctx);
		bfree(ctx->audio_uuid);
		ctx->audio_uuid = bstrdup(uuid);
		ctx->audio_retry_left = 0.0f;
	}

	const float threshold_db = (float)obs_data_get_int(settings, S_THRESHOLD);
	atomic_store_explicit(&ctx->threshold, powf(10.0f, threshold_db / 10.0f), memory_order_relaxed);

	ctx->hold_seconds = (float)obs_data_get_int(settings, S_HOLD) / 1000.0f;
	ctx->idle_multiplier = percent_to_multiplier(obs_data_get_int(settings, S_IDLE));
	ctx->talk_multiplier = percent_to_multiplier(obs_data_get_int(settings, S_TALK));

	/* 1 ms stands in for 0: it finishes within a single frame. */
	ctx->fade_rate = 1000.0f / fmaxf((float)obs_data_get_int(settings, S_FADE), 1.0f);
	ctx->hop_rate = 1000.0f / fmaxf((float)obs_data_get_int(settings, S_BOUNCE_TIME), 1.0f);

	ctx->bounce_px = 0;
	if (obs_data_get_bool(settings, S_BOUNCE)) {
		const float percent = (float)obs_data_get_int(settings, S_BOUNCE_HEIGHT);
		ctx->bounce_px = (uint32_t)lroundf((float)ctx->image.image3.image2.image.cy * percent / 100.0f);
	}
	if (!ctx->bounce_px)
		ctx->hop = 0.0f;

	pose_update(ctx);
}

static void *rp_create(obs_data_t *settings, obs_source_t *source)
{
	struct reactive_png *ctx = bzalloc(sizeof(*ctx));
	ctx->source = source;
	ctx->channels = audio_output_get_channels(obs_get_audio());
	rp_update(ctx, settings);
	return ctx;
}

static void rp_destroy(void *data)
{
	struct reactive_png *ctx = data;

	audio_detach(ctx);

	obs_enter_graphics();
	gs_image_file4_free(&ctx->image);
	obs_leave_graphics();

	bfree(ctx->file);
	bfree(ctx->audio_uuid);
	bfree(ctx);
}

static void rp_tick(void *data, float seconds)
{
	struct reactive_png *ctx = data;

	/* Not in any visible scene: drop the audio tap entirely so a hidden
	 * source adds no work to the audio thread. */
	if (!obs_source_showing(ctx->source)) {
		audio_detach(ctx);
		ctx->audio_retry_left = 0.0f;
		ctx->hold_left = ctx->glow = ctx->hop = 0.0f;
		ctx->was_talking = false;
		pose_update(ctx);
		return;
	}

	audio_maintain(ctx, seconds);

	/* Counting blocks rather than storing a level means a loud block between
	 * two ticks is never missed, and a mic that stops sending audio
	 * altogether simply times out to idle. */
	const unsigned blocks = atomic_load_explicit(&ctx->voice_blocks, memory_order_relaxed);
	const bool voiced = blocks != ctx->voice_seen;
	ctx->voice_seen = blocks;
	ctx->hold_left = voiced ? ctx->hold_seconds : fmaxf(ctx->hold_left - seconds, 0.0f);
	const bool talking = voiced || ctx->hold_left > 0.0f;

	const float fade = ctx->fade_rate * seconds;
	ctx->glow = talking ? fminf(ctx->glow + fade, 1.0f) : fmaxf(ctx->glow - fade, 0.0f);

	/* One hop each time talking starts. Pauses shorter than the hold time
	 * count as the same stretch of talking, so they don't hop again. A hop
	 * that has started always lands, even if speech stops mid-air. */
	const bool started = talking && !ctx->was_talking;
	ctx->was_talking = talking;

	if (ctx->bounce_px && (started || ctx->hop > 0.0f)) {
		ctx->hop += ctx->hop_rate * seconds;
		if (ctx->hop >= 1.0f)
			ctx->hop = 0.0f;
	}

	pose_update(ctx);
}

static void rp_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);

	struct reactive_png *ctx = data;
	struct gs_image_file *image = &ctx->image.image3.image2.image;
	if (!image->texture)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), image->texture);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), ctx->multiplier);

	gs_matrix_push();
	gs_matrix_translate3f(0.0f, ctx->image_y, 0.0f);
	while (gs_effect_loop(effect, "DrawMultiply"))
		gs_draw_sprite(image->texture, 0, image->cx, image->cy);
	gs_matrix_pop();

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

static uint32_t rp_width(void *data)
{
	struct reactive_png *ctx = data;
	return ctx->image.image3.image2.image.cx;
}

static uint32_t rp_height(void *data)
{
	struct reactive_png *ctx = data;
	return ctx->image.image3.image2.image.cy + ctx->bounce_px;
}

static void rp_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_THRESHOLD, -40);
	obs_data_set_default_int(settings, S_HOLD, 200);
	obs_data_set_default_int(settings, S_IDLE, 50);
	obs_data_set_default_int(settings, S_TALK, 100);
	obs_data_set_default_int(settings, S_FADE, 80);
	obs_data_set_default_bool(settings, S_BOUNCE, true);
	obs_data_set_default_int(settings, S_BOUNCE_HEIGHT, 5);
	obs_data_set_default_int(settings, S_BOUNCE_TIME, 250);
}

static bool add_audio_source(void *param, obs_source_t *source)
{
	obs_property_t *list = param;
	if (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)
		obs_property_list_add_string(list, obs_source_get_name(source), obs_source_get_uuid(source));
	return true;
}

static bool bounce_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const bool enabled = obs_data_get_bool(settings, S_BOUNCE);
	obs_property_set_visible(obs_properties_get(props, S_BOUNCE_HEIGHT), enabled);
	obs_property_set_visible(obs_properties_get(props, S_BOUNCE_TIME), enabled);
	return true;
}

static obs_properties_t *rp_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	obs_properties_add_path(props, S_FILE, T_("Image"), OBS_PATH_FILE, "PNG (*.png);;All files (*.*)", NULL);

	p = obs_properties_add_list(props, S_AUDIO, T_("AudioSource"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("AudioSource.None"), "");
	obs_enum_sources(add_audio_source, p);

	p = obs_properties_add_int_slider(props, S_THRESHOLD, T_("Threshold"), -60, 0, 1);
	obs_property_int_set_suffix(p, " dB");

	p = obs_properties_add_int_slider(props, S_HOLD, T_("Hold"), 0, 1000, 10);
	obs_property_int_set_suffix(p, " ms");

	p = obs_properties_add_int_slider(props, S_IDLE, T_("IdleBrightness"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");

	p = obs_properties_add_int_slider(props, S_TALK, T_("TalkBrightness"), 0, 200, 1);
	obs_property_int_set_suffix(p, "%");

	p = obs_properties_add_int_slider(props, S_FADE, T_("Fade"), 0, 500, 10);
	obs_property_int_set_suffix(p, " ms");

	p = obs_properties_add_bool(props, S_BOUNCE, T_("Bounce"));
	obs_property_set_modified_callback(p, bounce_changed);

	p = obs_properties_add_int_slider(props, S_BOUNCE_HEIGHT, T_("BounceHeight"), 1, 25, 1);
	obs_property_int_set_suffix(p, "%");

	p = obs_properties_add_int_slider(props, S_BOUNCE_TIME, T_("BounceTime"), 100, 600, 10);
	obs_property_int_set_suffix(p, " ms");

	return props;
}

static struct obs_source_info reactive_png_info = {
	.id = "reactive_png",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
	.icon_type = OBS_ICON_TYPE_IMAGE,
	.get_name = rp_get_name,
	.create = rp_create,
	.destroy = rp_destroy,
	.update = rp_update,
	.get_defaults = rp_defaults,
	.get_properties = rp_properties,
	.get_width = rp_width,
	.get_height = rp_height,
	.video_tick = rp_tick,
	.video_render = rp_render,
};

MODULE_EXPORT const char *obs_module_description(void)
{
	return "PNG source that lights up and bounces when you talk";
}

bool obs_module_load(void)
{
	obs_register_source(&reactive_png_info);
	return true;
}
