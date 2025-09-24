/*
 * mod_amd.c
 *
 * Answering Machine Detection (non-blocking) with CLI/ESL trigger:
 *   - Dialplan app: amd [key=val;key=val;...]
 *   - API command: uuid_amd_detect <uuid> [key=val;key=val;...]
 *
 * The API locates the target session by UUID and invokes the same
 * amd_start_function used by the dialplan app, attaching a media bug
 * that analyzes inbound audio and sets:
 *   - channel vars: amd_result, amd_cause, amd_result_epoch
 *   - fires a custom event subclass "amd" with AMD-Result / AMD-Cause
 */

#include <switch.h>

#define AMD_PARAMS (2)
#define AMD_SYNTAX "<uuid> <command>"

#define BUG_AMD_NAME_READ "amd_read"

SWITCH_MODULE_LOAD_FUNCTION(mod_amd_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amd_shutdown);
SWITCH_MODULE_DEFINITION(mod_amd, mod_amd_load, mod_amd_shutdown, NULL);

SWITCH_STANDARD_APP(amd_start_function);
SWITCH_STANDARD_API(uuid_amd_detect_function);

/* -------------------------
   Configurable parameters
   ------------------------- */

typedef struct {
    uint32_t initial_silence;
    uint32_t greeting;
    uint32_t after_greeting_silence;
    uint32_t total_analysis_time;
    uint32_t minimum_word_length;
    uint32_t between_words_silence;
    uint32_t maximum_number_of_words;
    uint32_t maximum_word_length;
    uint32_t silence_threshold;
} amd_params_t;

static amd_params_t globals;

static switch_xml_config_item_t instructions[] = {
    SWITCH_CONFIG_ITEM(
        "initial_silence",
        SWITCH_CONFIG_INT, CONFIG_RELOADABLE,
        &globals.initial_silence, (void*)2500, NULL, NULL, NULL),

    SWITCH_CONFIG_ITEM(
        "greeting",
        SWITCH_CONFIG_INT, CONFIG_RELOADABLE,
        &globals.greeting, (void*)1500, NULL, NULL, NULL),

    SWITCH_CONFIG_ITEM(
        "after_greeting_silence",
        SWITCH_CONFIG_INT, CONFIG_RELOADABLE,
        &globals.after_greeting_silence, (void*)800, NULL, NULL, NULL),

    SWITCH_CONFIG_ITEM(
        "total_analysis_time",
        SWITCH_CONFIG_INT, CONFIG_RELOADABLE,
        &globals.total_analysis_time, (void*)5000, NULL, NULL, NULL),

    SWITCH_CONFIG_ITEM(
        "min_word_length",
        SWITCH_CONFIG_INT, CONFIG_RELOADABLE,
        &globals.minimum_word_length, (void*)100, NULL, NULL, NULL),

    SWITCH_CONFIG_ITEM(
        "between_words_silence",
        SWITCH_CONFIG_INT, CONFIG_RELOADABLE,
        &globals.between_words_silence, (void*)50, NULL, NULL, NULL),

    SWITCH_CONFIG_ITEM(
        "maximum_number_of_words",
        SWITCH_CONFIG_INT, CONFIG_RELOADABLE,
        &globals.maximum_number_of_words, (void*)3, NULL, NULL, NULL),

    SWITCH_CONFIG_ITEM(
        "maximum_word_length",
        SWITCH_CONFIG_INT, CONFIG_RELOADABLE,
        &globals.maximum_word_length, (void*)5000, NULL, NULL, NULL),

    SWITCH_CONFIG_ITEM(
        "silence_threshold",
        SWITCH_CONFIG_INT, CONFIG_RELOADABLE,
        &globals.silence_threshold, (void*)256, NULL, NULL, NULL),

    SWITCH_CONFIG_ITEM_END()
};

static switch_status_t do_config(switch_bool_t reload)
{
    memset(&globals, 0, sizeof(globals));
    if (switch_xml_config_parse_module_settings("amd.conf", reload, instructions) != SWITCH_STATUS_SUCCESS) {
        return SWITCH_STATUS_FALSE;
    }
    return SWITCH_STATUS_SUCCESS;
}

