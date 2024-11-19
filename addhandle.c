#include <stdio.h>
#include <stdlib.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>
#include <regex.h>
#include <microhttpd.h>

#define PORT 5677

void printDomainDB(sqlite3 *db);
int verify_did(const char *did);


int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <domain name> <user handle> <user DID>\n", argv[0]);
        return 1;
    }

    char *domainName = argv[1];
    char *userHandle = argv[2];
    char *userDid = argv[3];
    char databaseFileName[256];

    // Create database name by appending "-domain" to the provided domain name
    snprintf(databaseFileName, sizeof(databaseFileName), "%s-domain.db", domainName);

    sqlite3 *db;
    char *err_msg = 0;
    int rc;

    // Open the database
    rc = sqlite3_open(databaseFileName, &db);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database for %s: %s\n", domainName, sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    printf("Database '%s' created/opened successfully.\n", databaseFileName);

    // Create a table within domain database to store subdomain handles
    char *sql = "CREATE TABLE IF NOT EXISTS userHandleTable ("
                "userHandle TEXT PRIMARY KEY, "
                "userDID TEXT NOT NULL, "
                "userEmail TEXT NOT NULL, "
                "createdTime TEXT, "
                "updatedTime TEXT);";

    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return 1;
    }
	
    // Example DID to insert
    const char *example_did = "did:plc:7wm5b6dzk54ukznrxdlpp23f";
	const char *exampleEmail = "user1@example.com";

    // Validate the DID
    if (!verify_did(example_did)) {
        fprintf(stderr, "Invalid DID format: %s\n", example_did);
        sqlite3_close(db);
        return 1;
    }
	
	// Validate the DID
    if (!verify_did(userDid)) {
        fprintf(stderr, "Invalid DID format: %s\n", userDid);
        sqlite3_close(db);
        return 1;
    }	
	
    // Get the current Unix time
    time_t currentTime = time(NULL);
    char insertUserRecordSql[512];

    // Insert sample data with the current Unix time as the created_date
	snprintf(insertUserRecordSql, sizeof(insertUserRecordSql),
         "INSERT OR IGNORE INTO userHandleTable (userHandle, userDID, userEmail, createdTime, updatedTime) "
         "VALUES ('%s', '%s', '%s', %ld, NULL);",
         userHandle, userDid, exampleEmail, (long)currentTime);
		 
	printf("Generated SQL: %s\n", insertUserRecordSql);

    rc = sqlite3_exec(db, insertUserRecordSql, 0, 0, &err_msg);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
    }

    // Print out the contents of the table
    printDomainDB(db);


    // Close the database
    sqlite3_close(db);

    return 0;
}

void printDomainDB(sqlite3 *db) {
    const char *sql = "SELECT userHandle, userDID, userEmail, createdTime, updatedTime FROM userHandleTable;";
    sqlite3_stmt *stmt;

    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to fetch data: %s\n", sqlite3_errmsg(db));
        return;
    }

    printf("Contents of the 'userHandleTable' table:\n");
    printf("------------------------------------------------------------------------\n");
    printf("| Handle | DID               | Email           | Created   | Updated   |\n");
    printf("------------------------------------------------------------------------\n");

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *userHandle = sqlite3_column_text(stmt, 0);
        const unsigned char *userDid = sqlite3_column_text(stmt, 1);
        const unsigned char *userEmail = sqlite3_column_text(stmt, 2);
        const unsigned char *createdTime = sqlite3_column_text(stmt, 3);
        const unsigned char *updatedTime = sqlite3_column_text(stmt, 4);

        printf("| %s | %s | %s | %s | %s |\n", userHandle, userDid, userEmail, createdTime, updatedTime);
    }

    printf("------------------------------------------------------------------------\n");

    sqlite3_finalize(stmt);
}

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


