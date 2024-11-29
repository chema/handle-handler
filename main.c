// handle-handler (or handlerd) -  Manage Subdomain Handles for AT Protocol/Bluesky
// Copyright (c) 2024 Chema Hernández Gil / AGPL-3.0 license
// https://github.com/chema/handle-handler

#define _GNU_SOURCE // Added to have access to  strcasestr function

#include <sys/types.h>

#include <sys/select.h>
#include <sys/socket.h>
#include <dirent.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "handlerd.h"

#define DEBUG_FLAG 1
#define VERBOSE_FLAG
//#define NGINX_FLAG

#define VERSION "0.15"

// ***************************************************************************
// BEGIN GLOBAL VARS *********************************************************
// ***************************************************************************

// GLOBAL DATABASE DETAILS
const char *domainName = NULL;

// PATHS AND FILE NAMES
const char *baseDirectory = NULL;
const char *filterDatabaseGlobal = NULL;
const char *principalDatabaseGlobal = NULL;

#define RESERVED_HANDLES_FILENAME "reserved.txt"
#define FILTER_DB_FILENAME "filtered-handles.db"
#define PRINCIPAL_DB_FILENAME "active-user-handles.db"

// GLOBAL COMPILED REGEX
regex_t regexDid;
regex_t regexHandle;
regex_t regexLabel;
regex_t regexFullHandle;

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
	
	// HTTP RESPONSE BODY WE WILL RETURN, NULL IF NOT YET KNOWN.
	const char *answerstring;

	// HTTP STATUS CODE WE WILL RETURN, 0 FOR UNDECIDED.
	// unsigned int answercode;
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

// NO DID PLC Response Received
const char *noResponsePage = "<html><body>You did not enter any information. Enter your DID without the 'did=' at the beginning.</body></html>";


const char *userNotFoundPage = "<html><head></head><body>User not found</body></html>";

// HANDLE RECEIVED IS NOT VALID
const char *invalidHandlePage = "<html><body>Handle requested is not valid.</body></html>";
const char *notFoundResponsePage = "<html><body>404 Error: Requested content is not available.</body></html>";

// ***************************************************************************
// END HARD CODED HTML CODE **************************************************
// ***************************************************************************

int usageDaemon (void)
{
	printf("handle-handler (or handlerd) " VERSION " - Manage Subdomain Handles for AT Protocol/Bluesky.\n");
    printf("Copyright (c) 2024 Chema Hernández Gil / AGPL-3.0 license\n");
    printf("\n");
    printf("Commands:\n");
    printf("\n");
    printf("init {basedir}                      Creates restricted handle database (~/.handlerd is suggested)\n");
    printf("update {basedir}                    Updates restricted handle database\n");
    printf("httpd {basedir} {domain name}       Starts HTTPD daemon for given domain name\n");

    return 1;
}

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
		// Validate input
		if (!filePath) {
			fprintf(stderr, "Invalid file path.\n");
			return NULL;
		}	
	
		FILE *file = fopen(filePath, "r");
		if (!file) {
			perror("Failed to open file");
			return NULL;
		}

		// Seek to the end of the file to determine its size
		if (fseek(file, 0, SEEK_END) != 0) {
			perror("Failed to seek to end of file");
			fclose(file);
			return NULL;
		}
		
		long fileSize = ftell(file);
		if (fileSize < 0) {
			perror("Failed to determine file size");
			fclose(file);
			return NULL;
		}
		rewind(file);

		char *buffer = malloc(fileSize + 1);
		if (!buffer) {
			perror("Failed to allocate memory");
			fclose(file);
			return NULL;
		}

		// Read the file into the buffer
		size_t bytesRead = fread(buffer, 1, fileSize, file);
		if (bytesRead < (size_t)fileSize) {
			fprintf(stderr, "Warning: Could not read the entire file. Read %zu out of %ld bytes.\n", bytesRead, fileSize);
		}
		buffer[bytesRead] = '\0'; // Null-terminate the string

		fclose(file);
		return buffer;
	}

