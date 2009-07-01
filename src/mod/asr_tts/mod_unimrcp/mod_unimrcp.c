/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2009, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH mod_unimrcp
 *
 * The Initial Developer of the Original Code is
 * Christopher M. Rienzo <chris@rienzo.net>
 *
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Brian West <brian@freeswitch.org>
 *
 * mod_unimrcp.c -- UniMRCP module (MRCP client)
 *
 */
#include <switch.h>

/* UniMRCP includes */
#include "apt.h"
#include "apt_log.h"
#include "unimrcp_client.h"
#include "mrcp_application.h"
#include "mrcp_session.h"
#include "mrcp_message.h"
#include "mrcp_generic_header.h"
#include "mrcp_synth_header.h"
#include "mrcp_synth_resource.h"
#include "mrcp_recog_header.h"
#include "mrcp_recog_resource.h"

#include "mrcp_default_factory.h"
#include "mpf_engine.h"
#include "mpf_codec_manager.h"
#include "mpf_rtp_termination_factory.h"
#include "mrcp_sofiasip_client_agent.h"
#include "mrcp_unirtsp_client_agent.h"
#include "mrcp_client_connection.h"
#include "apt_net.h"

/*********************************************************************************************************************************************
 * mod_unimrcp : module interface to FreeSWITCH
 */

/* module name */
#define MOD_UNIMRCP "unimrcp"
/* module config file */
#define CONFIG_FILE "unimrcp.conf"


/**
 * A UniMRCP application.
 */
struct mod_unimrcp_application {
	/** UniMRCP application */
	mrcp_application_t *app;
	/** MRCP callbacks from UniMRCP to this module's application */
	mrcp_app_message_dispatcher_t dispatcher;
	/** Audio callbacks from UniMRCP to this module's application */
	mpf_audio_stream_vtable_t audio_stream_vtable;
	/** maps FreeSWITCH param to MRCP param name */
	switch_hash_t *fs_param_map;
	/** maps MRCP header to unimrcp header handler function */
	switch_hash_t *param_id_map;
};
typedef struct mod_unimrcp_application mod_unimrcp_application_t;

/**
 * module globals - global configuration and variables
 */
struct mod_unimrcp_globals {
	/** max-connection-count config */
	char *unimrcp_max_connection_count;
	/** offer-new-connection config */
	char *unimrcp_offer_new_connection;
	/** default-tts-profile config */
	char *unimrcp_default_synth_profile;
	/** default-asr-profile config */
	char *unimrcp_default_recog_profile;
	/** log level for UniMRCP library */	
	char *unimrcp_log_level;
	/** the MRCP client stack */
	mrcp_client_t *mrcp_client;
	/** synthesizer application */
	mod_unimrcp_application_t synth;
	/** recognizer application */
	mod_unimrcp_application_t recog;
	/** synchronize access for speech channel numbering */
	switch_mutex_t *mutex;
	/** next available speech channel number */
	int speech_channel_number;
	/** the available profiles */
	switch_hash_t *profiles;
};
typedef struct mod_unimrcp_globals mod_unimrcp_globals_t;

/** Module global variables */
static mod_unimrcp_globals_t globals;

/**
 * Profile-specific configuration.  This allows us to handle differing MRCP server behavior 
 * on a per-profile basis 
 */
struct profile {
	/** name of the profile */
	char *name;
	/** MIME type to use for JSGF grammars */
	const char *jsgf_mime_type;
	/** MIME type to use for GSL grammars */
	const char *gsl_mime_type;
	/** MIME type to use for SRGS XML grammars */
	const char *srgs_xml_mime_type;
	/** MIME type to use for SRGS ABNF grammars */
	const char *srgs_mime_type;
};
typedef struct profile profile_t;
static switch_status_t profile_create(profile_t **profile, const char *name, switch_memory_pool_t *pool);

/**
 * Defines XML parsing instructions
 */
static switch_xml_config_item_t instructions[] = {
	SWITCH_CONFIG_ITEM_STRING_STRDUP("max-connection-count", CONFIG_REQUIRED, &globals.unimrcp_max_connection_count, "100", "", "The max MRCPv2 connections to manage"),
	/* TODO figure out what this param does */
	SWITCH_CONFIG_ITEM_STRING_STRDUP("offer-new-connection", CONFIG_REQUIRED, &globals.unimrcp_offer_new_connection, "1", "", ""),
	SWITCH_CONFIG_ITEM_STRING_STRDUP("default-tts-profile", CONFIG_REQUIRED, &globals.unimrcp_default_synth_profile, "default", "", "The default profile to use for TTS"), 
	SWITCH_CONFIG_ITEM_STRING_STRDUP("default-asr-profile", CONFIG_REQUIRED, &globals.unimrcp_default_recog_profile, "default", "", "The default profile to use for ASR"),
	SWITCH_CONFIG_ITEM_STRING_STRDUP("log-level", CONFIG_REQUIRED, &globals.unimrcp_log_level, "WARNING", "EMERGENCY|ALERT|CRITICAL|ERROR|WARNING|NOTICE|INFO|DEBUG", "Logging level for UniMRCP"),
	SWITCH_CONFIG_ITEM_END()
};

/* mod_unimrcp interface to FreeSWITCH */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_unimrcp_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_unimrcp_runtime);
SWITCH_MODULE_LOAD_FUNCTION(mod_unimrcp_load);
SWITCH_MODULE_DEFINITION(mod_unimrcp, mod_unimrcp_load, mod_unimrcp_shutdown, NULL);

static switch_status_t mod_unimrcp_do_config();
static mrcp_client_t *mod_unimrcp_client_create(switch_memory_pool_t *mod_pool);
static int process_rtp_config(mrcp_client_t *client, mpf_rtp_config_t *rtp_config, const char *param, const char *val, apr_pool_t *pool);
static int process_mrcpv1_config(rtsp_client_config_t *config, const char *param, const char *val, apr_pool_t *pool);
static int process_mrcpv2_config(mrcp_sofia_client_config_t *config, const char *param, const char *val, apr_pool_t *pool);
static int process_profile_config(profile_t *profile, const char *param, const char *val, apr_pool_t *pool);

/* UniMRCP <--> FreeSWITCH logging bridge */
static apt_bool_t unimrcp_log(const char *file, int line, const char *id, apt_log_priority_e priority, const char *format, va_list arg_ptr);
static apt_log_priority_e str_to_log_level(const char *level);

static int get_next_speech_channel_number(void);

#define XML_ID  "<?xml"
#define GSL_ID  ";GSL2.0"
#define ABNF_ID "#ABNF"
#define JSGF_ID "#JSGF"
#define BUILTIN_ID "builtin:"
#define SESSION_ID "session:"
#define HTTP_ID "http://"
#define INLINE_ID "inline:"
static int text_starts_with(const char *text, const char *match);

/**
 * UniMRCP parameter ID container
 */
struct unimrcp_param_id {
	/** The parameter ID */
	int id;
};
typedef struct unimrcp_param_id unimrcp_param_id_t;
static unimrcp_param_id_t *unimrcp_param_id_create(int id, switch_memory_pool_t *pool);


/********************************************************************************************************************************************
 * AUDIO QUEUE : UniMRCP <--> FreeSWITCH audio buffering
 */

/* size of the buffer */
#define AUDIO_QUEUE_SIZE (1024 * 32)

/* Define to enable read/write logging and dumping of queue data to file */
#undef MOD_UNIMRCP_DEBUG_AUDIO_QUEUE

/**
 * Audio queue internals 
 */
struct audio_queue {
#ifdef MOD_UNIMRCP_DEBUG_AUDIO_QUEUE
	/** debug file for tx operations */
	switch_file_t *file_write;
	/** debug file name */
	char file_write_name[30];
	/** debug file for rx operations */
	switch_file_t *file_read;
	/** debug file name */
	char file_read_name[30];
#endif
	/** the buffer of audio data */
	switch_buffer_t *buffer;
	/** synchronizes access to queue */
	switch_mutex_t *mutex;
	/** signaling for blocked readers/writers */
	switch_thread_cond_t *cond;
	/** total bytes written */
	switch_size_t write_bytes;
	/** total bytes read */
	switch_size_t read_bytes;
	/** number of bytes reader is waiting for */
	switch_size_t waiting;
	/** name of this queue (for logging) */
	char *name;
};
typedef struct audio_queue audio_queue_t;

static switch_status_t audio_queue_create(audio_queue_t **queue, const char *name, switch_memory_pool_t *pool);
static switch_status_t audio_queue_write(audio_queue_t *queue, void *data, switch_size_t *data_len);
static switch_status_t audio_queue_read(audio_queue_t *queue, void *data, switch_size_t *data_len, int block);
static switch_status_t audio_queue_clear(audio_queue_t *queue);
static switch_status_t audio_queue_destroy(audio_queue_t *queue);

/*********************************************************************************************************************************************
 * SPEECH_CHANNEL : speech functions common to recognizer and synthesizer
 */

/* how long to wait for UniMRCP to process requests */
#define SPEECH_CHANNEL_TIMEOUT_USEC (10 * 1000000)

/**
 * Type of MRCP channel
 */
enum speech_channel_type {
	SPEECH_CHANNEL_SYNTHESIZER,
	SPEECH_CHANNEL_RECOGNIZER
};
typedef enum speech_channel_type speech_channel_type_t;

/**
 * channel states
 */
enum speech_channel_state {
	/** closed */
	SPEECH_CHANNEL_CLOSED,
	/** ready for speech request */
	SPEECH_CHANNEL_READY,
	/** processing speech request */
	SPEECH_CHANNEL_PROCESSING,
	/** error opening channel */
	SPEECH_CHANNEL_ERROR
};
typedef enum speech_channel_state speech_channel_state_t;



/**
 * An MRCP speech channel
 */
struct speech_channel {
	/** the name of this channel (for logging) */
	char *name;
	/** The profile used by this channel */
	profile_t *profile;
	/** type of channel */
	speech_channel_type_t type;
	/** application this channel is running */
	mod_unimrcp_application_t *application;
	/** UniMRCP session */
	mrcp_session_t *unimrcp_session;
	/** UniMRCP channel */
	mrcp_channel_t *unimrcp_channel;
	/** memory pool */
	switch_memory_pool_t *memory_pool;
	/** synchronizes channel state */
	switch_mutex_t *mutex;
	/** wait on channel states */
	switch_thread_cond_t *cond;
	/** channel state */
	speech_channel_state_t state;
	/** UniMRCP <--> FreeSWITCH audio buffer */
	audio_queue_t *audio_queue;
	/** codec */
	char *codec;
	/** rate */
	uint16_t rate;
	/** silence byte */
	int silence;
	/** speech channel params */
	switch_hash_t *params;
	/** app specific data */
	void *data;
};
typedef struct speech_channel speech_channel_t;

/* speech channel interface for UniMRCP */
static apt_bool_t speech_on_session_terminate(mrcp_application_t *application, mrcp_session_t *session, mrcp_sig_status_code_e status);
static apt_bool_t speech_on_channel_add(mrcp_application_t *application, mrcp_session_t *session, mrcp_channel_t *channel, mrcp_sig_status_code_e status);
static apt_bool_t speech_on_channel_remove(mrcp_application_t *application, mrcp_session_t *session, mrcp_channel_t *channel, mrcp_sig_status_code_e status);

/* speech_channel funcs */
static switch_status_t speech_channel_create(speech_channel_t **schannel, const char *name, speech_channel_type_t type, mod_unimrcp_application_t *app, const char *codec, uint16_t rate, switch_memory_pool_t *pool);
static switch_status_t speech_channel_open(speech_channel_t *schannel, profile_t *profile);
static switch_status_t speech_channel_destroy(speech_channel_t *schannel);
static switch_status_t speech_channel_stop(speech_channel_t *schannel);
static switch_status_t speech_channel_set_param(speech_channel_t *schannel, const char *name, const char *val);
static switch_status_t speech_channel_write(speech_channel_t *schannel, void *data, switch_size_t *len);
static switch_status_t speech_channel_read(speech_channel_t *schannel, void *data, switch_size_t *len, int block);
static switch_status_t speech_channel_set_state(speech_channel_t *schannel, speech_channel_state_t state);
static switch_status_t speech_channel_set_state_unlocked(speech_channel_t *schannel, speech_channel_state_t state);
static const char *speech_channel_state_to_string(speech_channel_state_t state);
static const char *speech_channel_type_to_string(speech_channel_type_t type);


/*********************************************************************************************************************************************
 * SYNTHESIZER : UniMRCP <--> FreeSWITCH tts interface
 */

/* synthesis languages */
#define MIME_TYPE_SSML_XML "application/ssml+xml"
#define MIME_TYPE_PLAIN_TEXT "text/plain"

static switch_status_t synth_load(switch_loadable_module_interface_t *module_interface, switch_memory_pool_t *pool);
static switch_status_t synth_shutdown();

/* synthesizer's interface for FreeSWITCH */
static switch_status_t synth_speech_open(switch_speech_handle_t *sh, const char *voice_name, int rate, switch_speech_flag_t *flags);
static switch_status_t synth_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags);
static switch_status_t synth_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags);
static switch_status_t synth_speech_read_tts(switch_speech_handle_t *sh, void *data, switch_size_t *datalen, switch_speech_flag_t *flags);
static void synth_speech_flush_tts(switch_speech_handle_t *sh);
static void synth_speech_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val);
static void synth_speech_numeric_param_tts(switch_speech_handle_t *sh, char *param, int val);
static void synth_speech_float_param_tts(switch_speech_handle_t *sh, char *param, double val);

/* synthesizer's interface for UniMRCP */
static apt_bool_t synth_message_handler(const mrcp_app_message_t *app_message);
static apt_bool_t synth_on_message_receive(mrcp_application_t *application, mrcp_session_t *session, mrcp_channel_t *channel, mrcp_message_t *message);
static apt_bool_t synth_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame);

/* synthesizer specific speech_channel funcs */
static switch_status_t synth_channel_speak(speech_channel_t *schannel, const char *text);
static switch_status_t synth_channel_set_params(speech_channel_t *schannel, mrcp_message_t *msg, mrcp_generic_header_t *gen_hdr, mrcp_synth_header_t *synth_hdr);
static switch_status_t synth_channel_set_header(speech_channel_t *schannel, int id, char *val, mrcp_message_t *msg, mrcp_synth_header_t *synth_hdr);

/*********************************************************************************************************************************************
 * GRAMMAR : recognizer grammar management 
 */

/**
 * type of the grammar
 */
enum grammar_type {
	GRAMMAR_TYPE_UNKNOWN,
	/* text/uri-list */
	GRAMMAR_TYPE_URI,
	/* application/srgs */
	GRAMMAR_TYPE_SRGS,
	/* application/srgs+xml */
	GRAMMAR_TYPE_SRGS_XML,
	/* application/x-nuance-gsl */
	GRAMMAR_TYPE_NUANCE_GSL,
	/* application/x-jsgf */
	GRAMMAR_TYPE_JSGF
};
typedef enum grammar_type grammar_type_t;

/**
 * A grammar for recognition
 */
struct grammar {
	/** name of this grammar */
	char *name;
	/** grammar MIME type */
	grammar_type_t type;
	/** the grammar or its URI, depending on type */
	char *data;
};
typedef struct grammar grammar_t;

static switch_status_t grammar_create(grammar_t **grammar, const char *name, grammar_type_t type, const char *data, switch_memory_pool_t *pool);
static const char *grammar_type_to_mime(grammar_type_t type, profile_t *profile);

/*********************************************************************************************************************************************
 * RECOGNIZER : UniMRCP <--> FreeSWITCH asr interface
 */

/**
 * Data specific to the recognizer
 */
struct recognizer_data {
	/** the speech recognition grammar */
	grammar_t *grammar;
	/** recognize result */
	char *result;
	/** true, if voice has started */
	int start_of_input;
	/** true, if input timers have started */
	int timers_started;
};
typedef struct recognizer_data recognizer_data_t;

static switch_status_t recog_load(switch_loadable_module_interface_t *module_interface, switch_memory_pool_t *pool);
static switch_status_t recog_shutdown() ;

/* recognizer's interface for FreeSWITCH */
static switch_status_t recog_asr_open(switch_asr_handle_t *ah, const char *codec, int rate, const char *dest, switch_asr_flag_t *flags);
static switch_status_t recog_asr_load_grammar(switch_asr_handle_t *ah, const char *grammar, const char *name);
static switch_status_t recog_asr_unload_grammar(switch_asr_handle_t *ah, const char *name);
static switch_status_t recog_asr_close(switch_asr_handle_t *ah, switch_asr_flag_t *flags);
static switch_status_t recog_asr_feed(switch_asr_handle_t *ah, void *data, unsigned int len, switch_asr_flag_t *flags);
static switch_status_t recog_asr_resume(switch_asr_handle_t *ah);
static switch_status_t recog_asr_pause(switch_asr_handle_t *ah);
static switch_status_t recog_asr_check_results(switch_asr_handle_t *ah, switch_asr_flag_t *flags);
static switch_status_t recog_asr_get_results(switch_asr_handle_t *ah, char **xmlstr, switch_asr_flag_t *flags);
static switch_status_t recog_asr_start_input_timers(switch_asr_handle_t *ah);
static void recog_asr_text_param(switch_asr_handle_t *ah, char *param, const char *val);
static void recog_asr_numeric_param(switch_asr_handle_t *ah, char *param, int val);
static void recog_asr_float_param(switch_asr_handle_t *ah, char *param, double val);

/* recognizer's interface for UniMRCP */
static apt_bool_t recog_message_handler(const mrcp_app_message_t *app_message);
static apt_bool_t recog_on_message_receive(mrcp_application_t *application, mrcp_session_t *session, mrcp_channel_t *channel, mrcp_message_t *message);
static apt_bool_t recog_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame);

