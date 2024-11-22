// Credit to https://www.gnu.org/software/libmicrohttpd/tutorial.html

#include <sys/types.h>

#include <sys/select.h>
#include <sys/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "handlerd.h"

const char *domainName = NULL;
char databaseFileName[256];

// Types of connections accepted.
enum connectionType
{
  GET = 0,
  POST = 1
};

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

// DID record structure NOT USED YET
struct didRecordStruct
{
	const char *did;
	const char *handle;
	const char *token;
	const char *email;
	const unsigned int *locked;
};

struct connectionInfoStruct
{
	enum connectionType connectiontype; // NOT USED YET
	
	// Handle to the POST processing state.
	struct MHD_PostProcessor *postprocessor;
	
	// DID records we need to keep track of.
	// struct didRecordStruct *didRecord;
	const char *did;
	const char *handle;
	const char *token;
	const char *email;
	
	// HTTP response body we will return, NULL if not yet known.
	const char *answerstring;

	// HTTP status code we will return, 0 for undecided.
	unsigned int answercode;
};


// REPLACE THE HARD CODED DOMAIN NAME
const char *landingPage =
"<html><body>\
<p>This subdomain is available as a handle. Enter your Bluesky DID if you want it associated with it.</p>\
<form action=\"/result\" method=\"post\">\
BluesSky DID: <input name=\"did\" type=\"text\" required><br>\
Email (optional): <input name=\"email\" type=\"text\"><br>\
<input type=\"submit\" value=\" Send \"></form><br>\
<p>Message me in the fediverse if you have any questions <a href=\"https://ctrvx.net/chema\" target=\"_blank\">@chema@ctrvx.net.</a></p>\
<p>Check out the <a href=\"https://bsky.app/profile/did:plc:7wm5b6dzk54ukznrxdlpp23f/feed/aaac5vicl4pww\" target=\"_blank\">Baysky Feed</a> and feel free to <a href=\"https://ko-fi.com/chemahg\" target=\"_blank\">get me a coffee.</a></p>\
<p><i>Terms: This free service is provided at my sole discretion and availability and may be modified, suspended, or discontinued at any time without prior notice. Not for commercial use. The database may be stolen, corrupted or destroyed but handles and DIDs are always public. </i></p>\
</body></html>";

const char *confirmationPage = "<html><body><p>The subdomain has been succesfully associated with your DID.</p><p>You control token is %s. Save it, you will need it to delete this record!</p></body></html>";

// TO DO: Pass on error message details
const char *errorpage = "<html><body>Error: Request failed.</body></html>";

// DID received is not valid
const char *invalidDidPage = "<html><body>DID entered is not valid. Remove the 'did=' at the beginning?</body></html>";

// Handled received is not valid
const char *invalidHandlePage = "<html><body>Handle requested is not valid.</body></html>";

void generateSecureToken(char *token) {
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    size_t charset_size = strlen(charset);

    FILE *urandom = fopen("/dev/urandom", "r");
    if (!urandom) {
        perror("Cannot open /dev/urandom");
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < TOKEN_LENGTH ; i++) {
        unsigned char random_byte;
        if (fread(&random_byte, sizeof(random_byte), 1, urandom) != 1) {
            perror("Error reading from /dev/urandom");
            fclose(urandom);
            exit(EXIT_FAILURE);
        }
        token[i] = charset[random_byte % charset_size];
    }

    fclose(urandom);
    token[TOKEN_LENGTH] = '\0'; // Null-terminate the token
}


int validateContent(const char *content, const char *pattern) {
    regex_t regex;
    int ret;
	
	if (!content || !pattern) {
		fprintf(stderr, "Invalid input: content or pattern is NULL.\n");
		return -1;
	}

    // Compile the regular expression from pattern
	// CONSIDER MOVING COMPILED PATTERNS OUTSIDE OF THE FUNCTION
    if ((ret = regcomp(&regex, pattern, REG_EXTENDED)) != 0) {
        char error_message[256];
        regerror(ret, &regex, error_message, sizeof(error_message));
        fprintf(stderr, "Regex compilation error: %s\n", error_message);
        return 0; // Return failure
    }

    // Execute the regular expression
    ret = regexec(&regex, content, 0, NULL, 0);
    regfree(&regex);

    // Return whether the regex matched
    return ret == 0;
}


// Open the database in databaseFileName
 sqlite3 *openDatabase(void) {
    sqlite3 *db = NULL;
	
    if (sqlite3_open(databaseFileName, &db) != SQLITE_OK) {
        fprintf(stderr, "Failed to open database '%s': %s\n", databaseFileName, sqlite3_errmsg(db));
        sqlite3_close(db); // Ensure cleanup in case of partial initialization
        return NULL;
    }
	
    return db;
}


