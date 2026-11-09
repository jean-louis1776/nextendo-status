const { redis } = require("../lib/redis");

function readBody(req) {
  const body = req.body;
  if (body && typeof body === "object" && !Buffer.isBuffer(body)) return body;
  const text = Buffer.isBuffer(body) ? body.toString("utf8") : typeof body === "string" ? body : "";
  if (!text) return {};
  try { return JSON.parse(text); } catch (e) { return {}; }
}

module.exports = async (req, res) => {
  const q = req.query || {};
  const body = readBody(req);
  const code = String(q.code || body.code || "").toUpperCase();
  const state = String(q.state || body.state || "");
  const challenge = String(q.challenge || body.challenge || "");
  if (!/^[A-Z2-9]{6}$/.test(code) || !/^[a-f0-9]{32}$/.test(state) || !/^[A-Za-z0-9_-]{43,128}$/.test(challenge)) {
    res.status(400).json({ error: "bad pair" });
    return;
  }
  try {
    await redis(["SET", "pc:" + code, state + " " + challenge, "EX", "180"]);
    res.status(204).end();
  } catch (err) {
    res.status(500).json({ error: err.code === "NO_REDIS" ? "no redis" : "redis" });
  }
};
