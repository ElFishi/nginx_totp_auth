
// Nginx authentication via TOTP. Subrequest authentication
// using a local FastCGI server.

// The auth endpoint is at /auth
// The server will produce a 401 error whenever the request
// lacks the right authentication Cookie. This error must be
// caught by nginx and handled as a redirection to /login
// which will serve the login page configured.
// Once login is completed correctly, the cookie will be set
// and visiting the endpoint will produce a redirect to the
// original website.

#include <thread>
#include <mutex>
#include <regex>
#include <memory>
#include <cmath>
#include <unordered_map>
#include <fstream>
#include <fcgio.h>
#include <unistd.h>
#include <signal.h>
#include <libconfig.h>
#include <list>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "templates.h"
#include "queue.h"
#include "util.h"
#include "ratelimit.h"
#include "logger.h"

#define TOTP_DEF_DIGITS         6
#define TOTP_DEF_PERIOD        30
#define TOTP_DEF_GENS           1      // Allows a window of 90s by default
#define TOTP_DEF_ALGO      "sha1"

enum htAlgo {
	hAlgoSha1    = 0,
	hAlgoSha256  = 1,
	hAlgoSha512  = 2
};

const std::unordered_map<std::string, htAlgo> algnames = {
	{"sha1",    hAlgoSha1},
	{"sha-256", hAlgoSha256},
	{"sha-512", hAlgoSha512},
};

// Use some reasonable default.
int nthreads = 4;

#define MAX_REQ_SIZE    (4*1024)
#define RET_ERR(x) { std::cerr << x << std::endl; return 1; }

typedef std::unordered_map<std::string, std::string> StrMap;

struct cred_t {
	std::string password, totp;  // Pass and TOTP (binary)
	unsigned sduration;          // Duration of a valid session (seconds)
	unsigned digits;             // Digits of TOTP
	unsigned period;             // Period of TOTP
	htAlgo algorithm;            // TOTP hashing algorithm
};

struct web_t {
	std::string webtemplate;      // Template to use
	unsigned totp_generations;    // 0 means only current code is valid,
	                              // 1 means previous and next code is also valid
	                              // 2 means the 2 previous and next codes are also valid, etc
	std::unordered_map<std::string, cred_t> users;  // User to credential
};

std::unordered_map<std::string, web_t> webcfg;   // Hostname -> Config

struct web_req {
	std::string method, host, uri;
	StrMap getvars, postvars, cookies;
	uint64_t ip64;
};

class AuthenticationServer {
private:
	// Secret 'random' string that is used to authenticate cookies
	std::string cookie_secret;

	// Thread to spawn
	std::thread cthread;

	// Shared queue
	ConcurrentQueue<std::unique_ptr<FCGX_Request>> *rq;

	// Rate limiter for auth attempts
	RateLimiter* const rl;

	// Event logging
	Logger *logger;

	// Signal end of workers
	bool end;

	std::string create_cookie(std::string user) {
		std::string payload = std::to_string(time(0)) + ":" + hexencode(user);
		return payload + ":" + hexencode(hmac_sha1(this->cookie_secret, payload));
	}

	// Returns true if the cookie is good.
	bool check_cookie(std::string cookie, const web_t *wcfg) {
		// The cookie format is something like:
		// etime:hex(user):hex(hmac)
		auto p1 = cookie.find(':');
		if (p1 == std::string::npos)
			return false;
		auto p2 = cookie.find(':', p1 + 1);
		if (p2 == std::string::npos)
			return false;
		std::string c1 = cookie.substr(0, p1);
		std::string user = hexdecode(cookie.substr(p1+1, p2-p1-1));
		std::string hmac = hexdecode(cookie.substr(p2+1));
		uint64_t ets = atol(c1.c_str());
		// Lookup by username
		if (!wcfg->users.count(user))
			return false;
		unsigned duration = wcfg->users.at(user).sduration;
		// Not valid if the cookie is too old
		if ((unsigned)time(0) > ets + duration)
			return false;
		// Finally check the HMAC with the secret to ensure the cookie is valid
		std::string hmac_calc = hmac_sha1(this->cookie_secret, cookie.substr(0, p2));
		return (hmac == hmac_calc);
	}

