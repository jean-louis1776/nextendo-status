const { authorizeUrl } = require("../lib/oauth");

module.exports = (req, res) => {
  const state = String(req.query.state || "");
  const challenge = String(req.query.challenge || "");
  if (!/^[a-f0-9]{32}$/.test(state) || !/^[A-Za-z0-9_-]{43,128}$/.test(challenge)) {
    res.status(400).send("Bad sign-in link.");
    return;
  }
  res.redirect(302, authorizeUrl(state, challenge));
};
