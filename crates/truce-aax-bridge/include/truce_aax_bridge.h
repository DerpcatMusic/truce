/**
 * truce AAX bridge - C ABI between the AAX template and the Rust plugin.
 *
 * The AAX template (C++) dlopen()s the Rust cdylib and resolves these
 * symbols to delegate all plugin logic to the Rust side.
 */

#ifndef TRUCE_AAX_BRIDGE_H
#define TRUCE_AAX_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped any time the bridge ABI shape changes. The bridge loader
 * compares this against the cdylib's `truce_aax_abi_version()` and
 * refuses to load a mismatched pair - protects against a manual
 * cdylib swap against a stale C++ template (which would otherwise
 * read fields at the wrong offset).
 *
 * Version history:
 *   1 → 2: initial range_type field on TruceAaxParamInfo (log/discrete).
 *   2 → 3: SysEx I/O - push_sysex_input + output_sysex_count +
 *           output_sysex_at exports.
 *   3 → 4: legacy-state migration - legacy_chunk_ids on the
 *           descriptor + load_state_foreign export.
 *   4 → 5: offline-bounce awareness - set_render_mode export
 *           (host EnteringOfflineMode / ExitingOfflineMode).
 *   5 → 6: latency reporting - latency export (template pushes it
 *           via AAX_IController::SetSignalLatency).
 *   6 → 7: multiple bus layouts - layout_in_channels / layout_out_channels
 *           / num_layouts on the descriptor (one AAX stem-format config
 *           per bus_layouts() entry).
 *   7 → 8: sidechain input - sidechain_in_channels on the descriptor; the
 *           template registers an AAX mono side-chain port (AddSideChainIn)
 *           and appends it after the main input channels.
 *   8 → 9: parameter text entry - truce_aax_parse_param, routing the
 *           display delegate's StringToValue to the plugin's parse_value.
 *   9 → 10: custom taper - truce_aax_normalize / truce_aax_denormalize
 *           route a skewed param's coefficient<->plain mapping through the
 *           plugin's ParamRange (AAX has no native skew taper).
 *   10 → 11: one strict native MIDI 1.0 / SysEx event lane with explicit
 *            loss status and a sequential output cursor.
 *   11 → 12: completed output-event delivery status.
 *   12 → 13: process-time audio-bus activation masks. */
#define TRUCE_AAX_ABI_VERSION 13u

/* Capacity of TruceAaxDescriptor::legacy_chunk_ids. */
#define TRUCE_AAX_MAX_LEGACY_CHUNKS 8u

/* Wire values for TruceAaxParamInfo::range_type. The shim picks the
 * matching AAX_ITaperDelegate per param so AAX's normalize/denormalize
 * mirrors what truce-params does on the Rust side - a mismatched taper
 * (e.g. AAX-linear over a log-ranged knob) round-trips editor writes
 * back through RenderAudio as a different plain value, which the GUI
 * sees as the knob fighting the user mid-drag. */
#define TRUCE_AAX_RANGE_LINEAR   0u
#define TRUCE_AAX_RANGE_LOG      1u
#define TRUCE_AAX_RANGE_DISCRETE 2u
/* Opaque taper: the shim uses a custom AAX_ITaperDelegate that routes
 * normalize/denormalize through truce_aax_normalize / _denormalize, for
 * skewed shapes AAX has no native taper for. */
#define TRUCE_AAX_RANGE_CUSTOM   3u