	std::string process_req(web_req *req, const web_t *wcfg) {
		std::string rpage = req->getvars["follow_page"];
		if (rpage.empty())
			rpage = req->postvars["follow_page"];
		if (rpage.empty())
			rpage = "/";    // Make sure we never return empty location, default to index

		if (req->uri == "/auth") {
			// Read cookie and validate the authorization
			bool authed = check_cookie(req->cookies["authentication-token"], wcfg);
			// logger->log("Requested auth with result: " + std::to_string(authed));
			if (authed) {
				logger->log("Requested authentication succeeded");
				return "Status: 200\r\nContent-Type: text/plain\r\n"
				       "Content-Length: 24\r\n\r\nAuthentication Succeeded";
			}
			else {
				logger->log("Requested authentication denied");
				return "Status: 401\r\nContent-Type: text/plain\r\n"
				       "Content-Length: 21\r\n\r\nAuthentication Denied";
			}
		}
		else if (req->uri == "/login") {
			// Die hard if someone's bruteforcing this
			if (rl->check(req->ip64)) {
				logger->log("Rate limit hit for ip id " + std::to_string(req->ip64));
				return "Status: 429\r\nContent-Type: text/plain\r\n"
				       "Content-Length: 34\r\n\r\nToo many requests, request blocked";
			}
			rl->consume(req->ip64);

			bool lerror = false;
			if (req->method == "POST") {
				std::string user = req->postvars["username"];
				std::string pass = req->postvars["password"];
				unsigned    totp = atoi(req->postvars["totp"].c_str());

				// Validate the authentication to issue a cookie or throw an error
				if (wcfg->users.count(user) &&
				    wcfg->users.at(user).password == pass &&
				    totp_valid(wcfg->users.at(user), totp, wcfg->totp_generations)) {

					logger->log("Login successful for user " + user);

					// Render a redirect page to the redirect address (+cookie)
					std::string token = create_cookie(user);
					return "Status: 302\r\nSet-Cookie: authentication-token=" + token +
					       "\r\nLocation: " + stripnl(rpage) + "\r\n\r\n";
				}
				else {
					logger->log("Failed login for user " + user);
					lerror = true;   // Render login page with err message
				}
			}

			// Just renders the login page
			if (!templates.count(wcfg->webtemplate))
				return "Status: 500\r\nContent-Type: text/plain\r\n"
					   "Content-Length: 23\r\n\r\nCould not find template";
			else {
				std::string page = templates.at(wcfg->webtemplate)(req->host, rpage, lerror);
				return "Status: 200\r\nContent-Type: text/html\r\n"
					   "Content-Length: " + std::to_string(page.size()) + "\r\n\r\n" + page;
			}
		}
		else if (req->uri == "/logout") {
			logger->log("Logout requested");
			// Just redirect to the page (if present, otherwise login) deleting cookie
			return "Status: 302\r\nSet-Cookie: authentication-token=null\r\n"
				   "Cache-Control: no-cache, no-store, max-age=0\r\n"
				   "Location: /login\r\n\r\n";
		}
		logger->log("Unknown request for URL: " + req->uri);
		return "Status: 404\r\nContent-Type: text/plain\r\n"
			   "Content-Length: 48\r\nNot found, valid endpoints: /auth /login /logout\r\n\r\n";
	}

public:
	AuthenticationServer(ConcurrentQueue<std::unique_ptr<FCGX_Request>> *rq,
		std::string csecret, RateLimiter* const rl, Logger *logger)
	: rq(rq), rl(rl), logger(logger), end(false)
	{
		// Use work() as thread entry point
		cthread = std::thread(&AuthenticationServer::work, this);
		if (csecret.empty())
			this->cookie_secret = randstr();
		else
			this->cookie_secret = csecret;
	}

	~AuthenticationServer() {
		// Now join the thread
		cthread.join();
	}

	bool totp_valid(cred_t user, unsigned input, unsigned generations) {
		uint32_t ct = time(0) / user.period;
		for (int i = -(signed)generations; i <= (signed)generations; i++)
			if (totp_calc(user.totp, user.algorithm, user.digits, ct + i) == input)
				return true;
		return false;
	}

