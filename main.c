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

#define PLACEHOLDER_ERROR "{{ ERROR }}"
#define PLACEHOLDER_TOKEN "{{ TOKEN }}"


// GLOBAL COMPILED REGEX
regex_t regexDid;
regex_t regexHandle;
regex_t regexLabel;
regex_t regexFullHandle;
regex_t regexDidPLC;

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

// Struct to hold CURL response data
struct curlResponse {
    char *data;
    size_t size;
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


// **************************************************************************
// ****** CURL FUNCTIONS ****************************************************
// **************************************************************************


// CALLBACK FUNCTION TO HANDLE INCOMING DATA
size_t curlReceiveData (void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total_size = size * nmemb;
    struct curlResponse *response = (struct curlResponse *)userdata;

    // Allocate or expand memory to hold the response data
    char *new_data = realloc(response->data, response->size + total_size + 1);
    if (new_data == NULL) {
        fprintf(stderr, "CURL: Failed to allocate memory for response data\n");
        return 0;
    }

    response->data = new_data;
    memcpy(response->data + response->size, ptr, total_size);
    response->size += total_size;
    response->data[response->size] = '\0'; // Null-terminate the string

    return total_size;
}

const char *getWellKnownDID (const char *handle)
{
    CURL *curl;
    CURLcode res;
	char url[URL_MAX_SIZE]; // WHAT IS THE MAX SIZE?
    char errbuf[CURL_ERROR_SIZE];
	const char *DID = NULL;
    struct curlResponse response = {NULL, 0};
	
    // INITIALIZE LIBCURL
    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "CURL: Failed to initialize curl\n");
        return NULL;
    }
	
    // PROVIDE A BUFFER TO STORE ERRORS IN
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
	 
    // SET THE ERROR BUFFER AS EMPTY BEFORE PERFORMING A REQUEST
    errbuf[0] = 0;

    // Construct the URL using the URL_WELL_KNOWN
    snprintf(url, sizeof(url), _URL_WELL_KNOWN_, handle);
	if (snprintf(url, sizeof(url), _URL_WELL_KNOWN_, handle) >= sizeof(url)) {
        fprintf(stderr, "CURL: URL is too long\n");
        curl_easy_cleanup(curl);
        return NULL;
    }

   // Set curl options
	curl_easy_setopt(curl, CURLOPT_URL, url);                  			// URL to fetch
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlReceiveData );	// Callback function to store data
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);      			// Pass the response struct

	// PERFORM THE FILE TRANSFER
	res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "CURL: Error from libcurl: %s\n", curl_easy_strerror(res));
        if (response.data) free(response.data);
        curl_easy_cleanup(curl);
        return NULL;
    }
	
	// CONFIRM HEADER AND CODE CHECK
	// ADD VALIDATOR
	
	if( response.size != MAX_SIZE_DID_PLC) {
		printf("CURL: Response is not the correct size (32): %ld\n", response.size);
        if (response.data) free(response.data);
        curl_easy_cleanup(curl);
        return NULL;
    }
	
	// Output the received DID
	printf("CURL: Received DID: %s\n", response.data);
	printf("CURL: Received DID size: %ld\n", response.size);

	DID = strndup(response.data, response.size);
	if (!DID) {
		fprintf(stderr, "CURL: Failed to allocate memory for DID\n");
		free(response.data);
		curl_easy_cleanup(curl);
		return NULL;
	}
	
	// Clean up
	free(response.data); // Free the allocated memory
	curl_easy_cleanup(curl);
	return DID;
}


// **************************************************************************
// ****** FILE READING ******************************************************
// **************************************************************************
	
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

// *********************************
// ********* FILES PATHS ***********
// *********************************

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


