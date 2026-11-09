const { redis } = require("../lib/redis");
const { authorizeUrl } = require("../lib/oauth");

function page(text) {
  return (
    "<!doctype html><meta charset=utf-8><title>NexStatus</title>" +
    "<body style=\"font-family:sans-serif;background:#111;color:#fff;text-align:center;padding:48px\">" +
    "<p>" + text + "</p><p><a href=\"/\" style=\"color:#7eb6ff\">Try another code</a></p></body>"
  );
}

module.exports = async (req, res) => {
  const code = String(req.query.code || "").toUpperCase().replace(/[^A-Z0-9]/g, "");
  if (!/^[A-Z2-9]{6}$/.test(code)) {
    res.status(400).send(page("Enter the 6-character code from the Switch."));
    return;
  }
  try {
    const packed = await redis(["GET", "pc:" + code]);
    const parts = String(packed || "").split(" ");
    if (parts.length !== 2) {
      res.status(404).send(page("That code is not active. Start sign-in on the Switch and try again."));
      return;
    }
    res.redirect(302, authorizeUrl(parts[0], parts[1]));
  } catch (err) {
    res.status(500).send(page("Could not look up the code."));
  }
};