/* -------------------------
   VAD state and classifier
   ------------------------- */

typedef enum {
    SILENCE,
    VOICED
} amd_frame_classifier;

typedef enum {
    VAD_STATE_IN_WORD,
    VAD_STATE_IN_SILENCE,
} amd_vad_state_t;

typedef struct amd_vad_c {
    switch_core_session_t *session;
    switch_channel_t *channel;
    switch_codec_implementation_t read_impl;

    amd_vad_state_t state;
    amd_params_t params;
    uint32_t frame_ms;
    int32_t sample_count_limit;

    uint32_t silence_duration;
    uint32_t voice_duration;
    uint32_t words;

    uint32_t in_initial_silence:1;
    uint32_t in_greeting:1;
} amd_vad_t;

/* Fire a custom event and queue a clone to the session */
static void amd_fire_event(const char *result, const char *cause, switch_core_session_t *fs_s)
{
    switch_event_t *event = NULL, *event_copy = NULL;

    if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, "amd") != SWITCH_STATUS_SUCCESS) {
        return;
    }
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "AMD-Result", result);
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "AMD-Cause", cause);

    if (switch_event_dup(&event_copy, event) != SWITCH_STATUS_SUCCESS) {
        switch_event_destroy(&event);
        return;
    }

    switch_core_session_queue_event(fs_s, &event);
    switch_event_fire(&event_copy);
}

static amd_frame_classifier classify_frame(uint32_t silence_threshold, const switch_frame_t *f, const switch_codec_implementation_t *codec)
{
    (void)codec;
    int16_t *audio = (int16_t *)f->data;
    uint32_t score = 0, count = 0, j = 0;
    double energy = 0.0;

    for (energy = 0, j = 0, count = 0; count < f->samples; count++) {
        energy += abs(audio[j++]);
    }
    score = (uint32_t)(energy / (f->samples));

    return (score >= silence_threshold) ? VOICED : SILENCE;
}

static switch_bool_t amd_handle_silence_frame(amd_vad_t *vad, const switch_frame_t *f)
{
    (void)f;
    vad->silence_duration += vad->frame_ms;

    if (vad->silence_duration >= vad->params.between_words_silence) {
        if (vad->state != VAD_STATE_IN_SILENCE) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
                              "AMD: Changed state to VAD_STATE_IN_SILENCE\n");
        }
        vad->state = VAD_STATE_IN_SILENCE;
        vad->voice_duration = 0;
    }

    if (vad->in_initial_silence && vad->silence_duration >= vad->params.initial_silence) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
                          "AMD: HUMAN (silence_duration: %u, initial_silence: %u)\n",
                          vad->silence_duration, vad->params.initial_silence);
        switch_channel_set_variable(vad->channel, "amd_result", "HUMAN");
        switch_channel_set_variable(vad->channel, "amd_cause", "INITIALSILENCE");
        amd_fire_event("HUMAN", "INITIALSILENCE", vad->session);
        return SWITCH_TRUE;
    }

    if (vad->silence_duration >= vad->params.after_greeting_silence && vad->in_greeting) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
                          "AMD: HUMAN (silence_duration: %u, after_greeting_silence: %u)\n",
                          vad->silence_duration, vad->params.after_greeting_silence);
        switch_channel_set_variable(vad->channel, "amd_result", "HUMAN");
        switch_channel_set_variable(vad->channel, "amd_cause", "SILENCEAFTERGREETING");
        amd_fire_event("HUMAN", "SILENCEAFTERGREETING", vad->session);
        return SWITCH_TRUE;
    }

    return SWITCH_FALSE;
}