/* recognizer specific speech_channel_funcs */
static switch_status_t recog_channel_start(speech_channel_t *schannel);
static switch_status_t recog_channel_load_grammar(speech_channel_t *schannel, const char *name, grammar_type_t type, const char *data);
static switch_status_t recog_channel_unload_grammar(speech_channel_t *schannel, const char *name);
static switch_status_t recog_channel_check_results(speech_channel_t *schannel);
static switch_status_t recog_channel_set_start_of_input(speech_channel_t *schannel);
static switch_status_t recog_channel_start_input_timers(speech_channel_t *schannel);
static switch_status_t recog_channel_set_results(speech_channel_t *schannel, const char *results);
static switch_status_t recog_channel_get_results(speech_channel_t *schannel, char **results);
static switch_status_t recog_channel_set_params(speech_channel_t *schannel, mrcp_message_t *msg, mrcp_generic_header_t *gen_hdr, mrcp_recog_header_t *recog_hdr);
static switch_status_t recog_channel_set_header(speech_channel_t *schannel, int id, char *val, mrcp_message_t *msg, mrcp_recog_header_t *recog_hdr);
static switch_status_t recog_channel_set_timers_started(speech_channel_t *schannel);


/**
 * Create a mod_unimrcp profile
 * @param profile the created profile
 * @param name the profile name
 * @param pool the memory pool to use
 * @return SWITCH_STATUS_SUCCESS if the profile is created
 */
static switch_status_t profile_create(profile_t **profile, const char *name, switch_memory_pool_t *pool)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	profile_t *lprofile = NULL;
	
	lprofile = (profile_t *)switch_core_alloc(pool, sizeof(profile_t));
	if (lprofile) {
		lprofile->name = switch_core_strdup(pool, name);
		lprofile->srgs_mime_type = "application/srgs";
		lprofile->srgs_xml_mime_type = "application/srgs+xml";
		lprofile->gsl_mime_type = "application/x-nuance-gsl";
		lprofile->jsgf_mime_type = "application/x-jsgf";
		*profile = lprofile;
	} else {
		*profile = NULL;
		status = SWITCH_STATUS_FALSE;
	}
	
	return status;
}

/**
 * Inspect text to determine if its first non-whitespace text matches "match"
 * @param text the text to inspect.
 * @param match the text to match
 * @return true if matches
 */
static int text_starts_with(const char *text, const char *match)
{
	int result = 0;

	if (!switch_strlen_zero(text)) {
		size_t textlen, matchlen;

		/* find first non-space character */
		while (switch_isspace(*text)) {
			text++;
		}
		textlen = strlen(text);
		matchlen = strlen(match);
		/* is there a match? */
		result = textlen > matchlen && !strncmp(match, text, matchlen);
	}
	
	return result;
}

/**
 * Create the audio queue
 *
 * @param audio_queue the created queue
 * @param name the name of this queue (for logging)
 * @param pool memory pool to allocate queue from 
 * @return SWITCH_STATUS_SUCCESS if successful.  SWITCH_STATUS_FALSE if unable to allocate queue
 */
static switch_status_t audio_queue_create(audio_queue_t **audio_queue, const char *name, switch_memory_pool_t *pool)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	audio_queue_t *laudio_queue = NULL;
#ifdef MOD_UNIMRCP_DEBUG_AUDIO_QUEUE
	int flags;
#endif
	char *lname = "";
	*audio_queue = NULL;

	if (switch_strlen_zero(name)) {
		lname = "";
	} else {
		lname = switch_core_strdup(pool, name);
	}

	if ((laudio_queue = (audio_queue_t *)switch_core_alloc(pool, sizeof(audio_queue_t))) == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) unable to create audio queue\n", lname);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	laudio_queue->name = lname;

	if (switch_buffer_create(pool, &laudio_queue->buffer, AUDIO_QUEUE_SIZE) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) unable to create audio queue buffer\n", laudio_queue->name);
		status = SWITCH_STATUS_FALSE;
		goto done;
    }

	if (switch_mutex_init(&laudio_queue->mutex, SWITCH_MUTEX_UNNESTED, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) unable to create audio queue mutex\n", laudio_queue->name);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	if (switch_thread_cond_create(&laudio_queue->cond, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) unable to create audio queue condition variable\n", laudio_queue->name);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

#ifdef MOD_UNIMRCP_DEBUG_AUDIO_QUEUE
	flags = SWITCH_FOPEN_CREATE | SWITCH_FOPEN_WRITE | SWITCH_FOPEN_TRUNCATE | SWITCH_FOPEN_BINARY;
	strcpy(laudio_queue->file_read_name, "/tmp/mod_unimrcp_rx_XXXXXX");	
	if (switch_file_mktemp(&laudio_queue->file_read, laudio_queue->file_read_name, flags, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) unable to create audio queue read file\n", laudio_queue->name);
		laudio_queue->file_read = NULL;
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) queue rx saved to %s\n", laudio_queue->name, laudio_queue->file_read_name);
	}
	strcpy(laudio_queue->file_write_name, "/tmp/mod_unimrcp_tx_XXXXXX");
	if (switch_file_mktemp(&laudio_queue->file_write, laudio_queue->file_write_name, flags, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) unable to create audio queue write file\n", laudio_queue->name);
		laudio_queue->file_write = NULL;
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) queue tx saved to %s\n", laudio_queue->name, laudio_queue->file_write_name);
	}
#endif

	laudio_queue->write_bytes = 0;
	laudio_queue->read_bytes = 0;
	laudio_queue->waiting = 0;
	*audio_queue = laudio_queue;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) audio queue created\n", laudio_queue->name);

 done:

	if(status != SWITCH_STATUS_SUCCESS) {
		audio_queue_destroy(laudio_queue);
	}
	return status;
}

/**
 * Write to the audio queue
 *
 * @param queue the queue to write to
 * @param data the data to write
 * @param data_len the number of octets to write
 * @return SWITCH_STATUS_SUCCESS if data was written, SWITCH_STATUS_FALSE if data can't be written because queue is full
 */
static switch_status_t audio_queue_write(audio_queue_t *queue, void *data, switch_size_t *data_len)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_mutex_lock(queue->mutex);

#ifdef MOD_UNIMRCP_DEBUG_AUDIO_QUEUE
	switch_size_t len = *data_len;
	if(queue->file_write) {
		switch_file_write(queue->file_write, data, &len);
	}
#endif

	if (switch_buffer_write(queue->buffer, data, *data_len) > 0) {
		queue->write_bytes = queue->write_bytes + *data_len;
#ifdef MOD_UNIMRCP_DEBUG_AUDIO_QUEUE
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) audio queue write total = %d\trequested = %d\n", queue->name, queue->write_bytes, *data_len);
#endif
		if (queue->waiting <= switch_buffer_inuse(queue->buffer)) {
			switch_thread_cond_signal(queue->cond);
		}
	} else {
		*data_len = 0;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) audio queue overflow!\n", queue->name);
		status = SWITCH_STATUS_FALSE;
	}

	switch_mutex_unlock(queue->mutex);
	return status;
}

/**
 * Read from the audio queue
 *
 * @param queue the queue to read from 
 * @param data the read data
 * @param data_len the amount of data requested / actual amount of data read (returned)
 * @param block 1 if blocking is allowed
 * @return SWITCH_STATUS_SUCCESS if successful.  SWITCH_STATUS_FALSE if no data was requested, or there was a timeout while waiting to read
 */
static switch_status_t audio_queue_read(audio_queue_t *queue, void *data, switch_size_t *data_len, int block)
{
	switch_size_t requested = *data_len;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_mutex_lock(queue->mutex);

	/* wait for data, if allowed */
	if (block) {
		if (switch_buffer_inuse(queue->buffer) < requested) {
			queue->waiting = requested;
			switch_thread_cond_timedwait(queue->cond, queue->mutex, SPEECH_CHANNEL_TIMEOUT_USEC);
			if (switch_buffer_inuse(queue->buffer) < requested) {
				requested = switch_buffer_inuse(queue->buffer);
			}
		}
		queue->waiting = 0;
	} else {
		if (switch_buffer_inuse(queue->buffer) < requested) {
			requested = switch_buffer_inuse(queue->buffer);
		}
	}
	if (requested == 0) {
		*data_len = 0;
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	/* read the data */
	*data_len = switch_buffer_read(queue->buffer, data, requested);
	queue->read_bytes = queue->read_bytes + *data_len;
#ifdef MOD_UNIMRCP_DEBUG_AUDIO_QUEUE
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) audio queue read total = %d\tread = %d\trequested = %d\n", queue->name, queue->read_bytes, *data_len, requested);
	switch_size_t len = *data_len;
	if (queue->file_read) {
		switch_file_write(queue->file_read, data, &len);
	}
#endif

 done:

	switch_mutex_unlock(queue->mutex);
	return status;
}

/**
 * Empty the queue
 *
 * @param queue the queue to empty
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t audio_queue_clear(audio_queue_t *queue)
{
	switch_mutex_lock(queue->mutex);
	switch_buffer_zero(queue->buffer);
	switch_thread_cond_signal(queue->cond);
	switch_mutex_unlock(queue->mutex);
	return SWITCH_STATUS_SUCCESS;
}

/**
 * Destroy the audio queue
 *
 * @param queue the queue to clean up
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t audio_queue_destroy(audio_queue_t *queue)
{
	if (queue) {
		char *name = queue->name;
		if (switch_strlen_zero(name)) {
			name = "";
		}
		if (queue->buffer) {
			switch_buffer_destroy(&queue->buffer);
		}
		if (queue->mutex) {
			switch_mutex_destroy(queue->mutex);
			queue->mutex = NULL;
		}
		if (queue->cond) {
			switch_thread_cond_destroy(queue->cond);
			queue->cond = NULL;
		}
#ifdef MOD_UNIMRCP_DEBUG_AUDIO_QUEUE
		if (queue->file_read) {
			switch_file_close(queue->file_read);
			queue->file_read = NULL;
		}
		if (queue->file_write) {
			switch_file_close(queue->file_write);
			queue->file_write = NULL;
		}
#endif
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) audio queue destroyed\n", name);
	}
	return SWITCH_STATUS_SUCCESS;
}

/**
 * Create a speech channel
 *
 * @param schannel the created channel
 * @param name the name of the channel
 * @param type the type of channel to create
 * @param app the application
 * @param codec the codec to use
 * @param rate the rate to use
 * @param pool the memory pool to use
 * @return SWITCH_STATUS_SUCCESS if successful.  SWITCH_STATUS_FALSE if the channel cannot be allocated.
 */
static switch_status_t speech_channel_create(speech_channel_t **schannel, const char *name, speech_channel_type_t type, mod_unimrcp_application_t *app, const char *codec, uint16_t rate, switch_memory_pool_t *pool)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	speech_channel_t *schan = NULL;
	*schannel = NULL;

	if ((schan = (speech_channel_t *)switch_core_alloc(pool, sizeof(speech_channel_t))) == NULL) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	schan->profile = NULL;
	schan->type = type;
	schan->application = app;
	schan->state = SPEECH_CHANNEL_CLOSED;
	schan->memory_pool = pool;
	schan->params = NULL;
	schan->rate = rate;
	schan->codec = switch_core_strdup(pool, codec);
	
	if (!strcmp("L16", schan->codec)) {
		schan->silence = 0;
	} else {
		/* 8-bit PCMU, PCMA */
		schan->silence = 128;
	}

	if (switch_mutex_init(&schan->mutex, SWITCH_MUTEX_UNNESTED, pool) != SWITCH_STATUS_SUCCESS || 
		switch_thread_cond_create(&schan->cond, pool) != SWITCH_STATUS_SUCCESS ||
		audio_queue_create(&schan->audio_queue, name, pool) != SWITCH_STATUS_SUCCESS) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	switch_core_hash_init(&schan->params, pool);
	schan->data = NULL;
	if (switch_strlen_zero(name)) {
		schan->name = "";
	} else {
		schan->name = switch_core_strdup(pool, name);
	}
	*schannel = schan;

 done:

	return status;
}

/**
 * Destroy the speech channel
 *
 * @param schannel the channel to destroy
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t speech_channel_destroy(speech_channel_t *schannel)
{
	/* destroy the channel and session if not already done */
	switch_mutex_lock(schannel->mutex);
	if (schannel->state != SPEECH_CHANNEL_CLOSED) {
		mrcp_application_channel_remove(schannel->unimrcp_session, schannel->unimrcp_channel);
		switch_thread_cond_timedwait(schannel->cond, schannel->mutex, SPEECH_CHANNEL_TIMEOUT_USEC);
		audio_queue_destroy(schannel->audio_queue);
		if (schannel->state != SPEECH_CHANNEL_CLOSED) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Failed to destroy channel.  Continuing\n", schannel->name);
		}
	}
	if (schannel->params) {
		switch_core_hash_destroy(&schannel->params);
	}
	switch_mutex_unlock(schannel->mutex);
	return SWITCH_STATUS_SUCCESS;
}

/**
 * Open the speech channel
 *
 * @param schannel the channel to open
 * @param profile the profile to use
 * @return SWITCH_STATUS_FALSE if failed, SWITCH_STATUS_RESTART if retry can be attempted with another profile, SWITCH_STATUS_SUCCESS if successful
 */
static switch_status_t speech_channel_open(speech_channel_t *schannel, profile_t *profile)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	mpf_termination_t *termination = NULL;
	mpf_codec_descriptor_t *codec = NULL;
	mrcp_resource_type_e resource_type;

	switch_mutex_lock(schannel->mutex);
	
	/* make sure we can open channel */
	if (schannel->state != SPEECH_CHANNEL_CLOSED) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	schannel->profile = profile;

	/* create MRCP session */
	if ((schannel->unimrcp_session = mrcp_application_session_create(schannel->application->app, profile->name, schannel)) == NULL) {	
		/* profile doesn't exist? */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Unable to create session with %s\n", schannel->name, profile->name);
		status = SWITCH_STATUS_RESTART;
		goto done;
	}

	/* create RTP endpoint and link to session channel */
	codec = (mpf_codec_descriptor_t *)apr_palloc(schannel->unimrcp_session->pool, sizeof(mpf_codec_descriptor_t));
	mpf_codec_descriptor_init(codec);
	codec->channel_count = 1;
	codec->payload_type = 96;
	codec->sampling_rate = schannel->rate;
	if (!strcmp(schannel->codec, "L16")) {
		/* "LPCM" is UniMRCP's name for L16 host byte ordered */
		apt_string_set(&codec->name, "LPCM");
	} else {
		apt_string_set(&codec->name, schannel->codec);
	}
	/* see RFC 1890 for payload types */
	if (!strcmp(schannel->codec, "PCMU") && schannel->rate == 8000) {
		codec->payload_type = 0;
	} else if (!strcmp(schannel->codec, "PCMA") && schannel->rate == 8000) {
		codec->payload_type = 8;
	} 
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) requesting codec %s/%d/%d\n", schannel->name, schannel->codec, codec->payload_type, codec->sampling_rate);
	if(schannel->type == SPEECH_CHANNEL_SYNTHESIZER) {
		termination = mrcp_application_sink_termination_create(schannel->unimrcp_session, &schannel->application->audio_stream_vtable, codec, schannel);
	} else {
		termination = mrcp_application_source_termination_create(schannel->unimrcp_session, &schannel->application->audio_stream_vtable, codec, schannel);
	}
	if(termination == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Unable to create termination with %s\n", schannel->name, profile->name);
		mrcp_application_session_destroy(schannel->unimrcp_session);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	if (schannel->type == SPEECH_CHANNEL_SYNTHESIZER) {
		resource_type = MRCP_SYNTHESIZER_RESOURCE;
	} else {
		resource_type = MRCP_RECOGNIZER_RESOURCE;
	}
	if ((schannel->unimrcp_channel = mrcp_application_channel_create(schannel->unimrcp_session, resource_type, termination, NULL, schannel)) == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Unable to create channel with %s\n", schannel->name, profile->name);
		mrcp_application_session_destroy(schannel->unimrcp_session);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	/* add channel to session... this establishes the connection to the MRCP server */
	if (mrcp_application_channel_add(schannel->unimrcp_session, schannel->unimrcp_channel) != TRUE) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Unable to add channel to session with %s\n", schannel->name, profile->name);
		mrcp_application_session_destroy(schannel->unimrcp_session);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	/* wait for channel to be ready */
	switch_thread_cond_timedwait(schannel->cond, schannel->mutex, SPEECH_CHANNEL_TIMEOUT_USEC);
	if (schannel->state == SPEECH_CHANNEL_READY) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) channel is ready\n", schannel->name);
	}
	else if (schannel->state == SPEECH_CHANNEL_CLOSED) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Timed out waiting for channel to be ready\n", schannel->name);
		/* can't retry */
		status = SWITCH_STATUS_FALSE;
	} else if (schannel->state == SPEECH_CHANNEL_ERROR) {
		/* Wait for session to be cleaned up */
		switch_thread_cond_timedwait(schannel->cond, schannel->mutex, SPEECH_CHANNEL_TIMEOUT_USEC);
		if (schannel->state != SPEECH_CHANNEL_CLOSED) {
			/* major issue... can't retry */
			status = SWITCH_STATUS_FALSE;
		} else {
			/* failed to open profile, retry is allowed */
			status = SWITCH_STATUS_RESTART;
		}
	}

 done:

	switch_mutex_unlock(schannel->mutex);
	return status;
}

/**
 * Send SPEAK request to synthesizer
 *
 * @param schannel the synthesizer channel
 * @param text The text to speak.  This may be plain text or SSML.
 * @return SWITCH_STATUS_SUCCESS if successful
 */
