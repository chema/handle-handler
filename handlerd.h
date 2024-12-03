#ifndef HANDLERD_H
#define HANDLERD_H

#include <regex.h>
#include <microhttpd.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <ctype.h>

#ifndef PORT
#define PORT 8123  		// Default port for production
#endif

#define POSTBUFFERSIZE  1024

#define URL_WELL_KNOWN_ATPROTO 			"/.well-known/atproto-did"
#define _URL_WELL_KNOWN_		"https://%s/.well-known/atproto-did"
#define URL_MAX_SIZE		512
#define DATA_MAX_SIZE		256

#define MAX_SIZE_DID_PLC	32    // Explicit max from https://web.plc.directory/spec/v0.1/did-plc
#define MAXDIDSIZE			256   // Implied desired max from https://atproto.com/specs/did (UNUSED)

#define CONTENT_TEXT		"text/plain"
#define CONTENT_HTML		"text/html"



// DEFINE THE REGULAR EXPRESSION PATTERNS FOR DATA VALIDATION
// Unused generic DID pattern based on recommendation from https://atproto.com/specs/did (note lack of limit length and case control)
#define VALID_PATTERN_DID			"^did:[a-z]+:[a-zA-Z0-9]*[a-zA-Z0-9._-]$"
// DID PLC pattern based on https://web.plc.directory/spec/v0.1/did-plc (exactly 32 characters, "did:plc:" prefix+24 base 32 encoding set characters)
#define VALID_PATTERN_DID_PLC		"^did:plc:[a-zA-Z2-7]{24}$"
// Handle pattern based on regex from https://atproto.com/specs/handle, but for only one segment 
// NOT ACTUALLY A HANDLE, SINCE IT EXCLUDES THE DOMAIN NAME, THIS IS A "SEGMENT" OR "A LABEL"
#define VALID_PATTERN_HANDLE		"^([a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)$"
#define VALID_PATTERN_LABEL			"^([a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)$"
// FULL Handle pattern (including the domain name) based on regex from https://atproto.com/specs/handle. Added a second \.
#define VALID_PATTERN_FULL_HANDLE	"^([a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\\.)+[a-zA-Z]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$"
// DID PLC Syntax based on https://web.plc.directory/spec/v0.1/did-plc: exactly 32 characters, "did:plc:" prefix+24 base 32 encoding set characters.
#define DID_PLC_SPEC_PATTERN		"did:plc:[a-zA-Z2-7]{24}"

// STATIC HTML FILENAMES
#define STATIC_REGISTER		"static/register.html"
#define STATIC_DELETE		"static/delete.html"
#define STATIC_ERROR		"static/error.html"
#define STATIC_SUCCESS		"static/success.html"
#define STATIC_ACTIVE		"static/active.html"
#define STATIC_RESERVED		"static/reserved.html"
#define STATIC_NOTFOUND		"static/404.html"

#define TOKEN_LENGTH 10

#define TRUE 1
#define FALSE 0

#define HANDLE_ACTIVE 1
#define HANDLE_INACTIVE 0
#define HANDLE_ERROR -1

#define VALUE_ACTIVE 1
#define VALUE_INACTIVE 0
#define VALUE_ERROR -1

#define KEY_VALID 0
#define KEY_INVALID 1

#define DATABASE_SUCCESS 0
#define DATABASE_ERROR 1

#define RECORD_ADDED 			0
#define INVALID_DID				1
#define INVALID_HANDLE  		2
#define INVALID_LABEL			3
#define RECORD_DATABASE_ERROR	4
#define DUPLICATE_RECORD		5

#define ERROR_INVALID_DID "Error: Invalid DID."
#define ERROR_INVALID_LABEL "Error: Invalid label."
#define ERROR_INVALID_HANDLE "Error: Invalid handle."
#define ERROR_NULL_OR_EMPTY_DATA "Error: Data is null or empty."
#define ERROR_DATABASE "Error: Database operation failed."
#define ERROR_DUPLICATE_DATA "Error: Duplicate record found, unable to process."
#define ERROR_REQUEST_FAILED "Error: Request failed."

#define ERROR_INVALID_DID_ENTERED "The bsky.social handle or DID you entered appears to be invalid. Please double-check it and try again. If the issue persists, ensure the handle is active and properly set up."

// TYPES OF CONNECTIONS ACCEPTED.
enum connectionType {
  GET,
  POST
} ;

// NOT USED YET
typedef enum {
  NOT_LOCKED = 0,
  LOCKED = 1
} lockedStatus;

// NOT USED YET
typedef enum {
  HANDLE_COLUMN = 0,
  DID_COLUMN = 1,
  TOKEN_COLUMN = 2,
  EMAIL_COLUMN = 3,
  LOCKED_COLUMN = 4
} tableColumns;

typedef enum {
    RECORD_VALID,
	RECORD_INVALID_DID,
	RECORD_INVALID_LABEL,
	RECORD_INVALID_HANDLE,
	RECORD_NULL_DATA,
	RECORD_EMPTY_DATA,
	RECORD_ERROR_DATABASE,
	RECORD_ERROR_DUPLICATE_DATA
} validatorResult;

typedef struct {
    validatorResult result;         // recordValidator result
    char *token; 				    // Token on RECORD_VALID result, NULL otherwise
} newRecordResult;

#endif // SERVER_H