// CONSTRUCT AN ABSOLUTE PATH FILE NAMES, TO FREE AT END
const char *buildAbsolutePath (const char *baseDirectory, const char *relativePath) {
    if (!baseDirectory || !relativePath) {
        fprintf(stderr, "Error: Invalid base directory or relative path provided.\n");
        return NULL;
    }

    size_t baseLength = strlen(baseDirectory);
    size_t relLength = strlen(relativePath);

    // Check if the base directory ends with a slash
    int needSlash = (baseLength > 0 && baseDirectory[baseLength - 1] != '/');

    // Allocate memory for the full path
    size_t totalLength = baseLength + relLength + (needSlash ? 1 : 0) + 1 ; // +1 for '\0'
    char *absolutePath = malloc(totalLength);
    if (!totalLength) {
        fprintf(stderr, "Error: Full path memory allocation failed.\n");
        return NULL;
    }

    // Construct the full path
    if (needSlash) {
        snprintf(absolutePath, totalLength, "%s/%s", baseDirectory, relativePath);
    } else {
        snprintf(absolutePath, totalLength, "%s%s", baseDirectory, relativePath);
    }

    return absolutePath;
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

// FUNCTION TO FREE GLOBAL REGEXES (CALLED WHEN PROGRAM EXITS)
void freeGlobalRegexes() {
	// FREE COMPILED REGEXS
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
// CALLER MUST FREE THE RETURNED POINTER
char* removeDomainName(const char *host) {
    // ENSURE HOST AND DOMAINNAME ARE VALID
    if (!host || !domainName) return NULL;

    // Find the domain name in the host
    const char *domainStart = strcasestr(host, domainName);
    if (!domainStart) return NULL; // Domain not found

    size_t hostLength = strlen(host);
    size_t domainLength = strlen(domainName);

    // Validate structure: must have at least one label + '.' before the domain
    if (hostLength <= domainLength + 1 || *(domainStart - 1) != '.') return NULL;

    // Calculate the label length (excluding '.' and domain name)
    size_t labelLength = domainStart - host - 1;

    // Allocate memory for the label segment, including null terminator
    char *labelSegment = malloc(labelLength + 1);
    if (!labelSegment) return NULL; // Handle allocation failure

    // Copy the label segment
    memcpy(labelSegment, host, labelLength);
    labelSegment[labelLength] = '\0'; // Null-terminate the string

    return labelSegment; // CALLER MUST FREE
}

// END REGEX AND VALIDATORS  ******************************************


// ***************************************************************************
// BEGIN GENERIC DATABASE FUNCTIONS ******************************************
// ***************************************************************************

// OPEN A SPECIFIC DATABASE
sqlite3 *openDatabase(const char *dbFile) {
    sqlite3 *db = NULL;
	
    if (sqlite3_open(dbFile, &db) != SQLITE_OK) {
        fprintf(stderr, "Failed to open database '%s': %s\n", dbFile, sqlite3_errmsg(db));
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
        fprintf(stderr, "ERROR: SQL Binding error for key %s: %s\n", key, sqlite3_errmsg(db));
        return SQLITE_ERROR; // Failure
    }
    return SQLITE_OK; // Success
}

// INITIALIZE DATABASE, RETURNS DATABASE_SUCCESS 0 IF SUCESSFUL, DATABASE_ERROR 1 IF IT FAILED.
int initializeUserDatabase( ) {
	char *err_msg = 0;
    int rc;
	
    sqlite3 *db = openDatabase(principalDatabaseGlobal);
    if (!db) {
        return DATABASE_ERROR;
    }

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
        fprintf(stderr, "ERROR: Principal User Table creation failed: %s\n", err_msg);
		sqlite3_free(err_msg);
        sqlite3_close(db);
        return DATABASE_ERROR;
    }

	// #ifdef VERBOSE_FLAG
	// printf("Principal user table ready: '%s'\n", principalDatabaseGlobal);
	// #endif

    // CLOSE THE DATABASE
	sqlite3_free(err_msg);
    sqlite3_close(db);

	return DATABASE_SUCCESS;
}

// INITIALIZE/REBUILD FILTER DATABASE
// RETURNS DATABASE_SUCCESS 0 IF SUCESSFUL, DATABASE_ERROR 1 IF IT FAILED.
int initializeFilterDatabase( ) {
	char *err_msg = 0;
    int rc;
	
    sqlite3 *db = openDatabase( filterDatabaseGlobal );
    if (!db) {
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
	
	const char *reservedHandleFile = buildAbsolutePath( baseDirectory, RESERVED_HANDLES_FILENAME );
	if (!reservedHandleFile) {
		fprintf(stderr, "Memory allocation failure (file name).\n");
		return DATABASE_ERROR;
	}

	#ifdef VERBOSE_FLAG
	printf("Loading database from reserved handle list: %s\n", reservedHandleFile);
	#endif	
	
    char line[512];		// Buffer for reading lines from the file
    FILE *file = fopen(reservedHandleFile, "r");
    if (!file) {
        perror("Failed to open file reserved word list");
		free((char *)reservedHandleFile);
		reservedHandleFile = NULL;
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return DATABASE_ERROR;
    }

    // Read lines from the file and insert them into the database
    while (fgets(line, sizeof(line), file)) {
        // Remove trailing newline character
        line[strcspn(line, "\n")] = '\0';

		// Skip lines that start with '#', which are comments
		if (line[0] == '#') {
			continue;
		}

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
 	free((char *)reservedHandleFile);
	reservedHandleFile = NULL;
	sqlite3_finalize(stmt);

	#ifdef VERBOSE_FLAG
	printf("Reserved handle database is ready: %s\n", filterDatabaseGlobal);
	#endif

    // CLOSE THE DATABASE
	sqlite3_free(err_msg);
    sqlite3_close(db);

	return DATABASE_SUCCESS;
}


// ***************************************************************************
// BEGIN HANDLERD SPECIFIC DATABASE FUNCTIONS ********************************
// ***************************************************************************

void freeNewRecordResult(newRecordResult *record) {
    if (record) {
        free(record->token);  // Free the token
        free(record);         // Free the struct
    }
}

// TRY ADDING A NEW REPORT. RETURNS newRecordResult (result and token IF SUCCESSFUL). CONSIDER MAKING TOKEN INTO A POINTER.
newRecordResult *addNewRecord (const char *handle, const char *did, const char *email) {
	newRecordResult *newRecord = malloc(sizeof(newRecordResult));
	
	if (!newRecord) {
		fprintf(stderr, "ERROR: Memory allocation failed\n");
		return NULL;  // Signal failure to allocate memory
	}
	
	newRecord->result = RECORD_ERROR_DATABASE;  // Default error status
	newRecord->token = NULL;                    // NULL to indicate no token yet

	// ADD AN EMAIL DETAIL CHECKER HERE

    if (!handle || !did ) {
        newRecord->result = RECORD_NULL_DATA;
        return newRecord;
    }	

    if (strlen(handle) == 0 || strlen(did) == 0) {
        newRecord->result = RECORD_EMPTY_DATA;
        return newRecord;
    }	
	
	// VALIDATION
	if ( validateHandle(handle) == KEY_INVALID ) {
		newRecord->result = RECORD_INVALID_HANDLE;
		return newRecord;
	}

	if ( validateDid(did) == KEY_INVALID ) {
		newRecord->result = RECORD_INVALID_DID;
		return newRecord;
	}
	
	// BASIC INFO VALID, CREATE TOKEN
	char tempToken[TOKEN_LENGTH + 1]; // +1 for the null terminator
	generateSecureToken(tempToken);

    sqlite3 *db = openDatabase(principalDatabaseGlobal);
    if (!db) return newRecord;

    // SQL STATEMENT WITH PLACEHOLDERS FOR THE PARAMETERS
	// ALL VALUES ARE NORMALIZED TO LOWERCASE EXCEPT FOR TOKEN
    const char *insertUserRecordSql = 
        "INSERT INTO userTable (handle, did, token, email) "
        "VALUES (LOWER(?), LOWER(?), ?, LOWER(?));";

	// PREPARE SQL STATEMENT
    sqlite3_stmt *stmt = prepareSQLStatement (db, insertUserRecordSql);	
	if (!stmt) return newRecord;

    // Bind parameters to the prepared statement using bindKeyToSQLStatement
    if (bindKeyToSQLStatement(stmt, 1, handle, db) != SQLITE_OK || bindKeyToSQLStatement(stmt, 2, did, db) != SQLITE_OK ||
        bindKeyToSQLStatement(stmt, 3, tempToken, db) != SQLITE_OK || bindKeyToSQLStatement(stmt, 4, email, db) != SQLITE_OK)
	{
        sqlite3_finalize(stmt);
        sqlite3_close(db);
		return newRecord;
	}

    // Execute the statement
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "ERROR: New record creation failed (%d): %s\n", rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
		return newRecord;
    }

    // Dynamically allocate memory for the token
    newRecord->token = strndup(tempToken, TOKEN_LENGTH + 1); // Ensure caller frees this memory
    if (!newRecord->token) {
        fprintf(stderr, "ERROR: Memory allocation for token failed\n");
        free(newRecord);  // Clean up previously allocated memory
        return NULL;
    }
    newRecord->result = RECORD_VALID; // Ensure caller frees this memory
	printf("New record created successfully: %s\n", handle);

    // Finalize the statement and close the database
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return newRecord;
}


// QUERY DATABASE FOR EXISTANCE OF SPECIFIC 'handle'
int handleRegistered (const char *handle) {
	const char *sql = "SELECT 1 FROM userTable WHERE handle = ? LIMIT 1;";
	
	// OPEN DATABASE
    sqlite3 *db = openDatabase(principalDatabaseGlobal);
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
		sqlite3_close(db);
		return HANDLE_ACTIVE; // Exists
	} else if (rc == SQLITE_DONE) {
		if (DEBUG_FLAG) printf("DEBUG: Handle '%s' does not exist in the database.\n", handle);
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return HANDLE_INACTIVE; // Does not exist
	} else {
		fprintf(stderr, "Error executing query: %s\n", sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return HANDLE_ERROR; // Error
	}
}

// QUERY DB TABLE RESERVED WORDS FOR SPECIFIC 'LABEL' 'handle'
int labelReserved (const char *label) {
	const char *sql = "SELECT 1 FROM reservedHandleTable WHERE word = ? LIMIT 1;";

	// OPEN DATABASE
    sqlite3 *db = openDatabase( filterDatabaseGlobal );
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
		sqlite3_close(db);
		return HANDLE_ACTIVE; // Exists
	} else if (rc == SQLITE_DONE) {
		if (DEBUG_FLAG) printf("DEBUG: Label '%s' is not a reserved word .\n", label);
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return HANDLE_INACTIVE; // Does not exist
	} else {
		fprintf(stderr, "Error executing query: %s\n", sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return HANDLE_ERROR; // Error
	}
}


// QUERY DATABASE FOR A VALID IDENTIFIER ASSOCIATED WITH HANDLE
const char* queryForDid (const char *handle) {
	#ifdef VERBOSE_FLAG
	printf("VERBOSE: Begin queryForDid for handle '%s'.\n", handle);
	#endif	

	// OPEN DATABASE
    sqlite3 *db = openDatabase(principalDatabaseGlobal);
	if (!db) return NULL;

	// PREPARE SQL STATEMENT
    const char *sql = "SELECT did FROM userTable WHERE handle = ?";
    sqlite3_stmt *stmt = prepareSQLStatement (db, sql);	
	if (!stmt) return NULL;

	// BIND HANDLE KEY TO PREPARED SQL STATEMENT (1 IS SUCCESS)
    if ( bindKeyToSQLStatement(stmt, 1, handle, db) != SQLITE_OK ) {
        sqlite3_finalize(stmt);
		sqlite3_close(db);
        return NULL;
    }

	// REMEMBER TO FREE RESULT
	char *result = NULL;
	
    // Execute SQL statement
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *tempResult = (const char *)sqlite3_column_text(stmt, 0);
        // Validate and copy the result
        if (tempResult && validateDid(tempResult) != KEY_INVALID) {
            result = strndup(tempResult, MAX_SIZE_DID_PLC + 1);
            if (!result) fprintf(stderr, "ERROR: Failed to allocate memory for DID result buffer.\n");
        } else fprintf(stderr, "ERROR: DID value for handle '%s' is null or invalid. Remove record.\n", handle);
    }
	
    // Clean up
    sqlite3_finalize(stmt);
    sqlite3_close(db);

	#ifdef VERBOSE_FLAG
	if (result) printf("VERBOSE: queryForDid found 'did' result for handle '%s': '%s'.\n", handle, result);
	else printf("VERBOSE: No result found for handle '%s'.\n", handle);
	#endif	

    return result; // CALLER MUST FREE THE RESULT
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

// Send Response with Specific Content-Type Header
// TO DO: ADD A unsigned int status_code OPTION
static enum MHD_Result sendDynamicResponse (struct MHD_Connection *connection, const char *content, const char *contentType)
{
	enum MHD_Result ret;
	struct MHD_Response *response;

	response = MHD_create_response_from_buffer (strlen (content), (void *) content, MHD_RESPMEM_MUST_COPY);
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
        return MHD_NO; // Return failure if file reading failed
    }

    response = MHD_create_response_from_buffer(strlen(htmlContent), (void *)htmlContent, MHD_RESPMEM_MUST_FREE);
	if (! response) {
		free(htmlContent); // Cleanup in case of failure
		return MHD_NO; 
	}

	// Add content type header to response
	if (contentType != NULL) MHD_add_response_header(response, "Content-Type", contentType);

	// Queue the response
	if (filename == STATIC_NOTFOUND) ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
	else ret = MHD_queue_response(connection, MHD_HTTP_OK, response);

	// Clean up
	MHD_destroy_response(response);

	return ret;
}

// SEND THE RESPONSE TO THE WELL-KNOWN DID PLC METHOD
// TO DO: CHANGE FROM LABEL/SEGMENT TO FULL HANDLE
static enum MHD_Result sendWellKnownResponse (struct MHD_Connection *connection, const char *label)
{
	// QUERY DATABASE FOR *VALID* DID PLC ASSOCIATED WITH 'label'
	const char *tempDid = queryForDid(label);
	struct MHD_Response *response;
	enum MHD_Result ret;
	
	if (tempDid != NULL ) { 
		// CONFIRM DID EXISTS AND SEND IT AS PLAIN TEXT
       if (DEBUG_FLAG) printf("DEBUG: Found DID for label '%s': %s\n", label, tempDid);	
	
	     // MHD TAKES OWNERSHIP OF tempDid AND WILL FREE IT   
		// response = MHD_create_response_from_buffer (strlen (tempDid), (void *)tempDid, MHD_RESPMEM_MUST_COPY);
		response = MHD_create_response_from_buffer (strlen (tempDid), (void *)tempDid, MHD_RESPMEM_MUST_FREE);
		if (!response) {
			free((char *)tempDid);
			return MHD_NO;
		}
		// Add text content type header to response
		MHD_add_response_header( response, MHD_HTTP_HEADER_CONTENT_TYPE, CONTENT_TEXT );
		// Queue the response
		ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
		// Clean up
		MHD_destroy_response(response);
		// free((char *)tempDid);
		return ret; // tempDid FREED BY MHD
	}

    // HANDLE CASE WHERE DID IS NOT FOUND	
    if (DEBUG_FLAG) printf("DEBUG: DID not found for label '%s'. Sending 404 response.\n", label);
	
	response = MHD_create_response_from_buffer (strlen (userNotFoundPage), (void *)userNotFoundPage, MHD_RESPMEM_PERSISTENT);	
	if (!response) return MHD_NO;
	MHD_add_response_header( response, MHD_HTTP_HEADER_CONTENT_TYPE, CONTENT_HTML );		
	ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
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
  struct connectionInfoStruct *con_info = coninfo_cls;
  (void) kind;               /* Unused. Silent compiler warning. */
  (void) filename;           /* Unused. Silent compiler warning. */
  (void) content_type;       /* Unused. Silent compiler warning. */
  (void) transfer_encoding;  /* Unused. Silent compiler warning. */
  (void) off;                /* Unused. Silent compiler warning. */
  
	if (DEBUG_FLAG) printf("DEBUG: Begin iteratePost: key=%s, data=%s\n", key, data);
	if (DEBUG_FLAG) printf("connection: handle=%s, did=%s, email=%s\n", con_info->handle, con_info->did, con_info->email);

	if ( validateHandle (con_info->handle) ==  KEY_INVALID ) {
		if (DEBUG_FLAG) printf("DEBUG: Invalid iteratePost handle value, exiting: '%s'\n", con_info->handle);
		con_info->answerstring = invalidHandlePage;
		return MHD_NO;
	}
  
	// THIS KEY IS A GENERAL DID:PLC FIELD WITH EITHER A DID OR A FULL HANDLE. ONLY NEED TO MAKE SURE IT EXISTS AND THAT IT IS >0
  	if ( strcmp(key, "did") == 0 ) {
		if ( (size > 0) && (size <= MAXDIDSIZE) )  
		{
			con_info->did = strndup(data, size);
			return MHD_YES; // Iterate again looking for email.
		} else
		{
			printf("No DID:PLC Data Entered, exiting\n"); // NO DATA ENTERED
			con_info->answerstring = noResponsePage;
			return MHD_NO;
		}
	}
	
	if ( (NULL != con_info->did) && (strcmp(key, "email") == 0) && (size <= MAXDIDSIZE) ) {
		if ( size == 0 ) {
			char *emailDeclinedString = "NO EMAIL PROVIDED";
			con_info->email = strndup(emailDeclinedString, strlen(emailDeclinedString));
			return MHD_YES;
		} else {
			con_info->email = strndup(data, size);
			return MHD_YES;
		}
	}

	return MHD_YES;
}

// CLEAN-UP FUNCTION AFTER REQUEST COMPLETED
static void requestCompleted (void *cls, struct MHD_Connection *connection, void **con_cls, enum MHD_RequestTerminationCode toe)
{
	struct connectionInfoStruct *con_info = *con_cls;
	(void) cls;         /* Unused. Silent compiler warning. */
	(void) connection;  /* Unused. Silent compiler warning. */
	(void) toe;         /* Unused. Silent compiler warning. */

	if (NULL == con_info) return;
	
	if (con_info->connectiontype == POST)
    {
		MHD_destroy_post_processor (con_info->postprocessor);
	}
	
/* 	// COMMENTED OUT BECAUSE ONE WILL 1ST REQUIRES FREEING, BUT SECOND ONE DOES NOT
	#ifdef NGINX_FLAG

	#else
	// FIX THIS, WHICH WILL BE DYNAMICALLY ALLOCATED, NEEDS TO BE FREED
	#endif */

	// Free each member that was dynamically allocated	
	if (con_info->handle) {
		free((char *)con_info->handle);
	}

	if (con_info->did) {
		free((char *)con_info->did);
	}
	
	if (con_info->email) {
		free((char *)con_info->email);
	}	
	
	if (con_info->answerstring) {
		free((char *)con_info->answerstring);
	}	

	
    // Free the structure itself
	free (con_info);
	*con_cls = NULL;
}

// *********************************************
// ********* PRINCIPAL REQUEST HANDLER *********
// *********************************************


static enum MHD_Result
print_out_key (void *cls, enum MHD_ValueKind kind, const char *key,
               const char *value)
{
  (void) cls;    /* Unused. Silent compiler warning. */
  (void) kind;   /* Unused. Silent compiler warning. */
  printf ("%s: %s\n", key, value);
  return MHD_YES;
}



static enum MHD_Result requestHandler	   (void *cls, struct MHD_Connection *connection,
												const char *url, const char *method,
												const char *version, const char *upload_data,
												size_t *upload_data_size, void **con_cls)
{
	printf ("New %s request for %s using version %s\n", method, url, version);	
	// MHD_get_connection_values (connection, MHD_HEADER_KIND, print_out_key, NULL);
	
	(void) cls;               /* Unused. Silent compiler warning. */
	//(void) version;           /* Unused. Silent compiler warning. */
	
	if (NULL == *con_cls) { // FIRST CALL, SETUP DATA STRUCTURES, SOME ONLY APPLY TO 'POST' REQUESTS
		printf ("IF CONDITION: con_cls is null\n\n"); // TEST
		
		// VALIDATE HOST HEADER. A REVERSE PROXY SHOULD MAKE THIS UNNECESSARY
		const char *hostHeader = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Host");		
		if (!hostHeader || (NULL == strcasestr(hostHeader, domainName))) {			
			printf ("IF CONDITION: invalid host\n"); // TEST
			// REJECT REQUEST IF HOST HEADER DOES NOT CONTAIN THE EXPECTED DOMAIN
			return MHD_NO;
		}
		
		struct connectionInfoStruct *con_info;

		con_info = malloc (sizeof (struct connectionInfoStruct));
		if (NULL == con_info) {
			fprintf(stderr, "Error: Memory allocation failed for connection information.\n");
			return MHD_NO;	// INTERNAL ERROR
		}

		// DIRECT COPIES OF CONSTANTS, NO NEED TO FREE
		con_info->host = hostHeader;

 		#ifdef NGINX_FLAG
			// 'X-ATPROTO-HANDLE' OPTIONAL AND REQUIRES NGINX REVERSE PROXY.
			con_info->handle = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-ATPROTO-HANDLE");
		#else
			// DYNAMICALLY ALLOCATED, NEEDS TO BE FREED
			con_info->handle = removeDomainName(con_info->host);
		#endif

		printf ("con_info->host=%s & con_info->handle: %s\n",con_info->host,con_info->handle);

		con_info->did = NULL;
		con_info->email = NULL;

		// INITIALIZE THE CONNECTIONINFO STRUCT. SOME ONLY APPLY TO 'POST' REQUESTS.
		if ( 0 == strcasecmp (method, MHD_HTTP_METHOD_POST) ) {
			con_info->postprocessor = MHD_create_post_processor (connection, POSTBUFFERSIZE, iteratePost, (void *) con_info);

			// Creating postprocessor failed, free memory manually and exit.
			if (NULL == con_info->postprocessor) {
				free (con_info);
				fprintf(stderr, "ERROR: Creating postprocessor failed.\n");
				return MHD_NO; // INTERNAL ERROR
			}
			
			con_info->connectiontype = POST;
		}
		else { 
			//PROBABLY BEST TO CONFIRM IT IS A GET REQUEST?
			con_info->connectiontype = GET;
		}

		// INITIALIZE ANSWER VARIABLES
		con_info->answerstring = NULL;			// Make sure answer string is empty

		*con_cls = (void *) con_info;

		printf("MHD_YES: con_info: host=%s, handle=%s, connectiontype=%d\n\n", con_info->host, con_info->handle, con_info->connectiontype);
		return MHD_YES; // QUITE CERTAIN THIS IS NOT NECESSARY FOR GETS
	}

	struct connectionInfoStruct *con_info = *con_cls;

	// **************************************
	// ************ GET REQUESTS ************
	// **************************************
	if (0 == strcasecmp (method, MHD_HTTP_METHOD_GET))
	{	
		// WELL-KNOWN URL FOR DID PLC PAYLOAD (ATPROTO REQUIREMENT) GETS RESPONSE BASED ON DATABASE RESULTS
		// NO NEED FOR HANDLE/SUBDOMAIN VERIFICATION
		if ( 0 == strcasecmp (url, URL_WELL_KNOWN_ATPROTO) ) return sendWellKnownResponse (connection, con_info->handle);
		
		if ( (0 == strcasecmp (url, "/")) && (validateHandle (con_info->handle) ==  KEY_VALID ) ) {
			// CREATE A NEW RESPONSE PAGE FOR THIS BLOCK
			if ( (handleRegistered(con_info->handle) == HANDLE_ACTIVE) ) return sendFileResponse (connection, STATIC_ACTIVE, HTML_CONTENT);
			if ( (labelReserved(con_info->handle) == HANDLE_ACTIVE) ) return sendFileResponse (connection, STATIC_RESERVED, HTML_CONTENT); 
			return sendFileResponse (connection, STATIC_REGISTER, HTML_CONTENT); 
		}
			
		// ALL OTHER URLS RECEIVE A 404 RESPONSE
		return sendFileResponse (connection, STATIC_NOTFOUND, HTML_CONTENT);
	}
	
	// ***************************************
	// ************ POST REQUESTS ************
	// ***************************************	
	if (0 == strcasecmp (method, MHD_HTTP_METHOD_POST))	
	{

		// Checks to see if all data has been received from iterative POST requests
		if (*upload_data_size != 0)
		{
			MHD_post_process (con_info->postprocessor, upload_data, *upload_data_size);
			*upload_data_size = 0;
			return MHD_YES;
		}
		
		if ( con_info->did && con_info->email ) {
			if (DEBUG_FLAG) printf("ALL DATA RECEIVED: handle=%s, did=%s, email=%s\n", con_info->handle, con_info->did, con_info->email);
			
			newRecordResult *record = addNewRecord(con_info->handle, con_info->did, con_info->email);
			
			if (!record) {
				fprintf(stderr, "ERROR: Memory allocation failed (newRecordResult).\n");
				con_info->answerstring = strdup("ERROR: Memory allocation failed (newRecordResult).");
				return MHD_NO; // Signal failure to allocate memory
			}		
			
			switch (record->result) {
				case RECORD_VALID:
					printf("Valid record created. Token: %s\n", record->token);

					// CREATE ANSWER STRING
					char tempAnswer[MAXANSWERSIZE];

					// Format the string into the temporary array
					snprintf(tempAnswer, MAXANSWERSIZE, confirmationPage, record->token);

					// Assign the string to con_info->answerstring, duplicating it for persistent storage
					con_info->answerstring = strdup(tempAnswer);
					break;
				case RECORD_INVALID_DID:
					printf( ERROR_INVALID_DID "\n");
					con_info->answerstring = strdup(ERROR_INVALID_DID);
					break;
				case RECORD_INVALID_LABEL:
					printf( ERROR_INVALID_LABEL " \n");
					con_info->answerstring = strdup(ERROR_INVALID_LABEL);
					break;
				case RECORD_INVALID_HANDLE:
					printf( ERROR_INVALID_HANDLE " \n");
					con_info->answerstring = strdup(ERROR_INVALID_HANDLE);
					break;
				case RECORD_NULL_DATA:  // Consolidated handling
				case RECORD_EMPTY_DATA:
					printf( ERROR_NULL_OR_EMPTY_DATA " \n");
					con_info->answerstring = strdup(ERROR_NULL_OR_EMPTY_DATA);
					break;
				case RECORD_ERROR_DATABASE:
					printf( ERROR_DATABASE " \n");
					con_info->answerstring = strdup(ERROR_DATABASE);
					break;
				default:
					printf("Error: Unknown validation result.\n");
					con_info->answerstring = strdup("ERROR: Record creation failed.");
					break;
			}
			freeNewRecordResult(record);  // Clean up

			if (!con_info->answerstring) return MHD_NO;  // Handle strdup failure			
			
			return sendResponse (connection, con_info->answerstring, HTML_CONTENT);
		}
	}

	// Not a GET or a POST, generate error
	// ADD A STATUS OPTION FOR 404 AND 403
	return sendResponse (connection, errorPage, HTML_CONTENT);
}

// *********************************
// ********* FILES PATHS ***********
// *********************************

// CONSTRUCT GLOBAL PATH FILE NAMES, TO FREE AT END
int buildAbsoluteDatabasePaths ()
{
	principalDatabaseGlobal = buildAbsolutePath( baseDirectory, PRINCIPAL_DB_FILENAME );
	if (!principalDatabaseGlobal) {
		fprintf(stderr, "Memory allocation failure (DB).\n");
		return 1;
	}
	
	filterDatabaseGlobal = buildAbsolutePath( baseDirectory, FILTER_DB_FILENAME );
	if (!filterDatabaseGlobal) {
		fprintf(stderr, "Memory allocation failure (DB).\n");
		return 1;
	}
	
	return 0; // Success
}

// NAIVELY CONFIRM BASE DIRECTORY IS AN ABSOLUTE PATH
int isAbsolutePath(const char *path) {
    return path && path[0] == '/';
}

// FUNCTION TO FREE GLOBAL PATHS (CALLED WHEN PROGRAM EXITS)
void freeGlobalPaths () {
	// FREE DATABASE PATH GLOBALS
	free((char *) principalDatabaseGlobal);
	principalDatabaseGlobal = NULL;
	free((char *) filterDatabaseGlobal);
	filterDatabaseGlobal = NULL;
}


// *********************************
// ********* MAIN FUNCTION *********
// *********************************

int main(int argc, char *argv[])
{	
	if ( argc < 3) {
        return usageDaemon();
    }
	
	const char *commandArg = argv[1];  	  // Initial command
	baseDirectory = argv[2];  			  // User-provided base directory	
	domainName = argv[3];


	// CONSTRUCT GLOBAL PATH FILE NAMES. TO FREE AT END
	int confirmBase = isAbsolutePath(baseDirectory);
	if ( confirmBase == FALSE ){
		fprintf(stderr, "Error: Invalid base directory.\n");
		return 1;
	}
	
	int DBPaths = buildAbsoluteDatabasePaths();
	if ( DBPaths == 1) return 1;
	
	
	// ***************************************	
	// COMMAND: FILTER DATABASE INITIALIZATION
	// ***************************************	

	if ( (strcmp(commandArg, "init") == 0) || (strcmp(commandArg, "update") == 0) ) {
		printf("Base directory: %s\n", baseDirectory);
		
		// INITIALIZE FILTER DATABASE, RETURNS DATABASE_SUCCESS=0 ON SUCCESS, DATABASE_ERROR=1 ON FAILURE.
		int rc;	
		rc = initializeFilterDatabase ();
		
		if (rc != DATABASE_SUCCESS ) {
			fprintf(stderr, "Error: Filter Database Failure.\n");
			return 1;
		}
				
		// FREE GLOBALS
		freeGlobalPaths ();
	
		return 0;
	}

	// ************************************
	// COMMAND NORMAL HTTP DEAMON OPERATION
	// ************************************

	if ( strcmp( commandArg, "httpd") == 0 ) {
		#ifdef VERBOSE_FLAG
		printf("Base directory: %s\n", baseDirectory);
		printf("Active domain name: %s\n", domainName);
		#endif
		
		// COMPILE GLOBAL REGEX	
		if (compileGlobalRegex() != 0) {
			fprintf(stderr, "Error compiling global regex\n");
			return 1;
		}
		
		// INITIALIZE THE DATABASE, RETURNS DATABASE_SUCCESS 0 IF SUCESSFUL, DATABASE_ERROR 1 IF IT FAILED.
		int rc;	
		rc = initializeUserDatabase ();
		
		if (rc != DATABASE_SUCCESS ) {
			fprintf(stderr, "Error: User Database Failure.\n");
			return 1;
		}
		
		#ifdef VERBOSE_FLAG
		printf("User database is active: %s\n", principalDatabaseGlobal);
		printf("Reserved handle database: %s\n", filterDatabaseGlobal);	
		#endif
	
		// START THE HTTP DAEMON
		struct MHD_Daemon *daemon;

		// TO DO, ADD MHD_OPTION_STRICT_FOR_CLIENT OF 1 FOR STRICT HOST DETAILS
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

		// FREE REGEXES
		freeGlobalRegexes ();
		// FREE GLOBALS
		freeGlobalPaths ();
		
		return 0;
	}
	
	// FREE GLOBALS
	freeGlobalPaths ();
		
	return 0;
}