	static unsigned totp_calc(std::string key, htAlgo algo, uint8_t digits, uint32_t epoch) {
		const uint32_t po10[] = {
			1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000 };
		std::string(* const algtbl[])(std::string, std::string) = {
			hmac_sha1, hmac_sha256, hmac_sha512
		};
		// Key comes in binary format already!
		// Concatenate the epoc in big endian fashion
		uint8_t msg [8] = {
			0, 0, 0, 0,
			(uint8_t)(epoch >> 24),
			(uint8_t)((epoch >> 16) & 255),
			(uint8_t)((epoch >>  8) & 255),
			(uint8_t)(epoch & 255)
		};

		std::string hashs = algtbl[(unsigned)algo](key, std::string((char*)msg, sizeof(msg)));
		uint8_t *hash = (uint8_t*)hashs.c_str();

		// The last nibble of the hash is an offset:
		unsigned off = hash[hashs.size() - 1] & 15;
		// The result is a substr in hash at that offset (pick 32 bits)
		uint32_t value = (hash[off] << 24) | (hash[off+1] << 16) | (hash[off+2] << 8) | hash[off+3];
		value &= 0x7fffffff;
		return value % po10[digits];
	}

	// Receives requests and processes them by replying via a side http call.
	void work() {
		std::unique_ptr<FCGX_Request> req;
		while (rq->pop(&req)) {
			// Read request body and validate it
			int bsize = atoi(FCGX_GetParam("CONTENT_LENGTH", req->envp));
			bsize = std::max(0, std::min(bsize, MAX_REQ_SIZE));

			// Get streams to write
			fcgi_streambuf reqout(req->out);
			fcgi_streambuf reqin(req->in);
			std::iostream obuf(&reqout);
			std::iostream ibuf(&reqin);

			char body[MAX_REQ_SIZE+1];
			ibuf.read(body, bsize);
			body[bsize] = 0;

			// Find out basic info
			web_req wreq;
			wreq.method   = FCGX_GetParam("REQUEST_METHOD", req->envp) ?: "";
			wreq.uri      = FCGX_GetParam("DOCUMENT_URI", req->envp) ?: "";
			wreq.getvars  = parse_vars(FCGX_GetParam("QUERY_STRING", req->envp) ?: "");
			wreq.postvars = parse_vars(body);
			wreq.host     = FCGX_GetParam("HTTP_HOST", req->envp) ?: "";
			wreq.cookies  = parse_cookies(FCGX_GetParam("HTTP_COOKIE", req->envp) ?: "");

			// Extract source IP
			const char *sip = FCGX_GetParam("REMOTE_ADDR", req->envp) ?: "0.0.0.0";
			struct in6_addr res6; struct in_addr res4;
			if (inet_pton(AF_INET6, sip, &res6) == 1)
				wreq.ip64 = ((uint64_t)res6.s6_addr[0] << 40) | ((uint64_t)res6.s6_addr[1] << 32) |
				            ((uint64_t)res6.s6_addr[2] << 24) | ((uint64_t)res6.s6_addr[3] << 16) |
				            ((uint64_t)res6.s6_addr[4] <<  8) | ((uint64_t)res6.s6_addr[5]);
			else if (inet_pton(AF_INET, sip, &res4) == 1)
				wreq.ip64 = res4.s_addr;
			else
				wreq.ip64 = 0;

			// Lookup hostname for this request
			if (!webcfg.count(wreq.host)) {
				logger->log("Failed to find host '" + wreq.host + "'");
				obuf << "Status: 500\r\nContent-Type: text/plain\r\n"
					 << "Content-Length: " << (wreq.host.size() + 18) << "\r\n\r\n"
					 << "Unknown hostname: " << wreq.host;
			}
			else {
				const web_t* wptr = &webcfg.at(wreq.host);
				std::string resp = process_req(&wreq, wptr);

				// Respond with an immediate update JSON encoded too
				obuf << resp;
			}

			FCGX_Finish_r(req.get());
			req.reset();
		}
	}
};

