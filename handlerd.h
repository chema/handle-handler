#ifndef HANDLERD_H
#define HANDLERD_H


#include <regex.h>
#include <microhttpd.h>

#define PORT            8123
#define POSTBUFFERSIZE  2048

#define TARGET_URL 			"/.well-known/atproto-did"
#define MAXDIDSIZE			256   // Implied desired max from https://atproto.com/specs/did
#define MAX_SIZE_DID_PLC	32    // Explicit max from https://web.plc.directory/spec/v0.1/did-plc
#define MAXANSWERSIZE   	1024
#define HTML_CONTENT		"text/html"

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


// #define GET             0
// #define POST            1

// STATIC HTML FILENAMES
#define STATIC_REGISTER		"static/register.html"
#define STATIC_DELETE		"static/delete.html"
#define STATIC_ERROR		"static/error.html"
#define STATIC_SUCCESS		"static/success.html"
#define STATIC_ACTIVE		"static/active.html"

#define TOKEN_LENGTH 12

#define TRUE 1
#define FALSE 0

#define HANDLE_ACTIVE 1
#define HANDLE_INACTIVE 0
#define HANDLE_ERROR -1

#define KEY_VALID 0
#define KEY_INVALID 1

#define DATABASE_SUCCESS 0
#define DATABASE_ERROR 1

// NOT USED YET
enum lockedStatus
{
  NOT_LOCKED = 0,
  LOCKED = 1
};

// NOT USED YET
enum tableColumns
{
  HANDLE_COLUMN = 0,
  DID_COLUMN = 1,
  TOKEN_COLUMN = 2,
  EMAIL_COLUMN = 3,
  LOCKED_COLUMN = 4
};

#endif // SERVER_H