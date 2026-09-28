#ifndef CARTOGRAPHER_CARTO_H
#define CARTOGRAPHER_CARTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct carto_context carto_context;
typedef struct carto_document carto_document;

typedef enum carto_status {
    CARTO_OK = 0,
    CARTO_INVALID_ARGUMENT = 1,
    CARTO_NOT_FOUND = 2,
    CARTO_CONFLICT = 3,
    CARTO_UNSUPPORTED_VERSION = 4,
    CARTO_IO_ERROR = 5,
    CARTO_VALIDATION_FAILED = 6,
    CARTO_INTERNAL_ERROR = 100
} carto_status;

typedef struct carto_error {
    int32_t code;
    char* message;
} carto_error;

/* Returns the ABI version; callers should gate optional operations on it. */
uint32_t carto_abi_version(void);

/*
 * config_json may contain {"project_root":"..."}. When omitted, the
 * current working directory is the root. Open and save paths are resolved
 * beneath that root; absolute paths outside it and any '..' component are
 * rejected. The context owns the root policy for all documents it opens.
 */
carto_status carto_context_create(
    const char* config_json,
    carto_context** out_context,
    carto_error* out_error);

carto_status carto_document_open(
    carto_context* context,
    const char* path_utf8,
    carto_document** out_document,
    carto_error* out_error);

carto_status carto_document_query_json(
    carto_document* document,
    const char* request_json,
    char** out_response_json,
    carto_error* out_error);

carto_status carto_document_execute_json(
    carto_document* document,
    const char* request_json,
    char** out_response_json,
    carto_error* out_error);

void carto_document_close(carto_document* document);
void carto_context_destroy(carto_context* context);
/* Messages returned through carto_error and response strings are freed here. */
void carto_free(void* allocation);

#ifdef __cplusplus
}
#endif

#endif