bool serving = true;
void sighandler(int) {
	std::cerr << "Signal caught" << std::endl;
	// Just tweak a couple of vars really
	serving = false;
	// Ask for CGI lib shutdown
	FCGX_ShutdownPending();
	// Close stdin so we stop accepting
	close(0);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " file.conf" << std::endl;
		return 1;
	}

	config_t cfg;
	config_init(&cfg);
	if (!config_read_file(&cfg, argv[1]))
		RET_ERR("Error reading config file");

	// Read config vars
	config_lookup_int(&cfg, "nthreads", (int*)&nthreads);
	nthreads = std::max(nthreads, 1);
	// Number of auth attempts (per ~IP?) per second
	unsigned auths_per_second = 2;
	config_lookup_int(&cfg, "auth_per_second", (int*)&auths_per_second);
	// Secret holds the server secret used to create cookies
	const char *secret;
	if (!config_lookup_string(&cfg, "secret", &secret))
		RET_ERR("'secret' missing, this field is required");
	// Secret holds the server secret used to create cookies
	const char *logpath = "/tmp/totp_auth";
	if (!config_lookup_string(&cfg, "log-path", &logpath))
		std::cerr << "'log-path' not specified, using default /tmp/totp_auth path" << std::endl;

	config_setting_t *webs_cfg = config_lookup(&cfg, "webs");
	if (!webs_cfg)
		RET_ERR("Missing 'webs' config array definition");
	int webscnt = config_setting_length(webs_cfg);
	if (!webscnt)
		RET_ERR("webscnt must be an array of 1 or more elements");

	for (int i = 0; i < webscnt; i++) {
		config_setting_t *webentry  = config_setting_get_elem(webs_cfg, i);
		config_setting_t *hostname  = config_setting_get_member(webentry, "hostname");
		config_setting_t *wtemplate = config_setting_get_member(webentry, "template");
		config_setting_t *totp_gens = config_setting_get_member(webentry, "totp_generations");
		config_setting_t *users_cfg = config_setting_lookup(webentry, "users");

		if (!webentry || !hostname || !wtemplate || !users_cfg)
			RET_ERR("hostname, template and users must be present in the web group");

		web_t wentry = {
			.webtemplate = config_setting_get_string(wtemplate),
			.totp_generations = !totp_gens ? TOTP_DEF_GENS : (unsigned)config_setting_get_int(totp_gens) };

		for (int j = 0; j < config_setting_length(users_cfg); j++) {
			config_setting_t *userentry = config_setting_get_elem(users_cfg, j);
			config_setting_t *user = config_setting_get_member(userentry, "username");
			config_setting_t *pass = config_setting_get_member(userentry, "password");
			config_setting_t *totp = config_setting_get_member(userentry, "totp");
			config_setting_t *algo = config_setting_get_member(userentry, "algorithm");
			config_setting_t *digi = config_setting_get_member(userentry, "digits");
			config_setting_t *peri = config_setting_get_member(userentry, "period");
			config_setting_t *durt = config_setting_get_member(userentry, "duration");

			std::string algorithm = !algo ? TOTP_DEF_ALGO : config_setting_get_string(algo);
			int digits = !digi ? TOTP_DEF_DIGITS : config_setting_get_int(digi);
			int period = !peri ? TOTP_DEF_PERIOD : config_setting_get_int(peri);

			if (!user || !pass || !totp || !durt)
				RET_ERR("username, password, totp and duration must be present in the user group");
			if (digits < 6 || digits > 9)
				RET_ERR("digits must be between 6 and 9 (included)");
			if (period <= 0)
				RET_ERR("period must be bigger than zero");
			if (!algnames.count(algorithm))
				RET_ERR("invalid algorithm specified");

			wentry.users[config_setting_get_string(user)] = cred_t {
				.password = config_setting_get_string(pass),
				.totp = b32dec(b32pad(config_setting_get_string(totp))),
				.sduration = (unsigned)config_setting_get_int(durt),
				.digits = (unsigned)digits,
				.period = (unsigned)period,
				algnames.at(algorithm) };
		}

		webcfg[config_setting_get_string(hostname)] = wentry;
	}

	// Start FastCGI interface
	FCGX_Init();

	// Signal handling
	signal(SIGINT, sighandler); 
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);

	// Start worker threads for this
	auto logger = std::make_unique<Logger>(logpath);
	RateLimiter globalrl(auths_per_second);
	ConcurrentQueue<std::unique_ptr<FCGX_Request>> reqqueue;
	std::vector<std::unique_ptr<AuthenticationServer>> workers;
	for (int i = 0; i < nthreads; i++)
		workers.emplace_back(new AuthenticationServer(
			&reqqueue, secret, &globalrl, logger.get()));

	std::cerr << "All workers up, serving until SIGINT/SIGTERM" << std::endl;

	// Now keep ingesting incoming requests, we do this in the main
	// thread since threads are much slower, unlikely to be a bottleneck.
	while (serving) {
		std::unique_ptr<FCGX_Request> request(new FCGX_Request());
		FCGX_InitRequest(request.get(), 0, 0);

		if (FCGX_Accept_r(request.get()) >= 0)
			// Get a worker that's free and queue it there
			reqqueue.push(std::move(request));
	}

	std::cerr << "Signal caught! Starting shutdown" << std::endl;
	reqqueue.close();
	workers.clear();
	logger.release();

	std::cerr << "All clear, service is down" << std::endl;
}