static switch_bool_t amd_handle_voiced_frame(amd_vad_t *vad, const switch_frame_t *f)
{
    (void)f;
    vad->voice_duration += vad->frame_ms;

    if (vad->voice_duration >= vad->params.minimum_word_length && vad->state == VAD_STATE_IN_SILENCE) {
        vad->words++;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
                          "AMD: Word detected (words: %u)\n", vad->words);
        vad->state = VAD_STATE_IN_WORD;
    }

    if (vad->voice_duration >= vad->params.maximum_word_length) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
                          "AMD: MACHINE (voice_duration: %u, maximum_word_length: %u)\n",
                          vad->voice_duration, vad->params.maximum_word_length);
        switch_channel_set_variable(vad->channel, "amd_result", "MACHINE");
        switch_channel_set_variable(vad->channel, "amd_cause", "MAXWORDLENGTH");
        amd_fire_event("MACHINE", "MAXWORDLENGTH", vad->session);
        return SWITCH_TRUE;
    }

    if (vad->words >= vad->params.maximum_number_of_words) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
                          "AMD: MACHINE (words: %u, maximum_number_of_words: %u)\n",
                          vad->words, vad->params.maximum_number_of_words);
        switch_channel_set_variable(vad->channel, "amd_result", "MACHINE");
        switch_channel_set_variable(vad->channel, "amd_cause", "MAXWORDS");
        amd_fire_event("MACHINE", "MAXWORDS", vad->session);
        return SWITCH_TRUE;
    }

    if (vad->in_greeting && vad->voice_duration >= vad->params.greeting) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
                          "AMD: MACHINE (voice_duration: %u, greeting: %u)\n",
                          vad->voice_duration, vad->params.greeting);
        switch_channel_set_variable(vad->channel, "amd_result", "MACHINE");
        switch_channel_set_variable(vad->channel, "amd_cause", "LONGGREETING");
        amd_fire_event("MACHINE", "LONGGREETING", vad->session);
        return SWITCH_TRUE;
    }

    if (vad->voice_duration >= vad->params.minimum_word_length) {
        if (vad->silence_duration) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
                              "AMD: Detected Talk, previous silence duration: %ums\n",
                              vad->silence_duration);
        }
        vad->silence_duration = 0;
    }

    if (vad->voice_duration >= vad->params.minimum_word_length && !vad->in_greeting) {
        if (vad->silence_duration) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
                              "AMD: Before Greeting Time (silence_duration: %u, voice_duration: %u)\n",
                              vad->silence_duration, vad->voice_duration);
        }
        vad->in_initial_silence = 0;
        vad->in_greeting = 1;
    }

    return SWITCH_FALSE;
}

static switch_bool_t amd_read_audio_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    amd_vad_t *vad = (amd_vad_t *)user_data;

    switch (type) {
    case SWITCH_ABC_TYPE_INIT: {
        switch_core_session_get_read_impl(vad->session, &vad->read_impl);
        if (vad->params.total_analysis_time) {
            vad->sample_count_limit = (vad->read_impl.actual_samples_per_second / 1000) * vad->params.total_analysis_time;
        }
        break;
    }
    case SWITCH_ABC_TYPE_CLOSE: {
        const char *result = NULL;

        if (switch_channel_ready(vad->channel)) {
            switch_channel_set_variable(vad->channel, "amd_result_epoch",
                switch_mprintf("%" SWITCH_TIME_T_FMT, switch_time_now() / 1000000));

            result = switch_channel_get_variable(vad->channel, "amd_result");
            if (result) {
                if (!strcasecmp(result, "MACHINE")) {
                    switch_channel_execute_on(vad->channel, "amd_on_machine");
                } else if (!strcasecmp(result, "HUMAN")) {
                    switch_channel_execute_on(vad->channel, "amd_on_human");
                } else {
                    switch_channel_execute_on(vad->channel, "amd_on_notsure");
                }
            } else {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_WARNING,
                                  "No amd_result found; setting NOTSURE/TOOLONG\n");
                switch_channel_set_variable(vad->channel, "amd_result", "NOTSURE");
                switch_channel_set_variable(vad->channel, "amd_cause", "TOOLONG");
                amd_fire_event("NOTSURE", "TOOLONG", vad->session);
            }
        }
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG, "AMD: close\n");
        break;
    }
    case SWITCH_ABC_TYPE_READ_PING: {
        uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t read_frame = { 0 };
        switch_status_t status;

        read_frame.data = data;
        read_frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

        status = switch_core_media_bug_read(bug, &read_frame, SWITCH_FALSE);
        if (status != SWITCH_STATUS_SUCCESS && status != SWITCH_STATUS_BREAK) {
            return SWITCH_TRUE;
        }

        if (vad->sample_count_limit) {
            vad->sample_count_limit -= read_frame.samples;
            if (vad->sample_count_limit <= 0) {
                switch_channel_set_variable(vad->channel, "amd_result", "NOTSURE");
                switch_channel_set_variable(vad->channel, "amd_cause", "TOOLONG");
                amd_fire_event("NOTSURE", "TOOLONG", vad->session);
                return SWITCH_FALSE;
            }
        }

        vad->frame_ms = 1000 / (vad->read_impl.actual_samples_per_second / read_frame.samples);

        switch (classify_frame(vad->params.silence_threshold, &read_frame, &vad->read_impl)) {
        case SILENCE:
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG, "AMD: Silence\n");
            if (amd_handle_silence_frame(vad, &read_frame)) return SWITCH_FALSE;
            break;
        case VOICED:
        default:
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG, "AMD: Voiced\n");
            if (amd_handle_voiced_frame(vad, &read_frame)) return SWITCH_FALSE;
            break;
        }
        break;
    }
    default:
        break;
    }

    return SWITCH_TRUE;
}