static switch_status_t synth_channel_speak(speech_channel_t *schannel, const char *text)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	mrcp_message_t *mrcp_message = NULL;
	mrcp_generic_header_t *generic_header = NULL;
	mrcp_synth_header_t *synth_header = NULL;

	switch_mutex_lock(schannel->mutex);
	if (schannel->state != SPEECH_CHANNEL_READY) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	mrcp_message = mrcp_application_message_create(schannel->unimrcp_session, schannel->unimrcp_channel, SYNTHESIZER_SPEAK);
	if (mrcp_message == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Failed to create SPEAK message\n", schannel->name);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	/* set generic header fields (content-type) */
	if ((generic_header = (mrcp_generic_header_t *)mrcp_generic_header_prepare(mrcp_message)) == NULL) {	
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	/* good enough way of determining SSML or plain text body */
	if (text_starts_with(text, XML_ID)) {
		apt_string_assign(&generic_header->content_type, MIME_TYPE_SSML_XML, mrcp_message->pool);
	} else {
		apt_string_assign(&generic_header->content_type, MIME_TYPE_PLAIN_TEXT, mrcp_message->pool);
	}
	mrcp_generic_header_property_add(mrcp_message, GENERIC_HEADER_CONTENT_TYPE);

	/* set synthesizer header fields (voice, rate, etc.) */
	if ((synth_header = (mrcp_synth_header_t *)mrcp_resource_header_prepare(mrcp_message)) == NULL) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	/* add params to MRCP message */
	synth_channel_set_params(schannel, mrcp_message, generic_header, synth_header);

	/* set body (plain text or SSML) */
	apt_string_assign(&mrcp_message->body, text, schannel->memory_pool);

	/* Empty audio queue and send SPEAK to MRCP server */
	audio_queue_clear(schannel->audio_queue);
	if (mrcp_application_message_send(schannel->unimrcp_session, schannel->unimrcp_channel, mrcp_message) == FALSE) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	/* wait for IN PROGRESS */
	switch_thread_cond_timedwait(schannel->cond, schannel->mutex, SPEECH_CHANNEL_TIMEOUT_USEC);
	if (schannel->state != SPEECH_CHANNEL_PROCESSING) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

 done:

	switch_mutex_unlock(schannel->mutex);
	return status;
}

/**
 * Set parameters in a synthesizer MRCP header
 *
 * @param schannel the speech channel containing the params
 * @param msg the MRCP message to set
 * @param gen_hdr the generic headers to set
 * @param synth_hdr the synthesizer headers to set
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t synth_channel_set_params(speech_channel_t *schannel, mrcp_message_t *msg, mrcp_generic_header_t *gen_hdr, mrcp_synth_header_t *synth_hdr)
{
	/* loop through each param and add to synth header or vendor-specific-params */
	switch_hash_index_t *hi = NULL;
	for (hi = switch_hash_first(NULL, schannel->params); hi; hi = switch_hash_next(hi)) {
		char *param_name = NULL, *param_val = NULL;
		const void *key;
		void *val;
		switch_hash_this(hi, &key, NULL, &val);
		param_name = (char *)key;
		param_val = (char *)val;
		if (!switch_strlen_zero(param_name) && !switch_strlen_zero(param_val)) {
			unimrcp_param_id_t *id = (unimrcp_param_id_t *)switch_core_hash_find(schannel->application->param_id_map, param_name);
			if (id) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) %s: %s\n", schannel->name, param_name, param_val);
				synth_channel_set_header(schannel, id->id, param_val, msg, synth_hdr);
			} else {
				apt_str_t apt_param_name = { 0 };
				apt_str_t apt_param_val = { 0 };

				/* this is probably a vendor-specific MRCP param */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) (vendor-specific value) %s: %s\n", schannel->name, param_name, param_val);
				apt_string_set(&apt_param_name, param_name); /* copy isn't necessary since apt_pair_array_append will do it */
				apt_string_set(&apt_param_val, param_val);
				if (!gen_hdr->vendor_specific_params) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) creating vendor specific pair array\n", schannel->name);
					gen_hdr->vendor_specific_params = apt_pair_array_create(10, msg->pool);
				}
				apt_pair_array_append(gen_hdr->vendor_specific_params, &apt_param_name, &apt_param_val, msg->pool);
			}
		}
	}
	
	if (gen_hdr->vendor_specific_params) {
		mrcp_generic_header_property_add(msg, GENERIC_HEADER_VENDOR_SPECIFIC_PARAMS);
	}

	return SWITCH_STATUS_SUCCESS;
}

/**
 * Set parameter in a synthesizer MRCP header
 *
 * @param schannel the speech channel containing the param
 * @param id the UniMRCP header enum
 * @param val the value to set
 * @param msg the MRCP message to set
 * @param synth_hdr the synthesizer header to set
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t synth_channel_set_header(speech_channel_t *schannel, int id, char *val, mrcp_message_t *msg, mrcp_synth_header_t *synth_hdr)
{
	switch (id) {
	case SYNTHESIZER_HEADER_VOICE_GENDER:
		if (!strcasecmp("male", val)) {
			synth_hdr->voice_param.gender = VOICE_GENDER_MALE;
		} else if (!strcasecmp("female", val)) {
			synth_hdr->voice_param.gender = VOICE_GENDER_FEMALE;
		} else if (!strcasecmp("neutral", val)) {
			synth_hdr->voice_param.gender = VOICE_GENDER_NEUTRAL;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) ignoring invalid voice gender, %s\n", schannel->name, val);
			break;
		}
		mrcp_resource_header_property_add(msg, SYNTHESIZER_HEADER_VOICE_GENDER);
		break;

	case SYNTHESIZER_HEADER_VOICE_AGE: {
		int age = atoi(val);
		if (age > 0 && age < 1000) {
			synth_hdr->voice_param.age = age;
			mrcp_resource_header_property_add(msg, SYNTHESIZER_HEADER_VOICE_AGE);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) ignoring invalid voice age, %s\n", schannel->name, val);
		}
		break;
	}

	case SYNTHESIZER_HEADER_VOICE_VARIANT: {
		int variant = atoi(val);
		if (variant > 0) {
			synth_hdr->voice_param.variant = variant;
			mrcp_resource_header_property_add(msg, SYNTHESIZER_HEADER_VOICE_VARIANT);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) ignoring invalid voice variant, %s\n", schannel->name, val);
		}
		break;
	}

	case SYNTHESIZER_HEADER_VOICE_NAME:
		apt_string_assign(&synth_hdr->voice_param.name, val, msg->pool);
		mrcp_resource_header_property_add(msg, SYNTHESIZER_HEADER_VOICE_NAME);
		break;

	case SYNTHESIZER_HEADER_KILL_ON_BARGE_IN:
		synth_hdr->kill_on_barge_in = !strcasecmp("true", val);
		mrcp_resource_header_property_add(msg, SYNTHESIZER_HEADER_KILL_ON_BARGE_IN);
		break;

	case SYNTHESIZER_HEADER_PROSODY_VOLUME:
		if (switch_isdigit(*val) || *val == '.') {
			synth_hdr->prosody_param.volume.type = PROSODY_VOLUME_TYPE_NUMERIC;
			synth_hdr->prosody_param.volume.value.numeric = (float)atof(val);
		} else if (*val == '+' || *val == '-') {
			synth_hdr->prosody_param.volume.type = PROSODY_VOLUME_TYPE_RELATIVE_CHANGE;
			synth_hdr->prosody_param.volume.value.relative = (float)atof(val);
		} else if (!strcasecmp("silent", val)) {
			synth_hdr->prosody_param.volume.type = PROSODY_VOLUME_TYPE_LABEL;
			synth_hdr->prosody_param.volume.value.label = PROSODY_VOLUME_SILENT;
		} else if (!strcasecmp("x-soft", val)) {
			synth_hdr->prosody_param.volume.type = PROSODY_VOLUME_TYPE_LABEL;
			synth_hdr->prosody_param.volume.value.label = PROSODY_VOLUME_XSOFT;
		} else if (!strcasecmp("soft", val)) {
			synth_hdr->prosody_param.volume.type = PROSODY_VOLUME_TYPE_LABEL;
			synth_hdr->prosody_param.volume.value.label = PROSODY_VOLUME_SOFT;
		} else if (!strcasecmp("medium", val)) {
			synth_hdr->prosody_param.volume.type = PROSODY_VOLUME_TYPE_LABEL;
			synth_hdr->prosody_param.volume.value.label = PROSODY_VOLUME_MEDIUM;
		} else if (!strcasecmp("loud", val)) {
			synth_hdr->prosody_param.volume.type = PROSODY_VOLUME_TYPE_LABEL;
			synth_hdr->prosody_param.volume.value.label = PROSODY_VOLUME_LOUD;
		} else if (!strcasecmp("x-loud", val)) {
			synth_hdr->prosody_param.volume.type = PROSODY_VOLUME_TYPE_LABEL;
			synth_hdr->prosody_param.volume.value.label = PROSODY_VOLUME_XLOUD;
		} else if (!strcasecmp("default", val)) {
			synth_hdr->prosody_param.volume.type = PROSODY_VOLUME_TYPE_LABEL;
			synth_hdr->prosody_param.volume.value.label = PROSODY_VOLUME_DEFAULT;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) ignoring invalid prosody volume, %s\n", schannel->name, val);
			break;
		}
		mrcp_resource_header_property_add(msg, SYNTHESIZER_HEADER_PROSODY_VOLUME);
		break;

	case SYNTHESIZER_HEADER_PROSODY_RATE:
		if (switch_isdigit(*val) || *val == '.') {
			synth_hdr->prosody_param.rate.type = PROSODY_RATE_TYPE_RELATIVE_CHANGE;
			synth_hdr->prosody_param.rate.value.relative = (float)atof(val);
		} else if (!strcasecmp("x-slow", val)) {
			synth_hdr->prosody_param.rate.type = PROSODY_RATE_TYPE_LABEL;
			synth_hdr->prosody_param.rate.value.label = PROSODY_RATE_XSLOW;
		} else if (!strcasecmp("slow", val)) {
			synth_hdr->prosody_param.rate.type = PROSODY_RATE_TYPE_LABEL;
			synth_hdr->prosody_param.rate.value.label = PROSODY_RATE_SLOW;
		} else if (!strcasecmp("medium", val)) {
			synth_hdr->prosody_param.rate.type = PROSODY_RATE_TYPE_LABEL;
			synth_hdr->prosody_param.rate.value.label = PROSODY_RATE_MEDIUM;
		} else if (!strcasecmp("fast", val)) {
			synth_hdr->prosody_param.rate.type = PROSODY_RATE_TYPE_LABEL;
			synth_hdr->prosody_param.rate.value.label = PROSODY_RATE_FAST;
		} else if (!strcasecmp("x-fast", val)) {
			synth_hdr->prosody_param.rate.type = PROSODY_RATE_TYPE_LABEL;
			synth_hdr->prosody_param.rate.value.label = PROSODY_RATE_XFAST;
		} else if (!strcasecmp("default", val)) {
			synth_hdr->prosody_param.rate.type = PROSODY_RATE_TYPE_LABEL;
			synth_hdr->prosody_param.rate.value.label = PROSODY_RATE_DEFAULT;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) ignoring invalid prosody rate, %s\n", schannel->name, val);
			break;
		}
		mrcp_resource_header_property_add(msg, SYNTHESIZER_HEADER_PROSODY_RATE);
		break;

	case SYNTHESIZER_HEADER_SPEECH_LANGUAGE:
		apt_string_assign(&synth_hdr->speech_language, val, msg->pool);
		mrcp_resource_header_property_add(msg, SYNTHESIZER_HEADER_SPEECH_LANGUAGE);
		break;

	/* unsupported by this module */
	case SYNTHESIZER_HEADER_JUMP_SIZE:
	case SYNTHESIZER_HEADER_SPEAKER_PROFILE:
	case SYNTHESIZER_HEADER_COMPLETION_CAUSE:
	case SYNTHESIZER_HEADER_COMPLETION_REASON:
	case SYNTHESIZER_HEADER_SPEECH_MARKER:
	case SYNTHESIZER_HEADER_FETCH_HINT:
	case SYNTHESIZER_HEADER_AUDIO_FETCH_HINT:
	case SYNTHESIZER_HEADER_FAILED_URI:
	case SYNTHESIZER_HEADER_FAILED_URI_CAUSE:
	case SYNTHESIZER_HEADER_SPEAK_RESTART:
	case SYNTHESIZER_HEADER_SPEAK_LENGTH:
	case SYNTHESIZER_HEADER_LOAD_LEXICON:
	case SYNTHESIZER_HEADER_LEXICON_SEARCH_ORDER:
	default:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) unsupported SYNTHESIZER_HEADER type\n", schannel->name);
	}

	return SWITCH_STATUS_SUCCESS;
}

/**
 * Stop SPEAK/RECOGNIZE request on speech channel
 * 
 * @param schannel the channel
 * @return SWITCH_STATUS_SUCCESS if successful
 */ 
static switch_status_t speech_channel_stop(speech_channel_t *schannel)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_mutex_lock(schannel->mutex);

	if (schannel->state == SPEECH_CHANNEL_PROCESSING) {
		mrcp_method_id method;
		mrcp_message_t *mrcp_message;
		if (schannel->type == SPEECH_CHANNEL_SYNTHESIZER) {
			method = SYNTHESIZER_STOP;
		} else {
			method = RECOGNIZER_STOP;
		} 
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) Stopping %s\n", schannel->name, speech_channel_type_to_string(schannel->type));
		/* Send STOP to MRCP server */
		mrcp_message = mrcp_application_message_create(schannel->unimrcp_session, schannel->unimrcp_channel, method);
		if (mrcp_message == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Failed to create STOP message\n", schannel->name);
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
		mrcp_application_message_send(schannel->unimrcp_session, schannel->unimrcp_channel, mrcp_message);
		switch_thread_cond_timedwait(schannel->cond, schannel->mutex, SPEECH_CHANNEL_TIMEOUT_USEC);

		if (schannel->state == SPEECH_CHANNEL_PROCESSING) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Timed out waiting for session to close.  Continuing.\n", schannel->name);
			schannel->state = SPEECH_CHANNEL_ERROR;
			status = SWITCH_STATUS_FALSE;
			goto done;
		} else if (schannel->state == SPEECH_CHANNEL_ERROR) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Channel error\n", schannel->name);
			schannel->state = SPEECH_CHANNEL_ERROR;
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) %s stopped\n", schannel->name, speech_channel_type_to_string(schannel->type));
	}

 done:

	switch_mutex_unlock(schannel->mutex);
	return status;
}

/**
 * Convert speech channel type into a string
 *
 * @param type the speech channel type
 * @return the speech channel type as a string
 */
static const char *speech_channel_type_to_string(speech_channel_type_t type)
{
	switch(type) {
		case SPEECH_CHANNEL_SYNTHESIZER: return "SYNTHESIZER";
		case SPEECH_CHANNEL_RECOGNIZER: return "RECOGNIZER";
	}

	return "UNKNOWN";
}

/**
 * Set parameter
 *
 * @param schannel the speech channel
 * @param param the parameter to set
 * @param val the parameter value
 * @return SWITCH_STATUS_SUCCESS if successful
 */
static switch_status_t speech_channel_set_param(speech_channel_t *schannel, const char *param, const char *val)
{
	switch_mutex_lock(schannel->mutex);
	if (!switch_strlen_zero(param) && val != NULL) {
		/* check if this is a FreeSWITCH param that needs to be translated to an MRCP param: e.g. voice ==> voice-name */
		char *mrcp_param = (char *)switch_core_hash_find(schannel->application->fs_param_map, param);
		char *lcparam = NULL;
		if (switch_strlen_zero(mrcp_param)) {
			lcparam = switch_lc_strdup(param);
			mrcp_param = lcparam;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) param = %s, val = %s\n", schannel->name, mrcp_param, val);
		switch_core_hash_insert(schannel->params, mrcp_param, val);
		switch_safe_free(lcparam);
	}
	switch_mutex_unlock(schannel->mutex);
	return SWITCH_STATUS_SUCCESS;
}

/**
 * Write synthesized speech / speech to be recognized
 *
 * @param schannel the speech channel
 * @param data the speech data
 * @param the number of octets to write / actual number written
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t speech_channel_write(speech_channel_t *schannel, void *data, switch_size_t *len)
{
	audio_queue_t *queue = schannel->audio_queue;
	switch_mutex_lock(schannel->mutex);
	if (schannel->state == SPEECH_CHANNEL_PROCESSING) {
		audio_queue_write(queue, data, len);
	}
	switch_mutex_unlock(schannel->mutex);
	return SWITCH_STATUS_SUCCESS;
}

/**
 * Read synthesized speech / speech to be recognized
 *
 * @param schannel the speech channel
 * @param data the speech data
 * @param the number of octets to read / actual number read
 * @param block 1 if blocking is allowed
 * @return SWITCH_STATUS_SUCCESS if successful, SWITCH_STATUS_BREAK if channel is no longer processing
 */
static switch_status_t speech_channel_read(speech_channel_t *schannel, void *data, switch_size_t *len, int block)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	audio_queue_t *queue = schannel->audio_queue;
	switch_mutex_lock(schannel->mutex);
	if (schannel->state == SPEECH_CHANNEL_PROCESSING) {
		audio_queue_read(queue, data, len, block);
	} else {
		status = SWITCH_STATUS_BREAK;
	}
	switch_mutex_unlock(schannel->mutex);
	return status;
}

/**
 * Convert channel state to string
 *
 * @param state the channel state
 * @return string representation of the state
 */
static const char *speech_channel_state_to_string(speech_channel_state_t state) 
{
	switch(state) {
		case SPEECH_CHANNEL_CLOSED: return "CLOSED";
		case SPEECH_CHANNEL_READY: return "READY";
		case SPEECH_CHANNEL_PROCESSING: return "PROCESSING";
		case SPEECH_CHANNEL_ERROR: return "ERROR";
	}

	return "UNKNOWN";
}

/**
 * Set the current channel state
 *
 * @param schannel the channel
 * @param state the new channel state
 * @return SWITCH_STATUS_SUCCESS, if successful
 */
static switch_status_t speech_channel_set_state(speech_channel_t *schannel, speech_channel_state_t state)
{
	switch_status_t status;
	switch_mutex_lock(schannel->mutex);
	status = speech_channel_set_state_unlocked(schannel, state);
	switch_mutex_unlock(schannel->mutex);
	return status;
}

