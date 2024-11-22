#ifndef HANDLERD_H
#define HANDLERD_H


#include <regex.h>
#include <microhttpd.h>

#define PORT            8123
#define POSTBUFFERSIZE  1024

#define TARGET_URL 			"/.well-known/atproto-did"
#define MAXDIDSIZE			256   // Implied desired max from https://atproto.com/specs/did
#define MAXANSWERSIZE   	1024
#define HTML_CONTENT		"text/html"

// DEFINE THE REGULAR EXPRESSION PATTERNS FOR DATA VALIDATION
// DID pattern based on recommendation from https://atproto.com/specs/did (note lack of limit length and case control)
#define VALID_PATTERN_DID			"^did:[a-z]+:[a-zA-Z0-9._:%-]*[a-zA-Z0-9._-]$"
// Handle pattern based on regex from https://atproto.com/specs/handle, but for only one segment 
#define VALID_PATTERN_HANDLE		"^([a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)$"

// #define GET             0
// #define POST            1

#define TOKEN_LENGTH 12

#define DEBUG_FLAG 1

#define TRUE 1
#define FALSE 0


#endif // SERVER_H