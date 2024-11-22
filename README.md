# handle-handler
Handles subdomain handles for AT Protocol/Bluesky

Small and simple, `handlerd` uses SQLite, [GNU libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) and Nginx for some improved security and SSL. This is a very, very early state process, but works for good enough for bulk verification using the database.
 
You can add handles directly from the web interface! Just go to the subdomain you want to use as your Bluesky handle and enter the DID without the initial `"did="`. The program does very basic verification of DID and handle formatting. DIDs are read from and stored in a SQLite DB.
 
## TO DO

* Allow only one subdomain per handle.
* Add 15 minute cool down period.
* Verify locked status.
* Implement `disassociate` page.
* Implement stronger handle review.
* Clean up debug code and comments.
* Add stronger concurrent connection handling.
* Compile regex code once.
* Add Podman container language.
* Write man pages.
* Add external config.
* Add install option to man.

## NOTE

Bluesky will require HTTPS. Use certbot using the following command and edit default-ssl-nginx file as necessary:

```
sudo certbot certonly --manual --preferred-challenges=dns -d example.com -d *.example.com
```