/**
 * Use this function to set the current channel state without locking the 
 * speech channel.  Do this if you already have the speech channel locked.
 * 
 * @param schannel the channel
 * @param state the new channel state
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t speech_channel_set_state_unlocked(speech_channel_t *schannel, speech_channel_state_t state)
{
	if (schannel->state == SPEECH_CHANNEL_PROCESSING && state != SPEECH_CHANNEL_PROCESSING) {
		/* wake anyone waiting for audio data */
		audio_queue_clear(schannel->audio_queue);	
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) %s ==> %s\n", schannel->name, speech_channel_state_to_string(schannel->state), speech_channel_state_to_string(state));
	schannel->state = state;
	switch_thread_cond_signal(schannel->cond);
	return SWITCH_STATUS_SUCCESS;
}

/**
 * Process speech_open request from FreeSWITCH.  This is expected to be called before every tts request made
 * with synth_speech_feed_tts(), though the FreeSWITCH code has the option to cache the speech handle between
 * TTS requests.
 *
 * @param sh the FreeSWITCH speech handle
 * @param voice_name the voice to use
 * @param rate the sampling rate requested
 * @param flags other options
 * @return SWITCH_STATUS_SUCCESS if successful, otherwise SWITCH_STATUS_FALSE
 */
static switch_status_t synth_speech_open(switch_speech_handle_t *sh, const char *voice_name, int rate, switch_speech_flag_t *flags)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	speech_channel_t *schannel = NULL;
	const char *profile_name = sh->param;
	profile_t *profile = NULL;
	int speech_channel_number = get_next_speech_channel_number();
	char name[200] = { 0 };
	
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "speech_handle: name = %s, rate = %d, speed = %d, samples = %d, voice = %s, engine = %s, param = %s\n",
						sh->name, sh->rate, sh->speed, sh->samples, sh->voice, sh->engine, sh->param); 
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "voice = %s, rate = %d\n", voice_name, rate);

	/* It would be nice if FreeSWITCH let us know which session owns this handle, but it doesn't.  So, lets
		make our own unique name */
	switch_snprintf(name, sizeof(name) - 1, "TTS-%d", speech_channel_number);
	name[sizeof(name) - 1] = '\0';
	
	/* create channel container with L16 codec (what FreeSWITCH needs) */
	if (speech_channel_create(&schannel, name, SPEECH_CHANNEL_SYNTHESIZER, &globals.synth, "L16", (uint16_t)rate, sh->memory_pool) != SWITCH_STATUS_SUCCESS) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	sh->private_info = schannel;

	/* try to open an MRCP channel */
	if (switch_strlen_zero(profile_name)) {
		profile_name = globals.unimrcp_default_synth_profile;
	}
	profile = (profile_t *)switch_core_hash_find(globals.profiles, profile_name);
	if (!profile) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Can't find profile, %s\n", name, profile_name);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	if (speech_channel_open(schannel, profile) != SWITCH_STATUS_SUCCESS) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	/* Set session TTS params */
	if (!switch_strlen_zero(voice_name)) {
		speech_channel_set_param(schannel, "Voice-Name", voice_name);
	}

 done:

	return status;
}

/**
 * Process speech_close request from FreeSWITCH.  This is called after the TTS request has completed
 * and FreeSWITCH does not wish to cache the speech handle for another request.
 *
 * @param sh the FreeSWITCH speech handle
 * @param flags other options
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t synth_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags)
{
	speech_channel_t *schannel = (speech_channel_t *)sh->private_info;
	speech_channel_stop(schannel);
	speech_channel_destroy(schannel);
	return SWITCH_STATUS_SUCCESS;
}

/**
 * Process feed_tts request from FreeSWITCH.  This is called by FreeSWITCH after speech_open.
 * Send SPEAK request to MRCP server.
 *
 * @param sh the FreeSWITCH speech handle
 * @param text the text to speak.  This could be plain text, ssml, vxml, etc...  this function will figure it out.
 * @param flags other options
 * @return SWITCH_STATUS_SUCCESS if TTS started successfully, SWITCH_STATUS_FALSE otherwise.
 */
static switch_status_t synth_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	speech_channel_t *schannel = (speech_channel_t *)sh->private_info;
	
	if (switch_strlen_zero(text)) {
		status = SWITCH_STATUS_FALSE;
	} else {
		status = synth_channel_speak(schannel, text);
	}
	return status;
}

/**
 * Process read_tts request from FreeSWITCH.  FreeSWITCH is expecting L16 host byte ordered data.  We must return
 * exactly what is requested, otherwise FreeSWITCH won't play any audio.  Pad the data with silence, if necessary.
 *
 * @param sh the FreeSWITCH speech handle
 * @param data the read data
 * @param datalen the amount of data requested / amount of data read
 * @param flags other options
 * @return SWITCH_STATUS_SUCCESS if data was read, SWITCH_STATUS_BREAK if TTS is done
 */
static switch_status_t synth_speech_read_tts(switch_speech_handle_t *sh, void *data, switch_size_t *datalen, switch_speech_flag_t *flags)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_size_t bytes_read;
	speech_channel_t *schannel = (speech_channel_t *) sh->private_info;
	bytes_read = *datalen;
	if (speech_channel_read(schannel, data, &bytes_read, (*flags & SWITCH_SPEECH_FLAG_BLOCKING)) == SWITCH_STATUS_SUCCESS) {
		/* pad data, if not enough read */
		if (bytes_read < *datalen) {
#ifdef MOD_UNIMRCP_DEBUG_AUDIO_QUEUE
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) adding %d bytes of padding\n", schannel->name, *datalen - bytes_read);
#endif
			memset((uint8_t *)data + bytes_read, schannel->silence, *datalen - bytes_read);
		}
	} else {
		*datalen = 0;
		status = SWITCH_STATUS_BREAK;
	}

	return status;
}

/**
 * Process flush_tts request from FreeSWITCH.  Interrupt current TTS request with STOP.
 * This method is called by FreeSWITCH after a TTS request has finished, or if a request needs to be interrupted.
 *
 * @param sh the FreeSWITCH speech handle
 */
static void synth_speech_flush_tts(switch_speech_handle_t *sh)
{
	speech_channel_t *schannel = (speech_channel_t *)sh->private_info;
	speech_channel_stop(schannel);
}

/**
 * Process text_param_tts request from FreeSWITCH.
 * Update MRCP session text parameters.
 *
 * @param sh the FreeSWITCH speech handle
 * @param param the parameter to set
 * @param val the value to set the parameter to
 */
static void synth_speech_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val)
{
	speech_channel_t *schannel = (speech_channel_t *)sh->private_info;
	speech_channel_set_param(schannel, param, val);
}

/**
 * Process numeric_param_tts request from FreeSWITCH.
 * Update MRCP session numeric parameters
 *
 * @param sh the FreeSWITCH speech handle
 * @param param the parameter to set
 * @param val the value to set the parameter to
 */
static void synth_speech_numeric_param_tts(switch_speech_handle_t *sh, char *param, int val)
{
	speech_channel_t *schannel = (speech_channel_t *)sh->private_info;
	char *val_str = switch_mprintf("%d", val);
	speech_channel_set_param(schannel, param, val_str);
	switch_safe_free(val_str);
}

/**
 * Process float_param_tts request from FreeSWITCH.
 * Update MRCP session float parameters
 *
 * @param sh the FreeSWITCH speech handle
 * @param param the parameter to set
 * @param val the value to set the parameter to
 */
static void synth_speech_float_param_tts(switch_speech_handle_t *sh, char *param, double val)
{
	speech_channel_t *schannel = (speech_channel_t *)sh->private_info;
	char *val_str = switch_mprintf("%f", val);
	speech_channel_set_param(schannel, param, val_str);
	switch_safe_free(val_str);
}

/**
 * Process UniMRCP messages for the synthesizer application.  All MRCP synthesizer callbacks start here first.
 *
 * @param app_message the application message
 */
static apt_bool_t synth_message_handler(const mrcp_app_message_t *app_message)
{
	/* call the appropriate callback in the dispatcher function table based on the app_message received */
	return mrcp_application_message_dispatch(&globals.synth.dispatcher, app_message);
}

/**
 * Handle the UniMRCP responses sent to session terminate requests 
 *
 * @param application the MRCP application
 * @param session the MRCP session
 * @param status the result of the session terminate request
 * @return TRUE
 */
static apt_bool_t speech_on_session_terminate(mrcp_application_t *application, mrcp_session_t *session, mrcp_sig_status_code_e status)
{
	speech_channel_t *schannel = (speech_channel_t *)mrcp_application_session_object_get(session);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) Destroying MRCP session\n", schannel->name);
	mrcp_application_session_destroy(session);
	speech_channel_set_state(schannel, SPEECH_CHANNEL_CLOSED);

	return TRUE;
}

/**
 * Handle the UniMRCP responses sent to channel add requests 
 *
 * @param application the MRCP application
 * @param session the MRCP session
 * @param channel the MRCP channel
 * @param status the result of the channel add request
 * @return TRUE
 */
static apt_bool_t speech_on_channel_add(mrcp_application_t *application, mrcp_session_t *session, mrcp_channel_t *channel, mrcp_sig_status_code_e status)
{
	speech_channel_t *schannel =  (speech_channel_t *)mrcp_application_channel_object_get(channel);
	if (session && schannel && status == MRCP_SIG_STATUS_CODE_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "(%s) %s channel is ready\n", schannel->name, speech_channel_type_to_string(schannel->type));
		speech_channel_set_state(schannel, SPEECH_CHANNEL_READY);
	} else {
		if (schannel) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) %s channel error!\n", schannel->name, speech_channel_type_to_string(schannel->type));
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(unknown) channel error!\n");
		}
		if (session) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Terminating MRCP session\n");
			speech_channel_set_state(schannel, SPEECH_CHANNEL_ERROR);
			mrcp_application_session_terminate(session);
		}
	}

	return TRUE;
}

/**
 * Handle the UniMRCP responses sent to channel remove requests
 *
 * @param application the MRCP application
 * @param session the MRCP session
 * @param channel the MRCP channel
 * @param status the result of the channel remove request
 * @return TRUE
 */
static apt_bool_t speech_on_channel_remove(mrcp_application_t *application, mrcp_session_t *session, mrcp_channel_t *channel, mrcp_sig_status_code_e status)
{
	speech_channel_t *schannel = (speech_channel_t *)mrcp_application_channel_object_get(channel);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "(%s) %s channel is removed\n", schannel->name, speech_channel_type_to_string(schannel->type));
	schannel->unimrcp_channel = NULL;
	
	if (session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) Terminating MRCP session\n", schannel->name);
		mrcp_application_session_terminate(session);
	}

	return TRUE;
}

/**
 * Handle the MRCP synthesizer responses/events from UniMRCP
 *
 * @param application the MRCP application
 * @param session the MRCP session
 * @param channel the MRCP channel
 * @param message the MRCP message
 * @return TRUE
 */
static apt_bool_t synth_on_message_receive(mrcp_application_t *application, mrcp_session_t *session, mrcp_channel_t *channel, mrcp_message_t *message)
{
	speech_channel_t *schannel = (speech_channel_t *)mrcp_application_channel_object_get(channel);
	if (message->start_line.message_type == MRCP_MESSAGE_TYPE_RESPONSE) {
		/* received MRCP response */
		if (message->start_line.method_id == SYNTHESIZER_SPEAK) {
			/* received the response to SPEAK request */
			if (message->start_line.request_state == MRCP_REQUEST_STATE_INPROGRESS) {
				/* waiting for SPEAK-COMPLETE event */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) REQUEST IN PROGRESS\n", schannel->name);
				speech_channel_set_state(schannel, SPEECH_CHANNEL_PROCESSING);
			} else {
				/* received unexpected request_state */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) unexpected SPEAK response, request_state = %d\n", schannel->name, message->start_line.request_state);
				speech_channel_set_state(schannel, SPEECH_CHANNEL_ERROR);
			}
		} else if (message->start_line.method_id == SYNTHESIZER_STOP) {
			/* received response to the STOP request */
			if (message->start_line.request_state == MRCP_REQUEST_STATE_COMPLETE) {
				/* got COMPLETE */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) COMPLETE\n", schannel->name);
				speech_channel_set_state(schannel, SPEECH_CHANNEL_READY);
			} else {
				/* received unexpected request state */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) unexpected STOP response, request_state = %d\n", schannel->name, message->start_line.request_state);
				speech_channel_set_state(schannel, SPEECH_CHANNEL_ERROR);
			}
		} else {
			/* received unexpected response */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) unexpected response, method_id = %d\n", schannel->name, (int)message->start_line.method_id);
			speech_channel_set_state(schannel, SPEECH_CHANNEL_ERROR);
		}
	} else if (message->start_line.message_type == MRCP_MESSAGE_TYPE_EVENT) {
		/* received MRCP event */
		if (message->start_line.method_id == SYNTHESIZER_SPEAK_COMPLETE) {
			/* got SPEAK-COMPLETE */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) SPEAK-COMPLETE\n", schannel->name);
			speech_channel_set_state(schannel, SPEECH_CHANNEL_READY);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) unexpected event, method_id = %d\n", schannel->name, (int)message->start_line.method_id);
			speech_channel_set_state(schannel, SPEECH_CHANNEL_ERROR);
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) unexpected message type, message_type = %d\n", schannel->name, message->start_line.message_type);
		speech_channel_set_state(schannel, SPEECH_CHANNEL_ERROR);
	}

	return TRUE;
}

/**
 * Incoming TTS data from UniMRCP
 *
 * @param stream the audio stream sending data
 * @param frame the data
 * @return TRUE
 */
static apt_bool_t synth_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame)
{
	switch_size_t size = frame->codec_frame.size;
	speech_channel_t *schannel = (speech_channel_t *)stream->obj;
	speech_channel_write(schannel, frame->codec_frame.buffer, &size);
	return TRUE;
}

/**
 * Link the synthesizer module interface to FreeSWITCH and UniMRCP
 *
 * @param module_interface mod_unimrcp's interface
 * @param pool the memory pool to use for all allocations
 * @return SWITCH_STATUS_SUCCESS if successful, SWITCH_STATUS_FALSE otherwise
 */