/* -------------------------
   Dialplan application
   ------------------------- */

SWITCH_STANDARD_APP(amd_start_function)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = NULL;
    switch_media_bug_flag_t flags = SMBF_READ_STREAM | SMBF_READ_PING;

    char *arg = (char *)data;
    char delim = ' ';

    amd_vad_t *vad = NULL;

    if (!session) {
        return;
    }

    vad = switch_core_session_alloc(session, sizeof(*vad));
    vad->params = globals;
    vad->channel = channel;
    vad->session = session;
    vad->state = VAD_STATE_IN_WORD;
    vad->silence_duration = 0;
    vad->voice_duration = 0;
    vad->frame_ms = 0;
    vad->in_initial_silence = 1;
    vad->in_greeting = 0;
    vad->words = 0;

    /* Parse inline overrides: key=value;key=value... (space or custom delim via ^^X) */
    if (!zstr(arg) && *arg == '^' && *(arg+1) == '^') {
        arg += 2;
        delim = *arg++;
    }

    if (arg && *arg) {
        int x, argc;
        char *argv[16] = { 0 };
        char *param[2] = { 0 };
        char *work = switch_core_session_strdup(session, arg);
        /* allow semicolon- or space-separated list */
        for (char *p = work; *p; ++p) { if (*p == ';' || *p == ',') *p = ' '; }

        argc = switch_separate_string(work, ' ', argv, (int)switch_arraylen(argv));
        for (x = 0; x < argc; x++) {
            if (switch_separate_string(argv[x], '=', param, (int)switch_arraylen(param)) == 2) {
                int value = atoi(param[1]);
                if (value > 0) {
                    if (!strcasecmp(param[0], "initial_silence"))        vad->params.initial_silence = value;
                    else if (!strcasecmp(param[0], "greeting"))           vad->params.greeting = value;
                    else if (!strcasecmp(param[0], "after_greeting_silence")) vad->params.after_greeting_silence = value;
                    else if (!strcasecmp(param[0], "total_analysis_time"))    vad->params.total_analysis_time = value;
                    else if (!strcasecmp(param[0], "min_word_length"))        vad->params.minimum_word_length = value;
                    else if (!strcasecmp(param[0], "between_words_silence"))  vad->params.between_words_silence = value;
                    else if (!strcasecmp(param[0], "maximum_number_of_words")) vad->params.maximum_number_of_words = value;
                    else if (!strcasecmp(param[0], "maximum_word_length"))    vad->params.maximum_word_length = value;
                    else if (!strcasecmp(param[0], "silence_threshold"))      vad->params.silence_threshold = value;
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AMD: Apply [%s]=[%d]\n", param[0], value);
                } else {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                                      "AMD: Invalid [%s]=[%s]; must be positive integer.\n", param[0], param[1]);
                }
            } else {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "AMD: Ignored arg [%s]\n", argv[x]);
            }
        }
    }

    if (!switch_channel_media_up(channel) || !switch_core_session_get_read_codec(session)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Cannot start AMD. Media is not up on channel.\n");
        return;
    }

    if (switch_core_media_bug_add(session,
                                  BUG_AMD_NAME_READ,
                                  NULL,
                                  amd_read_audio_callback,
                                  vad,
                                  0,
                                  flags,
                                  &bug) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Failed to add media bug for AMD.\n");
        return;
    }
}