// HELPER FUNCTION TO REPLACE PLACEHOLDER_ERROR TEXT IN A STRING
char* replacePlaceholder(const char *html, const char* placeholder, const char *message)
{
	// printf("PLACEHOLDER: %s\n", placeholder);
	// printf("MESSAGE: %s\n", message);

    const char *pos = strstr(html, placeholder);
    if (pos == NULL) {
        fprintf(stderr, "ERROR: Placeholder not found.\n");
        return NULL;
    }

    // Calculate sizes
    size_t html_len = strlen(html);
    size_t placeholder_len = strlen(placeholder);
    size_t replacement_len = strlen(message);

	// printf("LENGTHS: %ld %ld %ld\n", html_len, placeholder_len, replacement_len );


    // Allocate new buffer for the updated string
    size_t new_len = html_len - placeholder_len + replacement_len;

	// printf("NEW LENGTH: %ld\n", new_len );

    char *new_html = malloc(new_len + 1);
    if (new_html == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    // Copy parts of the original string and insert the message
    size_t prefix_len = pos - html;
    strncpy(new_html, html, prefix_len); // Copy up to the placeholder
    new_html[prefix_len] = '\0'; // Null-terminate the prefix
    strcat(new_html, message); // Append the replacement message
    strcat(new_html, pos + placeholder_len); // Append the rest

	// printf("NEW HTML: %s\n", new_html );

    return new_html;
}

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

	// Compile DID PLC regex
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
	
	// Compile DID PLC regex
	ret = regcomp(&regexDidPLC, DID_PLC_SPEC_PATTERN, REG_EXTENDED | REG_ICASE );
    if ( ret != 0 ) return handleRegexError(ret, &regexDidPLC, "DID PLC Regex");		
	
    return 0;  // Success
}

