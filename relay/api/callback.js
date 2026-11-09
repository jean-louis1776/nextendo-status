const { redis } = require("../lib/redis");

function page(text) {
  return (
    "<!doctype html><meta charset=utf-8><title>NexStatus</title>" +
    "<body style=\"font-family:sans-serif;background:#111;color:#fff;text-align:center;padding:48px\">" +
    "<p>" + text + "</p></body>"
  );
}

module.exports = async (req, res) => {
  const state = String(req.query.state || "");
  const code = String(req.query.code || "");
  const error = String(req.query.error || "");
  res.setHeader("Content-Type", "text/html; charset=utf-8");
  if (error || !/^[a-f0-9]{32}$/.test(state) || !/^[A-Za-z0-9._~-]{8,512}$/.test(code)) {
    res.status(400).send(page("Sign-in was not completed."));
    return;
  }
  try {
    await redis(["SET", "nx:" + state, code, "EX", "180"]);
  } catch (err) {
    res.status(500).send(page(err.code === "NO_REDIS" ? "Relay storage is not configured." : "Could not save the sign-in code."));
    return;
  }
  res.status(200).send(page("Done. You can close this page and return to the Switch."));
};