/* -------------------------
   API command (CLI/ESL)
   ------------------------- */

SWITCH_STANDARD_API(uuid_amd_detect_function)
{
    /* Syntax:
     *   uuid_amd_detect <uuid> [key=val;key=val;...]
     *
     * Examples:
     *   uuid_amd_detect aaaa-bbbb-cccc-dddd
     *   uuid_amd_detect aaaa-bbbb-cccc-dddd initial_silence=2000;greeting=1200
     */
    char *dup = NULL;
    char *uuid = NULL;
    char *args = NULL;
    switch_core_session_t *ts = NULL;

    if (zstr(cmd)) {
        stream->write_function(stream, "-ERR Usage: uuid_amd_detect <uuid> [key=val;...]\n");
        return SWITCH_STATUS_SUCCESS;
    }

    dup = strdup(cmd);
    if (!dup) return SWITCH_STATUS_MEMERR;

    /* Split on first whitespace: UUID then rest as args */
    uuid = dup;
    while (*uuid && isspace((unsigned char)*uuid)) uuid++;
    args = uuid;
    while (*args && !isspace((unsigned char)*args)) args++;
    if (*args) {
        *args++ = '\0';
        while (*args && isspace((unsigned char)*args)) args++;
        if (!*args) args = NULL;
    } else {
        args = NULL;
    }

    if (zstr(uuid)) {
        stream->write_function(stream, "-ERR Usage: uuid_amd_detect <uuid> [key=val;...]\n");
        free(dup);
        return SWITCH_STATUS_SUCCESS;
    }

    ts = switch_core_session_locate(uuid);
    if (!ts) {
        stream->write_function(stream, "-ERR No such channel %s\n", uuid);
        free(dup);
        return SWITCH_STATUS_SUCCESS;
    }

    do {
        switch_channel_t *channel = switch_core_session_get_channel(ts);
        if (!switch_channel_ready(channel) || !switch_channel_media_up(channel)) {
            stream->write_function(stream, "-ERR Channel not ready (no media)\n");
            break;
        }

        /* Directly execute the same dialplan app on this live session */
        if (switch_core_session_execute_application(ts, "amd", args) != SWITCH_STATUS_SUCCESS) {
            stream->write_function(stream, "-ERR Failed to start AMD\n");
            break;
        }

        stream->write_function(stream, "+OK AMD detection started\n");
    } while (0);

    switch_core_session_rwunlock(ts);
    free(dup);
    return SWITCH_STATUS_SUCCESS;
}

/* -------------------------
   Module load / shutdown
   ------------------------- */

SWITCH_MODULE_LOAD_FUNCTION(mod_amd_load)
{
    switch_application_interface_t *app_interface = NULL;
    switch_api_interface_t *api_interface = NULL;

    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    if (do_config(SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_amd: configuration failed\n");
        return SWITCH_STATUS_FALSE;
    }

    /* Dialplan app: amd */
    SWITCH_ADD_APP(app_interface,
                   "amd",
                   "Voice activity detection (non-blocking)",
                   "Asterisk-like AMD (Non-blocking)",
                   amd_start_function,
                   "[key=val;key=val...]",
                   SAF_NONE);

    /* API: uuid_amd_detect */
    SWITCH_ADD_API(api_interface,
                   "uuid_amd_detect",
                   "Start AMD detection on a channel by UUID",
                   uuid_amd_detect_function,
                   "<uuid> [key=val;key=val;...]");

    /* fs_cli tab-completion for UUIDs */
    switch_console_set_complete("add uuid_amd_detect ::console::list_uuid");

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_amd loaded\n");
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amd_shutdown)
{
    switch_xml_config_cleanup(instructions);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_amd shutdown\n");
    return SWITCH_STATUS_SUCCESS;
}
