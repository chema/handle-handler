#include <stdio.h>
#include <stdlib.h>
#include <sqlite3.h>
#include <string.h>
#include <regex.h>
#include <microhttpd.h>

#define PORT 8123
#define TARGET_URL "/.well-known/atproto-did"
#define DEBUG_FLAG 0

// REQUIRES -lmicrohttpd -lsqlite3

const char *domainName = NULL;
char databaseFileName[256];

int verify_did(const char *did) {
    const char *pattern = "^did:[a-z]+:[a-zA-Z0-9._:%-]*[a-zA-Z0-9._-]$";
    regex_t regex;
    int ret;

    // Compile DID pattern using recommandation from https://atproto.com/specs/did
    ret = regcomp(&regex, pattern, REG_EXTENDED);
    if (ret) {
        fprintf(stderr, "Could not compile regex\n");
        return 0;
    }

    // Execute the regular expression
    ret = regexec(&regex, did, 0, NULL, 0);
    regfree(&regex);

    // Return whether the regex matched
    return ret == 0;
}

// Function to query the database for a DID associated with the handle
const char* get_did(const char *handle) {
    sqlite3 *db;
    const char *sql = "SELECT userDID FROM userHandleTable WHERE userHandle = ?";
    sqlite3_stmt *stmt;	
	static char result[256]; // Static buffer to hold the DID (CONFIRM max size of the DID!)
    result[0] = '\0';        // Ensure the buffer is empty

    // Open SQLite database
    if (sqlite3_open(databaseFileName, &db) != SQLITE_OK) {
        printf("Database error: %s\n", sqlite3_errmsg(db));
        return NULL;
    }

    // Prepare the SQL statement
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        printf("Query preparation error: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }

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
        const char *did = (const char *)sqlite3_column_text(stmt, 0);
        if (did) {
			// Validate DID
			if (!verify_did(did)) {
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


// Request handler
static enum MHD_Result requestHandler(void *cls, struct MHD_Connection *connection,
                                      const char *url, const char *method,
                                      const char *version, const char *upload_data,
                                      size_t *upload_data_size, void **con_cls) {
    const char *responseData = NULL;
    const char *did = NULL;
	struct MHD_Response *response;
    enum MHD_Result ret;

	if (DEBUG_FLAG) printf("Request received: url=%s, method=%s\n", url, method); 	// DEBUG

	// Extract header details from connection
    const char *hostHeader = NULL;
	const char *handleHeader = NULL;

    // Get the 'Host' header value
    hostHeader = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Host");
	// Get the 'Host' header value
    handleHeader = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "X-ATPROTO-HANDLE");

    // DEBUG
	if (DEBUG_FLAG) {
		if (hostHeader) printf("Host: %s\n", hostHeader);
		if (handleHeader) printf("X-ATPROTO-HANDLE: %s\n", handleHeader);
		printf("Domain Name from Header: %s\n", hostHeader);
		printf("Domain Name from Arguments: %s\n", domainName);
		printf("Domain Name from comparison 1: %s\n", strstr(hostHeader, domainName));
		printf("Domain Name from comparison 2: %d\n", strcmp(method, "GET"));
		printf("Domain Name from comparison 3: %s\n", strstr(url, TARGET_URL));
		printf("Response data: %s\n", responseData);
	}

	// CONFIRM THREE CONDITIONS:
	// 1. Target domain name is included in the host header (nginx is already doing this)
	// 2. Confirm GET method (Bluesky requirement)
	// 3. Confirm the target URL (Bluesky requirement)
	if ( strstr(hostHeader, domainName) != NULL && strcmp(method, "GET") == 0 && strstr(url, TARGET_URL) != NULL ) {
		// Request DID
		did = get_did(handleHeader);
		if ( did ) {
			// Set the response data
			responseData = did;

			// Respond with the DID or error
			response = MHD_create_response_from_buffer(strlen(responseData),
													  (void*)responseData,
													  MHD_RESPMEM_PERSISTENT);
			if (!response) return MHD_NO;

			// Add headers to response (content type and close)
			// NEED TO ADD LANGUAGE TO THE NGINX CONNECTION CONFIG
			MHD_add_response_header(response, "Content-Type", "text/plain");
			MHD_add_response_header(response, "Connection", "close");
			
			// Queue the response
			ret = MHD_queue_response(connection, MHD_HTTP_OK, response);

			// Clean up
			MHD_destroy_response(response);
			
			return ret;
		}
	}
	
    // If not a valid request, return a 404
    responseData = "Not found\n";

    // Create a response
    response = MHD_create_response_from_buffer(strlen(responseData),
                                               (void*)responseData,
                                               MHD_RESPMEM_PERSISTENT);
    if (!response) return MHD_NO;

	// Add Connection close header
	MHD_add_response_header(response, "Connection", "close");

    // Queue the response
    ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);

    // Clean up
    MHD_destroy_response(response);

    return ret;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
        fprintf(stderr, "Usage: %s <domain name>\n", argv[0]);
        return 1;
    }

    domainName = argv[1];
	
    // Create database name by appending "-domain" to the provided domain name
    snprintf(databaseFileName, sizeof(databaseFileName), "%s-domain.db", domainName);
	
	printf("Domain Name: %s\n", domainName);
	printf("Database Filename: %s\n", databaseFileName);

	
    struct MHD_Daemon *server;

    // Start the HTTP server
    server = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION, PORT,
                              NULL, NULL,  // No client connect/disconnect callbacks
                              &requestHandler, NULL,  // Request handler
                              MHD_OPTION_END);
    if (!server) {
        fprintf(stderr, "Failed to start server\n");
        return 1;
    }

    printf("Server running on port %d...\n", PORT);

    // Keep running until the user presses Enter
    getchar();

    // Stop the server
    MHD_stop_daemon(server);

    return 0;
}