static switch_status_t synth_load(switch_loadable_module_interface_t *module_interface, switch_memory_pool_t *pool)
{
	/* link to FreeSWITCH ASR / TTS callbacks */
	switch_speech_interface_t *speech_interface = NULL;
	if ((speech_interface = (switch_speech_interface_t *)switch_loadable_module_create_interface(module_interface, SWITCH_SPEECH_INTERFACE)) == NULL) {
		return SWITCH_STATUS_FALSE;
	}
	speech_interface->interface_name = MOD_UNIMRCP;
	speech_interface->speech_open = synth_speech_open;
	speech_interface->speech_close = synth_speech_close;
	speech_interface->speech_feed_tts = synth_speech_feed_tts;
	speech_interface->speech_read_tts = synth_speech_read_tts;
	speech_interface->speech_flush_tts = synth_speech_flush_tts;
	speech_interface->speech_text_param_tts = synth_speech_text_param_tts;
	speech_interface->speech_numeric_param_tts = synth_speech_numeric_param_tts;
	speech_interface->speech_float_param_tts = synth_speech_float_param_tts;

	/* Create the synthesizer application and link its callbacks to UniMRCP */
	if ((globals.synth.app = mrcp_application_create(synth_message_handler, (void *)0, pool)) == NULL) {
		return SWITCH_STATUS_FALSE;
	}
	globals.synth.dispatcher.on_session_update = NULL;
	globals.synth.dispatcher.on_session_terminate = speech_on_session_terminate;
	globals.synth.dispatcher.on_channel_add = speech_on_channel_add;
	globals.synth.dispatcher.on_channel_remove = speech_on_channel_remove;
	globals.synth.dispatcher.on_message_receive = synth_on_message_receive;
	globals.synth.audio_stream_vtable.destroy = NULL;
	globals.synth.audio_stream_vtable.open_rx = NULL;
	globals.synth.audio_stream_vtable.close_rx = NULL;
	globals.synth.audio_stream_vtable.read_frame = NULL;
	globals.synth.audio_stream_vtable.open_tx = NULL;
	globals.synth.audio_stream_vtable.close_tx =  NULL;
	globals.synth.audio_stream_vtable.write_frame = synth_stream_write;
	mrcp_client_application_register(globals.mrcp_client, globals.synth.app, "synth");

	/* map FreeSWITCH params to MRCP param */
	switch_core_hash_init_case(&globals.synth.fs_param_map, pool, SWITCH_FALSE /* case_sensitive */);
	switch_core_hash_insert(globals.synth.fs_param_map, "voice", "voice-name");

	/* map MRCP params to UniMRCP ID */
	switch_core_hash_init_case(&globals.synth.param_id_map, pool, SWITCH_FALSE /* case_sensitive */);
	switch_core_hash_insert(globals.synth.param_id_map, "jump-size", unimrcp_param_id_create(SYNTHESIZER_HEADER_JUMP_SIZE, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "kill-on-barge-in", unimrcp_param_id_create(SYNTHESIZER_HEADER_KILL_ON_BARGE_IN, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "speaker-profile", unimrcp_param_id_create(SYNTHESIZER_HEADER_SPEAKER_PROFILE, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "completion-cause", unimrcp_param_id_create(SYNTHESIZER_HEADER_COMPLETION_CAUSE, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "completion-reason", unimrcp_param_id_create(SYNTHESIZER_HEADER_COMPLETION_REASON, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "voice-gender", unimrcp_param_id_create(SYNTHESIZER_HEADER_VOICE_GENDER, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "voice-age", unimrcp_param_id_create(SYNTHESIZER_HEADER_VOICE_AGE, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "voice-variant", unimrcp_param_id_create(SYNTHESIZER_HEADER_VOICE_VARIANT, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "voice-name", unimrcp_param_id_create(SYNTHESIZER_HEADER_VOICE_NAME, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "prosody-volume", unimrcp_param_id_create(SYNTHESIZER_HEADER_PROSODY_VOLUME, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "prosody-rate", unimrcp_param_id_create(SYNTHESIZER_HEADER_PROSODY_RATE, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "speech-marker", unimrcp_param_id_create(SYNTHESIZER_HEADER_SPEECH_MARKER, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "speech-language", unimrcp_param_id_create(SYNTHESIZER_HEADER_SPEECH_LANGUAGE, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "fetch-hint", unimrcp_param_id_create(SYNTHESIZER_HEADER_FETCH_HINT, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "audio-fetch-hint", unimrcp_param_id_create(SYNTHESIZER_HEADER_AUDIO_FETCH_HINT, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "failed-uri", unimrcp_param_id_create(SYNTHESIZER_HEADER_FAILED_URI, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "failed-uri-cause", unimrcp_param_id_create(SYNTHESIZER_HEADER_FAILED_URI_CAUSE, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "speak-restart", unimrcp_param_id_create(SYNTHESIZER_HEADER_SPEAK_RESTART, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "speak-length", unimrcp_param_id_create(SYNTHESIZER_HEADER_SPEAK_LENGTH, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "load-lexicon", unimrcp_param_id_create(SYNTHESIZER_HEADER_LOAD_LEXICON, pool));
	switch_core_hash_insert(globals.synth.param_id_map, "lexicon-search-order", unimrcp_param_id_create(SYNTHESIZER_HEADER_LEXICON_SEARCH_ORDER, pool));

	return SWITCH_STATUS_SUCCESS;
}

/**
 * Shut down the synthesizer
 *
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t synth_shutdown() 
{
	if (globals.synth.fs_param_map) {
		switch_core_hash_destroy(&globals.synth.fs_param_map);
	}
	if (globals.synth.param_id_map) {
		switch_core_hash_destroy(&globals.synth.param_id_map);
	}
	return SWITCH_STATUS_SUCCESS;
}

/**
 * Create a grammar object to reference in recognition requests
 *
 * @param grammar the grammar
 * @param name the name of the grammar
 * @param type the type of the grammar (URI, SRGS, or GSL)
 * @param data the grammar data (or URI)
 * @param pool memory pool to allocate from
 * @return SWITCH_STATUS_SUCCESS if successful
 */
static switch_status_t grammar_create(grammar_t **grammar, const char *name, grammar_type_t type, const char *data, switch_memory_pool_t *pool)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	grammar_t *g = (grammar_t *)switch_core_alloc(pool, sizeof(grammar_t));
	if (g == NULL) {
		status = SWITCH_STATUS_FALSE;
		*grammar = NULL;
	} else {
		g->name = switch_core_strdup(pool, name);
		g->type = type;
		g->data = switch_core_strdup(pool, data);
		*grammar = g;
	}

	return status;
}

/**
 * Get the MIME type for this grammar type
 * @param type the grammar type
 * @param profile the profile requesting the type
 * @return the MIME type
 */
static const char *grammar_type_to_mime(grammar_type_t type, profile_t *profile)
{
	switch(type) {
		case GRAMMAR_TYPE_UNKNOWN: return "";
		case GRAMMAR_TYPE_URI: return "text/uri-list";
		case GRAMMAR_TYPE_SRGS: return profile->srgs_mime_type;
		case GRAMMAR_TYPE_SRGS_XML: return profile->srgs_xml_mime_type;
		case GRAMMAR_TYPE_NUANCE_GSL: return profile->gsl_mime_type;
		case GRAMMAR_TYPE_JSGF: return profile->jsgf_mime_type;
	}
	return "";
}

/**
 * Start RECOGNIZE request
 *
 * @param schannel the channel to start
 * @return SWITCH_STATUS_SUCCESS if successful
 */
static switch_status_t recog_channel_start(speech_channel_t *schannel)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	mrcp_message_t *mrcp_message;
	mrcp_recog_header_t *recog_header;
	mrcp_generic_header_t *generic_header;
	recognizer_data_t *r;
	char *start_input_timers;
	const char *mime_type;

	switch_mutex_lock(schannel->mutex);
	if (schannel->state != SPEECH_CHANNEL_READY) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	if (schannel->data == NULL) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	r = (recognizer_data_t *)schannel->data;
	r->result = NULL;
	r->start_of_input = 0;

	/* input timers are started by default unless the start-input-timers=false param is set */
	start_input_timers = (char *)switch_core_hash_find(schannel->params, "start-input-timers");
	r->timers_started = switch_strlen_zero(start_input_timers) || strcasecmp(start_input_timers, "false");
		
	/* create MRCP message */
	mrcp_message = mrcp_application_message_create(schannel->unimrcp_session, schannel->unimrcp_channel, RECOGNIZER_RECOGNIZE);
	if (mrcp_message == NULL) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	/* allocate generic header */
	generic_header = (mrcp_generic_header_t *)mrcp_generic_header_prepare(mrcp_message);
	if (generic_header == NULL) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	/* set Content-Type */
	mime_type = grammar_type_to_mime(r->grammar->type, schannel->profile);
	if (switch_strlen_zero(mime_type)) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	apt_string_assign(&generic_header->content_type, mime_type, mrcp_message->pool);
	mrcp_generic_header_property_add(mrcp_message, GENERIC_HEADER_CONTENT_TYPE);

	/* set Content-ID for inline grammars */
	if (r->grammar->type != GRAMMAR_TYPE_URI) {
		apt_string_assign(&generic_header->content_id, r->grammar->name, mrcp_message->pool);
		mrcp_generic_header_property_add(mrcp_message, GENERIC_HEADER_CONTENT_ID);
	}

	/* allocate recognizer-specific header */
	recog_header = (mrcp_recog_header_t *)mrcp_resource_header_prepare(mrcp_message);
	if (recog_header == NULL) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	/* set Cancel-If-Queue */
	if (mrcp_message->start_line.version == MRCP_VERSION_2) {	
		recog_header->cancel_if_queue = FALSE;
		mrcp_resource_header_property_add(mrcp_message, RECOGNIZER_HEADER_CANCEL_IF_QUEUE);
	}

	/* set parameters */
	recog_channel_set_params(schannel, mrcp_message, generic_header, recog_header);

	/* set message body */
	apt_string_assign(&mrcp_message->body, r->grammar->data, mrcp_message->pool);

	/* Empty audio queue and send RECOGNIZE to MRCP server */
	audio_queue_clear(schannel->audio_queue);
	if (mrcp_application_message_send(schannel->unimrcp_session, schannel->unimrcp_channel, mrcp_message) == FALSE) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	/* wait for IN PROGRESS */
	switch_thread_cond_timedwait(schannel->cond, schannel->mutex, SPEECH_CHANNEL_TIMEOUT_USEC);
	if (schannel->state != SPEECH_CHANNEL_PROCESSING) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

 done:

	switch_mutex_unlock(schannel->mutex);
	return status;
}

/**
 * Load speech recognition grammar
 *
 * @param schannel the recognizer channel
 * @param name the name of this grammar
 * @param type the grammar type
 * @param data the grammar data (or URI)
 * @return SWITCH_STATUS_SUCCESS if successful
 */
static switch_status_t recog_channel_load_grammar(speech_channel_t *schannel, const char *name, grammar_type_t type, const char *data)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	grammar_t *g = NULL;
	char *ldata = NULL;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) Loading grammar %s, data = %s\n", schannel->name, name, data);

	switch_mutex_lock(schannel->mutex);
	if (schannel->state != SPEECH_CHANNEL_READY) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	/* if inline, use DEFINE-GRAMMAR to cache it on the server */
	if (type != GRAMMAR_TYPE_URI) {
		mrcp_message_t *mrcp_message;
		mrcp_generic_header_t *generic_header;
		const char *mime_type;

		/* create MRCP message */
		mrcp_message = mrcp_application_message_create(schannel->unimrcp_session, schannel->unimrcp_channel, RECOGNIZER_DEFINE_GRAMMAR);
		if (mrcp_message == NULL) {
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
	
		/* set Content-Type and Content-ID in message */
		generic_header = (mrcp_generic_header_t *)mrcp_generic_header_prepare(mrcp_message);
		if (generic_header == NULL) {
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
		mime_type = grammar_type_to_mime(type, schannel->profile);
		if (switch_strlen_zero(mime_type)) {
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
		apt_string_assign(&generic_header->content_type, mime_type, mrcp_message->pool);
		mrcp_generic_header_property_add(mrcp_message, GENERIC_HEADER_CONTENT_TYPE);
		apt_string_assign(&generic_header->content_id, name, mrcp_message->pool);
		mrcp_generic_header_property_add(mrcp_message, GENERIC_HEADER_CONTENT_ID);
	
		/* put grammar in message body */
		apt_string_assign(&mrcp_message->body, data, mrcp_message->pool);

		/* send message and wait for response */
		speech_channel_set_state_unlocked(schannel, SPEECH_CHANNEL_PROCESSING);
		if (mrcp_application_message_send(schannel->unimrcp_session, schannel->unimrcp_channel, mrcp_message) == FALSE) {
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
		switch_thread_cond_timedwait(schannel->cond, schannel->mutex, SPEECH_CHANNEL_TIMEOUT_USEC);
		if (schannel->state != SPEECH_CHANNEL_READY) {
			status = SWITCH_STATUS_FALSE;
			goto done;
		}

		/* set up name, type for future RECOGNIZE requests.  We'll reference this cached grammar by name */
		ldata = switch_mprintf("session:%s", name);
		data = ldata;
		type = GRAMMAR_TYPE_URI;
	}

	/* Create the grammar and save it */
	if ((status = grammar_create(&g, name, type, data, schannel->memory_pool)) == SWITCH_STATUS_SUCCESS) {
		recognizer_data_t *r = (recognizer_data_t *)schannel->data;
		r->grammar = g;
	}

 done:
	
	switch_mutex_unlock(schannel->mutex);
	switch_safe_free(ldata);

	return status;
}

/**
 * Unload speech recognition grammar
 *
 * @param schannel the recognizer channel
 * @param grammar_name the name of the grammar to unload
 * @return SWITCH_STATUS_SUCCESS if successful
 */
static switch_status_t recog_channel_unload_grammar(speech_channel_t *schannel, const char *grammar_name)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (switch_strlen_zero(grammar_name)) {
		status = SWITCH_STATUS_FALSE;
	} else {
		recognizer_data_t *r = (recognizer_data_t *)schannel->data;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) Unloading grammar %s\n", schannel->name, grammar_name);
		r->grammar = NULL;
	}

	return status;
}

/**
 * Check if recognition is complete
 *
 * @return SWITCH_STATUS_SUCCESS if results available or start of input
 */
static switch_status_t recog_channel_check_results(speech_channel_t *schannel) 
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	recognizer_data_t *r;
	switch_mutex_lock(schannel->mutex);
	r = (recognizer_data_t *)schannel->data;
	if (!switch_strlen_zero(r->result)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) SUCCESS, have result\n", schannel->name);
	} else if (r->start_of_input) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) SUCCESS, start of input\n", schannel->name);
	} else {
		status = SWITCH_STATUS_FALSE;
	}

	switch_mutex_unlock(schannel->mutex);
	return status;
}

/**
 * Start recognizer's input timers
 *
 * @return SWITCH_STATUS_SUCCESS if timers were started
 */
static switch_status_t recog_channel_start_input_timers(speech_channel_t *schannel)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	recognizer_data_t *r = (recognizer_data_t *)schannel->data;
	switch_mutex_lock(schannel->mutex);

	if (schannel->state == SPEECH_CHANNEL_PROCESSING && !r->timers_started) {
		mrcp_message_t *mrcp_message;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) Starting input timers\n", schannel->name);
		/* Send START-INPUT-TIMERS to MRCP server */
		mrcp_message = mrcp_application_message_create(schannel->unimrcp_session, schannel->unimrcp_channel, RECOGNIZER_START_INPUT_TIMERS);
		if (mrcp_message == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Failed to create START-INPUT-TIMERS message\n", schannel->name);
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
		/* set it and forget it */
		mrcp_application_message_send(schannel->unimrcp_session, schannel->unimrcp_channel, mrcp_message);
	}

 done:

	switch_mutex_unlock(schannel->mutex);
	return status;
}

/**
 * Flag that input has started
 *
 * @param schannel the channel that has heard input
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t recog_channel_set_start_of_input(speech_channel_t *schannel)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	recognizer_data_t *r;
	switch_mutex_lock(schannel->mutex);
	r = (recognizer_data_t *)schannel->data;
	r->start_of_input = 1;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) start of input\n", schannel->name);
	switch_mutex_unlock(schannel->mutex);
	return status;
}

/**
 * Set the recognition results
 *
 * @param schannel the channel whose results are set
 * @param result the results
 * @return SWITCH_STATUS_SUCCESS if successful
 */
static switch_status_t recog_channel_set_results(speech_channel_t *schannel, const char *result)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	recognizer_data_t *r;
	switch_mutex_lock(schannel->mutex);
	r = (recognizer_data_t *)schannel->data;
	if (!switch_strlen_zero(r->result)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) result is already set\n", schannel->name);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	if (switch_strlen_zero(result)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) result is NULL\n", schannel->name);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) result:\n\n%s\n", schannel->name, result);
	r->result = switch_core_strdup(schannel->memory_pool, result);

 done:

	switch_mutex_unlock(schannel->mutex);
	return status;
}

/**
 * Get the recognition results.
 *
 * @param schannel the channel to get results from
 * @param result the results.  free() the results when finished with them.
 * @return SWITCH_STATUS_SUCCESS if there are results, SWITCH_STATUS_BREAK if start of input
 */
static switch_status_t recog_channel_get_results(speech_channel_t *schannel, char **result)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	recognizer_data_t *r = (recognizer_data_t *)schannel->data;
	switch_mutex_lock(schannel->mutex);
	if (!switch_strlen_zero(r->result)) {
		*result = strdup(r->result);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) result:\n\n%s\n", schannel->name, *result);
		r->result = NULL;
		r->start_of_input = 0;
	} else if (r->start_of_input) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) start of input\n", schannel->name);
		status = SWITCH_STATUS_BREAK;
		r->start_of_input = 0;
	} else {
		status = SWITCH_STATUS_FALSE;
	}
	
	switch_mutex_unlock(schannel->mutex);
	return status;
}

/**
 * Set parameters in a recognizer MRCP header
 *
 * @param schannel the speech channel containing the params
 * @param msg the MRCP message to set
 * @param gen_hdr the generic headers to set
 * @param recog_hdr the recognizer headers to set
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t recog_channel_set_params(speech_channel_t *schannel, mrcp_message_t *msg, mrcp_generic_header_t *gen_hdr, mrcp_recog_header_t *recog_hdr)
{
	/* loop through each param and add to recog header or vendor-specific-params */
	switch_hash_index_t *hi = NULL;
	for (hi = switch_hash_first(NULL, schannel->params); hi; hi = switch_hash_next(hi)) {
		char *param_name = NULL, *param_val = NULL;
		const void *key;
		void *val;
		switch_hash_this(hi, &key, NULL, &val);
		param_name = (char *)key;
		param_val = (char *)val;
		if (!switch_strlen_zero(param_name) && !switch_strlen_zero(param_val)) {
			unimrcp_param_id_t *id = (unimrcp_param_id_t *)switch_core_hash_find(schannel->application->param_id_map, param_name);
			if (id) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) \"%s\": \"%s\"\n", schannel->name, param_name, param_val);
				recog_channel_set_header(schannel, id->id, param_val, msg, recog_hdr);
			} else {
				/* this is probably a vendor-specific MRCP param */
				apt_str_t apt_param_name = { 0 };
				apt_str_t apt_param_val = { 0 };
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) (vendor-specific value) %s: %s\n", schannel->name, param_name, param_val);
				apt_string_set(&apt_param_name, param_name); /* copy isn't necessary since apt_pair_array_append will do it */
				apt_string_set(&apt_param_val, param_val);
				if (!gen_hdr->vendor_specific_params) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) creating vendor specific pair array\n", schannel->name);
					gen_hdr->vendor_specific_params = apt_pair_array_create(10, msg->pool);
				}
				apt_pair_array_append(gen_hdr->vendor_specific_params, &apt_param_name, &apt_param_val, msg->pool);
			}
		}
	}
	
	if (gen_hdr->vendor_specific_params) {
		mrcp_generic_header_property_add(msg, GENERIC_HEADER_VENDOR_SPECIFIC_PARAMS);
	}

	return SWITCH_STATUS_SUCCESS;
}