/* Plugin descriptor - read once at load time. */
typedef struct {
    const char* name;           /* Display name */
    const char* vendor;         /* Vendor name */
    uint32_t version;           /* Version as integer */
    uint32_t num_inputs;        /* 0 for instruments */
    uint32_t num_outputs;       /* Typically 2 (stereo) */
    uint32_t num_params;        /* Parameter count */
    int32_t manufacturer_id;    /* FourCC */
    int32_t product_id;         /* FourCC */
    int32_t plugin_id;          /* FourCC (unique per stem format) */
    int wants_input_midi;       /* 1 if the plugin accepts MIDI input -
                                 * gates the LocalInput MIDI node and the
                                 * per-render MIDI event collection. */
    int emits_midi;             /* 1 if the plugin emits MIDI to the host
                                 * - gates the LocalOutput MIDI node. */
    uint32_t category;          /* AAX_ePlugInCategory bitmask */
    int has_editor;             /* 1 if plugin provides custom GUI */
    uint32_t bypass_param_id;   /* IS_BYPASS-flagged param ID, or
                                 * UINT32_MAX for no bypass param. The
                                 * AAX C++ template registers this as
                                 * the master bypass via
                                 * cDefaultMasterBypassID. */
    /* Chunk fourccs a pre-truce build stored its state under
     * (`aax_chunk_ids` in truce.toml's [plugin.legacy_state]). The
     * template declares them alongside truce's own chunk so Pro Tools
     * hands old sessions' chunks to SetChunk, which routes them to
     * truce_aax_load_state_foreign / the plugin's migrate_state. */
    uint32_t num_legacy_chunk_ids;
    uint32_t legacy_chunk_ids[8]; /* TRUCE_AAX_MAX_LEGACY_CHUNKS */
    /* Main-bus (in, out) channel counts of each bus_layouts() entry. The
     * describe template maps each to an AAX stem format and registers one
     * component config per layout, so Pro Tools offers every declared I/O
     * width. num_layouts == 0 keeps the legacy mono/stereo-only describe
     * (single-layout plugins); arrays are num_layouts long. */
    const int16_t* layout_in_channels;
    const int16_t* layout_out_channels;
    uint32_t num_layouts;
    /* First sidechain bus width from the default layout. > 0 makes the
     * describe template register AAX's single side-chain input. Pro Tools
     * supplies one mono source, duplicated across this bus's channels.
     * Later declared sidechain buses remain unavailable. */
    uint32_t sidechain_in_channels;
} TruceAaxDescriptor;

/* Parameter info. */
typedef struct {
    uint32_t id;
    const char* name;
    double min;
    double max;
    double default_value;
    uint32_t step_count;
    const char* unit;           /* "dB", "Hz", "%", "" etc. */
    uint8_t range_type;         /* One of TRUCE_AAX_RANGE_*. */
    uint8_t _pad[7];            /* Match Rust-side trailing pad. */
} TruceAaxParamInfo;

#define TRUCE_AAX_NATIVE_EVENT_MIDI1 1u
#define TRUCE_AAX_NATIVE_EVENT_SYSEX 2u

/* Bounded input storage shared by the C++ template and Rust EventList. */
#define TRUCE_AAX_NATIVE_EVENT_CAP 256u
#define TRUCE_AAX_SYSEX_POOL_CAP 131072u

/* Sequential event-adapter statuses. END is also the successful block status. */
#define TRUCE_AAX_EVENT_END 0u
#define TRUCE_AAX_EVENT_EMITTED 1u
#define TRUCE_AAX_EVENT_UNSUPPORTED 2u
#define TRUCE_AAX_EVENT_INVALID 3u
#define TRUCE_AAX_EVENT_QUEUE_FULL 4u

/* One host-native AAX event. AAX exposes one MIDI stream, so port must be 0.
 * MIDI1 uses exactly data_len bytes from midi (1..3). SysEx uses data_len
 * bytes at sysex, without F0/F7 framing. The pointer is borrowed only for the
 * synchronous process/output callback which receives it. */
typedef struct {
    uint32_t sample_offset;
    uint16_t port;
    uint8_t kind;
    uint8_t reserved;
    uint32_t data_len;
    uint8_t midi[3];
    uint8_t _pad;
    const uint8_t* sysex;
} TruceAaxNativeEvent;

/* Transport snapshot filled by the AAX template each render from
 * AAX_ITransport. Fields default to 0 / false when Pro Tools does not
 * report them. */
typedef struct {
    int32_t valid;              /* 1 if AAX_ITransport returned anything */
    int32_t playing;
    int32_t recording;
    int32_t loop_active;
    int32_t time_sig_num;       /* 0 = not reported */
    int32_t time_sig_den;
    double  tempo;              /* 0 = not reported */
    double  position_samples;
    double  position_beats;
    double  bar_start_beats;
    double  loop_start_beats;
    double  loop_end_beats;
} TruceAaxTransportSnapshot;

/* GUI editor info returned by editor_create. */
typedef struct {
    int has_editor;
    uint32_t width;
    uint32_t height;
} TruceAaxEditorInfo;

/* Callback vtable for GUI → host parameter gestures. */
typedef struct {
    void* aax_ctx;
    void (*touch_param)(void* aax_ctx, uint32_t param_id);
    void (*set_param)(void* aax_ctx, uint32_t param_id, double normalized);
    void (*release_param)(void* aax_ctx, uint32_t param_id);
    int  (*request_resize)(void* aax_ctx, uint32_t w, uint32_t h);
} TruceAaxGuiCallbacks;