// Prepare the SQL statement
sqlite3_stmt *prepareSQLStatement (sqlite3 *db, const char *sql) {
	sqlite3_stmt *stmt;
	
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		fprintf(stderr, "Query SQL preparation error: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
		sqlite3_close(db);
        return NULL;
    }
	
	return stmt;
}


// Try adding a new report. Returns TRUE if successful, returns FALSE if failed. Maybe a pointer to an error msg?
int addNewRecord (const char *handle, const char *did, const char *token, const char *email) {
	int rc;
    char insertUserRecordSql[512];
    char *err_msg = 0;

	printf("ADD NEW RECORD DATA: handle=%s, did=%s, token=%s, email=%s\n", handle, did, token, email);

    sqlite3 *db = openDatabase();
	if (!db) return FALSE;

	// ADD VALIDATION??
	
    // Insert data
	snprintf(insertUserRecordSql, sizeof(insertUserRecordSql),
         "INSERT OR IGNORE INTO userTable (handle, did, token, email) "
         "VALUES ('%s', '%s', '%s', '%s');",
         handle, did, token, email);
		 
		 
    rc = sqlite3_exec(db, insertUserRecordSql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "New record creation failed: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return FALSE;
    } else printf("New record created succesfully: %s\n", handle);

	// Close the database
    sqlite3_close(db);

	return TRUE;
}


// Function to query the database for a DID associated with the handle
const char* queryForDid (const char *handle) {
    const char *sql = "SELECT did FROM userTable WHERE handle = ?";
	static char result [MAXDIDSIZE]; // Static buffer to hold the DID
    result[0] = '\0';        		// Ensure the buffer is empty

	printf("Handle: %s\n", handle); // debug

    sqlite3 *db = openDatabase();
	if (!db) return NULL;

    sqlite3_stmt *stmt = prepareSQLStatement (db, sql);	
	if (!stmt) return NULL;
	
    // Bind the handle to the query
    if (sqlite3_bind_text(stmt, 1, handle, -1, SQLITE_STATIC) != SQLITE_OK) {
        printf("Binding error: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return NULL;
    }

    // Execute the query
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        // If a valid DID is found, copy it to the result buffer
        const char *did = (const char *)sqlite3_column_text(stmt, 0); //casting 

		printf("DID: %s\n", did); // debug
        if (did) {
			// Validate DID
			if (!validateContent(did, VALID_PATTERN_DID)) {
				printf("Invalid DID format: %s\n", did);
				sqlite3_finalize(stmt);
				sqlite3_close(db);
				return NULL;
			}	
            strncpy(result, did, sizeof(result) - 1);
            result[sizeof(result) - 1] = '\0'; // Null-terminate the string
        }
        printf("Valid DID found: %s\n", result);
    } else {
        printf("No DID found for handle: %s\n", handle);
    }	

    // Clean up
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    // Return the result if a DID was found, or NULL if not
    return result[0] ? result : NULL;
}


// Send Response with Specific Content-Type Header
static enum MHD_Result sendResponse (struct MHD_Connection *connection, const char *content, const char *contentType)
{
	enum MHD_Result ret;
	struct MHD_Response *response;

	response = MHD_create_response_from_buffer (strlen (content), (void *) content, MHD_RESPMEM_PERSISTENT);
	if (! response) return MHD_NO;

	// Add content type header to response
	if (contentType != NULL) MHD_add_response_header(response, "Content-Type", contentType);

	// Queue the response
	ret = MHD_queue_response(connection, MHD_HTTP_OK, response);

	// Clean up
	MHD_destroy_response(response);

	return ret;
}

