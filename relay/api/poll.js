const { redis } = require("../lib/redis");

module.exports = async (req, res) => {
  const state = String(req.query.state || "");
  if (!/^[a-f0-9]{32}$/.test(state)) {
    res.status(400).json({ error: "bad state" });
    return;
  }
  try {
    const code = await redis(["GETDEL", "nx:" + state]);
    if (!code) {
      res.status(204).end();
      return;
    }
    res.status(200).json({ code });
  } catch (err) {
    res.status(500).json({ error: err.code === "NO_REDIS" ? "no redis" : "redis" });
  }
};