/* --- Functions exported by the Rust cdylib --- */

/* Bridge ABI version. Must equal `TRUCE_AAX_ABI_VERSION` above. */
uint32_t truce_aax_abi_version(void);

/* Plugin descriptor. Returned pointer is valid for the library lifetime. */
void truce_aax_get_descriptor(TruceAaxDescriptor* out);

/* Parameter info for each parameter (0-indexed). */
void truce_aax_get_param_info(uint32_t index, TruceAaxParamInfo* out);

/* Instance lifecycle. */
void* truce_aax_create(void);
void  truce_aax_destroy(void* ctx);
void  truce_aax_reset(void* ctx, double sample_rate, uint32_t max_frames);

/* Render-mode signal, as a ProcessMode discriminant (0 realtime, 1
 * buffered, 2 offline). The template calls this from its
 * NotificationReceived override when the host posts
 * AAX_eNotificationEvent_EnteringOfflineMode / ExitingOfflineMode
 * (offline bounce). The Rust side stashes it in an atomic that reset
 * and every process block read. */
void  truce_aax_set_render_mode(void* ctx, uint32_t mode);

/* Current plugin latency in samples, for host delay compensation. The
 * template polls this from its TimerWakeup idle callback and pushes
 * changes to the host via AAX_IController::SetSignalLatency. */
uint32_t truce_aax_latency(void* ctx);

/* Strict audio/event processing. `input_status` is END when the full host
 * packet stream fit and validated; otherwise no partial event prefix is
 * delivered to the plugin. The return value reports Rust-side validation or
 * bounded-lane failure. `transport` may be NULL. */
uint32_t truce_aax_process_native(void* ctx,
    const float** inputs, float** outputs,
    uint32_t num_input_channels, uint32_t num_output_channels,
    uint32_t input_bus_active, uint32_t output_bus_active,
    uint32_t num_frames,
    const TruceAaxNativeEvent* events, uint32_t num_events,
    uint32_t input_status,
    const TruceAaxTransportSnapshot* transport);

/* One ordered output transaction. begin preflights the entire lossless lane;
 * next returns one logical MIDI1/SysEx event at a time or an explicit error.
 * An error is returned before any event when the block cannot round-trip. */
void     truce_aax_begin_output_events(void* ctx, uint32_t num_frames);
uint32_t truce_aax_next_output_event(void* ctx, TruceAaxNativeEvent* out);
void     truce_aax_finish_output_events(void* ctx, uint32_t status);

/* Parameters (plain values, not normalized). */
double truce_aax_get_param(void* ctx, uint32_t id);
void   truce_aax_set_param(void* ctx, uint32_t id, double value);
void   truce_aax_format_param(void* ctx, uint32_t id, double value,
                               char* out, uint32_t out_len);
/* Parse host text-entry into a plain value. Returns 1 and writes
 * out_plain on success, 0 when the text isn't parseable for the param. */
int32_t truce_aax_parse_param(void* ctx, uint32_t id, const char* text,
                               double* out_plain);
/* Custom-taper coefficient<->plain mapping through the param's ParamRange
 * (for range_type CUSTOM - skewed shapes AAX has no native taper for). */
double truce_aax_normalize(void* ctx, uint32_t id, double plain);
double truce_aax_denormalize(void* ctx, uint32_t id, double normalized);

/* State serialization. */
uint32_t truce_aax_save_state(void* ctx, uint8_t** out_data);
void     truce_aax_load_state(void* ctx, const uint8_t* data, uint32_t len);
void     truce_aax_free_state(uint8_t* data, uint32_t len);
/* Bytes found under a legacy chunk id (see the descriptor's
 * legacy_chunk_ids): offered to the plugin's migrate_state hook.
 * Returns 1 when the plugin translated and accepted them. */
int32_t  truce_aax_load_state_foreign(void* ctx, uint32_t chunk_id,
                                      const uint8_t* data, uint32_t len);

/* GUI editor. */
void truce_aax_editor_create(void* ctx, TruceAaxEditorInfo* out);
void truce_aax_editor_open(void* ctx, void* parent_view, int platform,
                            const TruceAaxGuiCallbacks* callbacks);
void truce_aax_editor_close(void* ctx);
void truce_aax_editor_idle(void* ctx);
int  truce_aax_editor_get_size(void* ctx, uint32_t* w, uint32_t* h);

#ifdef __cplusplus
}
#endif

#endif /* TRUCE_AAX_BRIDGE_H */
