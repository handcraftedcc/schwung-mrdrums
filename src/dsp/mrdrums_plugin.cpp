#include <dirent.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vector>

#include "mrdrums_engine.h"
#include "mrdrums_params.h"

extern "C" {
#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void *(*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;
}

static const host_api_v1_t *g_host = NULL;

static void plugin_log(const char *msg) {
    if (!msg) return;
    if (g_host && g_host->log) {
        char line[256];
        snprintf(line, sizeof(line), "[mrdrums] %s", msg);
        g_host->log(line);
    }
}

static inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

typedef struct {
    float *data;
    int length;
    int sample_rate;
} sample_buffer_t;

typedef struct {
    char module_dir[512];
    char last_error[256];
    int ui_auto_select_pad;
    int ui_current_pad;
    char ui_last_sample_dir[512];

    mrdrums_engine_t engine;
    char pad_sample_paths[MRDRUMS_ENGINE_PAD_COUNT][512];
    sample_buffer_t samples[MRDRUMS_ENGINE_PAD_COUNT];
} mrdrums_instance_t;

static void set_error(mrdrums_instance_t *inst, const char *msg) {
    if (!inst) return;
    if (!msg) {
        inst->last_error[0] = '\0';
        return;
    }
    snprintf(inst->last_error, sizeof(inst->last_error), "%s", msg);
}

static void free_sample(sample_buffer_t *s) {
    if (!s) return;
    free(s->data);
    s->data = NULL;
    s->length = 0;
    s->sample_rate = 0;
}

static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t rd_i24_le(const uint8_t *p) {
    int32_t v = ((int32_t)p[0]) | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
    if (v & 0x00800000) v |= ~0x00FFFFFF;
    return v;
}

static int load_wav_mono(const char *path, sample_buffer_t *out, char *err, int err_len) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(err, err_len, "Could not open sample file: %s", path);
        return -1;
    }

    uint8_t riff[12];
    if (fread(riff, 1, 12, fp) != 12) {
        fclose(fp);
        snprintf(err, err_len, "Invalid WAV header");
        return -1;
    }

    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(fp);
        snprintf(err, err_len, "Not a RIFF/WAVE file");
        return -1;
    }

    int have_fmt = 0;
    int have_data = 0;
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t block_align = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_size = 0;
    long data_offset = 0;

    while (!feof(fp)) {
        uint8_t chdr[8];
        if (fread(chdr, 1, 8, fp) != 8) break;

        uint32_t chunk_size = rd_u32_le(chdr + 4);
        long chunk_data_pos = ftell(fp);

        if (memcmp(chdr, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            uint32_t want = (chunk_size < sizeof(fmt)) ? chunk_size : (uint32_t)sizeof(fmt);
            if (fread(fmt, 1, want, fp) != want || want < 16) {
                fclose(fp);
                snprintf(err, err_len, "Corrupt fmt chunk");
                return -1;
            }
            audio_format = rd_u16_le(fmt + 0);
            channels = rd_u16_le(fmt + 2);
            sample_rate = rd_u32_le(fmt + 4);
            block_align = rd_u16_le(fmt + 12);
            bits_per_sample = rd_u16_le(fmt + 14);
            have_fmt = 1;
        } else if (memcmp(chdr, "data", 4) == 0) {
            data_size = chunk_size;
            data_offset = chunk_data_pos;
            have_data = 1;
        }

        long next = chunk_data_pos + (long)chunk_size + (chunk_size & 1u);
        if (fseek(fp, next, SEEK_SET) != 0) {
            break;
        }
    }

    if (!have_fmt || !have_data) {
        fclose(fp);
        snprintf(err, err_len, "WAV missing fmt/data chunk");
        return -1;
    }

    if (channels < 1 || block_align < 1 || data_size < (uint32_t)block_align) {
        fclose(fp);
        snprintf(err, err_len, "Invalid WAV channel/layout");
        return -1;
    }

    if (!((audio_format == 1 && (bits_per_sample == 8 || bits_per_sample == 16 || bits_per_sample == 24 || bits_per_sample == 32)) ||
          (audio_format == 3 && bits_per_sample == 32))) {
        fclose(fp);
        snprintf(err, err_len, "Unsupported WAV format (need PCM 8/16/24/32 or float32)");
        return -1;
    }

    if (fseek(fp, data_offset, SEEK_SET) != 0) {
        fclose(fp);
        snprintf(err, err_len, "Failed to seek WAV data");
        return -1;
    }

    uint8_t *raw = (uint8_t *)malloc(data_size);
    if (!raw) {
        fclose(fp);
        snprintf(err, err_len, "Out of memory reading WAV");
        return -1;
    }

    if (fread(raw, 1, data_size, fp) != data_size) {
        free(raw);
        fclose(fp);
        snprintf(err, err_len, "Failed to read WAV data");
        return -1;
    }
    fclose(fp);

    int frame_count = (int)(data_size / block_align);
    if (frame_count <= 0) {
        free(raw);
        snprintf(err, err_len, "Empty WAV data");
        return -1;
    }

    out->data = (float *)malloc((size_t)frame_count * sizeof(float));
    if (!out->data) {
        free(raw);
        snprintf(err, err_len, "Out of memory allocating sample data");
        return -1;
    }

    int bytes_per_sample = bits_per_sample / 8;
    for (int i = 0; i < frame_count; i++) {
        float mono = 0.0f;
        const uint8_t *frame_ptr = raw + (size_t)i * block_align;

        for (int ch = 0; ch < channels; ch++) {
            const uint8_t *sp = frame_ptr + ch * bytes_per_sample;
            float v = 0.0f;

            if (audio_format == 1) {
                if (bits_per_sample == 8) {
                    int x = (int)sp[0] - 128;
                    v = (float)x / 128.0f;
                } else if (bits_per_sample == 16) {
                    int16_t x = (int16_t)rd_u16_le(sp);
                    v = (float)x / 32768.0f;
                } else if (bits_per_sample == 24) {
                    int32_t x = rd_i24_le(sp);
                    v = (float)x / 8388608.0f;
                } else {
                    int32_t x = (int32_t)rd_u32_le(sp);
                    v = (float)x / 2147483648.0f;
                }
            } else {
                float x;
                memcpy(&x, sp, sizeof(float));
                v = x;
            }

            mono += v;
        }

        mono /= (float)channels;
        out->data[i] = clampf(mono, -1.0f, 1.0f);
    }

    free(raw);

    out->length = frame_count;
    out->sample_rate = (int)sample_rate;
    return 0;
}