/**
 * Set parameter in a recognizer MRCP header
 *
 * @param schannel the speech channel containing the param
 * @param id the UniMRCP header enum
 * @param val the value to set
 * @param msg the MRCP message to set
 * @param recog_hdr the recognizer header to set
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t recog_channel_set_header(speech_channel_t *schannel, int id, char *val, mrcp_message_t *msg, mrcp_recog_header_t *recog_hdr)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch (id) {
	case RECOGNIZER_HEADER_CONFIDENCE_THRESHOLD:
		recog_hdr->confidence_threshold = (float)atof(val);
		mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_CONFIDENCE_THRESHOLD);
		break;

	case RECOGNIZER_HEADER_SENSITIVITY_LEVEL:
		recog_hdr->sensitivity_level = (float)atof(val);
		mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_SENSITIVITY_LEVEL);
		break;

	case RECOGNIZER_HEADER_SPEED_VS_ACCURACY:
		recog_hdr->speed_vs_accuracy = (float)atof(val);
		mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_SPEED_VS_ACCURACY);
		break;

	case RECOGNIZER_HEADER_N_BEST_LIST_LENGTH: {
		int n_best_list_length = atoi(val);
		if (n_best_list_length > 0) {
			recog_hdr->n_best_list_length = n_best_list_length;
			mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_N_BEST_LIST_LENGTH);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) Ignoring invalid n best list length, \"%s\"\n", schannel->name, val);
		}
		break;
	}
	case RECOGNIZER_HEADER_NO_INPUT_TIMEOUT: {
		int no_input_timeout = atoi(val);
		if (no_input_timeout >= 0) {
			recog_hdr->no_input_timeout = no_input_timeout;
			mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_NO_INPUT_TIMEOUT);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) Ignoring invalid no input timeout, \"%s\"\n", schannel->name, val);
		}
		break;
	}
	case RECOGNIZER_HEADER_RECOGNITION_TIMEOUT: { 
		int recognition_timeout = atoi(val);
		if (recognition_timeout >= 0) {
			recog_hdr->recognition_timeout = recognition_timeout;
			mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_RECOGNITION_TIMEOUT);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) Ignoring invalid recognition timeout, \"%s\"\n", schannel->name, val);
		}
		break;
	}
	case RECOGNIZER_HEADER_START_INPUT_TIMERS:
		recog_hdr->start_input_timers = !strcasecmp("true", val);
		mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_START_INPUT_TIMERS);
		break;
		case RECOGNIZER_HEADER_SPEECH_COMPLETE_TIMEOUT: {
		int speech_complete_timeout = atoi(val);
		if (speech_complete_timeout >= 0) {
			recog_hdr->speech_complete_timeout = speech_complete_timeout;
			mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_SPEECH_COMPLETE_TIMEOUT);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) Ignoring invalid speech complete timeout, \"%s\"\n", schannel->name, val);
		}
		break;
	}
	case RECOGNIZER_HEADER_SPEECH_INCOMPLETE_TIMEOUT: {
		int speech_incomplete_timeout = atoi(val);
		if (speech_incomplete_timeout >= 0) {
			recog_hdr->speech_incomplete_timeout = speech_incomplete_timeout;
			mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_SPEECH_INCOMPLETE_TIMEOUT);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) Ignoring invalid speech incomplete timeout, \"%s\"\n", schannel->name, val);
		}
		break;
	}
	case RECOGNIZER_HEADER_DTMF_INTERDIGIT_TIMEOUT: {
		int dtmf_interdigit_timeout = atoi(val);
		if (dtmf_interdigit_timeout >= 0) {
			recog_hdr->dtmf_interdigit_timeout = dtmf_interdigit_timeout;
			mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_DTMF_INTERDIGIT_TIMEOUT);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) Ignoring invalid dtmf interdigit timeout, \"%s\"\n", schannel->name, val);
		}
		break;
	}
	case RECOGNIZER_HEADER_DTMF_TERM_TIMEOUT: {
		int dtmf_term_timeout = atoi(val);
		if (dtmf_term_timeout >= 0) {
			recog_hdr->dtmf_term_timeout = dtmf_term_timeout;
			mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_DTMF_TERM_TIMEOUT);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) Ignoring invalid dtmf term timeout, \"%s\"\n", schannel->name, val);
		}
		break;
	}
	case RECOGNIZER_HEADER_DTMF_TERM_CHAR:
		if (strlen(val) == 1) {
			recog_hdr->dtmf_term_char = *val;
			mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_DTMF_TERM_CHAR);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) Ignoring invalid dtmf term char, \"%s\"\n", schannel->name, val);
		}
		break;

	case RECOGNIZER_HEADER_SAVE_WAVEFORM:
		recog_hdr->save_waveform = !strcasecmp("true", val);
		mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_SAVE_WAVEFORM);
		break;

	case RECOGNIZER_HEADER_NEW_AUDIO_CHANNEL:
		recog_hdr->new_audio_channel = !strcasecmp("true", val);
		mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_NEW_AUDIO_CHANNEL);
		break;

	case RECOGNIZER_HEADER_SPEECH_LANGUAGE:
		apt_string_assign(&recog_hdr->speech_language, val, msg->pool);
		mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_SPEECH_LANGUAGE);
		break;

	case RECOGNIZER_HEADER_RECOGNITION_MODE:
		apt_string_assign(&recog_hdr->recognition_mode, val, msg->pool);
		mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_RECOGNITION_MODE);
		break;

	case RECOGNIZER_HEADER_HOTWORD_MAX_DURATION: {
		int hotword_max_duration = atoi(val);
		if (hotword_max_duration >= 0) {
			recog_hdr->hotword_max_duration = hotword_max_duration;
			mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_HOTWORD_MAX_DURATION);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) Ignoring invalid hotword max duration, \"%s\"\n", schannel->name, val);
		}
		break;
	}
	case RECOGNIZER_HEADER_HOTWORD_MIN_DURATION: {
		int hotword_min_duration = atoi(val);
		if (hotword_min_duration >= 0) {
			recog_hdr->hotword_min_duration = hotword_min_duration;
			mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_HOTWORD_MIN_DURATION);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) Ignoring invalid hotword min duration, \"%s\"\n", schannel->name, val);
		}
		break;
	}
	case RECOGNIZER_HEADER_CLEAR_DTMF_BUFFER:
		recog_hdr->clear_dtmf_buffer = !strcasecmp("true", val);
		mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_CLEAR_DTMF_BUFFER);
		break;

	case RECOGNIZER_HEADER_EARLY_NO_MATCH:
		recog_hdr->early_no_match = !strcasecmp("true", val);
		mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_EARLY_NO_MATCH);
		break;

	case RECOGNIZER_HEADER_INPUT_WAVEFORM_URI:
		apt_string_assign(&recog_hdr->input_waveform_uri, val, msg->pool);
		mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_INPUT_WAVEFORM_URI);
		break;

	case RECOGNIZER_HEADER_MEDIA_TYPE:
		apt_string_assign(&recog_hdr->media_type, val, msg->pool);
		mrcp_resource_header_property_add(msg, RECOGNIZER_HEADER_MEDIA_TYPE);
		break;

	/* Unsupported headers */

	/* MRCP server headers */
	case RECOGNIZER_HEADER_WAVEFORM_URI:
	case RECOGNIZER_HEADER_COMPLETION_CAUSE:
	case RECOGNIZER_HEADER_FAILED_URI:
	case RECOGNIZER_HEADER_FAILED_URI_CAUSE:
	case RECOGNIZER_HEADER_INPUT_TYPE:
	case RECOGNIZER_HEADER_COMPLETION_REASON:
	/* module handles this automatically */
	case RECOGNIZER_HEADER_CANCEL_IF_QUEUE:
	/* GET-PARAMS method only */
	case RECOGNIZER_HEADER_RECOGNIZER_CONTEXT_BLOCK:
	case RECOGNIZER_HEADER_DTMF_BUFFER_TIME:

	/* INTERPRET method only */
	case RECOGNIZER_HEADER_INTERPRET_TEXT:

	/* unknown */
	case RECOGNIZER_HEADER_VER_BUFFER_UTTERANCE:

	default:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%s) unsupported RECOGNIZER header\n", schannel->name);
	}

	return status;
}

/**
 * Flag that the recognizer channel timers are started
 * @param schannel the recognizer channel to flag 
 */
static switch_status_t recog_channel_set_timers_started(speech_channel_t *schannel)
{
	recognizer_data_t *r;
	switch_mutex_lock(schannel->mutex);
	r = (recognizer_data_t *)schannel->data;
	r->timers_started = 1;
	switch_mutex_unlock(schannel->mutex);
	return SWITCH_STATUS_SUCCESS;
}

/**
 * Process asr_open request from FreeSWITCH.
 *
 * @param ah the FreeSWITCH speech rec handle
 * @param codec the codec to use
 * @param rate the sample rate of the codec
 * @param dest the profile to use
 * @param flags other flags
 * @return SWITCH_STATUS_SUCCESS if successful, otherwise SWITCH_STATUS_FALSE
 */
static switch_status_t recog_asr_open(switch_asr_handle_t *ah, const char *codec, int rate, const char *dest, switch_asr_flag_t *flags)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	speech_channel_t *schannel = NULL;
	int speech_channel_number = get_next_speech_channel_number();
	char name[200] = { 0 };
	const char *profile_name;
	profile_t *profile = NULL;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "asr_handle: name = %s, codec = %s, rate = %d, grammar = %s, param = %s\n",
		ah->name, ah->codec, ah->rate, ah->grammar, ah->param);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "codec = %s, rate = %d, dest = %s\n", codec, rate, dest);

	/* It would be nice if FreeSWITCH let us know which session owns this handle, but it doesn't.  So, lets
		make our own unique name */
	switch_snprintf(name, sizeof(name) - 1, "ASR-%d", speech_channel_number);
	name[sizeof(name) - 1] = '\0';

	if (speech_channel_create(&schannel, name, SPEECH_CHANNEL_RECOGNIZER, &globals.recog, "L16", (uint16_t)rate, ah->memory_pool) != SWITCH_STATUS_SUCCESS) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	ah->private_info = schannel;
	schannel->data = switch_core_alloc(ah->memory_pool, sizeof(recognizer_data_t));
	memset(schannel->data, 0, sizeof(recognizer_data_t));

	/* try to open an MRCP channel */
	profile_name = switch_strlen_zero(dest) ? globals.unimrcp_default_recog_profile : dest;
	profile = (profile_t *)switch_core_hash_find(globals.profiles, profile_name);
	if (!profile) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Can't find profile, %s\n", name, profile_name);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	status = speech_channel_open(schannel, profile);

 done:

	return status;
}

/**
 * Process asr_load_grammar request from FreeSWITCH.
 *
 * FreeSWITCH sends this request to start recognition.
 * @param ah the FreeSWITCH speech recognition handle
 * @param grammar the grammar data.  This can be an absolute file path, a URI, or the grammar text.
 * @param name used to reference grammar for unloading or for subsequent recognition requests
 */
static switch_status_t recog_asr_load_grammar(switch_asr_handle_t *ah, const char *grammar, const char *name)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	speech_channel_t *schannel = (speech_channel_t *)ah->private_info;
	const char *grammar_data = NULL;
	char *grammar_file_data = NULL;
	switch_file_t *grammar_file = NULL;
	switch_size_t grammar_file_size = 0, to_read = 0;
	grammar_type_t type = GRAMMAR_TYPE_UNKNOWN;
	const char *lname = switch_strlen_zero(name) ? "grammar" : name;
	char *filename = NULL;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) grammar = %s, name = %s\n", schannel->name, grammar, name);

	if (switch_strlen_zero(grammar)) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	
	/* stop recognition */
	if (speech_channel_stop(schannel) != SWITCH_STATUS_SUCCESS) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	/* figure out what type of grammar this is */
	if (text_starts_with(grammar, HTTP_ID) || text_starts_with(grammar, SESSION_ID) || text_starts_with(grammar, BUILTIN_ID)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) Grammar is URI\n", schannel->name);
		type = GRAMMAR_TYPE_URI;
		grammar_data = grammar;
	} else if (text_starts_with(grammar, INLINE_ID)) {
		grammar_data = grammar + strlen(INLINE_ID);
	} else {
		/* grammar points to file containing the grammar text.  We assume the MRCP server can't get to this file
		 * so read the data from the file and cache it */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) Grammar is inside file\n", schannel->name);
		if (switch_is_file_path(grammar)) {
			filename = switch_mprintf("%s.gram", grammar);
		} else {
			filename = switch_mprintf("%s%s%s.gram", SWITCH_GLOBAL_dirs.grammar_dir, SWITCH_PATH_SEPARATOR, grammar);
		}
		grammar_data = NULL;
		if (switch_file_open(&grammar_file, filename, SWITCH_FOPEN_READ, 0, schannel->memory_pool) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Could not read grammar file: %s\n", schannel->name, filename);
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
		grammar_file_size = switch_file_get_size(grammar_file);
		if (grammar_file_size == 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Grammar file is empty: %s\n", schannel->name, filename);
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
		grammar_file_data = (char *)switch_core_alloc(schannel->memory_pool, grammar_file_size + 1);
		to_read = grammar_file_size;
		if (switch_file_read(grammar_file, grammar_file_data, &to_read) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Grammar file read error: %s\n", schannel->name, filename);
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
		if (to_read != grammar_file_size) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Could not read entire grammar file: %s\n", schannel->name, filename);
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
		grammar_file_data[to_read] = '\0';
		grammar_data = grammar_file_data;
	}

	/* determine content type of file grammar or inline grammar */
	if (type == GRAMMAR_TYPE_UNKNOWN) {
		if (text_starts_with(grammar_data, XML_ID)) {
			type = GRAMMAR_TYPE_SRGS_XML;
		} else if (text_starts_with(grammar_data, GSL_ID)) {
			type = GRAMMAR_TYPE_NUANCE_GSL;
		} else if (text_starts_with(grammar_data, ABNF_ID)) {
			type = GRAMMAR_TYPE_SRGS;
		} else if (text_starts_with(grammar_data, JSGF_ID)) {
			type = GRAMMAR_TYPE_JSGF;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) unable to determine grammar type: %s\n", schannel->name, grammar_data);
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) grammar is %s\n", schannel->name, grammar_type_to_mime(type, schannel->profile));

	/* load the grammar and start recognition */
	if (recog_channel_load_grammar(schannel, lname, type, grammar_data) != SWITCH_STATUS_SUCCESS) {
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	
	status = recog_channel_start(schannel);

 done:

	switch_safe_free(filename);
	if (grammar_file) {
		switch_file_close(grammar_file);
	}
	return status;
}

/**
 * Process asr_unload_grammar request from FreeSWITCH.
 *
 * FreeSWITCH sends this request to stop recognition on this grammar.
 * @param ah the FreeSWITCH speech recognition handle
 * @param name the grammar name.
 */
static switch_status_t recog_asr_unload_grammar(switch_asr_handle_t *ah, const char *name)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	speech_channel_t *schannel = (speech_channel_t *)ah->private_info;
	const char *lname = switch_strlen_zero(name) ? "grammar" : name;
	if (speech_channel_stop(schannel) != SWITCH_STATUS_SUCCESS ||
		recog_channel_unload_grammar(schannel, lname) != SWITCH_STATUS_SUCCESS) {
		status = SWITCH_STATUS_FALSE;
	}
	return status;
}

/**
 * Process asr_close request from FreeSWITCH
 *
 * @param ah the FreeSWITCH speech recognition handle
 */
static switch_status_t recog_asr_close(switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
	speech_channel_t *schannel = (speech_channel_t *)ah->private_info;
	speech_channel_stop(schannel);
	speech_channel_destroy(schannel);

	/* this lets FreeSWITCH's speech_thread know the handle is closed */
    switch_set_flag(ah, SWITCH_ASR_FLAG_CLOSED);

	return SWITCH_STATUS_SUCCESS;
}

/**
 * Process asr_feed request from FreeSWITCH
 *
 * @param ah the FreeSWITCH speech recognition handle
 */
static switch_status_t recog_asr_feed(switch_asr_handle_t *ah, void *data, unsigned int len, switch_asr_flag_t *flags)
{
	switch_size_t slen = len;
	speech_channel_t *schannel = (speech_channel_t *)ah->private_info;
	return speech_channel_write(schannel, data, &slen);
}

/**
 * Process asr_resume request from FreeSWITCH
 *
 * @param ah the FreeSWITCH speech recognition handle
 */
static switch_status_t recog_asr_resume(switch_asr_handle_t *ah)
{
	speech_channel_t *schannel = (speech_channel_t *)ah->private_info;
	return recog_channel_start(schannel);
}

/**
 * Process asr_pause request from FreeSWITCH
 *
 * @param ah the FreeSWITCH speech recognition handle
 */
static switch_status_t recog_asr_pause(switch_asr_handle_t *ah)
{
	speech_channel_t *schannel = (speech_channel_t *)ah->private_info;
	return speech_channel_stop(schannel);
}

/**
 * Process asr_check_results request from FreeSWITCH
 * This method is polled by FreeSWITCH until we return SWITCH_STATUS_SUCCESS.  Then
 * the results are fetched.
 *
 * @param ah the FreeSWITCH speech recognition handle
 * @param flags other flags
 */
static switch_status_t recog_asr_check_results(switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
	speech_channel_t *schannel = (speech_channel_t *)ah->private_info;
	return recog_channel_check_results(schannel);
}

/**
 * Process asr_get_results request from FreeSWITCH.  Return the XML string back
 * to FreeSWITCH.  FreeSWITCH will free() the xmlstr.
 *
 * @param ah the FreeSWITCH speech recognition handle
 * @param flags other flags
 */
static switch_status_t recog_asr_get_results(switch_asr_handle_t *ah, char **xmlstr, switch_asr_flag_t *flags)
{
	speech_channel_t *schannel = (speech_channel_t *)ah->private_info;
	return recog_channel_get_results(schannel, xmlstr);
}

/**
 * Send START-INPUT-TIMERS to executing recognition request
 * @param ah the handle to start timers on
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t recog_asr_start_input_timers(switch_asr_handle_t *ah)
{
	speech_channel_t *schannel = (speech_channel_t *)ah->private_info;
	return recog_channel_start_input_timers(schannel);
}

/**
 * Process text_param request from FreeSWITCH.
 * Update MRCP session text parameters.
 *
 * @param ah the FreeSWITCH asr handle
 * @param param the parameter to set
 * @param val the value to set the parameter to
 */
static void recog_asr_text_param(switch_asr_handle_t *ah, char *param, const char *val)
{
	speech_channel_t *schannel = (speech_channel_t *)ah->private_info;
	speech_channel_set_param(schannel, param, val);
}

/**
 * Process numeric_param request from FreeSWITCH.
 * Update MRCP session numeric parameters
 *
 * @param ah the FreeSWITCH asr handle
 * @param param the parameter to set
 * @param val the value to set the parameter to
 */
static void recog_asr_numeric_param(switch_asr_handle_t *ah, char *param, int val)
{
	speech_channel_t *schannel = (speech_channel_t *)ah->private_info;
	char *val_str = switch_mprintf("%d", val);
	speech_channel_set_param(schannel, param, val_str);
	switch_safe_free(val_str);
}

/**
 * Process float_param request from FreeSWITCH.
 * Update MRCP session float parameters
 *
 * @param ah the FreeSWITCH asr handle
 * @param param the parameter to set
 * @param val the value to set the parameter to
 */
static void recog_asr_float_param(switch_asr_handle_t *ah, char *param, double val)
{
	speech_channel_t *schannel = (speech_channel_t *)ah->private_info;
	char *val_str = switch_mprintf("%f", val);
	speech_channel_set_param(schannel, param, val_str);
	switch_safe_free(val_str);
}

/**
 * Process messages from UniMRCP for the recognizer application
 */
static apt_bool_t recog_message_handler(const mrcp_app_message_t *app_message)
{
	return mrcp_application_message_dispatch(&globals.recog.dispatcher, app_message);
}

/**
 * Handle the MRCP responses/events
 */
static apt_bool_t recog_on_message_receive(mrcp_application_t *application, mrcp_session_t *session, mrcp_channel_t *channel, mrcp_message_t *message)
{
	speech_channel_t *schannel = (speech_channel_t *)mrcp_application_channel_object_get(channel);
	mrcp_recog_header_t *recog_hdr = (mrcp_recog_header_t *)mrcp_resource_header_get(message);
	if (message->start_line.message_type == MRCP_MESSAGE_TYPE_RESPONSE) {
		/* received MRCP response */
		if (message->start_line.method_id == RECOGNIZER_RECOGNIZE) {
			/* received the response to RECOGNIZE request */
			if (message->start_line.request_state == MRCP_REQUEST_STATE_INPROGRESS) {
				/* RECOGNIZE in progress */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) RECOGNIZE IN PROGRESS\n", schannel->name);
				speech_channel_set_state(schannel, SPEECH_CHANNEL_PROCESSING);
			} else if (message->start_line.request_state == MRCP_REQUEST_STATE_COMPLETE) {
				/* RECOGNIZE failed to start */
				if (recog_hdr->completion_cause == RECOGNIZER_COMPLETION_CAUSE_UNKNOWN) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) RECOGNIZE failed: status = %d\n", schannel->name, 	message->start_line.status_code);
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) RECOGNIZE failed: status = %d, completion-cause = %03d\n", schannel->name, message->start_line.status_code, recog_hdr->completion_cause);
				}
				speech_channel_set_state(schannel, SPEECH_CHANNEL_ERROR);
			} else if (message->start_line.request_state == MRCP_REQUEST_STATE_PENDING) {
				/* RECOGNIZE is queued */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) RECOGNIZE PENDING\n", schannel->name);
			} else {
				/* received unexpected request_state */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) unexpected RECOGNIZE request state: %d\n", schannel->name, message->start_line.request_state);
				speech_channel_set_state(schannel, SPEECH_CHANNEL_ERROR);
			}
		} else if (message->start_line.method_id == RECOGNIZER_STOP) {
			/* received response to the STOP request */
			if (message->start_line.request_state == MRCP_REQUEST_STATE_COMPLETE) {
				/* got COMPLETE */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) RECOGNIZE STOPPED\n", schannel->name);
				speech_channel_set_state(schannel, SPEECH_CHANNEL_READY);
			} else {
				/* received unexpected request state */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) unexpected STOP request state: %d\n", schannel->name, message->start_line.request_state);
				speech_channel_set_state(schannel, SPEECH_CHANNEL_ERROR);
				}
		} else if (message->start_line.method_id == RECOGNIZER_START_INPUT_TIMERS) {
			/* received response to START-INPUT-TIMERS request */
			if (message->start_line.request_state == MRCP_REQUEST_STATE_COMPLETE) {
				if (message->start_line.status_code >= 200 && message->start_line.status_code <= 299) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) timers started\n", schannel->name);
					recog_channel_set_timers_started(schannel);
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) timers failed to start, status code = %d\n", schannel->name, message->start_line.status_code);
				}
			}
		} else if (message->start_line.method_id == RECOGNIZER_DEFINE_GRAMMAR) {
			/* received response to DEFINE-GRAMMAR request */
			if (message->start_line.request_state == MRCP_REQUEST_STATE_COMPLETE) {
				if (message->start_line.status_code >= 200 && message->start_line.status_code <= 299) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) grammar loaded\n", schannel->name);
					speech_channel_set_state(schannel, SPEECH_CHANNEL_READY);
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) grammar failed to load, status code = %d\n", schannel->name, message->start_line.status_code);
					speech_channel_set_state(schannel, SPEECH_CHANNEL_ERROR);
				}
			}
		} else {
			/* received unexpected response */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) unexpected response, method_id = %d\n", schannel->name, (int)message->start_line.method_id);
			speech_channel_set_state(schannel, SPEECH_CHANNEL_ERROR);
		}
	} else if (message->start_line.message_type == MRCP_MESSAGE_TYPE_EVENT) {
		/* received MRCP event */
		if (message->start_line.method_id == RECOGNIZER_RECOGNITION_COMPLETE) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) RECOGNITION COMPLETE, Completion-Cause: %03d\n", schannel->name, recog_hdr->completion_cause);
			if (message->body.length > 0) {
				if (message->body.buf[message->body.length - 1] == '\0') {
					recog_channel_set_results(schannel, message->body.buf);
				} else {
					/* string is not null terminated */
					char *result = (char *)switch_core_alloc(schannel->memory_pool, message->body.length + 1);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) Recognition result is not null-terminated.  Appending null terminator.\n", schannel->name);
					strncpy(result, message->body.buf, message->body.length);
					result[message->body.length] = '\0';
					recog_channel_set_results(schannel, result);
				}
			} else {
				char *completion_cause = switch_mprintf("Completion-Cause: %03d", recog_hdr->completion_cause);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) No result\n", schannel->name);
				recog_channel_set_results(schannel, completion_cause);
				switch_safe_free(completion_cause);
			}
			speech_channel_set_state(schannel, SPEECH_CHANNEL_READY);
		} else if (message->start_line.method_id == RECOGNIZER_START_OF_INPUT) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) START OF INPUT\n", schannel->name);
			recog_channel_set_start_of_input(schannel);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) unexpected event, method_id = %d\n", schannel->name, (int)message->start_line.method_id);
			speech_channel_set_state(schannel, SPEECH_CHANNEL_ERROR);
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) unexpected message type, message_type = %d\n", schannel->name, message->start_line.message_type);
		speech_channel_set_state(schannel, SPEECH_CHANNEL_ERROR);
	}

	return TRUE;
}

