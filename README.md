# handle-handler: Manage Subdomain Handles for AT Protocol/Bluesky

Handle-handler (or `handlerd`) is a lightweight solution for resolving Bluesky handles via the `.well-known` HTTPS route instead of DNS. Designed to be small and simple, `handlerd` uses SQLite, [GNU libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/), and a reverse proxy. While still in early development, it performs well enough for bulk verification using the database.

You can easily add handles directly through the web interface. Simply navigate to the subdomain you want to use as your Bluesky handle and enter the DID (without the initial `"did="`). The program performs basic verification of DID and handle formatting. All DIDs are stored in a SQLite database, which can be easily read, exported, and backed up.

## BUILDING

This program is written in portable C. The only external dependencies are `libmicrohttpd` and `libsqlite3`. Additionally, you will need a properly configured reverse proxy, such as `NGINX`, to handle SSL, add necessary headers, and filter out unnecessary requests. Sample configuration files are available in the `examples/` directory.

On Debian/Ubuntu, you can install the required dependencies by running:

```sh
sudo apt install libmicrohttpd-dev libsqlite3-dev
```

After installing the dependencies, you can build the program by running `make` in the source directory. The domain database will be stored in the same directory as the program (a configurable option is planned for a future update).

## REVERSE PROXY CONFIGURATION

Bluesky requires HTTPS for handle verification. To configure HTTPS, you will need a wildcard SSL/TLS certificate, which can be obtained using `certbot`. A sample command is provided below. Please notes that this process involves adding a `_acme-challenge` TXT record to the target domain's DNS configuration. certbot will generate the required record during the process. Once the DNS changes propagate, `certbot` will validate the domain and issue the certificates.

```sh
sudo certbot certonly --manual --preferred-challenges=dns -d example.com -d *.example.com
```

Once you have the wildcard certificates, you can edit the sample `default-ssl-nginx` file as necessary, add it to your `etc/nginx/sites-available/` directory, enable it, test it and restart your server.

Once you have the wildcard certificates, edit the sample `default-ssl-nginx` configuration file as needed. Add it to your `etc/nginx/sites-available/` directory, enable it, test the configuration, and restart your NGINX server.

## TO DO

* Add mirroring options
* Add multiple domains
* Improve the NGINX forwarding
* Improve the CURL requests
* Add automatic forwarding from active handles (302 temp)
* Add reverse extraction via lresolv from the full handle
* Add 15 minute cool down period between changes.
* Verify locked status of handle.
* Implement `disassociate` page.
* Clean up debug code and comments.
* Add stronger concurrent connection handling.
* Add Podman container language.
* Write man pages.
* Add external config.
* Add install option to man.

## MORE INFORMATION

* From Bluesky: ["How to verify your Bluesky account (for domain handles)"](https://bsky.social/about/blog/4-28-2023-domain-handle-tutorial)

## QUESTIONS

Message me in [the fediverse](https://ctrvx.net/chema) if you have any questions.