static enum MHD_Result iteratePost	(void *coninfo_cls, enum MHD_ValueKind kind, const char *key,
										const char *filename, const char *content_type,
										const char *transfer_encoding, const char *data, uint64_t off,
										size_t size)
{
  struct connectionInfoStruct *con_info = coninfo_cls;
  (void) kind;               /* Unused. Silent compiler warning. */
  (void) filename;           /* Unused. Silent compiler warning. */
  (void) content_type;       /* Unused. Silent compiler warning. */
  (void) transfer_encoding;  /* Unused. Silent compiler warning. */
  (void) off;                /* Unused. Silent compiler warning. */
  
	if (DEBUG_FLAG) printf("Data: key=%s, data=%s\n", key, data);

	if ( !(validateContent(con_info->handle, VALID_PATTERN_HANDLE)) ) {
		printf("Invalid Handle, exiting");
		con_info->answerstring = invalidHandlePage;
		return MHD_NO;
	}
  
  	if ( (strcmp(key, "did") == 0) && (size > 0) && (size <= MAXDIDSIZE) ) {
		// DID key exists, now validate data
		if (validateContent(data, VALID_PATTERN_DID)) {
			con_info->did = strndup(data, size);
			if (DEBUG_FLAG) printf("DID valid and added: handle=%s, did=%s, token=%s, email=%s\n", con_info->handle, con_info->did, con_info->token, con_info->email);
			return MHD_YES; // Iterate again looking for email.
		} else {
			printf("DID invalid, exiting\n");
			con_info->answerstring = invalidDidPage;
			return MHD_NO;
		}
	}
	
	if ( (NULL != con_info->did) && (strcmp(key, "email") == 0) && (size <= MAXDIDSIZE) ) {
		if ( size == 0 ) {
			char *emailDeclinedString = "NO EMAIL PROVIDED";
			con_info->email = strndup(emailDeclinedString, strlen(emailDeclinedString));
		} else {
			con_info->email = strndup(data, size);
		}
		
		char token[TOKEN_LENGTH + 1]; // +1 for the null terminator
		generateSecureToken(token);
		con_info->token = strndup(token, TOKEN_LENGTH + 1);

		if (DEBUG_FLAG) printf("ALL DATA RECEIVED: handle=%s, did=%s, token=%s, email=%s\n", con_info->handle, con_info->did, con_info->token, con_info->email);

		addNewRecord (con_info->handle, con_info->did, con_info->token, con_info->email);
		
		char *answerstring;

		answerstring = malloc (MAXANSWERSIZE);
		if (! answerstring) return MHD_NO;
		
		snprintf (answerstring, MAXANSWERSIZE, confirmationPage, con_info->token);	

		con_info->answerstring = answerstring;
		return MHD_NO;
	} else {
		printf("Record Invalid (Test)\n");
		con_info->answerstring = NULL;
		return MHD_NO;
	}
  
  return MHD_YES;
}












// Clean-up function after request completed
static void requestCompleted (void *cls, struct MHD_Connection *connection, void **con_cls, enum MHD_RequestTerminationCode toe)
{
  struct connectionInfoStruct *con_info = *con_cls;
  (void) cls;         /* Unused. Silent compiler warning. */
  (void) connection;  /* Unused. Silent compiler warning. */
  (void) toe;         /* Unused. Silent compiler warning. */

  if (NULL == con_info)	return;

  if (DEBUG_FLAG) printf("Data struct at requestCompleted: handle=%s, did=%s, token=%s, email=%s\n", con_info->handle, con_info->did, con_info->token, con_info->email);

  if (con_info->connectiontype == POST)
  {
	   if (NULL != con_info->postprocessor) MHD_destroy_post_processor (con_info->postprocessor);
	   if (DEBUG_FLAG) printf("Freed post processor\n");
  }

	free (con_info);
	*con_cls = NULL;
}