/**
 * UniMRCP callback requesting next frame for speech recognition
 *
 * @param stream the UniMRCP stream
 * @param frame the frame to fill
 * @return TRUE
 */
static apt_bool_t recog_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame)
{
	speech_channel_t *schannel = (speech_channel_t *)stream->obj;
	switch_size_t to_read = frame->codec_frame.size;
	
	/* grab the data.  pad it if there isn't enough */
	if (speech_channel_read(schannel, frame->codec_frame.buffer, &to_read, 0) == SWITCH_STATUS_SUCCESS) {
		if (to_read < frame->codec_frame.size) {
			memset((uint8_t *)frame->codec_frame.buffer + to_read, schannel->silence, frame->codec_frame.size - to_read);
		}
		frame->type |= MEDIA_FRAME_TYPE_AUDIO;
	}

	return TRUE;
}

/**
 * Link the recognizer module interface to FreeSWITCH and UniMRCP
 */
static switch_status_t recog_load(switch_loadable_module_interface_t *module_interface, switch_memory_pool_t *pool)
{
	/* link to FreeSWITCH ASR / TTS callbacks */
	switch_asr_interface_t *asr_interface = NULL;
	if ((asr_interface = (switch_asr_interface_t *)switch_loadable_module_create_interface(module_interface, SWITCH_ASR_INTERFACE)) == NULL) {
		return SWITCH_STATUS_FALSE;
	}
	asr_interface->interface_name = MOD_UNIMRCP;
	asr_interface->asr_open = recog_asr_open;
	asr_interface->asr_load_grammar = recog_asr_load_grammar;
	asr_interface->asr_unload_grammar = recog_asr_unload_grammar;
	asr_interface->asr_close = recog_asr_close;
	asr_interface->asr_feed = recog_asr_feed;
	asr_interface->asr_resume = recog_asr_resume;
	asr_interface->asr_pause = recog_asr_pause;
	asr_interface->asr_check_results = recog_asr_check_results;
	asr_interface->asr_get_results = recog_asr_get_results;
	asr_interface->asr_start_input_timers = recog_asr_start_input_timers;
	asr_interface->asr_text_param = recog_asr_text_param;
	asr_interface->asr_numeric_param = recog_asr_numeric_param;
	asr_interface->asr_float_param = recog_asr_float_param;
	
	/* Create the recognizer application and link its callbacks */
	if ((globals.recog.app = mrcp_application_create(recog_message_handler, (void *)0, pool)) == NULL) {
		return SWITCH_STATUS_FALSE;
	}
	globals.recog.dispatcher.on_session_update = NULL;
	globals.recog.dispatcher.on_session_terminate = speech_on_session_terminate;
	globals.recog.dispatcher.on_channel_add = speech_on_channel_add;
	globals.recog.dispatcher.on_channel_remove = speech_on_channel_remove;
	globals.recog.dispatcher.on_message_receive = recog_on_message_receive;
	globals.recog.audio_stream_vtable.destroy = NULL;
	globals.recog.audio_stream_vtable.open_rx = NULL;
	globals.recog.audio_stream_vtable.close_rx = NULL;
	globals.recog.audio_stream_vtable.read_frame = recog_stream_read;
	globals.recog.audio_stream_vtable.open_tx = NULL;
	globals.recog.audio_stream_vtable.close_tx = NULL;
	globals.recog.audio_stream_vtable.write_frame = NULL;
	mrcp_client_application_register(globals.mrcp_client, globals.recog.app, "recog");
	
	/* map FreeSWITCH params or old params to MRCPv2 param */
	switch_core_hash_init_case(&globals.recog.fs_param_map, pool, SWITCH_FALSE /* case_sensitive */);
	/* MRCPv1 param */
	switch_core_hash_insert(globals.recog.fs_param_map, "recognizer-start-timers", "start-input-timers");
	
	/* map MRCP params to UniMRCP ID */
	switch_core_hash_init_case(&globals.recog.param_id_map, pool, SWITCH_FALSE /* case_sensitive */);
	switch_core_hash_insert(globals.recog.param_id_map, "Confidence-Threshold", unimrcp_param_id_create(RECOGNIZER_HEADER_CONFIDENCE_THRESHOLD, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Sensitivity-Level", unimrcp_param_id_create(RECOGNIZER_HEADER_SENSITIVITY_LEVEL, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Speed-Vs-Accuracy", unimrcp_param_id_create(RECOGNIZER_HEADER_SPEED_VS_ACCURACY, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "N-Best-List-Length", unimrcp_param_id_create(RECOGNIZER_HEADER_N_BEST_LIST_LENGTH, pool));	
	switch_core_hash_insert(globals.recog.param_id_map, "No-Input-Timeout", unimrcp_param_id_create(RECOGNIZER_HEADER_NO_INPUT_TIMEOUT, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Recognition-Timeout", unimrcp_param_id_create(RECOGNIZER_HEADER_RECOGNITION_TIMEOUT, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Waveform-Url", unimrcp_param_id_create(RECOGNIZER_HEADER_WAVEFORM_URI, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Completion-Cause", unimrcp_param_id_create(RECOGNIZER_HEADER_COMPLETION_CAUSE, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Recognizer-Context-Block", unimrcp_param_id_create(RECOGNIZER_HEADER_RECOGNIZER_CONTEXT_BLOCK, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Start-Input-Timers", unimrcp_param_id_create(RECOGNIZER_HEADER_START_INPUT_TIMERS, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Speech-Complete-Timeout", unimrcp_param_id_create(RECOGNIZER_HEADER_SPEECH_COMPLETE_TIMEOUT, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Speech-Incomplete-Timeout", unimrcp_param_id_create(RECOGNIZER_HEADER_SPEECH_INCOMPLETE_TIMEOUT, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "DTMF-Interdigit-Timeout", unimrcp_param_id_create(RECOGNIZER_HEADER_DTMF_INTERDIGIT_TIMEOUT, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "DTMF-Term-Timeout", unimrcp_param_id_create(RECOGNIZER_HEADER_DTMF_TERM_TIMEOUT, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "DTMF-Term-Char", unimrcp_param_id_create(RECOGNIZER_HEADER_DTMF_TERM_CHAR, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Failed-Uri", unimrcp_param_id_create(RECOGNIZER_HEADER_FAILED_URI, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Failed-Uri-Cause", unimrcp_param_id_create(RECOGNIZER_HEADER_FAILED_URI_CAUSE, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Save-Waveform", unimrcp_param_id_create(RECOGNIZER_HEADER_SAVE_WAVEFORM, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "New-Audio-Channel", unimrcp_param_id_create(RECOGNIZER_HEADER_NEW_AUDIO_CHANNEL, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Speech-Language", unimrcp_param_id_create(RECOGNIZER_HEADER_SPEECH_LANGUAGE, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Input-Type", unimrcp_param_id_create(RECOGNIZER_HEADER_INPUT_TYPE, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Input-Waveform-Uri", unimrcp_param_id_create(RECOGNIZER_HEADER_INPUT_WAVEFORM_URI, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Completion-Reason", unimrcp_param_id_create(RECOGNIZER_HEADER_COMPLETION_REASON, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Media-Type", unimrcp_param_id_create(RECOGNIZER_HEADER_MEDIA_TYPE, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Ver-Buffer-Utterance", unimrcp_param_id_create(RECOGNIZER_HEADER_VER_BUFFER_UTTERANCE, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Recognition-Mode", unimrcp_param_id_create(RECOGNIZER_HEADER_RECOGNITION_MODE, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Cancel-If-Queue", unimrcp_param_id_create(RECOGNIZER_HEADER_CANCEL_IF_QUEUE, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Hotword-Max-Duration", unimrcp_param_id_create(RECOGNIZER_HEADER_HOTWORD_MAX_DURATION, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Hotword-Min-Duration", unimrcp_param_id_create(RECOGNIZER_HEADER_HOTWORD_MIN_DURATION, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Interpret-Text", unimrcp_param_id_create(RECOGNIZER_HEADER_INTERPRET_TEXT, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "DTMF-Buffer-Time", unimrcp_param_id_create(RECOGNIZER_HEADER_DTMF_BUFFER_TIME, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Clear-DTMF-Buffer", unimrcp_param_id_create(RECOGNIZER_HEADER_CLEAR_DTMF_BUFFER, pool));
	switch_core_hash_insert(globals.recog.param_id_map, "Early-No-Match", unimrcp_param_id_create(RECOGNIZER_HEADER_EARLY_NO_MATCH, pool));

	return SWITCH_STATUS_SUCCESS;
}

/**
 * Shutdown the recognizer
 */
static switch_status_t recog_shutdown()
{
	if (globals.recog.fs_param_map) {
		switch_core_hash_destroy(&globals.recog.fs_param_map);
	}
	if (globals.recog.param_id_map) {
		switch_core_hash_destroy(&globals.recog.param_id_map);
	}
	return SWITCH_STATUS_SUCCESS;
}

/**
 * Process the XML configuration for this module
 * Uses the instructions[] defined in this module to process the configuration. 
 *
 * @return SWITCH_STATUS_SUCCESS if the configuration is OK
 */
static switch_status_t mod_unimrcp_do_config()
{
	switch_xml_t cfg, xml, settings;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!(xml = switch_xml_open_cfg(CONFIG_FILE, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Could not open %s\n", CONFIG_FILE);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	if ((settings = switch_xml_child(cfg, "settings"))) {
		if (switch_xml_config_parse(switch_xml_child(settings, "param"), SWITCH_FALSE, instructions) == SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,"Config parsed ok!\n");
			goto done;
		}
	}

 done:

	if (xml) {
		switch_xml_free(xml);
	}

	return status;
}

#define DEFAULT_LOCAL_IP_ADDRESS  "127.0.0.1"
#define DEFAULT_REMOTE_IP_ADDRESS "127.0.0.1"
#define DEFAULT_SIP_LOCAL_PORT    5090
#define DEFAULT_SIP_REMOTE_PORT   5060
#define DEFAULT_RTP_PORT_MIN      4000
#define DEFAULT_RTP_PORT_MAX      5000

#define DEFAULT_SOFIASIP_UA_NAME  "FreeSWITCH"
#define DEFAULT_SDP_ORIGIN        "FreeSWITCH"
#define DEFAULT_RESOURCE_LOCATION "media"

/**
 * Get IP address from IP address value
 *
 * @param value "auto" or IP address
 * @param pool the memory pool to use
 * @return IP address
 */
static char *ip_addr_get(const char *value, apr_pool_t *pool)
{
	if(!value || strcasecmp(value,"auto") == 0) {
		char *addr = DEFAULT_LOCAL_IP_ADDRESS;
		apt_ip_get(&addr,pool);
		return addr;
	}
	return apr_pstrdup(pool,value);
}

/**
 * set mod_unimrcp-specific profile configuration
 * 
 * @param profile the MRCP profile to configure
 * @param param the param name
 * @param val the param value
 * @param pool the memory pool to use
 */
static int process_profile_config(profile_t *profile, const char *param, const char *val, switch_memory_pool_t *pool)
{
	int mine = 1;
	if (strcasecmp(param, "jsgf-mime-type") == 0) {
		profile->jsgf_mime_type = switch_core_strdup(pool, val);
	} else if (strcasecmp(param, "gsl-mime-type") == 0) {
		profile->gsl_mime_type = switch_core_strdup(pool, val);
	} else if (strcasecmp(param, "srgs-xml-mime-type") == 0) {
		profile->srgs_xml_mime_type = switch_core_strdup(pool, val);
	} else if (strcasecmp(param, "srgs-mime-type") == 0) {
		profile->srgs_mime_type = switch_core_strdup(pool, val);
	} else {
		mine = 0;
	}

	return mine;
}

/**
 * set RTP config struct with param, val pair 
 * @param client the MRCP client
 * @param rtp_config the config struct to set
 * @param param the param name
 * @param val the param value
 * @param pool memory pool to use
 * @return true if this param belongs to RTP config
 */
static int process_rtp_config(mrcp_client_t *client, mpf_rtp_config_t *rtp_config, const char *param, const char *val, apr_pool_t *pool)
{
	int mine = 1;
	if (strcasecmp(param, "rtp-ip") == 0) {
		apt_string_set(&rtp_config->ip, ip_addr_get(val, pool));
	} else if (strcasecmp(param, "rtp-ext-ip") == 0) {
		apt_string_set(&rtp_config->ext_ip, ip_addr_get(val, pool));
	} else if (strcasecmp(param, "rtp-port-min") == 0) {
		rtp_config->rtp_port_min = (apr_port_t)atol(val);
	} else if (strcasecmp(param, "rtp-port-max") == 0) {
		rtp_config->rtp_port_max = (apr_port_t)atol(val);
	} else if (strcasecmp(param, "playout-delay") == 0) {
		rtp_config->jb_config.initial_playout_delay = atol(val);
	} else if (strcasecmp(param, "min-playout-delay") == 0) {
		rtp_config->jb_config.min_playout_delay = atol(val);
	} else if (strcasecmp(param, "max-playout-delay") == 0) {
		rtp_config->jb_config.max_playout_delay = atol(val);
	} else if (strcasecmp(param, "codecs") == 0) {
		const mpf_codec_manager_t *codec_manager = mrcp_client_codec_manager_get(client);
		if(codec_manager) {
			mpf_codec_manager_codec_list_load(codec_manager, &rtp_config->codec_list, val, pool);
		}
	} else if (strcasecmp(param, "ptime") == 0) {
		rtp_config->ptime = (apr_uint16_t)atol(val);
	} else {
		mine = 0;
	}

	return mine;
}

/**
 * set RTSP client config struct with param, val pair
 * @param config the config struct to set
 * @param param the param name
 * @param val the param value
 * @param pool memory pool to use
 * @return true if this param belongs to RTSP config 
 */
static int process_mrcpv1_config(rtsp_client_config_t *config, const char *param, const char *val, apr_pool_t *pool)
{
	int mine = 1;
	if (strcasecmp(param, "server-ip") == 0) {
		config->server_ip = ip_addr_get(val, pool);
	} else if (strcasecmp(param, "server-port") == 0) {
		config->server_port = (apr_port_t)atol(val);
	} else if (strcasecmp(param, "resource-location") == 0) {
		config->resource_location = apr_pstrdup(pool, val);
	} else if (strcasecmp(param, "sdp-origin") == 0) {
		config->origin = apr_pstrdup(pool, val);
	} else if (strcasecmp(param, "max-connection-count") == 0) {
		config->max_connection_count = atol(val);
	} else if (strcasecmp(param, "force-destination") == 0) {
		config->force_destination = atoi(val);
	} else if (strcasecmp(param, "speechsynth") == 0 || strcasecmp(param, "speechrecog") == 0) {
		apr_table_set(config->resource_map, param, val);
	} else {
		mine = 0;
	}
	return mine;
}

/**
 * set SofiaSIP client config struct with param, val pair
 * @param config the config struct to set
 * @param param the param name
 * @param val the param value
 * @param pool memory pool to use
 * @return true if this param belongs to SofiaSIP config
 */
static int process_mrcpv2_config(mrcp_sofia_client_config_t *config, const char *param, const char *val, apr_pool_t *pool)
{
	int mine = 1;
	if (strcasecmp(param, "client-ip") == 0) {
		config->local_ip = ip_addr_get(val, pool);
	} else if (strcasecmp(param,"client-ext-ip") == 0) {
		config->ext_ip = ip_addr_get(val, pool);
	} else if (strcasecmp(param,"client-port") == 0) {
		config->local_port = (apr_port_t)atol(val);
	} else if (strcasecmp(param, "server-ip") == 0) {
		config->remote_ip = ip_addr_get(val, pool);
	} else if (strcasecmp(param, "server-port") == 0) {
		config->remote_port = (apr_port_t)atol(val);
	} else if (strcasecmp(param, "server-username") == 0) {
		config->remote_user_name = apr_pstrdup(pool, val);
	} else if (strcasecmp(param, "force-destination") == 0) {
		config->force_destination = atoi(val);
	} else if (strcasecmp(param, "sip-transport") == 0) {
		config->transport = apr_pstrdup(pool, val);
	} else if (strcasecmp(param, "ua-name") == 0) {
		config->user_agent_name = apr_pstrdup(pool, val);
	} else if (strcasecmp(param, "sdp-origin") == 0) {
		config->origin = apr_pstrdup(pool, val);
	} else {
		mine = 0;
	}
	return mine;
}

/**
 * Create the MRCP client and configure it with profiles defined in FreeSWITCH XML config
 *
 * Some code and ideas borrowed from unimrcp-client.c
 * Please check libs/unimrcp/platform/libunimrcp-client/src/unimrcp-client.c when upgrading
 * the UniMRCP library to ensure nothing new needs to be set up.
 *
 * @return the MRCP client
 */
static mrcp_client_t *mod_unimrcp_client_create(switch_memory_pool_t *mod_pool)
{
	switch_xml_t cfg = NULL, xml = NULL, profiles = NULL, profile = NULL;
	mrcp_client_t *client = NULL;
	apr_pool_t *pool = NULL;
	mrcp_resource_factory_t *resource_factory = NULL;
	mpf_codec_manager_t *codec_manager = NULL;
	apr_size_t max_connection_count = 0;
	apt_bool_t offer_new_connection = FALSE;
	mrcp_connection_agent_t *connection_agent;
	mpf_engine_t *media_engine;
	apt_dir_layout_t *dir_layout;
	
	/* create the client */
	if ((dir_layout = apt_default_dir_layout_create("../", mod_pool)) == NULL) {
		goto done;
	}
	client = mrcp_client_create(dir_layout);
	if (!client)
		goto done;

	pool = mrcp_client_memory_pool_get(client);
	if (!pool)
		goto done;

	resource_factory = mrcp_default_factory_create(pool);
	if (resource_factory) {
		mrcp_client_resource_factory_register(client, resource_factory);
	}

	codec_manager = mpf_engine_codec_manager_create(pool);
	if (codec_manager) {
		mrcp_client_codec_manager_register(client, codec_manager);
	}

	/* set up MRCPv2 connection agent that will be shared with all profiles */
	if (!switch_strlen_zero(globals.unimrcp_max_connection_count)) {
		max_connection_count = atoi(globals.unimrcp_max_connection_count);
	}
	if (max_connection_count <= 0) {
		max_connection_count = 100;
	}
	if (!switch_strlen_zero(globals.unimrcp_offer_new_connection)) {
		offer_new_connection = strcasecmp("true", globals.unimrcp_offer_new_connection);
	}
	connection_agent = mrcp_client_connection_agent_create(max_connection_count, offer_new_connection, pool);
	if (connection_agent) {
		mrcp_client_connection_agent_register(client, connection_agent, "MRCPv2ConnectionAgent");
	}

	/* Set up the media engine that will be shared with all profiles */
	media_engine = mpf_engine_create(pool);
	if (media_engine) {
		mrcp_client_media_engine_register(client, media_engine, "MediaEngine");
	}

	/* configure the client profiles */
	if (!(xml = switch_xml_open_cfg(CONFIG_FILE, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Could not open %s\n", CONFIG_FILE);
		client = NULL;
		goto done;
	}
	if ((profiles = switch_xml_child(cfg, "profiles"))) {
		for (profile = switch_xml_child(profiles, "profile"); profile; profile = switch_xml_next(profile)) {
			/* a profile is a signaling agent + termination factory + media engine + connection agent (MRCPv2 only) */
			mrcp_sig_agent_t *agent = NULL;
			mpf_termination_factory_t *termination_factory = NULL;
			mrcp_profile_t * mprofile = NULL;
			mpf_rtp_config_t *rtp_config = NULL;
			profile_t *mod_profile = NULL;
			
			/* get profile attributes */
			const char *name = apr_pstrdup(pool, switch_xml_attr(profile, "name"));
			const char *version = switch_xml_attr(profile, "version");
			if (switch_strlen_zero(name) || switch_strlen_zero(version)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "<profile> missing name or version attribute\n");
				client = NULL;
				goto done;
			}

			/* prepare mod_unimrcp's profile for configuration */
			profile_create(&mod_profile, name, mod_pool);
			switch_core_hash_insert(globals.profiles, mod_profile->name, mod_profile);

			/* create RTP config, common to MRCPv1 and MRCPv2 */
			rtp_config = mpf_rtp_config_create(pool);
			rtp_config->rtp_port_min = DEFAULT_RTP_PORT_MIN;
			rtp_config->rtp_port_max = DEFAULT_RTP_PORT_MAX;
			apt_string_set(&rtp_config->ip, DEFAULT_LOCAL_IP_ADDRESS);
			
			if (strcmp("1", version) == 0) {
				/* MRCPv1 configuration */
				rtsp_client_config_t *config = mrcp_unirtsp_client_config_alloc(pool);
				switch_xml_t param = NULL;
				config->origin = DEFAULT_SDP_ORIGIN;
				config->resource_location = DEFAULT_RESOURCE_LOCATION;

				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Loading MRCPv1 profile: %s\n", name);
				for (param = switch_xml_child(profile, "param"); param; param = switch_xml_next(param)) {
					const char *param_name = switch_xml_attr(param, "name");
					const char *param_value = switch_xml_attr(param, "value");
					if (switch_strlen_zero(param_name)) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing param name\n");
						client = NULL;
						goto done;
					}
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Loading Param %s:%s\n", param_name, param_value);
					if (!process_mrcpv1_config(config, param_name, param_value, pool) && 
						!process_rtp_config(client, rtp_config, param_name, param_value, pool) &&
						!process_profile_config(mod_profile, param_name, param_value, mod_pool)) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Unknown param %s\n", param_name);
					}
				}
				agent = mrcp_unirtsp_client_agent_create(config, pool);
			} else if (strcmp("2", version) == 0) {
				/* MRCPv2 configuration */
				mrcp_sofia_client_config_t *config = mrcp_sofiasip_client_config_alloc(pool);
				switch_xml_t param = NULL;
				config->local_ip = DEFAULT_LOCAL_IP_ADDRESS;
				config->local_port = DEFAULT_SIP_LOCAL_PORT;
				config->remote_ip = DEFAULT_REMOTE_IP_ADDRESS;
				config->remote_port = DEFAULT_SIP_REMOTE_PORT;
				config->ext_ip = NULL;
				config->user_agent_name = DEFAULT_SOFIASIP_UA_NAME;
				config->origin = DEFAULT_SDP_ORIGIN;
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Loading MRCPv2 profile: %s\n", name);
				for (param = switch_xml_child(profile, "param"); param; param = switch_xml_next(param)) {
					const char *param_name = switch_xml_attr(param, "name");
					const char *param_value = switch_xml_attr(param, "value");
					if (switch_strlen_zero(param_name)) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing param name\n");
						client = NULL;
						goto done;
					}
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Loading Param %s:%s\n", param_name, param_value);
					if (!process_mrcpv2_config(config, param_name, param_value, pool) &&
						!process_rtp_config(client, rtp_config, param_name, param_value, pool) &&
						!process_profile_config(mod_profile, param_name, param_value, mod_pool)) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Unknown param %s\n", param_name);
					}
				}
				agent = mrcp_sofiasip_client_agent_create(config, pool);
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "version must be either \"1\" or \"2\"\n");
				client = NULL;
				goto done;
			}

			termination_factory = mpf_rtp_termination_factory_create(rtp_config, pool); 
			if (termination_factory) {
				mrcp_client_rtp_factory_register(client, termination_factory, name);
			}
			if (agent) {
				mrcp_client_signaling_agent_register(client, agent, name);
			}

			/* create the profile and register it */
			mprofile = mrcp_client_profile_create(NULL, agent, connection_agent, media_engine, termination_factory, pool);
			if (mprofile) {
				mrcp_client_profile_register(client, mprofile, name);
			}
		}
	}

 done:

	if (xml) {
		switch_xml_free(xml);
	}

	return client;
}

/**
 * Macro expands to: switch_status_t mod_unimrcp_load(switch_loadable_module_interface_t **module_interface, switch_memory_pool_t *pool) 
 */
SWITCH_MODULE_LOAD_FUNCTION(mod_unimrcp_load)
{
	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	memset(&globals, 0, sizeof(globals));
	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_UNNESTED, pool);
	globals.speech_channel_number = 0;
	switch_core_hash_init_case(&globals.profiles, pool, SWITCH_FALSE /* case_sensitive */);

	/* get MRCP module configuration */
	mod_unimrcp_do_config();
	if (switch_strlen_zero(globals.unimrcp_default_synth_profile)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing default-tts-profile\n");
		return SWITCH_STATUS_FALSE;
	}
	if (switch_strlen_zero(globals.unimrcp_default_recog_profile)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing default-asr-profile\n");
		return SWITCH_STATUS_FALSE;
	}

	/* link UniMRCP logs to FreeSWITCH */
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "UniMRCP log level = %s\n", globals.unimrcp_log_level);
	if (apt_log_instance_create(APT_LOG_OUTPUT_NONE, str_to_log_level(globals.unimrcp_log_level), pool) == FALSE) {
		/* already created */
		apt_log_priority_set(str_to_log_level(globals.unimrcp_log_level));
	}
	apt_log_ext_handler_set(unimrcp_log);

	/* Create the MRCP client */
	if ((globals.mrcp_client = mod_unimrcp_client_create(pool)) == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create mrcp client\n");
		return SWITCH_STATUS_FALSE;
	}

	/* Create the synthesizer interface */
	if (synth_load(*module_interface, pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	/* Create the recognizer interface */
	if (recog_load(*module_interface, pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	/* Start the client stack */
	mrcp_client_start(globals.mrcp_client);

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/**
 * Called when the system shuts down
 * Macro expands to: switch_status_t mod_unimrcp_shutdown() 
 */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_unimrcp_shutdown)
{
	synth_shutdown();
	recog_shutdown();

	/* Stop the MRCP client stack */
	mrcp_client_shutdown(globals.mrcp_client);
	mrcp_client_destroy(globals.mrcp_client);
	globals.mrcp_client = 0;

	switch_mutex_destroy(globals.mutex);
	switch_core_hash_destroy(&globals.profiles);

	return SWITCH_STATUS_SUCCESS;
}

/**
 * If it exists, this is called in it's own thread when the module-load completes
 * If it returns anything but SWITCH_STATUS_TERM it will be called again automatically
 * Macro expands to: switch_status_t mod_unimrcp_runtime()
 */
SWITCH_MODULE_RUNTIME_FUNCTION(mod_unimrcp_runtime)
{
	return SWITCH_STATUS_TERM;
}

/**
 * Translate log level string to enum
 * @param level log level string
 * @return log level enum
 */
static apt_log_priority_e str_to_log_level(const char *level)
{
	if (strcmp(level, "EMERGENCY") == 0) {
		return APT_PRIO_EMERGENCY;
	} else if (strcmp(level, "ALERT") == 0) {
		return APT_PRIO_ALERT;
	} else if (strcmp(level, "CRITICAL") == 0) {
		return APT_PRIO_CRITICAL;
	} else if (strcmp(level, "ERROR") == 0) {
		return APT_PRIO_ERROR;
	} else if (strcmp(level, "WARNING") == 0) {
		return APT_PRIO_WARNING;
	} else if (strcmp(level, "NOTICE") == 0) {
		return APT_PRIO_NOTICE;
	} else if (strcmp(level, "INFO") == 0) {
		return APT_PRIO_INFO;
	} else if (strcmp(level, "DEBUG") == 0) {
		return APT_PRIO_DEBUG;
	}
	return APT_PRIO_DEBUG;
}

/**
 * Connects UniMRCP logging to FreeSWITCH
 * @return TRUE
 */
static apt_bool_t unimrcp_log(const char *file, int line, const char *id, apt_log_priority_e priority, const char *format, va_list arg_ptr)
{
	switch_log_level_t level;
	char log_message[4096] = { 0 }; /* same size as MAX_LOG_ENTRY_SIZE in UniMRCP apt_log.c */
	size_t msglen;

	if (switch_strlen_zero(format)) {
		return TRUE;
	}

	switch(priority) {
	case APT_PRIO_EMERGENCY:
		/* pass through */
	case APT_PRIO_ALERT:
		/* pass through */
	case APT_PRIO_CRITICAL:
		level = SWITCH_LOG_CRIT;
		break;
	case APT_PRIO_ERROR:
		level = SWITCH_LOG_ERROR;
		break;
	case APT_PRIO_WARNING:
		level = SWITCH_LOG_WARNING;
		break;
	case APT_PRIO_NOTICE:
		level = SWITCH_LOG_NOTICE;
		break;
	case APT_PRIO_INFO:
		level = SWITCH_LOG_INFO;
		break;
	case APT_PRIO_DEBUG:
		/* pass through */
	default:
		level = SWITCH_LOG_DEBUG;
		break;
	}
	
	/* apr_vsnprintf supports format extensions required by UniMRCP */ 
	apr_vsnprintf(log_message, sizeof(log_message), format, arg_ptr);
	msglen = strlen(log_message);
	if (msglen >= 2 && log_message[msglen - 2] == '\\' && log_message[msglen - 1] == 'n') {
		/* log_message already ends in \n */
		switch_log_printf(SWITCH_CHANNEL_ID_LOG, file, "", line, id, level, "%s", log_message);
	} else if (msglen > 0) {
		/* log message needs \n appended */
		switch_log_printf(SWITCH_CHANNEL_ID_LOG, file, "", line, id, level, "%s\n", log_message);
	}
	
	return TRUE;
}

/**
 * @return the next number to assign the channel
 */
static int get_next_speech_channel_number(void)
{
	int num;
	switch_mutex_lock(globals.mutex);
	num = globals.speech_channel_number;
	if (globals.speech_channel_number == INT_MAX) {
		globals.speech_channel_number = 0;
	} else {
		globals.speech_channel_number++;
	}
	switch_mutex_unlock(globals.mutex);

	return num;
}

/**
 * Create a parameter id
 * 
 * @param id the UniMRCP ID
 * @return the pair
 */
static unimrcp_param_id_t *unimrcp_param_id_create(int id, switch_memory_pool_t *pool)
{
	unimrcp_param_id_t *param = (unimrcp_param_id_t *)switch_core_alloc(pool, sizeof(unimrcp_param_id_t));
	if (param) {
		param->id = id;
	}
	return param;
}


/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */