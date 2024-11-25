// handle-handler (or handlerd) -  Manage Subdomain Handles for AT Protocol/Bluesky
// Copyright (c) 2024 Chema Hernández Gil / AGPL-3.0 license
// https://github.com/chema/handle-handler

#define _GNU_SOURCE // Added to have access to  strcasestr function

#include <sys/types.h>

#include <sys/select.h>
#include <sys/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "handlerd.h"

#define DEBUG_FLAG 1
#define VERBOSE_FLAG
#define NGINX_FLAG

// ***************************************************************************
// BEGIN GLOBAL VARS *********************************************************
// ***************************************************************************

// GLOBAL DATABASE DETAILS
const char *domainName = NULL;
char databaseFileName[256];

// GLOBAL COMPILED REGEX
regex_t regexDid;
regex_t regexHandle;
regex_t regexLabel;
regex_t regexFullHandle;

// TYPES OF CONNECTIONS ACCEPTED.
enum connectionType
{
  GET = 0,
  POST = 1
};

struct connectionInfoStruct
{
	enum connectionType connectiontype; // NOT USED YET
	
	// HANDLE TO THE POST PROCESSING STATE.
	struct MHD_PostProcessor *postprocessor;
	
	// CONNECTION DETAILS WE NEED TO TRACK, CONSIDER MAKING IT A STRUCT
	const char *host;
	const char *handle;
	
	// USER DETAILS WE NEED TO TRACK, CONSIDER REMOVING SOME
	const char *did;
	const char *email;
	const char *token;
	const unsigned int *locked;
	
	// HTTP RESPONSE BODY WE WILL RETURN, NULL IF NOT YET KNOWN.
	const char *answerstring;

	// HTTP STATUS CODE WE WILL RETURN, 0 FOR UNDECIDED.
	unsigned int answercode;
};


// ***************************************************************************
// BEGIN HARD CODED HTML CODE ************************************************
// ***************************************************************************

// TO DO: REPLACE HARD CODED DOMAIN NAME
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
const char *errorPage = "<html><body>Error: Request failed.</body></html>";

// DID RECEIVED IS NOT VALID
const char *invalidDidPage = "<html><body>DID entered is not valid. Remove the 'did=' at the beginning?</body></html>";

// HANDLE RECEIVED IS NOT VALID
const char *invalidHandlePage = "<html><body>Handle requested is not valid.</body></html>";
const char *invalidHandlePage2 = "<html><body>Error: Requested page is not available.</body></html>";


// ***************************************************************************
// BEGIN "SECURITY" **********************************************************
// ***************************************************************************

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

// ***************************************************************************
// BEGIN FILE READING ********************************************************
// ***************************************************************************
	
// Function to read the contents of the HTML file
char *readFile (const char *filePath) {
		FILE *file = fopen(filePath, "r");
		if (!file) {
			perror("Failed to open file");
			return NULL;
		}

		fseek(file, 0, SEEK_END);
		long fileSize = ftell(file);
		rewind(file);

		char *buffer = malloc(fileSize + 1);
		if (!buffer) {
			perror("Failed to allocate memory");
			fclose(file);
			return NULL;
		}

		fread(buffer, 1, fileSize, file);
		buffer[fileSize] = '\0'; // Null-terminate the string/file

		fclose(file);

		return buffer;
	}

/*
// Helper function to replace placeholder text in a string
static char *replacePlaceholder(const char *original, const char *placeholder, const char *replacement) {
    if (!original || !placeholder || !replacement) {
        return NULL; // Ensure inputs are not null
    }

    size_t originalLen = strlen(original);
    size_t placeholderLen = strlen(placeholder);
    size_t replacementLen = strlen(replacement);

    // Calculate the new size (assuming one placeholder)
    size_t newSize = originalLen + replacementLen - placeholderLen + 1;
    char *modified = (char *)malloc(newSize);
    if (!modified) {
        return NULL;
    }

    // Find the placeholder
    const char *tmp = strstr(original, placeholder);
    if (!tmp) {
        // If no placeholder found, copy the original string and return
        strcpy(modified, original);
        return modified;
    }

    // Perform the replacement
    const char *current = original;
    char *dest = modified;

    // Copy part before the placeholder
    size_t lenBefore = tmp - current;
    memcpy(dest, current, lenBefore); // Use memcpy for raw copy
    dest += lenBefore;

    // Copy the replacement
    memcpy(dest, replacement, replacementLen);
    dest += replacementLen;

    // Copy the remaining part of the original string after the placeholder
    const char *remaining = tmp + placeholderLen;
    strcpy(dest, remaining);

    return modified;
}
*/

// ***************************************************************************
// BEGIN REGEX AND VALIDATORS ************************************************
// ***************************************************************************

// HELPER FUNCTION TO HANDLE REGEX COMPILATION ERRORS
int handleRegexError(int ret, regex_t *regex, const char *patternName) {
    size_t errorBufferSize = regerror(ret, regex, NULL, 0); // Get required buffer size
    char *errorMessage = malloc(errorBufferSize);  // Dynamically allocate memory
	
    if (!errorMessage) {
        fprintf(stderr, "Memory allocation error for regex error message buffer.\n");
        return 1;
    }

    regerror(ret, regex, errorMessage, errorBufferSize);  // Populate the error message
    fprintf(stderr, "Regex compilation error for %s pattern: %s\n", patternName, errorMessage);        
    free(errorMessage);  // Free allocated memory
    return 1;
}

// FUNCTION TO COMPILE THE PRINCIPAL REGEXES
int compileGlobalRegex( ) {
    int ret;

	// Compile DID regex
	ret = regcomp(&regexDid, VALID_PATTERN_DID_PLC, REG_EXTENDED | REG_ICASE );
    if ( ret != 0 ) return handleRegexError(ret, &regexDid, "DID");

	// Compile handle regex
	ret = regcomp(&regexHandle, VALID_PATTERN_HANDLE, REG_EXTENDED | REG_ICASE );
    if ( ret != 0 ) return handleRegexError(ret, &regexHandle, "Handle");
	
	// Compile label regex
	ret = regcomp(&regexLabel, VALID_PATTERN_LABEL, REG_EXTENDED | REG_ICASE );
    if ( ret != 0 ) return handleRegexError(ret, &regexLabel, "Label");	
	
	// Compile FULL HANDLE regex
	ret = regcomp(&regexFullHandle, VALID_PATTERN_FULL_HANDLE, REG_EXTENDED | REG_ICASE );
    if ( ret != 0 ) return handleRegexError(ret, &regexFullHandle, "Full Handle");	
	
    return 0;  // Success
}

// FUNCTION TO FREE THE COMPILED REGEX (CALLED WHEN PROGRAM EXITS)
void freeGlobalRegex() {
    regfree(&regexDid);   // Free DID regex
    regfree(&regexHandle); // Free handle regex
    regfree(&regexLabel); // Free label regex
	regfree(&regexFullHandle); // Free handle regex
}

// VALIDATOR FOR IDENTIFIER
int validateDid(const char *did) {  // RETURNS KEY_VALID = 0 for successful validation
	if (did == NULL) return KEY_INVALID;
	
    if (regexec(&regexDid, did, 0, NULL, 0) != 0) {
		return KEY_INVALID;
	}
	
    return KEY_VALID;
}

// VALIDATOR FOR HANDLE
int validateHandle(const char *did) {  // RETURNS KEY_VALID = 0 for successful validation
	if (did == NULL) return KEY_INVALID;
	
    if (regexec(&regexHandle, did, 0, NULL, 0) != 0) {
		return KEY_INVALID;
	}
	
    return KEY_VALID;
}

// VALIDATOR FOR LABEL
int validateLabel(const char *label) {  // RETURNS KEY_VALID = 0 for successful validation
	if (label == NULL) return KEY_INVALID;
	
    if (regexec(&regexLabel, label, 0, NULL, 0) != 0) {
		return KEY_INVALID;
	}
	
    return KEY_VALID;
}

// REMOVE DOMAIN NAME FROM THE HOSTNAME
const char* removeDomainName (const char *hostName) {
    // Confirm that the domainName is in the hostName string
	if ( NULL != strcasestr(hostName, domainName) ) {
		// Remove one to eliminate the '.' also!
		size_t labelSize = strlen(hostName) - strlen(domainName) - 1;
		
		char *labelSegment = (char *)malloc(labelSize);
		if (labelSegment == NULL) {
				// Handle memory allocation failure
				return NULL;
		 }  
		 
		strncpy(labelSegment, hostName, labelSize);
		labelSegment[labelSize] = '\0';  // Null-terminate the new string

		return labelSegment;  // Return the new string
	}
	// If domainName is not found, return NULL
	return NULL;
}


// END REGEX AND VALIDATORS  ******************************************


// ***************************************************************************
// BEGIN GENERIC DATABASE FUNCTIONS ******************************************
// ***************************************************************************

// OPEN THE DATABASE IN databaseFileName
sqlite3 *openDatabase(void) {
    sqlite3 *db = NULL;
	
    if (sqlite3_open(databaseFileName, &db) != SQLITE_OK) {
        fprintf(stderr, "Failed to open database '%s': %s\n", databaseFileName, sqlite3_errmsg(db));
        if (db) sqlite3_close(db); // Ensure cleanup in case of partial initialization
        return NULL;
    }
	
    return db;
}

// PREPARE A SQL STATEMENT
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

// BIND KEY TO STATEMENT
int bindKeyToSQLStatement(sqlite3_stmt *stmt, int index, const char *key, sqlite3 *db) {
    if (sqlite3_bind_text(stmt, index, key, -1, SQLITE_STATIC) != SQLITE_OK) {
        fprintf(stderr, "SQL Binding error for key %s: %s\n", key, sqlite3_errmsg(db));
        return SQLITE_ERROR; // Failure
    }
    return SQLITE_OK; // Success
}

// INITIALIZE DATABASE, RETURNS DATABASE_SUCCESS 0 IF SUCESSFUL, DATABASE_ERROR 1 IF IT FAILED.
int initializeDatabase( ) {
	char *err_msg = 0;
    int rc;
	
    sqlite3 *db = openDatabase();
    if (!db) {
        return DATABASE_ERROR;
    }
    
	#ifdef VERBOSE_FLAG
	printf("Database '%s' created/opened successfully.\n", databaseFileName);
	#endif

	// SQL COMMENTARY: ADDITIONAL INDEXING IS UNNECESSARY SINCE 'did' COLUMN IS UNIQUE
	// VIEW THE INDEX LIST USING: PRAGMA index_list(userTable);
    char *sqlCreateTable = "CREATE TABLE IF NOT EXISTS userTable ("
                "handle TEXT PRIMARY KEY, "
				"did TEXT NOT NULL UNIQUE, "
                "token TEXT NOT NULL, "
				"email TEXT, "
				"locked BOOLEAN DEFAULT 0, "
				"notes TEXT,"
				"creation_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP);";

    rc = sqlite3_exec(db, sqlCreateTable, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Principal User Table creation failed: %s\n", err_msg);
		sqlite3_free(err_msg);
        sqlite3_close(db);
        return DATABASE_ERROR;
    }

	// RECREATE THE RESERVED HANDLE TABLE TO THE DB
    const char *sqlCreateReserveLabelTable  =  "BEGIN TRANSACTION;"
											"DROP TABLE IF EXISTS reservedHandleTable;"
											"CREATE TABLE reservedHandleTable (word TEXT NOT NULL);"
											"COMMIT;";

    rc = sqlite3_exec(db, sqlCreateReserveLabelTable, 0, 0, &err_msg);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "Reserved Label Table Creation Failed: %s\n", err_msg);
		sqlite3_free(err_msg);
		sqlite3_close(db);
		return DATABASE_ERROR;
	}

    // Prepare the INSERT statement
    const char *sqlInsertWord = "INSERT INTO reservedHandleTable (word) VALUES (?);";
    sqlite3_stmt *stmt = prepareSQLStatement (db, sqlInsertWord);	
	if (!stmt) {
		fprintf(stderr, "Reserved Label Table Creation Failed: %s\n", err_msg);
		return DATABASE_ERROR;
	}
	
	
    // OPEN THE INPUT FILE TO DO: MOVE FILE NAME TO CONFIGURATION
	// CURRENTLY USING https://github.com/Rad-Web-Hosting/banned-subdomain-prefixes/blob/master/common.txt
	// AND ATPSPECIFIC LIST HERE: https://github.com/bluesky-social/atproto/blob/main/packages/pds/src/handle/reserved.ts
    char line[256];		// Buffer for reading lines from the file
    FILE *file = fopen("static/reserved.txt", "r");
    if (!file) {
        perror("Failed to open file reserved word list");
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return DATABASE_ERROR;
    }

    // Read lines from the file and insert them into the database
    while (fgets(line, sizeof(line), file)) {
        // Remove trailing newline character
        line[strcspn(line, "\n")] = '\0';

        // Bind the line to the SQL statement
        sqlite3_bind_text(stmt, 1, line, -1, SQLITE_STATIC);

        // Execute the SQL statement
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "Failed to execute statement: %s\n", sqlite3_errmsg(db));
        }

        // Reset the statement for the next iteration
        sqlite3_reset(stmt);
    }

    // Clean up
    fclose(file);
    sqlite3_finalize(stmt);

	#ifdef VERBOSE_FLAG
	printf("Tables created successfully (or already existed).\n");
	#endif

    // CLOSE THE DATABASE
	sqlite3_free(err_msg);
    sqlite3_close(db);

	return DATABASE_SUCCESS;
}






// ***************************************************************************
// BEGIN HANDLERD SPECIFIC DATABASE FUNCTIONS ********************************
// ***************************************************************************

// TRY ADDING A NEW REPORT. RETURNS TRUE IF SUCCESSFUL, RETURNS FALSE IF FAILED. MAYBE A POINTER TO AN ERROR MSG AND VALIDATION?
int addNewRecord(const char *handle, const char *did, const char *token, const char *email) {
    int rc;
	
    sqlite3 *db = openDatabase();
    if (!db) return FALSE;

    // SQL STATEMENT WITH PLACEHOLDERS FOR THE PARAMETERS
	// ALL VALUES ARE NORMALIZED TO LOWERCASE EXCEPT FOR TOKEN
    const char *insertUserRecordSql = 
        "INSERT INTO userTable (handle, did, token, email) "
        "VALUES (LOWER(?), LOWER(?), ?, LOWER(?));";

	// PREPARE SQL STATEMENT
    sqlite3_stmt *stmt = prepareSQLStatement (db, insertUserRecordSql);	
	if (!stmt) return FALSE;

    // Bind parameters to the prepared statement using bindKeyToSQLStatement
    if (bindKeyToSQLStatement(stmt, 1, handle, db) != SQLITE_OK || bindKeyToSQLStatement(stmt, 2, did, db) != SQLITE_OK ||
        bindKeyToSQLStatement(stmt, 3, token, db) != SQLITE_OK || bindKeyToSQLStatement(stmt, 4, email, db) != SQLITE_OK)
	{
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return FALSE;
    }

    // Execute the statement
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "ERROR: New record creation failed (%d): %s\n", rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return FALSE;
    }
	
	printf("New record created successfully: %s\n", handle);

    // Finalize the statement and close the database
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return TRUE;
}

// QUERY DATABASE FOR EXISTANCE OF SPECIFIC 'handle'
int handleRegistered (const char *handle) {
	const char *sql = "SELECT 1 FROM userTable WHERE handle = ? LIMIT 1;";
	
	// OPEN DATABASE
    sqlite3 *db = openDatabase();
	if (!db) return HANDLE_ERROR;

	// PREPARE SQL STATEMENT
    sqlite3_stmt *stmt = prepareSQLStatement (db, sql);	
	if (!stmt) return HANDLE_ERROR;

	// BIND HANDLE KEY TO PREPARED SQL STATEMENT (1 IS SUCCESS)
    if ( bindKeyToSQLStatement(stmt, 1, handle, db) != SQLITE_OK ) {
        sqlite3_finalize(stmt);
		sqlite3_close(db);
        return HANDLE_ERROR;
    }

	// Execute the query
	int rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		if (DEBUG_FLAG) printf("DEBUG: Handle '%s' is active.\n", handle);
		sqlite3_finalize(stmt);
		return HANDLE_ACTIVE; // Exists
	} else if (rc == SQLITE_DONE) {
		if (DEBUG_FLAG) printf("DEBUG: Handle '%s' does not exist in the database.\n", handle);
		sqlite3_finalize(stmt);
		return HANDLE_INACTIVE; // Does not exist
	} else {
		fprintf(stderr, "Error executing query: %s\n", sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return HANDLE_ERROR; // Error
	}
}

// QUERY DB TABLE RESERVED WORDS FOR SPECIFIC 'LABEL' 'handle'
int labelReserved (const char *label) {
	const char *sql = "SELECT 1 FROM reservedHandleTable WHERE word = ? LIMIT 1;";

	// OPEN DATABASE
    sqlite3 *db = openDatabase();
	if (!db) return HANDLE_ERROR;

	// PREPARE SQL STATEMENT
    sqlite3_stmt *stmt = prepareSQLStatement (db, sql);	
	if (!stmt) return HANDLE_ERROR;

	// BIND LABEL KEY TO PREPARED SQL STATEMENT (1 IS SUCCESS)
    if ( bindKeyToSQLStatement(stmt, 1, label, db) != SQLITE_OK ) {
        sqlite3_finalize(stmt);
		sqlite3_close(db);
        return HANDLE_ERROR;
    }

	// Execute the query
	int rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		if (DEBUG_FLAG) printf("DEBUG: Label '%s' is a reserved word.\n", label);
		sqlite3_finalize(stmt);
		return HANDLE_ACTIVE; // Exists
	} else if (rc == SQLITE_DONE) {
		if (DEBUG_FLAG) printf("DEBUG: Label '%s' is not a reserved word .\n", label);
		sqlite3_finalize(stmt);
		return HANDLE_INACTIVE; // Does not exist
	} else {
		fprintf(stderr, "Error executing query: %s\n", sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return HANDLE_ERROR; // Error
	}
}


// QUERY DATABASE FOR A VALID IDENTIFIER ASSOCIATED WITH HANDLE
const char* queryForDid (const char *handle) {
    const char *sql = "SELECT did FROM userTable WHERE handle = ?";
	static char result [MAX_SIZE_DID_PLC + 1];	// Static buffer to hold the DID + 1 for the null terminator
	result[0] = '\0';        					// Ensure the buffer is empty
	
	if (DEBUG_FLAG) printf("DEBUG: Begin queryForDid for handle '%s'.\n", handle);

	// OPEN DATABASE
    sqlite3 *db = openDatabase();
	if (!db) return NULL;

	// PREPARE SQL STATEMENT
    sqlite3_stmt *stmt = prepareSQLStatement (db, sql);	
	if (!stmt) return NULL;

	// BIND HANDLE KEY TO PREPARED SQL STATEMENT (1 IS SUCCESS)
    if ( bindKeyToSQLStatement(stmt, 1, handle, db) != SQLITE_OK ) {
        sqlite3_finalize(stmt);
		sqlite3_close(db);
        return NULL;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        // Get the singular result from the query above, casted to did.
        const char *tempResult = (const char *)sqlite3_column_text(stmt, 0);
		
		if (DEBUG_FLAG) printf("DEBUG: queryForDid found 'did' result for handle '%s': '%s'.\n", handle, tempResult);

		// IDENTIFY SIGNIFICANT ERRORS IN THE DATABASE
		if ( (tempResult == NULL) || (validateDid(tempResult) == KEY_INVALID) ) {
			// TO DO: PROGRAM SHOULD REMOVE THESE INVALID RECORDS OR AT LEAST FLAG THEM.
			fprintf(stderr, "ERROR: DID value for handle '%s' is null or invalid. Remove record.\n", handle);
			sqlite3_finalize(stmt);
			sqlite3_close(db);
			return NULL;
		} else {
			
		// Copy	did to result;
		snprintf(result, sizeof(result), "%s", tempResult);
		}
	}
	
    // Clean up
    sqlite3_finalize(stmt);
    sqlite3_close(db);

	if (DEBUG_FLAG) printf("DEBUG: End queryForDid: handle=%s, result (did)=%s\n", handle, result);

    // Returns result if it's not empty, and NULL if remains empty (i.e., the first character remains '\0')
    return result[0] ? result : NULL;
}


// ************************************
// ********* SERVER FUNCTIONS *********
// ************************************

// Send Response with Specific Content-Type Header
// TO DO: ADD A unsigned int status_code OPTION
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

// SEND FILE WITH SPECIFIC CONTENT-TYPE HEADER (HTML, PLAIN TEXT, CSV, JSON, ETC)
static enum MHD_Result sendFileResponse (struct MHD_Connection *connection, const char *filename, const char *contentType)
{
	enum MHD_Result ret;
	struct MHD_Response *response;
		
    char *htmlContent = readFile(filename);
    if (!htmlContent) {
        return MHD_NO;
    }

    response = MHD_create_response_from_buffer(strlen(htmlContent), (void *)htmlContent, MHD_RESPMEM_MUST_FREE);
	if (! response) {
		free(htmlContent); // Cleanup in case of failure
		return MHD_NO;
	}

	// Add content type header to response
	if (contentType != NULL) MHD_add_response_header(response, "Content-Type", contentType);

	// Queue the response
	ret = MHD_queue_response(connection, MHD_HTTP_OK, response);

	// Clean up
	MHD_destroy_response(response);

	return ret;
}
/*
// SEND FILE WITH SPECIFIC ERROR MESSAGE
static enum MHD_Result sendErrorResponse (struct MHD_Connection *connection, const char *filename, const char *contentType, const char *errorMessage)
{
	enum MHD_Result ret;
	struct MHD_Response *response;
		
    char *htmlContent = readFile(filename);
    if (!htmlContent) {
        return MHD_NO;
    }

    // Replace {{ ERROR }} in htmlContent with an error message
    char *modifiedContent = replacePlaceholder(htmlContent, "{{ ERROR }}", errorMessage);
    free(htmlContent); // Free the original content

    if (!modifiedContent) {
        return MHD_NO; // Handle replacement failure
    }


    response = MHD_create_response_from_buffer(strlen(htmlContent), (void *)htmlContent, MHD_RESPMEM_MUST_FREE);
	if (! response) {
		free(htmlContent); // Cleanup in case of failure
		return MHD_NO;
	}

	// Add content type header to response
	if (contentType != NULL) MHD_add_response_header(response, "Content-Type", contentType);

	// Queue the response
	ret = MHD_queue_response(connection, MHD_HTTP_OK, response);

	// Clean up
	MHD_destroy_response(response);

	return ret;
}
*/


// POST REQUEST MANAGER
static enum MHD_Result iteratePost	(void *coninfo_cls, enum MHD_ValueKind kind, const char *key,
										const char *filename, const char *content_type,
										const char *transfer_encoding, const char *data, uint64_t off,
										size_t size)
{
  //struct connectionInfoStruct *con_info = coninfo_cls;
  struct connectionInfoStruct *con_info = coninfo_cls;
  (void) kind;               /* Unused. Silent compiler warning. */
  (void) filename;           /* Unused. Silent compiler warning. */
  (void) content_type;       /* Unused. Silent compiler warning. */
  (void) transfer_encoding;  /* Unused. Silent compiler warning. */
  (void) off;                /* Unused. Silent compiler warning. */
  
	if (DEBUG_FLAG) printf("DEBUG: Begin iteratePost: key=%s, data=%s\n", key, data);

	if ( validateHandle (con_info->handle) ==  KEY_INVALID ) {
		if (DEBUG_FLAG) printf("DEBUG: Invalid iteratePost handle value, exiting: '%s'\n", con_info->handle);
		con_info->answerstring = invalidHandlePage;
		return MHD_NO;
	}
  
  	if ( (strcmp(key, "did") == 0) && (size > 0) && (size <= MAXDIDSIZE) ) {
		// DID key exists, now validate data
		if ( validateDid (data) == KEY_VALID ) {
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
		
		// TO DO: MOVE TOKEN CREATION TO addNewRecord
		char token[TOKEN_LENGTH + 1]; // +1 for the null terminator
		generateSecureToken(token);
		con_info->token = strndup(token, TOKEN_LENGTH + 1);

		if (DEBUG_FLAG) printf("ALL DATA RECEIVED: handle=%s, did=%s, token=%s, email=%s\n", con_info->handle, con_info->did, con_info->token, con_info->email);

		if ( FALSE == addNewRecord (con_info->handle, con_info->did, con_info->token, con_info->email) ) return MHD_NO;
		
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

// CLEAN-UP FUNCTION AFTER REQUEST COMPLETED
static void requestCompleted (void *cls, struct MHD_Connection *connection, void **con_cls, enum MHD_RequestTerminationCode toe)
{
  //struct connectionInfoStruct *con_info = *con_cls;
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

// *********************************************
// ********* PRINCIPAL REQUEST HANDLER *********
// *********************************************

static enum MHD_Result requestHandler	   (void *cls, struct MHD_Connection *connection,
												const char *url, const char *method,
												const char *version, const char *upload_data,
												size_t *upload_data_size, void **con_cls)
{
	(void) cls;               /* Unused. Silent compiler warning. */
	(void) version;           /* Unused. Silent compiler warning. */
	
	// VALIDATE HOST HEADER. A REVERSE PROXY SHOULD MAKE THIS UNNECESSARY
	// TO DO: ADD FURTHER HOST VALIDATION (RIGHT FORMAT, ETC)
    const char *hostHeader = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Host");
	if (!hostHeader || (NULL == strcasestr(hostHeader, domainName))) {
		// REJECT REQUEST IF HOST HEADER DOES NOT CONTAIN THE EXPECTED DOMAIN
        return MHD_NO;
    }
	
	if (NULL == *con_cls) { // FIRST CALL, SETUP DATA STRUCTURES, SOME ONLY APPLY TO 'POST' REQUESTS
		struct connectionInfoStruct *con_info;

		con_info = malloc (sizeof (struct connectionInfoStruct));
		if (NULL == con_info) {
			fprintf(stderr, "Error: Memory allocation failed for connection information.\n");
			return MHD_NO;	// INTERNAL ERROR
		}

		con_info->host = hostHeader;
		
		// GET OR CALCULATE THE 'handle'
		#ifdef NGINX_FLAG
		// 'X-ATPROTO-HANDLE' OPTIONAL AND REQUIRES NGINX REVERSE PROXY. DUPLICATIVE.
		con_info->handle = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-ATPROTO-HANDLE");
		#else
		// TO DO: CONFIRM NULL STATUS.
		con_info->handle = removeDomainName( con_info->host );
		#endif

		con_info->did = NULL;
		con_info->email = NULL;
		con_info->token = NULL;
		con_info->locked = NULL;

		// INITIALIZE THE CONNECTIONINFO STRUCT. SOME ONLY APPLY TO 'POST' REQUESTS.
		if ( 0 == strcasecmp (method, MHD_HTTP_METHOD_POST) ) {
			con_info->postprocessor = MHD_create_post_processor (connection, POSTBUFFERSIZE, iteratePost, (void *) con_info);

			// Creating postprocessor failed, free memory manually and exit.
			if (NULL == con_info->postprocessor) {
				free (con_info);
				fprintf(stderr, "ERROR: Creating postprocessor failed.\n");
				return MHD_NO; // INTERNAL ERROR
			}
			
			// Setting connection type in structure
			con_info->connectiontype = POST;
			
		}
		else { //PROBABLY BEST TO CONFIRM IT IS A GET REQUEST?
		  con_info->connectiontype = GET;
		}

		// INITIALIZE ANSWER VARIABLES
		con_info->answerstring = NULL;			// Make sure answer string is empty
		con_info->answercode = 0;				// Make sure answercode string is empty

		#ifdef DEBUG_FLAG
		printf("DEBUG 1st CALL: host=%s, handle=%s, url=%s\n", con_info->host, con_info->handle, url);
		#endif

		// CONFIRM THIS IS NECESSARY
		*con_cls = (void *) con_info;

		return MHD_YES;
	}
	
    struct connectionInfoStruct *con_info = *con_cls;

	if (0 == strcasecmp (method, MHD_HTTP_METHOD_GET))
	{		
		if (  validateHandle (con_info->handle) ==  KEY_VALID )
		{	
			// Confirm *exact* target URL for DID PLC payload (Bluesky requirement)
			// NOTE THERE IS NO SUBDOMAIN VERIFICATION HERE, CONSIDER ADDING IT
			if ( 0 == strcasecmp (url, TARGET_URL) )
			{
				// Query database for a *valid* DID PLC associated with 'con_info->handle'
				con_info->did = queryForDid(con_info->handle);
				// Confirm DID exists and send it as plain text
				if ( con_info->did ) { 
					if (DEBUG_FLAG) printf("DEBUG: GET method using valid host, sent valid DID: %s\n", con_info->did);
					return sendResponse (connection, con_info->did, "text/plain"); 
				} else return sendResponse (connection, "ERROR: UNKNOWN HANDLE", "text/plain");
			}
			
			if ( 0 == strcasecmp (url, "/") ) { 
				if ( (handleRegistered(con_info->handle) == HANDLE_ACTIVE) ) return sendFileResponse (connection, STATIC_ACTIVE, HTML_CONTENT);
				if ( (labelReserved(con_info->handle) == HANDLE_ACTIVE) ) return sendFileResponse (connection, STATIC_RESERVED, HTML_CONTENT); 
				return sendFileResponse (connection, STATIC_REGISTER, HTML_CONTENT); 
			}
			
			// ALL OTHER URLS
			sendResponse (connection, invalidHandlePage2, HTML_CONTENT);
		}
	}
	
	if (0 == strcasecmp (method, MHD_HTTP_METHOD_POST))	
	{
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

	// Not a GET or a POST, generate error
	// ADD A STATUS OPTION FOR 404 AND 403
	return sendResponse (connection, errorPage, HTML_CONTENT);
}


// *********************************
// ********* MAIN FUNCTION *********
// *********************************

int main(int argc, char *argv[])
{
	if (argc < 2) {
        fprintf(stderr, "Usage: %s <domain name>\n", argv[0]);
        return 1;
    }
	
	// PREP GLOBAL VARIABLES
    domainName = argv[1];
    snprintf(databaseFileName, sizeof(databaseFileName), "%s.users.db", domainName);
	
	if (DEBUG_FLAG) {
		printf("Domain Name: %s\n", domainName);
		printf("Database Filename: %s\n", databaseFileName);
	}
	
	// COMPILE GLOBAL REGEX	
    if (compileGlobalRegex() != 0) {
        fprintf(stderr, "Error compiling global regex\n");
        return 1;
    }
	
	// INITIALIZE THE DATABASE, RETURNS DATABASE_SUCCESS 0 IF SUCESSFUL, DATABASE_ERROR 1 IF IT FAILED.
    int rc;	
	rc = initializeDatabase ();
	
    if (rc != DATABASE_SUCCESS ) {
        fprintf(stderr, "General database failure.\n");
        return 1;
    }
	
	printf("Database '%s' active.\n", databaseFileName);
	

    // START THE HTTP DAEMON
	struct MHD_Daemon *daemon;

	daemon = MHD_start_daemon (MHD_USE_AUTO | MHD_USE_INTERNAL_POLLING_THREAD, PORT,
                             NULL, NULL,  // No client connect/disconnect callbacks
                             &requestHandler, NULL,  // Request handler
                             MHD_OPTION_NOTIFY_COMPLETED, requestCompleted,
                             NULL, MHD_OPTION_END);

	if (NULL == daemon) {
        fprintf(stderr, "Failed to start Handler daemon.\n");
        return 1;
    }

    printf("Handler Daemon running on port %d. Type 'q' and press Enter to quit.\n", PORT);
    char input;

    while (1) { // INFINITE LOOP
        input = getchar();  // Get user input
        if (input == 'q') { // Check if input is 'q'
            printf("Exiting Handler daemon...\n");
            break; // Exit the loop
        }
    }

    // STOP HTTP DAEMON
	MHD_stop_daemon (daemon);

    // FREE COMPILED REGEXES BEFORE EXITING
    freeGlobalRegex();

	return 0;
}