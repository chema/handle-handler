# handle-handler
 Handles subdomain handles for AT Protocol/Bluesky

Currently split into two programs:

* **addhandle:** Adds handles and DIDs to a SQLite DB. Will create one if it doesn't exist. Does very basic verification of DID formatting.
* **handlerd:** Reads DIDs from a SQLite DB. Does very basic verification of DID formatting and serves it.

## NOTE

The domain will require HTTPS for use with Bluesky. Use certbot using the following command and edit default-ssl-nginx file as necessary: 

sudo certbot certonly --manual --preferred-challenges=dns -d example.com -d *.example.com