static int has_wav_extension(const char *path) {
    if (!path) return 0;
    int len = (int)strlen(path);
    if (len < 5) return 0;
    const char *ext = path + len - 4;
    return (ext[0] == '.' &&
            (ext[1] == 'w' || ext[1] == 'W') &&
            (ext[2] == 'a' || ext[2] == 'A') &&
            (ext[3] == 'v' || ext[3] == 'V'));
}

static int json_get_number(const char *json, const char *key, float *out) {
    if (!json || !key || !out) return -1;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    char *endp = NULL;
    double v = strtod(p, &endp);
    if (!endp || endp == p) return -1;
    *out = (float)v;
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    if (!json || !key || !out || out_len <= 1) return -1;

    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;

    int n = 0;
    while (*p && *p != '"' && n < out_len - 1) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = '\0';

    return (*p == '"') ? 0 : -1;
}

static int json_escape(const char *src, char *dst, int dst_len) {
    if (!src || !dst || dst_len <= 0) return 0;
    int o = 0;
    for (int i = 0; src[i] && o < dst_len - 1; i++) {
        char c = src[i];
        if ((c == '\\' || c == '"') && o < dst_len - 2) {
            dst[o++] = '\\';
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
    return o;
}

static void clear_pad_sample(mrdrums_instance_t *inst, int pad_index) {
    if (!inst || pad_index < 1 || pad_index > MRDRUMS_ENGINE_PAD_COUNT) return;
    int i = pad_index - 1;
    free_sample(&inst->samples[i]);
    mrdrums_engine_set_pad_sample(&inst->engine, pad_index, NULL, 0, MRDRUMS_ENGINE_SAMPLE_RATE);
}

static int set_pad_sample_path(mrdrums_instance_t *inst, int pad_index, const char *path) {
    if (!inst || pad_index < 1 || pad_index > MRDRUMS_ENGINE_PAD_COUNT || !path) return -1;
    int i = pad_index - 1;

    snprintf(inst->pad_sample_paths[i], sizeof(inst->pad_sample_paths[i]), "%s", path);

    if (!path[0]) {
        clear_pad_sample(inst, pad_index);
        set_error(inst, NULL);
        return 0;
    }

    if (has_wav_extension(path)) {
        snprintf(inst->ui_last_sample_dir, sizeof(inst->ui_last_sample_dir), "%s", path);
    }

    if (!has_wav_extension(path)) {
        clear_pad_sample(inst, pad_index);
        set_error(inst, "Selected file must be .wav");
        return -1;
    }

    char resolved[1024];
    if (path[0] == '/') {
        snprintf(resolved, sizeof(resolved), "%s", path);
    } else {
        snprintf(resolved, sizeof(resolved), "%s/%s", inst->module_dir, path);
    }

    sample_buffer_t loaded;
    char err[256];
    if (load_wav_mono(resolved, &loaded, err, sizeof(err)) != 0) {
        clear_pad_sample(inst, pad_index);
        set_error(inst, err);
        return -1;
    }

    free_sample(&inst->samples[i]);
    inst->samples[i] = loaded;
    mrdrums_engine_set_pad_sample(&inst->engine,
                                  pad_index,
                                  inst->samples[i].data,
                                  inst->samples[i].length,
                                  inst->samples[i].sample_rate);
    set_error(inst, NULL);
    return 0;
}

static int parse_mode_value(const char *val) {
    if (!val) return MRDRUMS_PAD_MODE_ONESHOT;
    if (strcasecmp(val, "gate") == 0 || strcmp(val, "0") == 0) return MRDRUMS_PAD_MODE_GATE;
    return MRDRUMS_PAD_MODE_ONESHOT;
}

static const char *mode_to_string(int mode) {
    return (mode == MRDRUMS_PAD_MODE_GATE) ? "gate" : "oneshot";
}

static int parse_on_off(const char *val) {
    if (!val) return 0;
    if (strcasecmp(val, "on") == 0 || strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0) return 1;
    if (strcasecmp(val, "off") == 0 || strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0) return 0;
    return atoi(val) != 0 ? 1 : 0;
}

static const char *on_off_to_string(int value) {
    return value ? "on" : "off";
}

static int parse_vel_curve(const char *val) {
    if (!val) return 0;
    if (strcasecmp(val, "soft") == 0 || strcmp(val, "1") == 0) return 1;
    if (strcasecmp(val, "hard") == 0 || strcmp(val, "2") == 0) return 2;
    return 0;
}

static const char *vel_curve_to_string(int vel_curve) {
    switch (vel_curve) {
        case 1: return "soft";
        case 2: return "hard";
        default: return "linear";
    }
}

typedef struct {
    const char *alias_key;
    const char *suffix;
} current_pad_alias_t;

static const current_pad_alias_t kCurrentPadAliases[] = {
    {"pad_sample_path", "sample_path"},
    {"pad_vol", "vol"},
    {"pad_pan", "pan"},
    {"pad_tune", "tune"},
    {"pad_start", "start"},
    {"pad_attack_ms", "attack_ms"},
    {"pad_decay_ms", "decay_ms"},
    {"pad_choke_group", "choke_group"},
    {"pad_mode", "mode"},
    {"pad_rand_pan_amt", "rand_pan_amt"},
    {"pad_rand_vol_amt", "rand_vol_amt"},
    {"pad_rand_decay_amt", "rand_decay_amt"},
    {"pad_chance_pct", "chance_pct"},
};

static const char *resolve_current_pad_alias_suffix(const char *key) {
    if (!key) return NULL;
    for (size_t i = 0; i < sizeof(kCurrentPadAliases) / sizeof(kCurrentPadAliases[0]); i++) {
        if (strcmp(key, kCurrentPadAliases[i].alias_key) == 0) {
            return kCurrentPadAliases[i].suffix;
        }
    }
    return NULL;
}

static int ui_note_to_pad(int note) {
    /* Primary mapping from requirements */
    int pad = mrdrums_engine_note_to_pad(note);
    if (pad > 0) return pad;

    /* Internal Move pad range fallback */
    if (note >= 68 && note <= 83) {
        return note - 68 + 1;
    }

    return 0;
}

static int set_param_value(mrdrums_instance_t *inst, const char *key, const char *val) {
    if (!inst || !key || !val) return 0;

    if (strcmp(key, "g_master_vol") == 0) {
        mrdrums_engine_set_master_vol(&inst->engine, (float)atof(val));
        return 1;
    }
    if (strcmp(key, "g_polyphony") == 0) {
        mrdrums_engine_set_polyphony(&inst->engine, atoi(val));
        return 1;
    }
    if (strcmp(key, "g_vel_curve") == 0) {
        inst->engine.vel_curve = clampi(parse_vel_curve(val), 0, 2);
        return 1;
    }
    if (strcmp(key, "g_humanize_ms") == 0) {
        inst->engine.humanize_ms = clampf((float)atof(val), 0.0f, 50.0f);
        return 1;
    }
    if (strcmp(key, "g_rand_seed") == 0) {
        unsigned long s = strtoul(val, NULL, 10);
        inst->engine.rand_seed = (uint32_t)s;
        if (!inst->engine.rand_seed) inst->engine.rand_seed = 1u;
        inst->engine.rng_state = inst->engine.rand_seed;
        return 1;
    }
    if (strcmp(key, "g_rand_loop_steps") == 0) {
        inst->engine.rand_loop_steps = clampi(atoi(val), 1, 128);
        return 1;
    }
    if (strcmp(key, "ui_auto_select_pad") == 0) {
        inst->ui_auto_select_pad = parse_on_off(val);
        return 1;
    }
    if (strcmp(key, "ui_current_pad") == 0) {
        inst->ui_current_pad = clampi(atoi(val), 1, 16);
        return 1;
    }
    if (strcmp(key, "ui_last_sample_dir") == 0) {
        snprintf(inst->ui_last_sample_dir, sizeof(inst->ui_last_sample_dir), "%s", val);
        return 1;
    }

    const char *alias_suffix = resolve_current_pad_alias_suffix(key);
    if (alias_suffix) {
        char target_key[64];
        if (!mrdrums_make_pad_key(clampi(inst->ui_current_pad, 1, 16), alias_suffix, target_key, sizeof(target_key))) {
            return 0;
        }
        return set_param_value(inst, target_key, val);
    }

    const mrdrums_param_desc_t *pad = mrdrums_find_pad_param(key);
    if (!pad) return 0;

    int pad_index = pad->pad_index;
    if (strcmp(pad->suffix, "sample_path") == 0) {
        set_pad_sample_path(inst, pad_index, val);
        return 1;
    }
    if (strcmp(pad->suffix, "vol") == 0) {
        mrdrums_engine_set_pad_vol(&inst->engine, pad_index, (float)atof(val));
        return 1;
    }
    if (strcmp(pad->suffix, "pan") == 0) {
        mrdrums_engine_set_pad_pan(&inst->engine, pad_index, (float)atof(val));
        return 1;
    }
    if (strcmp(pad->suffix, "tune") == 0) {
        mrdrums_engine_set_pad_tune(&inst->engine, pad_index, (float)atof(val));
        return 1;
    }
    if (strcmp(pad->suffix, "start") == 0) {
        mrdrums_engine_set_pad_start(&inst->engine, pad_index, (float)atof(val));
        return 1;
    }
    if (strcmp(pad->suffix, "attack_ms") == 0) {
        mrdrums_engine_set_pad_attack_ms(&inst->engine, pad_index, (float)atof(val));
        return 1;
    }
    if (strcmp(pad->suffix, "decay_ms") == 0) {
        mrdrums_engine_set_pad_decay_ms(&inst->engine, pad_index, (float)atof(val));
        return 1;
    }
    if (strcmp(pad->suffix, "choke_group") == 0) {
        mrdrums_engine_set_pad_choke_group(&inst->engine, pad_index, atoi(val));
        return 1;
    }
    if (strcmp(pad->suffix, "mode") == 0) {
        mrdrums_engine_set_pad_mode(&inst->engine, pad_index, parse_mode_value(val));
        return 1;
    }
    if (strcmp(pad->suffix, "rand_pan_amt") == 0) {
        mrdrums_engine_set_pad_rand_pan_amt(&inst->engine, pad_index, (float)atof(val));
        return 1;
    }
    if (strcmp(pad->suffix, "rand_vol_amt") == 0) {
        mrdrums_engine_set_pad_rand_vol_amt(&inst->engine, pad_index, (float)atof(val));
        return 1;
    }
    if (strcmp(pad->suffix, "rand_decay_amt") == 0) {
        mrdrums_engine_set_pad_rand_decay_amt(&inst->engine, pad_index, (float)atof(val));
        return 1;
    }
    if (strcmp(pad->suffix, "chance_pct") == 0) {
        mrdrums_engine_set_pad_chance_pct(&inst->engine, pad_index, (float)atof(val));
        return 1;
    }

    return 0;
}

static int get_param_value(mrdrums_instance_t *inst, const char *key, char *buf, int buf_len) {
    if (!inst || !key || !buf || buf_len <= 0) return -1;

    if (strcmp(key, "g_master_vol") == 0) return snprintf(buf, buf_len, "%.4f", inst->engine.master_vol);
    if (strcmp(key, "g_polyphony") == 0) return snprintf(buf, buf_len, "%d", inst->engine.polyphony);
    if (strcmp(key, "g_vel_curve") == 0) return snprintf(buf, buf_len, "%s", vel_curve_to_string(inst->engine.vel_curve));
    if (strcmp(key, "g_humanize_ms") == 0) return snprintf(buf, buf_len, "%.4f", inst->engine.humanize_ms);
    if (strcmp(key, "g_rand_seed") == 0) return snprintf(buf, buf_len, "%u", inst->engine.rand_seed);
    if (strcmp(key, "g_rand_loop_steps") == 0) return snprintf(buf, buf_len, "%d", inst->engine.rand_loop_steps);
    if (strcmp(key, "ui_auto_select_pad") == 0) return snprintf(buf, buf_len, "%s", on_off_to_string(inst->ui_auto_select_pad));
    if (strcmp(key, "ui_current_pad") == 0) return snprintf(buf, buf_len, "%d", inst->ui_current_pad);
    if (strcmp(key, "ui_last_sample_dir") == 0) return snprintf(buf, buf_len, "%s", inst->ui_last_sample_dir);

    const char *alias_suffix = resolve_current_pad_alias_suffix(key);
    if (alias_suffix) {
        char target_key[64];
        if (!mrdrums_make_pad_key(clampi(inst->ui_current_pad, 1, 16), alias_suffix, target_key, sizeof(target_key))) {
            return -1;
        }
        return get_param_value(inst, target_key, buf, buf_len);
    }

    const mrdrums_param_desc_t *pad = mrdrums_find_pad_param(key);
    if (!pad) return -1;
    const mrdrums_pad_t *p = &inst->engine.pads[pad->pad_index - 1];

    if (strcmp(pad->suffix, "sample_path") == 0) {
        return snprintf(buf, buf_len, "%s", inst->pad_sample_paths[pad->pad_index - 1]);
    }
    if (strcmp(pad->suffix, "vol") == 0) return snprintf(buf, buf_len, "%.4f", p->vol);
    if (strcmp(pad->suffix, "pan") == 0) return snprintf(buf, buf_len, "%.4f", p->pan);
    if (strcmp(pad->suffix, "tune") == 0) return snprintf(buf, buf_len, "%.4f", p->tune);
    if (strcmp(pad->suffix, "start") == 0) return snprintf(buf, buf_len, "%.4f", p->start);
    if (strcmp(pad->suffix, "attack_ms") == 0) return snprintf(buf, buf_len, "%.4f", p->attack_ms);
    if (strcmp(pad->suffix, "decay_ms") == 0) return snprintf(buf, buf_len, "%.4f", p->decay_ms);
    if (strcmp(pad->suffix, "choke_group") == 0) return snprintf(buf, buf_len, "%d", p->choke_group);
    if (strcmp(pad->suffix, "mode") == 0) return snprintf(buf, buf_len, "%s", mode_to_string(p->mode));
    if (strcmp(pad->suffix, "rand_pan_amt") == 0) return snprintf(buf, buf_len, "%.4f", p->rand_pan_amt);
    if (strcmp(pad->suffix, "rand_vol_amt") == 0) return snprintf(buf, buf_len, "%.4f", p->rand_vol_amt);
    if (strcmp(pad->suffix, "rand_decay_amt") == 0) return snprintf(buf, buf_len, "%.4f", p->rand_decay_amt);
    if (strcmp(pad->suffix, "chance_pct") == 0) return snprintf(buf, buf_len, "%.4f", p->chance_pct);

    return -1;
}

static void apply_defaults(mrdrums_instance_t *inst) {
    if (!inst) return;

    int global_count = 0;
    const mrdrums_param_desc_t *globals = mrdrums_global_params(&global_count);
    for (int i = 0; i < global_count; i++) {
        char num_buf[64];
        const char *value = globals[i].default_str;
        if (!value) {
            snprintf(num_buf, sizeof(num_buf), "%.6g", globals[i].default_num);
            value = num_buf;
        }
        set_param_value(inst, globals[i].key, value);
    }

    int field_count = 0;
    const mrdrums_pad_field_desc_t *fields = mrdrums_pad_fields(&field_count);
    for (int pad = 1; pad <= MRDRUMS_ENGINE_PAD_COUNT; pad++) {
        for (int fi = 0; fi < field_count; fi++) {
            char key[64];
            if (!mrdrums_make_pad_key(pad, fields[fi].suffix, key, sizeof(key))) continue;

            char num_buf[64];
            const char *value = fields[fi].default_str;
            if (!value) {
                snprintf(num_buf, sizeof(num_buf), "%.6g", fields[fi].default_num);
                value = num_buf;
            }
            set_param_value(inst, key, value);
        }
    }

    inst->ui_current_pad = clampi(inst->ui_current_pad, 1, 16);
    inst->ui_auto_select_pad = clampi(inst->ui_auto_select_pad, 0, 1);
    inst->ui_last_sample_dir[0] = '\0';
}

static void apply_state_json(mrdrums_instance_t *inst, const char *json) {
    if (!inst || !json || !json[0]) return;

    int global_count = 0;
    const mrdrums_param_desc_t *globals = mrdrums_global_params(&global_count);
    for (int i = 0; i < global_count; i++) {
        if (strcmp(globals[i].type, "enum") == 0) {
            char str_value[512];
            if (json_get_string(json, globals[i].key, str_value, sizeof(str_value)) == 0) {
                set_param_value(inst, globals[i].key, str_value);
            } else {
                float num_value;
                if (json_get_number(json, globals[i].key, &num_value) == 0) {
                    char num_buf[64];
                    snprintf(num_buf, sizeof(num_buf), "%.6g", num_value);
                    set_param_value(inst, globals[i].key, num_buf);
                }
            }
        } else if (strcmp(globals[i].type, "filepath") == 0) {
            char str_value[512];
            if (json_get_string(json, globals[i].key, str_value, sizeof(str_value)) == 0) {
                set_param_value(inst, globals[i].key, str_value);
            }
        } else {
            float num_value;
            if (json_get_number(json, globals[i].key, &num_value) == 0) {
                char num_buf[64];
                snprintf(num_buf, sizeof(num_buf), "%.6g", num_value);
                set_param_value(inst, globals[i].key, num_buf);
            }
        }
    }

    int field_count = 0;
    const mrdrums_pad_field_desc_t *fields = mrdrums_pad_fields(&field_count);
    for (int pad = 1; pad <= MRDRUMS_ENGINE_PAD_COUNT; pad++) {
        for (int fi = 0; fi < field_count; fi++) {
            char key[64];
            if (!mrdrums_make_pad_key(pad, fields[fi].suffix, key, sizeof(key))) continue;

            if (strcmp(fields[fi].type, "enum") == 0) {
                char str_value[512];
                if (json_get_string(json, key, str_value, sizeof(str_value)) == 0) {
                    set_param_value(inst, key, str_value);
                } else {
                    float num_value;
                    if (json_get_number(json, key, &num_value) == 0) {
                        char num_buf[64];
                        snprintf(num_buf, sizeof(num_buf), "%.6g", num_value);
                        set_param_value(inst, key, num_buf);
                    }
                }
            } else if (strcmp(fields[fi].type, "filepath") == 0) {
                char str_value[512];
                if (json_get_string(json, key, str_value, sizeof(str_value)) == 0) {
                    set_param_value(inst, key, str_value);
                }
            } else {
                float num_value;
                if (json_get_number(json, key, &num_value) == 0) {
                    char num_buf[64];
                    snprintf(num_buf, sizeof(num_buf), "%.6g", num_value);
                    set_param_value(inst, key, num_buf);
                }
            }
        }
    }

    char last_dir[512];
    if (json_get_string(json, "ui_last_sample_dir", last_dir, sizeof(last_dir)) == 0) {
        set_param_value(inst, "ui_last_sample_dir", last_dir);
    }
}

static void *v2_create_instance(const char *module_dir, const char *json_defaults) {
    mrdrums_instance_t *inst = (mrdrums_instance_t *)calloc(1, sizeof(mrdrums_instance_t));
    if (!inst) return NULL;

    snprintf(inst->module_dir, sizeof(inst->module_dir), "%s", module_dir ? module_dir : "");
    mrdrums_engine_init(&inst->engine);
    inst->ui_auto_select_pad = 1;
    inst->ui_current_pad = 1;
    set_error(inst, NULL);

    apply_defaults(inst);
    apply_state_json(inst, json_defaults);

    plugin_log("mrdrums instance created");
    return inst;
}

static void v2_destroy_instance(void *instance) {
    mrdrums_instance_t *inst = (mrdrums_instance_t *)instance;
    if (!inst) return;

    for (int i = 0; i < MRDRUMS_ENGINE_PAD_COUNT; i++) {
        free_sample(&inst->samples[i]);
    }

    free(inst);
    plugin_log("mrdrums instance destroyed");
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    mrdrums_instance_t *inst = (mrdrums_instance_t *)instance;
    if (!inst || !msg || len < 1) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t data1 = (len > 1) ? msg[1] : 0;
    uint8_t data2 = (len > 2) ? msg[2] : 0;

    if (status == 0x90) {
        if (data2 > 0) {
            int ui_pad = ui_note_to_pad((int)data1);
            if (inst->ui_auto_select_pad && ui_pad > 0) {
                inst->ui_current_pad = ui_pad;
            }
            mrdrums_engine_note_on(&inst->engine, (int)data1, (int)data2);
        } else {
            mrdrums_engine_note_off(&inst->engine, (int)data1);
        }
    } else if (status == 0x80) {
        mrdrums_engine_note_off(&inst->engine, (int)data1);
    } else if (status == 0xB0) {
        if (data1 == 123 || data1 == 120) {
            mrdrums_engine_all_notes_off(&inst->engine);
        }
    }
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    mrdrums_instance_t *inst = (mrdrums_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "state") == 0) {
        apply_state_json(inst, val);
        return;
    }

    if (strcmp(key, "all_notes_off") == 0) {
        mrdrums_engine_all_notes_off(&inst->engine);
        return;
    }

    set_param_value(inst, key, val);
}

static int append_state_key_value(mrdrums_instance_t *inst,
                                  char *buf,
                                  int buf_len,
                                  int *offset,
                                  const char *key,
                                  const char *type,
                                  int *is_first) {
    if (!inst || !buf || !offset || !key || !type || !is_first) return -1;

    char value[1024];
    if (get_param_value(inst, key, value, sizeof(value)) < 0) return -1;

    int off = *offset;
    if (!*is_first) {
        off += snprintf(buf + off, buf_len - off, ",");
    }

    if (strcmp(type, "enum") == 0 || strcmp(type, "filepath") == 0) {
        char escaped[1200];
        json_escape(value, escaped, sizeof(escaped));
        off += snprintf(buf + off, buf_len - off, "\"%s\":\"%s\"", key, escaped);
    } else if (strcmp(type, "int") == 0) {
        off += snprintf(buf + off, buf_len - off, "\"%s\":%d", key, atoi(value));
    } else {
        off += snprintf(buf + off, buf_len - off, "\"%s\":%.6g", key, atof(value));
    }

    *offset = off;
    *is_first = 0;
    return 0;
}

static int build_state_json(mrdrums_instance_t *inst, char *buf, int buf_len) {
    if (!inst || !buf || buf_len <= 2) return -1;

    int offset = 0;
    int is_first = 1;
    offset += snprintf(buf + offset, buf_len - offset, "{");

    int global_count = 0;
    const mrdrums_param_desc_t *globals = mrdrums_global_params(&global_count);
    for (int i = 0; i < global_count; i++) {
        append_state_key_value(inst, buf, buf_len, &offset, globals[i].key, globals[i].type, &is_first);
    }

    append_state_key_value(inst, buf, buf_len, &offset, "ui_last_sample_dir", "filepath", &is_first);

    int field_count = 0;
    const mrdrums_pad_field_desc_t *fields = mrdrums_pad_fields(&field_count);
    for (int pad = 1; pad <= MRDRUMS_ENGINE_PAD_COUNT; pad++) {
        for (int fi = 0; fi < field_count; fi++) {
            char key[64];
            if (!mrdrums_make_pad_key(pad, fields[fi].suffix, key, sizeof(key))) continue;
            append_state_key_value(inst, buf, buf_len, &offset, key, fields[fi].type, &is_first);
        }
    }

    offset += snprintf(buf + offset, buf_len - offset, "}");
    return (offset < buf_len) ? offset : -1;
}

static int build_chain_params_json(mrdrums_instance_t *inst, char *buf, int buf_len) {
    if (!buf || buf_len <= 2) return -1;

    int offset = 0;
    int is_first = 1;
    offset += snprintf(buf + offset, buf_len - offset, "[");

    int global_count = 0;
    const mrdrums_param_desc_t *globals = mrdrums_global_params(&global_count);
    for (int i = 0; i < global_count; i++) {
        const mrdrums_param_desc_t *g = &globals[i];
        if (!is_first) offset += snprintf(buf + offset, buf_len - offset, ",");
        is_first = 0;

        if (strcmp(g->type, "enum") == 0) {
            offset += snprintf(buf + offset, buf_len - offset,
                               "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"enum\",\"options\":%s,\"default\":\"%s\"}",
                               g->key,
                               g->name,
                               g->options_json ? g->options_json : "[]",
                               g->default_str ? g->default_str : "");
        } else {
            const char *type = strcmp(g->type, "int") == 0 ? "int" : "float";
            offset += snprintf(buf + offset, buf_len - offset,
                               "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g,\"step\":%g}",
                               g->key,
                               g->name,
                               type,
                               g->min_val,
                               g->max_val,
                               g->step > 0.0f ? g->step : 1.0f);
        }
    }

    int field_count = 0;
    const mrdrums_pad_field_desc_t *fields = mrdrums_pad_fields(&field_count);
    for (int fi = 0; fi < field_count; fi++) {
        const mrdrums_pad_field_desc_t *f = &fields[fi];
        char key[64];
        snprintf(key, sizeof(key), "pad_%s", f->suffix);

        if (!is_first) offset += snprintf(buf + offset, buf_len - offset, ",");
        is_first = 0;

        if (strcmp(f->type, "filepath") == 0) {
            const char *effective_start_path = (inst && inst->ui_last_sample_dir[0])
                ? inst->ui_last_sample_dir
                : (f->start_path ? f->start_path : "/data/UserData/UserLibrary/Samples");
            if (effective_start_path && effective_start_path[0]) {
                offset += snprintf(buf + offset, buf_len - offset,
                                   "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"filepath\",\"root\":\"%s\",\"start_path\":\"%s\",\"filter\":\"%s\",\"live_preview\":true,\"browser_hooks\":{\"on_open\":[{\"key\":\"ui_auto_select_pad\",\"value\":\"off\",\"restore\":true}]}}",
                                   key,
                                   f->name,
                                   f->root ? f->root : "/data/UserData/UserLibrary/Samples",
                                   effective_start_path,
                                   f->filter ? f->filter : ".wav");
            } else {
                offset += snprintf(buf + offset, buf_len - offset,
                                   "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"filepath\",\"root\":\"%s\",\"filter\":\"%s\",\"live_preview\":true,\"browser_hooks\":{\"on_open\":[{\"key\":\"ui_auto_select_pad\",\"value\":\"off\",\"restore\":true}]}}",
                                   key,
                                   f->name,
                                   f->root ? f->root : "/data/UserData/UserLibrary/Samples",
                                   f->filter ? f->filter : ".wav");
            }
        } else if (strcmp(f->type, "enum") == 0) {
            offset += snprintf(buf + offset, buf_len - offset,
                               "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"enum\",\"options\":%s,\"default\":\"%s\"}",
                               key,
                               f->name,
                               f->options_json ? f->options_json : "[]",
                               f->default_str ? f->default_str : "");
        } else {
            const char *type = strcmp(f->type, "int") == 0 ? "int" : "float";
            offset += snprintf(buf + offset, buf_len - offset,
                               "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g,\"step\":%g}",
                               key,
                               f->name,
                               type,
                               f->min_val,
                               f->max_val,
                               f->step > 0.0f ? f->step : 1.0f);
        }
    }

    for (int pad = 1; pad <= MRDRUMS_ENGINE_PAD_COUNT; pad++) {
        for (int fi = 0; fi < field_count; fi++) {
            const mrdrums_pad_field_desc_t *f = &fields[fi];
            char key[64];
            if (!mrdrums_make_pad_key(pad, f->suffix, key, sizeof(key))) continue;

            char name[96];
            snprintf(name, sizeof(name), "P%02d %s", pad, f->name);

            if (!is_first) offset += snprintf(buf + offset, buf_len - offset, ",");
            is_first = 0;

            if (strcmp(f->type, "filepath") == 0) {
                const char *effective_start_path = (inst && inst->ui_last_sample_dir[0])
                    ? inst->ui_last_sample_dir
                    : (f->start_path ? f->start_path : "/data/UserData/UserLibrary/Samples");
                if (effective_start_path && effective_start_path[0]) {
                    offset += snprintf(buf + offset, buf_len - offset,
                                       "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"filepath\",\"root\":\"%s\",\"start_path\":\"%s\",\"filter\":\"%s\",\"live_preview\":true,\"browser_hooks\":{\"on_open\":[{\"key\":\"ui_auto_select_pad\",\"value\":\"off\",\"restore\":true}]}}",
                                       key,
                                       name,
                                       f->root ? f->root : "/data/UserData/UserLibrary/Samples",
                                       effective_start_path,
                                       f->filter ? f->filter : ".wav");
                } else {
                    offset += snprintf(buf + offset, buf_len - offset,
                                       "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"filepath\",\"root\":\"%s\",\"filter\":\"%s\",\"live_preview\":true,\"browser_hooks\":{\"on_open\":[{\"key\":\"ui_auto_select_pad\",\"value\":\"off\",\"restore\":true}]}}",
                                       key,
                                       name,
                                       f->root ? f->root : "/data/UserData/UserLibrary/Samples",
                                       f->filter ? f->filter : ".wav");
                }
            } else if (strcmp(f->type, "enum") == 0) {
                offset += snprintf(buf + offset, buf_len - offset,
                                   "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"enum\",\"options\":%s,\"default\":\"%s\"}",
                                   key,
                                   name,
                                   f->options_json ? f->options_json : "[]",
                                   f->default_str ? f->default_str : "");
            } else {
                const char *type = strcmp(f->type, "int") == 0 ? "int" : "float";
                offset += snprintf(buf + offset, buf_len - offset,
                                   "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g,\"step\":%g}",
                                   key,
                                   name,
                                   type,
                                   f->min_val,
                                   f->max_val,
                                   f->step > 0.0f ? f->step : 1.0f);
            }
        }
    }

    offset += snprintf(buf + offset, buf_len - offset, "]");
    return (offset < buf_len) ? offset : -1;
}

static int build_ui_hierarchy(mrdrums_instance_t *inst, char *buf, int buf_len) {
    if (!buf || buf_len <= 0) return -1;

    (void)inst;

    int n = snprintf(
        buf,
        (size_t)buf_len,
        "{"
            "\"levels\":{"
                "\"root\":{"
                    "\"name\":\"MrDrums\","
                    "\"params\":["
                        "{\"label\":\"Global\",\"level\":\"global\"},"
                        "{\"label\":\"Pad Settings\",\"level\":\"pad_settings\"}"
                    "],"
                    "\"knobs\":[\"ui_auto_select_pad\",\"pad_vol\",\"pad_pan\",\"pad_tune\",\"pad_start\",\"pad_attack_ms\",\"pad_decay_ms\",\"pad_choke_group\",\"pad_mode\"]"
                "},"
                "\"global\":{"
                    "\"name\":\"Global\","
                    "\"params\":[\"g_master_vol\",\"g_polyphony\",\"g_vel_curve\",\"g_humanize_ms\",\"g_rand_seed\",\"g_rand_loop_steps\"],"
                    "\"knobs\":[\"g_master_vol\",\"g_polyphony\",\"g_vel_curve\",\"g_humanize_ms\",\"g_rand_seed\",\"g_rand_loop_steps\"]"
                "},"
                "\"pad_settings\":{"
                    "\"name\":\"Pad Settings\","
                    "\"params\":[\"ui_auto_select_pad\",\"ui_current_pad\",\"pad_sample_path\",\"pad_vol\",\"pad_pan\",\"pad_tune\",\"pad_start\",\"pad_attack_ms\",\"pad_decay_ms\",\"pad_choke_group\",\"pad_mode\",\"pad_rand_pan_amt\",\"pad_rand_vol_amt\",\"pad_rand_decay_amt\",\"pad_chance_pct\"],"
                    "\"knobs\":[\"ui_auto_select_pad\",\"pad_vol\",\"pad_pan\",\"pad_tune\",\"pad_start\",\"pad_attack_ms\",\"pad_decay_ms\",\"pad_choke_group\",\"pad_mode\"]"
                "}"
            "}"
        "}"
    );

    return (n >= 0 && n < buf_len) ? n : -1;
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    mrdrums_instance_t *inst = (mrdrums_instance_t *)instance;
    if (!inst || !key || !buf || buf_len <= 0) return -1;

    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "MrDrums");
    if (strcmp(key, "state") == 0) return build_state_json(inst, buf, buf_len);
    if (strcmp(key, "chain_params") == 0) return build_chain_params_json(inst, buf, buf_len);
    if (strcmp(key, "ui_hierarchy") == 0) return build_ui_hierarchy(inst, buf, buf_len);

    return get_param_value(inst, key, buf, buf_len);
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    mrdrums_instance_t *inst = (mrdrums_instance_t *)instance;
    if (!inst || !buf || buf_len <= 0) return -1;
    if (!inst->last_error[0]) return 0;
    return snprintf(buf, buf_len, "%s", inst->last_error);
}

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    mrdrums_instance_t *inst = (mrdrums_instance_t *)instance;
    if (!out_interleaved_lr || frames <= 0) return;

    if (!inst) {
        memset(out_interleaved_lr, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }

    std::vector<float> left((size_t)frames, 0.0f);
    std::vector<float> right((size_t)frames, 0.0f);

    mrdrums_engine_render(&inst->engine, left.data(), right.data(), frames);

    for (int i = 0; i < frames; i++) {
        float l = left[(size_t)i];
        float r = right[(size_t)i];

        if (l > 0.95f || l < -0.95f) l = tanhf(l);
        if (r > 0.95f || r < -0.95f) r = tanhf(r);

        int32_t sl = (int32_t)(l * 32767.0f);
        int32_t sr = (int32_t)(r * 32767.0f);
        if (sl > 32767) sl = 32767;
        if (sl < -32768) sl = -32768;
        if (sr > 32767) sr = 32767;
        if (sr < -32768) sr = -32768;

        out_interleaved_lr[i * 2] = (int16_t)sl;
        out_interleaved_lr[i * 2 + 1] = (int16_t)sr;
    }
}

static plugin_api_v2_t g_api;

extern "C" plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_api, 0, sizeof(g_api));
    g_api.api_version = MOVE_PLUGIN_API_VERSION_2;
    g_api.create_instance = v2_create_instance;
    g_api.destroy_instance = v2_destroy_instance;
    g_api.on_midi = v2_on_midi;
    g_api.set_param = v2_set_param;
    g_api.get_param = v2_get_param;
    g_api.get_error = v2_get_error;
    g_api.render_block = v2_render_block;

    plugin_log("mrdrums plugin initialized");
    return &g_api;
}
