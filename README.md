
TOTP based Nginx authentication
===============================

Based on the `ngx_http_auth_request_module` module.

What?
-----

This module allows you to gate a website/service behind an nginx webserver
(or proxy) and protect it using username/password and OTP code.
It is possible to configure multiple websites, users and passwords and OTP
code seeds too.

How!?
-----

First it is necessary to create a configuration for the authenticator service
itself. A config file can look like:


```
nthreads = 4;
secret = "some-random-string-that-is-relatively-long-used-for-cookie-minting";
webs = (
  {
    hostname = "someweb.example.com";
    template = "gradient";
    users = (
      {
        username = "user1";
        password = "password123!";
        totp = "base32otpsecretgoeshere";
        duration = 3600;
      },
      {
        username = "user2";
        password = "password123!";
        totp = "base32otpsecretgoeshere";
        duration = 3600;
        algorithm = "sha-512";
        digits = 6;
        period = 30;
      }
    );
  },
  {
    hostname = "anotherweb.com";
    template = "customtemplate";
    totp_generations = 2;
    users = (
      {
        username = "user2";
        password = "password123456";
        totp = "base32otpsecretgoeshere";
        duration = 7200;
        digits = 6;
        period = 30;
      }
    );
  }
);
```

The `secret` variable is a random secret string that must be the same across
servers (frontends) in case the service is replicated. This string is used to
calculate the HMAC for the authentication cookies. If empty, it will be generated
at startup, and this will cause logout of all users on a server restart.

Each entry in `webs` must have a valid `hostname` which must match the hostname
for the vhost in the nginx configuration. Since the authenticator supports
HTML templates for the login website, they must be chosen using the `template`
variable (currently only one exists: "gradient").

Each web has a list of configured `users` with a `password`, `username` and a
`totp` secret (base32 encoded). A cookie validity period (in seconds) must be
specified in `duration`, after this time the user will be logged out.
Optionally the `digit` count and `period` (in seconds) can be specified, being
6 and 30 the respective defaults. The default hashing algorithm is SHA1 but can
be changed by specifying a new `algorithm` with possible values being "sha1",
"sha-256" and "sha-512". As of February 2024 Google Authenticator and 2FAS Auth 
apps can generate codes with "sha-256" and "sha-512", while MS Authenticator and
Authy apps can not.

The service can be run using this example systemd service:

```
[Unit]
Description=NGINX TOTP authenticator service
After=network.target

[Service]
User=root
Type=simple
ExecStart=/usr/bin/spawn-fcgi -u www-data -s /var/www/totp_auth/sock -M 666 -n /usr/local/bin/totp_auth.bin /var/www/totp_auth/config.conf
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

In this example we use a local socket file for the fastcgi connection.


nginx config
------------

First of all, for a given vhost, we need to create the necessary auth endpoints:

```
# Authentication related endpoints
location ~ /(auth|login|logout)$ {
    include         fastcgi_params;
    fastcgi_pass    unix:/var/www/totp_auth/sock;
    break;
}
```

If these endspoints don't match your server, you will have to pick different
paths and rewrite the URLs to match them.

Then, in every location directive, you need to add the authentication request:

```
location ~ {
    auth_request /auth;
    error_page 401 = @error401;
}
```

This will cause nginx to call /auth every time you load a resource (locally)
to figure out whether the user is allowed or not (by forwarding the requests
headers so that the service can validate the cookie). On error the 401 status
code will be thrown, which we need to redirect to the right login page, ie:

```
location @error401 {
    return 302 https://yourwebsite.com/login?follow_page=$scheme://$http_host$request_uri;
}
```

This will cause a 302 redirect whenever the user doesn't have access. The
original URL is passed so that it can be redirected back once done.

And... that's pretty much it!


