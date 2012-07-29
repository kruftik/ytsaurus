exports.that = function YtLogRequest(logger) {
    "use strict";

    return function(req, rsp, next) {
        req._startTime = new Date();

        if (req._logging) {
            return next();
        } else {
            req._logging = true;
        }

        logger.info("Handling request", {
            request_id  : req.uuid,
            method      : req.method,
            url         : req.originalUrl,
            referrer    : req.headers["referer"] || req.headers["referrer"],
            remote_addr : req.socket && (req.socket.remoteAddress || (req.socket.socket && req.socket.socket.remoteAddress)),
            user_agent  : req.headers["user-agent"]
        });

        var end = rsp.end;
        rsp.end = function(chunk, encoding) {
            rsp.end = end;
            rsp.end(chunk, encoding);

            logger.info("Handled request", {
                request_id   : req.uuid,
                request_time : new Date() - req._startTime,
                status       : rsp.statusCode
            });
        };

        next();
    };
};