// FUNCTION TO FREE GLOBAL REGEXES (CALLED WHEN PROGRAM EXITS)
void freeGlobalRegexes() {
	// FREE COMPILED REGEXS
    regfree(&regexDid);   // Free DID regex
    regfree(&regexHandle); // Free handle regex
    regfree(&regexLabel); // Free label regex
	regfree(&regexFullHandle); // Free handle regex
	regfree(&regexDidPLC); // Free handle regex	
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

// EXTRACT THE FIRST DID FROM A GIVEN STRING
void extractDid (const char *input, char *output, size_t output_size) {
    regmatch_t match[1]; // Array to hold match result

    // Execute the regex on the input string using global regexDidPLC
    if (regexec(&regexDidPLC, input, 1, match, 0) == 0) {
        // Match found, extract the substring
        size_t start = match[0].rm_so; // Start of match
        size_t end = match[0].rm_eo;   // End of match
        size_t match_length = end - start;

        if (match_length < output_size) {
            strncpy(output, input + start, match_length);
            output[match_length] = '\0'; // Null-terminate the output
        } else {
            fprintf(stderr, "Match is too large for the output buffer\n");
        }
    } else {
        // No match found
        fprintf(stderr, "No match found\n");
        output[0] = '\0';
    }
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

// END REGEX AND VALIDATORS  *************************************************


// ***************************************************************************
// BEGIN GENERIC DATABASE FUNCTIONS ******************************************
// ***************************************************************************

// OPEN A SPECIFIC DATABASE
sqlite3 *databaseOpen(const char *dbFile) {
    sqlite3 *db = NULL;
	
    if (sqlite3_open(dbFile, &db) != SQLITE_OK) {
        fprintf(stderr, "Failed to open database '%s': %s\n", dbFile, sqlite3_errmsg(db));
        if (db) sqlite3_close(db); // Ensure cleanup in case of partial initialization
        return NULL;
    }
	
    return db;
}

// PREPARE A SQL STATEMENT
sqlite3_stmt *databasePrepareStatement (sqlite3 *db, const char *sql) {
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
int databaseBindKey(sqlite3_stmt *stmt, int index, const char *key, sqlite3 *db) {
    if (sqlite3_bind_text(stmt, index, key, -1, SQLITE_STATIC) != SQLITE_OK) {
        fprintf(stderr, "ERROR: SQL Binding error for key %s: %s\n", key, sqlite3_errmsg(db));
        return SQLITE_ERROR; // Failure
    }
    return SQLITE_OK; // Success
}

// GENERIC FUNCTION TO QUERY A DATABASE FOR EXISTENCE OF A SPECIFIC VALUE
int databaseGenericSingularQuery (const char *dbPath, const char *sql, const char *value) {
    // OPEN DATABASE
    sqlite3 *db = databaseOpen(dbPath);
    if (!db) return HANDLE_ERROR;

    // PREPARE SQL STATEMENT
    sqlite3_stmt *stmt = databasePrepareStatement(db, sql);
    if (!stmt) {
        sqlite3_close(db);
        return HANDLE_ERROR;
    }

    // BIND VALUE TO PREPARED SQL STATEMENT
    if (databaseBindKey(stmt, 1, value, db) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return HANDLE_ERROR;
    }

    // Execute the query
    int rc = sqlite3_step(stmt);
    int result;
    if (rc == SQLITE_ROW) {
        if (DEBUG_FLAG) printf("DEBUG: Value '%s' is active (isValueInTable)\n", value);
        result = HANDLE_ACTIVE; // Exists
    } else if (rc == SQLITE_DONE) {
        if (DEBUG_FLAG) printf("DEBUG: Value '%s' is not active (isValueInTable)\n", value);
        result = HANDLE_INACTIVE; // Does not exist
    } else {
        fprintf(stderr, "Error executing query: %s\n", sqlite3_errmsg(db));
        result = HANDLE_ERROR; // Error
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}


// ***************************************************************************
// BEGIN HANDLERD SPECIFIC DATABASE FUNCTIONS ********************************
// ***************************************************************************


// INITIALIZE DATABASE, RETURNS DATABASE_SUCCESS 0 IF SUCESSFUL, DATABASE_ERROR 1 IF IT FAILED.
int initializeUserDatabase( ) {
	char *err_msg = 0;
    int rc;
	
    sqlite3 *db = databaseOpen(principalDatabaseGlobal);
    if (!db) {
        return DATABASE_ERROR;
    }

	// SQL COMMENTARY: ADDITIONAL INDEXING IS UNNECESSARY SINCE 'did' COLUMN IS UNIQUE
	// VIEW THE INDEX LIST USING: PRAGMA index_list(did_plc_users);
    char *sqlCreateTable = "CREATE TABLE IF NOT EXISTS did_plc_users ("
                "handle TEXT PRIMARY KEY, "				// HOST / HANDLE, e.g., 'myhandle.baysky.social'
				"did TEXT NOT NULL UNIQUE, "
				"label TEXT NOT NULL UNIQUE, "			// LABEL, e.g., 'myhandle'
				"domain TEXT NOT NULL, "				// DOMAIN NAME, e.g., 'baysky.social'
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

	#ifdef VERBOSE_FLAG
	printf("Principal user table ready: '%s'\n", principalDatabaseGlobal);
	#endif

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
	
    sqlite3 *db = databaseOpen( filterDatabaseGlobal );
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
    sqlite3_stmt *stmt = databasePrepareStatement (db, sqlInsertWord);	
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


void freeNewRecordResult(newRecordResult *record) {
    if (record) {
        free(record->token);  // Free the token
        free(record);         // Free the struct
    }
}


// TRY ADDING A NEW REPORT. RETURNS newRecordResult (result and token IF SUCCESSFUL). CONSIDER MAKING TOKEN INTO A POINTER.
// HANDLE IS HOST, LABEL IS SUBDOMAIN
newRecordResult *addNewRecord (const char *handle, const char *label, const char *did, const char *email) {
	newRecordResult *newRecord = malloc(sizeof(newRecordResult));
	
	if (!newRecord) {
		fprintf(stderr, "ERROR: Memory allocation failed\n");
		return NULL;  // Signal failure to allocate memory
	}
	
	newRecord->result = RECORD_ERROR_DATABASE;  // Default error status
	newRecord->token = NULL;                    // NULL to indicate no token yet

	// ADD AN EMAIL DETAIL CHECKER HERE

    if (!handle || !label || !did ) {
        newRecord->result = RECORD_NULL_DATA;
        return newRecord;
    }	

    if ( strlen(handle) == 0 || strlen(label) == 0 || strlen(did) == 0 ) {
        newRecord->result = RECORD_EMPTY_DATA;
        return newRecord;
    }	
	
	// VALIDATION
	
	// ADD HANDLE (HOST) VALIDATION, UNTIL THEN ASSUME IT IS OK
	
	if ( validateHandle(label) == KEY_INVALID ) {
		newRecord->result = RECORD_INVALID_LABEL;
		return newRecord;
	}

	if ( validateDid(did) == KEY_INVALID ) {
		newRecord->result = RECORD_INVALID_DID;
		return newRecord;
	}
	
	// BASIC INFO VALID, CREATE TOKEN
	char tempToken[TOKEN_LENGTH + 1]; // +1 for the null terminator
	generateSecureToken(tempToken);

    sqlite3 *db = databaseOpen(principalDatabaseGlobal);
    if (!db) return newRecord;

    // SQL STATEMENT WITH PLACEHOLDERS FOR THE PARAMETERS
	// ALL VALUES ARE NORMALIZED TO LOWERCASE EXCEPT FOR TOKEN
    const char *insertUserRecordSql = 
        "INSERT INTO did_plc_users (handle, did, label, domain, token, email) "
        "VALUES (LOWER(?), LOWER(?), LOWER(?), LOWER(?), ?, ?);";

	// PREPARE SQL STATEMENT
    sqlite3_stmt *stmt = databasePrepareStatement (db, insertUserRecordSql);	
	if (!stmt) return newRecord;

    // Bind parameters to the prepared statement using databaseBindKey
    if (databaseBindKey(stmt, 1, handle, db) != SQLITE_OK || databaseBindKey(stmt, 2, did, db) != SQLITE_OK ||
		databaseBindKey(stmt, 3, label, db) != SQLITE_OK || databaseBindKey(stmt, 4, domainName, db) != SQLITE_OK ||
        databaseBindKey(stmt, 5, tempToken, db) != SQLITE_OK || databaseBindKey(stmt, 6, email, db) != SQLITE_OK)
	{
		fprintf(stderr, "ERROR: Unable to prepare SQL statement: %s\n", sqlite3_errmsg(db));
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


// QUERY PRINCIPAL DATABASE FOR EXISTENCE OF SPECIFIC 'handle' (FULL HOST NAME)
int handleRegistered(const char *handle) {
	const char *sql = "SELECT 1 FROM did_plc_users WHERE handle = ? LIMIT 1;";
    return databaseGenericSingularQuery(principalDatabaseGlobal, sql, handle);
}


// QUERY FILTER DATABASE FOR EXISTENCE OF SPECIFIC 'word' (LABEL/SUBDOMAIN)
int labelReserved(const char *word) {
    const char *sql = "SELECT 1 FROM reservedHandleTable WHERE word = ? LIMIT 1;";
    return databaseGenericSingularQuery(filterDatabaseGlobal, sql, word);
}


// QUERY DATABASE FOR A VALID IDENTIFIER ASSOCIATED WITH HANDLE
const char* queryForDid (const char *handle) {
	#ifdef VERBOSE_FLAG
	printf("VERBOSE: Begin queryForDid for handle '%s'.\n", handle);
	#endif	

	// OPEN DATABASE
    sqlite3 *db = databaseOpen(principalDatabaseGlobal);
	if (!db) return NULL;

	// PREPARE SQL STATEMENT
    const char *sql = "SELECT did FROM did_plc_users WHERE handle = ?";
    sqlite3_stmt *stmt = databasePrepareStatement (db, sql);	
	if (!stmt) return NULL;

	// BIND HANDLE KEY TO PREPARED SQL STATEMENT (1 IS SUCCESS)
    if ( databaseBindKey(stmt, 1, handle, db) != SQLITE_OK ) {
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


// SENDS AN HTML ERROR RESPONSE WITH A CUSTOM MESSAGE
// TO DO: MAKE SURE THE STATIC_ERROR IS A ABSOLUTE PATH
static enum MHD_Result sendErrorResponse (struct MHD_Connection *connection, const char *message)
{
	#ifdef VERBOSE_FLAG
	printf("RESPONSE: '%s' with '%s'\n", message, STATIC_ERROR);
	#endif
	
	enum MHD_Result ret;
	struct MHD_Response *response;
	char *htmlContent;	
	char *responseContent;

	htmlContent = readFile(STATIC_ERROR);
	if (!htmlContent) {
		fprintf(stderr, "ERROR: Failed to access file '%s' (sendErrorResponse)\n", STATIC_ERROR);
		return MHD_NO; // SIGNAL FAILURE TO READ FILE
	}

	responseContent = replacePlaceholder(htmlContent, PLACEHOLDER_ERROR, message);
	free(htmlContent);
	if (!responseContent) {
        fprintf(stderr, "ERROR: Failed to replace placeholder in file content (sendErrorResponse)\n");
		return MHD_NO; // SIGNAL FAILURE TO ALLOCATE MEMORY
	}
	
	response = MHD_create_response_from_buffer(strlen(responseContent), (void *)responseContent, MHD_RESPMEM_MUST_FREE );
	if (!response) {
		fprintf(stderr, "ERROR: Memory allocation failed (sendErrorResponse)\n");
		free(responseContent); // Must free, MHD_RESPMEM_MUST_FREE did not work
		return MHD_NO; // SIGNAL FAILURE TO ALLOCATE MEMORY
	}

	// Add content type header to response
	MHD_add_response_header(response, "Content-Type", CONTENT_HTML);

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
static enum MHD_Result sendWellKnownResponse (struct MHD_Connection *connection, const char *handle)
{
	// QUERY DATABASE FOR *VALID* DID PLC ASSOCIATED WITH 'label'
	const char *tempDid = queryForDid(handle);
	struct MHD_Response *response;
	enum MHD_Result ret;
	
	if (tempDid != NULL ) { 
		// CONFIRM DID EXISTS AND SEND IT AS PLAIN TEXT
       if (DEBUG_FLAG) printf("DEBUG: Found DID for handle '%s': %s\n", handle, tempDid);	
	
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
    if (DEBUG_FLAG) printf("DEBUG: DID not found for handle '%s'. Sending 404 response.\n", handle);
	
	response = MHD_create_response_from_buffer (strlen (userNotFoundPage), (void *)userNotFoundPage, MHD_RESPMEM_PERSISTENT);	
	if (!response) return MHD_NO;
	MHD_add_response_header( response, MHD_HTTP_HEADER_CONTENT_TYPE, CONTENT_HTML );		
	ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
	// Clean up
	MHD_destroy_response(response);

	return ret;
}

// SEND THE RESPONSE TO A NEW USER REQUEST
// TO DO: CHANGE FROM LABEL/SEGMENT TO FULL HANDLE
static enum MHD_Result sendNewUserResponse (struct MHD_Connection *connection, const char *handle, const char *label, const char *did, const char *email)
{
	if ( !handle || !label || !did || !email )
	{
		fprintf(stderr, "ERROR: Incomplete user information (sendNewUserResponse)\n");
		return MHD_NO; // SIGNAL PROCESSING INFORMATION
	}

	#ifdef VERBOSE_FLAG
	printf("RESPONSE: New Record Attempt for: handle=%s, label=%s, did=%s, email=%s\n", handle, label, did, email);
	#endif

	newRecordResult *record = addNewRecord(handle, label, did, email);
	if (!record)
	{
		fprintf(stderr, "ERROR: Unable to create new record result (sendNewUserResponse)\n");
		return MHD_NO; // SIGNAL FAILURE TO ALLOCATE MEMORY
	}
	
	if ( record->result != RECORD_VALID)
	{
		printf("ERROR: Unable to create new user record.\n"); //DEBUG
		const char *errorMessage;
		
		switch (record->result) {
			case RECORD_INVALID_DID:
				errorMessage = ERROR_INVALID_DID;
				break;		
			case RECORD_INVALID_HANDLE:
				errorMessage = ERROR_INVALID_HANDLE;
				break;
			case RECORD_INVALID_LABEL:
				errorMessage = ERROR_INVALID_LABEL;
				break;			
			case RECORD_NULL_DATA:  // CONSOLIDATED HANDLING
			case RECORD_EMPTY_DATA:
				errorMessage = ERROR_NULL_OR_EMPTY_DATA;
				break;
			case RECORD_ERROR_DATABASE:
				errorMessage = ERROR_DATABASE;
				break;
			default:
				errorMessage = "ERROR: Unknown validation result.";	
				break;
		}
		
		freeNewRecordResult(record);
		return sendErrorResponse (connection, errorMessage);
	}
		
	#ifdef VERBOSE_FLAG
	printf("RESPONSE: New Record Created, token: %s\n", record->token);
	#endif
	
	enum MHD_Result ret;
	struct MHD_Response *response;
	char *htmlContent;	
	char *responseContent;	
	
	htmlContent = readFile(STATIC_SUCCESS);
	if (!htmlContent) {
		freeNewRecordResult(record);
		fprintf(stderr, "ERROR: Failed to access file '%s' (sendNewUserResponse)\n", STATIC_SUCCESS);		
		return MHD_NO; // SIGNAL FAILURE TO READ FILE
	}

	// REPLACE TOKEN PLACEHOLDER
	responseContent = replacePlaceholder(htmlContent, PLACEHOLDER_TOKEN, record->token);

	free(htmlContent);
	if (!responseContent) {
        fprintf(stderr, "ERROR: Failed to replace placeholder in file content (sendNewUserResponse)\n");
		freeNewRecordResult(record);
		return MHD_NO; // SIGNAL FAILURE TO ALLOCATE MEMORY
	}		
	
	response = MHD_create_response_from_buffer(strlen(responseContent), (void *)responseContent, MHD_RESPMEM_MUST_FREE );
	if (!response) {
		fprintf(stderr, "ERROR: Memory allocation failed (sendNewUserResponse)\n");
		freeNewRecordResult(record); // Cleanup in case of failure
		free(responseContent); // Must free, MHD_RESPMEM_MUST_FREE did not work
		return MHD_NO; // SIGNAL FAILURE TO ALLOCATE MEMORY
	}

	// ADD CONTENT TYPE HEADER TO RESPONSE
	MHD_add_response_header(response, "Content-Type", CONTENT_HTML);

	// QUEUE THE RESPONSE
	ret = MHD_queue_response(connection, MHD_HTTP_OK, response);

	// CLEAN UP
	MHD_destroy_response(response);
	freeNewRecordResult(record);
	// free(responseContent); // NOT NECESSARY DUE TO MHD_RESPMEM_MUST_FREE FLAG
	return ret;
}


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
  
	// THIS KEY IS A GENERAL DID:PLC FIELD WITH EITHER A DID OR A FULL HANDLE. ONLY NEED TO MAKE SURE IT EXISTS AND THAT IT IS <= MAXDIDSIZE
  	if ( strcmp(key, "did") == 0 ) {
        printf("POST: DID key value: %s\n", data);
		
		// BASIC SIZE SANITY CHECK: (14-254 chars)
		if (size > 14 && size <= DATA_MAX_SIZE) {
			printf("POST: Data entered could contain valid DID:PLC or handle: '%s'\n", data);

			if (strcasestr(data, "bsky.social")) {
				printf("POST: Valid 'bsky.social' handle entered: %s\n", data);		
				const char *tempDID = getWellKnownDID(data);
				if (tempDID) {
					printf("POST: DID:PLC found via CURL: %s\n", tempDID);
					con_info->did = strndup(tempDID, MAX_SIZE_DID_PLC);
					free((void *)tempDID);
					return MHD_YES; // Iterate again looking for email.
				}
				printf("POST: No Valid DID:PLC found via CURL: '%s'\n", data);
				return MHD_NO;
			}

			char output[MAX_SIZE_DID_PLC + 1] = {0};
			extractDid(data, output, sizeof(output));
			if (output[0] != '\0') {
				printf("POST: Valid DID:PLC found in data: '%s'\n", output);
				con_info->did = strndup(output, strlen(output));
				return MHD_YES;
			}
			printf("POST: No valid DID:PLC or handle found in data: '%s'\n", data);
			return MHD_NO;
		}
		
		
/* 		// BASIC SIZE SANITY CHECK: (14-254 chars)
		if ( size > 14 && size <= DATA_MAX_SIZE )
		{					
			printf("POST: Data entered could contain valid DID:PLC or handle: '%s'\n", data); 
			if ( NULL != strcasestr(data, "bsky.social") ) // NOT NULL MEANS data INCLUDES "bsky.social"
			{
				const char *tempDID = getWellKnownDID (data);
				if (tempDID) { // Check if DID is not NULL
					printf("POST: Received Handle: %s\n", data);
					printf("POST: CURL Response: %s\n", tempDID);
					con_info->did = strndup(tempDID, MAX_SIZE_DID_PLC);
					free((void *)tempDID);
					return MHD_YES; // Iterate again looking for email.
				} else
				{
					// DO I NEED TO FREE tempDID HERE?
					printf("POST: No Valid DID:PLC found via CURL: '%s'\n", data);
					return MHD_NO; // included bsky.social was unable to be resolved into a valid DID.
				}
			} else
			{
				char output[MAX_SIZE_DID_PLC + 1];
				extractDid(data, output, sizeof(output));
				if (output[0] != '\0') {
					printf("POST: Valid DID:PLC found in data: '%s'\n", output);
					con_info->did = strndup(output, sizeof(output));
					return MHD_YES; // Iterate again looking for email.
				} else {			
					printf("POST: No Valid DID:PLC found in data: '%s'\n", data); 
					return MHD_NO;
				}
			}
		} */
	}

	if ( (strcmp(key, "email") == 0) && (size <= 256 ) ) {
		if ( (data != NULL) && (size > 0) )  con_info->email = strndup(data, size);		
		return MHD_NO;			
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

static enum MHD_Result requestHandler	   (void *cls, struct MHD_Connection *connection,
												const char *url, const char *method,
												const char *version, const char *upload_data,
												size_t *upload_data_size, void **con_cls)
{
	printf ("*****************************************************\n");	
	printf ("REQUEST: New %s request for %s using version %s\n", method, url, version);	
	
	(void) cls;               /* Unused. Silent compiler warning. */
	//(void) version;           /* Unused. Silent compiler warning. */
	
	if (NULL == *con_cls) { // FIRST CALL, SETUP DATA STRUCTURES, SOME ONLY APPLY TO 'POST' REQUESTS
	
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

		return MHD_YES; // QUITE CERTAIN THIS IS NOT NECESSARY FOR GETS
	}

	struct connectionInfoStruct *con_info = *con_cls;

	// **************************************
	// ************ GET REQUESTS ************
	// **************************************
	if (0 == strcasecmp (method, MHD_HTTP_METHOD_GET))
	{	
		// SEND RESPONSE TO WELL-KNOWN URL GET REQUEST BASED ON DATABASE RESULTS HANDLE/SUBDOMAIN
		// HANDLE/SUBDOMAIN VERIFICATION NOT NECESSARY
		if ( 0 == strcasecmp (url, URL_WELL_KNOWN_ATPROTO) )
		{
			return sendWellKnownResponse (connection, con_info->host);
		}
		
		if ( (0 == strcasecmp (url, "/")) && (validateHandle (con_info->handle) ==  KEY_VALID ) ) {
			// CREATE A NEW RESPONSE PAGE FOR THIS BLOCK
			if ( (handleRegistered(con_info->host) == HANDLE_ACTIVE) ) return sendFileResponse (connection, STATIC_ACTIVE, CONTENT_HTML);
			if ( (labelReserved(con_info->handle) == HANDLE_ACTIVE) ) return sendFileResponse (connection, STATIC_RESERVED, CONTENT_HTML); 
			return sendFileResponse (connection, STATIC_REGISTER, CONTENT_HTML); 
		}
			
		// ALL OTHER URLS RECEIVE A 404 RESPONSE
		return sendFileResponse (connection, STATIC_NOTFOUND, CONTENT_HTML);
	}
	
	// ***************************************
	// ************ POST REQUESTS ************
	// ***************************************	
	if (0 == strcasecmp (method, MHD_HTTP_METHOD_POST))	
	{
		// THIS VALIDATION IS PROBABLY UNNECESSARY SINCE IT IS PRINCIPALLY ABOUT LENGTH
		// if ( validateHandle (con_info->handle) ==  KEY_INVALID ) {
			// return sendErrorResponse (connection, ERROR_INVALID_LABEL);	
		// }
		
		if ( 0 != *upload_data_size )
		{
			MHD_post_process (con_info->postprocessor, upload_data, *upload_data_size);		
			*upload_data_size = 0;
			return MHD_YES;
		} 
		
		if ( con_info->did == NULL ) return sendErrorResponse (connection, "DID or Bluesky handle invalid, try again");
		
		
		if ( con_info->did != NULL && con_info->email != NULL )
		{
			return sendNewUserResponse (connection, con_info->host, con_info->handle, con_info->did, con_info->email);
		}
		
		if ( con_info->did != NULL  && con_info->email == NULL )
		{
			return sendNewUserResponse (connection, con_info->host, con_info->handle, con_info->did, "NO EMAIL PROVIDED");
		}

		// ALL OTHER POST REQUESTS GET A MHD_NO
		// return MHD_NO;
	}

	// GENERAL ERROR MESSAGE
	return sendErrorResponse (connection, ERROR_REQUEST_FAILED);	
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