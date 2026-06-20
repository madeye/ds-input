/*
 * dsime.h — C ABI for the DS Input core engine (Rust crate `dsime`).
 *
 * Stable interface shared by every platform frontend (macOS IMKit, Windows TSF).
 * All strings are UTF-8, NUL-terminated. Pointers returned by the library that
 * are documented as "caller frees" MUST be released with ds_string_free().
 *
 * Threading: ds_session_convert() is non-blocking. The result callback is
 * invoked from a background worker thread; marshal to your UI thread before
 * touching composition state. Calls into a single DsSession from multiple
 * threads must be externally serialized. DsEngine is internally synchronized
 * and may be shared by many sessions.
 */
#ifndef DSIME_H
#define DSIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DsEngine DsEngine;
typedef struct DsSession DsSession;

/* status codes passed to DsConvertCallback */
#define DS_OK            0
#define DS_ERR_NETWORK   1
#define DS_ERR_AUTH      2
#define DS_ERR_API       3
#define DS_ERR_CANCELLED 4
#define DS_ERR_CONFIG    5
#define DS_ERR_INTERNAL  6

/*
 * Result callback.
 *   user_data : the pointer passed to ds_session_convert.
 *   request_id: the id returned by ds_session_convert.
 *   status    : DS_OK or a DS_ERR_* code.
 *   text_utf8 : on DS_OK, the converted Chinese sentence; on error, a human
 *               readable message (may be NULL). Valid ONLY during the call —
 *               copy it if you need it later.
 * The callback runs on a worker thread. It must not call back into the same
 * DsSession synchronously; hop to your UI thread first.
 */
typedef void (*DsConvertCallback)(void *user_data,
                                  uint64_t request_id,
                                  int32_t status,
                                  const char *text_utf8);

/* ---- Engine lifecycle ---------------------------------------------------- */

/* Create an engine. config_path may be NULL to use the per-user default path.
 * Returns NULL on fatal error (see ds_last_error). */
DsEngine *ds_engine_new(const char *config_path);
void      ds_engine_free(DsEngine *engine);

/* Re-read the config file from disk. Returns DS_OK or DS_ERR_CONFIG. */
int32_t   ds_engine_reload_config(DsEngine *engine);

/* Current configuration as a JSON object string. Caller frees. */
char     *ds_engine_get_config_json(DsEngine *engine);

/* Replace configuration from a JSON object string and persist it to disk.
 * Returns DS_OK or DS_ERR_CONFIG (see ds_last_error for details). */
int32_t   ds_engine_set_config_json(DsEngine *engine, const char *json_utf8);

/* The configured config file path (caller frees). */
char     *ds_engine_config_path(DsEngine *engine);

/* ---- Session lifecycle --------------------------------------------------- */

DsSession *ds_session_new(DsEngine *engine);
void       ds_session_free(DsSession *session);

/* Replace the raw pinyin (ASCII) buffer with the full string typed so far. */
void       ds_session_set_input(DsSession *session, const char *pinyin_ascii);

/* The current raw pinyin buffer (caller frees). Never NULL. */
char      *ds_session_get_input(DsSession *session);

/* Kick off async conversion of the current buffer. Cancels any previous
 * in-flight request for this session. Returns a monotonic request id, or 0 if
 * the buffer is empty (callback is not invoked in that case). */
uint64_t   ds_session_convert(DsSession *session,
                              DsConvertCallback callback,
                              void *user_data);

/* Cancel any in-flight request (its callback fires with DS_ERR_CANCELLED). */
void       ds_session_cancel(DsSession *session);

/* Clear the buffer and cancel in-flight work (call after commit / escape). */
void       ds_session_reset(DsSession *session);

/* ---- Utilities ----------------------------------------------------------- */

/* The configured idle debounce in milliseconds the frontend should wait after
 * the last keystroke before calling ds_session_convert. */
uint32_t   ds_engine_debounce_ms(DsEngine *engine);

void        ds_string_free(char *s);
const char *ds_last_error(void); /* thread-local last error, never NULL */

/* SemVer of the core library (static string, do not free). */
const char *ds_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSIME_H */