static enum MHD_Result requestHandler	   (void *cls, struct MHD_Connection *connection,
												const char *url, const char *method,
												const char *version, const char *upload_data,
												size_t *upload_data_size, void **con_cls)
{
	(void) cls;               /* Unused. Silent compiler warning. */
	(void) url;               /* Unused. Silent compiler warning. */
	(void) version;           /* Unused. Silent compiler warning. */
	
	// Get the 'Host' and 'X-ATPROTO-HANDLE' header values from GET connection
    const char *hostHeader = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Host");
	const char *handleHeader = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-ATPROTO-HANDLE");
	// ADD SANITY CHECK FOR THE HOST HEADER

	// Confirm that the host header includes the domain name, otherwise skip directly to error.
	if ( ( (strstr (hostHeader, domainName)) != NULL) || DEBUG_FLAG) {

		if (NULL == *con_cls) { // First call, setup data structures
			struct connectionInfoStruct *con_info;

			con_info = malloc (sizeof (struct connectionInfoStruct));
			if (NULL == con_info) {
				fprintf(stderr, "Error: Memory allocation failed for connection information.\n");
				return MHD_NO;	// Malloc failed
			}

			// Initialize the DID recordt struct
			con_info->did = NULL;
			if (handleHeader) con_info->handle = handleHeader;
			else con_info->handle = NULL;
			con_info->token = NULL;
			con_info->email = NULL;

			// Initialize answer variables
			con_info->answerstring = NULL;			// Make sure answer string is empty
			con_info->answercode = 0;				// Make sure answercode string is empty

			//if (0 == strcasecmp (method, MHD_HTTP_METHOD_POST)) {
			if (0 == strcmp (method, "POST")) {
				con_info->postprocessor = MHD_create_post_processor (connection, POSTBUFFERSIZE, iteratePost, (void *) con_info);

				// Creating postprocessor failed, free memory manually and exit.
				if (NULL == con_info->postprocessor) {
					free (con_info);
					return MHD_NO;
				}
				
				// Setting connection type in structure
				con_info->connectiontype = POST;
			}
			else { //PROBABLY BEST TO CONFIRM IT IS A GET REQUEST?
			  con_info->connectiontype = GET;
			}

			*con_cls = (void *) con_info;

			return MHD_YES;
		}

		if (0 == strcmp (method, "GET")) {
			// CONFIRM TWO CONDITIONS:
			// 1. Target domain name is included in the host header (nginx is also doing this)
			// 2. Confirm the target URL (Bluesky requirement)
			if ( strstr(hostHeader, domainName) != NULL && strstr(url, TARGET_URL) != NULL ) {
				// Request DID
				const char *did = queryForDid(handleHeader);

				// Verify DID exists and send it as plain text
				if ( did ) { return sendResponse (connection, did, "text/plain"); }
		
			} else {
				return sendResponse (connection, landingPage, HTML_CONTENT);
			}
		}

		if (0 == strcmp (method, "POST")) {

			struct connectionInfoStruct *con_info = *con_cls;

			// Checks to see if all data has been received from iterative POST requests
			if (*upload_data_size != 0)
			{
			  MHD_post_process (con_info->postprocessor, upload_data, *upload_data_size);
			  *upload_data_size = 0;

			  return MHD_YES;
			}
			else
			// If there's no more data, then we are finished and we can send response if there is one.
			if (NULL != con_info->answerstring) {
				return sendResponse (connection, con_info->answerstring, HTML_CONTENT);
			}
		}

	} // END THE DOMAIN ACCESS
	
	return sendResponse (connection, errorpage, HTML_CONTENT);
}

// MAIN FUNCTION: TRACK ARGS, PREP GLOBALS AND CALL DEAMON
int main(int argc, char *argv[])
{
	if (argc < 2) {
        fprintf(stderr, "Usage: %s <domain name>\n", argv[0]);
        return 1;
    }
	
	// PREP GLOBAL VARIABLES
    domainName = argv[1];
	
    // Create database name by appending "-domain" to the provided domain name
    snprintf(databaseFileName, sizeof(databaseFileName), "%s.users.db", domainName);
	
	printf("Domain Name: %s\n", domainName);
	printf("Database Filename: %s\n", databaseFileName);
	
	// START CONFIRM DATABASE, MOVE TO A FUNCTION
	
	char *err_msg = 0;
    int rc;
	
	sqlite3 *db = openDatabase();
	if (!db) return 0;
	else printf("Database '%s' created/opened successfully.\n", databaseFileName);

    char *sqlCreateTable = "CREATE TABLE IF NOT EXISTS userTable ("
                "handle TEXT PRIMARY KEY, "
				"did TEXT NOT NULL, "
                "token TEXT NOT NULL, "
				"email TEXT, "
				"locked BOOLEAN DEFAULT 0, "
				"notes TEXT,"
				"creation_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP);";

    rc = sqlite3_exec(db, sqlCreateTable, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Table creation failed: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return 1;
    } else printf("Table created successfully (or already existed).\n");

	char *sqlCreateHandleIndex = "CREATE INDEX IF NOT EXISTS idx_did ON userTable(did);";

    rc = sqlite3_exec(db, sqlCreateHandleIndex, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Handle index creation failed: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return 1;
    } else printf("Handle index created successfully (or already existed).\n");
	
	
	// END CONFIRM DATABASE
	
	
	struct MHD_Daemon *daemon;

    // Start the HTTP daemon
	daemon = MHD_start_daemon (MHD_USE_AUTO | MHD_USE_INTERNAL_POLLING_THREAD, PORT,
                             NULL, NULL,  // No client connect/disconnect callbacks
                             &requestHandler, NULL,  // Request handler
                             MHD_OPTION_NOTIFY_COMPLETED, requestCompleted,
                             NULL, MHD_OPTION_END);

	if (NULL == daemon) {
        fprintf(stderr, "Failed to start handler daemon\n");
        return 1;
    }

    printf("Handler Daemon running on port %d...\n", PORT);

    // Keep running until the user presses Enter
	(void) getchar ();

    // Stop the server
	MHD_stop_daemon (daemon);

	return 0;
}