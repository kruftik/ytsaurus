var Q = require("q");

var YtError = require("./error").that;
var utils = require("./utils");

////////////////////////////////////////////////////////////////////////////////

var __DBG = require("./debug").that("A", "Authentication");

////////////////////////////////////////////////////////////////////////////////

function YtAuthentication(config, logger, authority)
{
    "use strict";
    this.__DBG = __DBG.Tagged();

    this.config = config;
    this.logger = logger;
    this.authority = authority;

    this.token = undefined;
    this.login = false;
    this.realm = false;

    this.__DBG("New");
}

YtAuthentication.prototype.dispatch = function(req, rsp, next)
{
    "use strict";
    this.__DBG("dispatch");

    if (this._extractToken(req, rsp)) {
        return this._epilogue(req, rsp, next);
    }

    var result = this.authority.authenticate(
        this.logger,
        req.origin || req.connection.remoteAddress,
        this.token);

    // XXX(sandello): Q is not able to determine that we have handled a rejection.
    Q.isPromise(result) && result.fail(function(){});

    this.login = Q(result).get("login");
    this.realm = Q(result).get("realm");

    return this._epilogue(req, rsp, next);
};

YtAuthentication.prototype._epilogue = function(req, rsp, next)
{
    "use strict";
    this.__DBG("_epilogue");

    var self = this;

    return void Q
    .all([ self.login, self.realm ])
    .spread(
    function(login, realm) {
        if (typeof(login) === "string" && typeof(realm) === "string") {
            self.logger.debug("Client has been authenticated", {
                authenticated_user: login,
                authenticated_from: realm
            });
            req.authenticated_user = login;
            req.authenticated_from = realm;
            process.nextTick(next);
            return;
        } else {
            self.logger.debug("Client has failed to authenticate");
            return utils.dispatchUnauthorized(
                rsp,
                "OAuth scope=" + JSON.stringify(self.config.grant));
        }
    },
    function(err) {
        var error = YtError.ensureWrapped(err);
        // XXX(sandello): Embed.
        self.logger.info("An error occured during authentication", {
            error: error.toJson()
        });
        return utils.dispatchLater(rsp, 60);
    })
    .done();
};

YtAuthentication.prototype._extractToken = function(req, rsp)
{
    "use strict";
    this.__DBG("_extractToken");

    if (!this.config.enable) {
        this.logger.debug("Authentication is disabled");
        // Fallback to root credentials.
        this.login = "root";
        this.realm = "root";
        return true;
    }

    if (!req.headers.hasOwnProperty("authorization")) {
        this.logger.debug("Client is missing Authorization header");
        // Fallback to guest credentials.
        this.login = this.config.guest_login;
        this.realm = this.config.guest_realm;
        return true;
    }

    var parts = req.headers["authorization"].split(/\s+/);
    var token = parts[1];

    if (parts[0] !== "OAuth" || !token) {
        this.logger.debug("Client has improper Authorization header", {
            header: req.headers["authorization"]
        });
        // Reject all invalid requests.
        return true;
    }

    this.token = token;
};

////////////////////////////////////////////////////////////////////////////////

exports.that = YtAuthentication;
