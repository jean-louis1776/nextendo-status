const http = require("http");
const https = require("https");

const CLIENT_ID = "nxc_dyNRS0L9SLND";
const TEN_MIN = 10 * 60 * 1000;

let cachedToken = "";
let tokenExp = 0;

function iamToken() {
  if (cachedToken && Date.now() < tokenExp) return Promise.resolve(cachedToken);
  return new Promise((resolve, reject) => {
    const req = http.request({
      host: "169.254.169.254",
      path: "/computeMetadata/v1/instance/service-accounts/default/token",
      headers: { "Metadata-Flavor": "Google" },
      timeout: 2000,
    }, (res) => {
      const chunks = [];
      res.on("data", (chunk) => chunks.push(chunk));
      res.on("end", () => {
        const text = Buffer.concat(chunks).toString("utf8");
        if (res.statusCode !== 200) {
          reject(new Error("token " + res.statusCode));
          return;
        }
        const data = JSON.parse(text);
        cachedToken = data.access_token;
        tokenExp = Date.now() + Math.max(0, (data.expires_in - 60) * 1000);
        resolve(cachedToken);
      });
    });
    req.on("error", reject);
    req.on("timeout", () => req.destroy(new Error("token timeout")));
    req.end();
  });
}

function s3(method, key, body) {
  const payload = Buffer.from(body || "");
  const path = "/" + [process.env.BUCKET, ...key.split("/")].map(encodeURIComponent).join("/");
  return iamToken().then((token) => new Promise((resolve, reject) => {
    const req = https.request({
      method,
      host: "storage.yandexcloud.net",
      path,
      headers: {
        Authorization: "Bearer " + token,
        "Content-Length": payload.length,
      },
    }, (res) => {
      const chunks = [];
      res.on("data", (chunk) => chunks.push(chunk));
      res.on("end", () => resolve({ status: res.statusCode, body: Buffer.concat(chunks).toString("utf8") }));
    });
    req.on("error", reject);
    req.end(payload);
  }));
}

async function putJson(key, value) {
  const res = await s3("PUT", key, JSON.stringify(value));
  if (res.status < 200 || res.status >= 300) {
    const code = (res.body.match(/<Code>([^<]+)<\/Code>/) || [])[1] || res.status;
    throw new Error("store " + code);
  }
}

async function getJson(key) {
  const res = await s3("GET", key, "");
  if (res.status === 404) return null;
  if (res.status < 200 || res.status >= 300) throw new Error("read " + res.status);
  const value = JSON.parse(res.body);
  if (!value || value.exp <= Date.now()) return null;
  return value;
}

function header(event, name) {
  const headers = event.headers || {};
  const found = Object.keys(headers).find((key) => key.toLowerCase() === name);
  return found ? headers[found] : "";
}

function reply(status, body, type, extra) {
  return {
    statusCode: status,
    isBase64Encoded: false,
    headers: Object.assign({ "Content-Type": type }, extra || {}),
    body: body || "",
  };
}

function page(status, body) {
  const html = `<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>NexStatus</title><body style="margin:0;background:#121214;color:#eee;font-family:sans-serif;display:flex;min-height:100vh;align-items:center;justify-content:center;text-align:center"><div style="padding:24px">${body}</div></body>`;
  return reply(status, html, "text/html; charset=utf-8");
}

function requestPath(event) {
  const raw = event.url || event.path || "/";
  if (raw.startsWith("http")) return new URL(raw).pathname || "/";
  return raw.split("?")[0] || "/";
}

function originOf(event) {
  const set = (process.env.PUBLIC_ORIGIN || "").replace(/\/$/, "");
  if (set) return set;
  return "https://" + (header(event, "x-forwarded-host") || header(event, "host"));
}

module.exports.handler = async (event) => {
  try {
    if (!process.env.BUCKET) return page(500, "<h1>NexStatus</h1><p>Storage is not configured.</p>");
    const path = requestPath(event);
    const query = event.queryStringParameters || {};
    const method = event.httpMethod || "GET";
    if (path === "/pair" && method === "POST") {
      let body = event.body;
      if (typeof body !== "object" || body === null) {
        let text = body || "";
        if (event.isBase64Encoded && text) text = Buffer.from(text, "base64").toString("utf8");
        try { body = JSON.parse(text || "{}"); } catch (e) { body = {}; }
      }
      const code = String(body.code || "").toUpperCase();
      const state = String(body.state || "");
      const challenge = String(body.challenge || "");
      if (!/^[A-Z2-9]{6}$/.test(code) || !/^[a-f0-9]{32}$/.test(state) || !/^[A-Za-z0-9_-]{43,128}$/.test(challenge)) {
        return reply(400, JSON.stringify({ error: "bad pair" }), "application/json");
      }
      await putJson("pair/" + code, { state, challenge, exp: Date.now() + TEN_MIN });
      return reply(200, JSON.stringify({ ok: true }), "application/json");
    }
    if (path === "/go") {
      const code = String(query.code || "").toUpperCase();
      const found = await getJson("pair/" + code);
      if (!found) return page(404, "<h1>NexStatus</h1><p>That code expired. Open Sign in on the console again.</p>");
      const next = new URL("https://nextendo.network/oauth/authorize");
      next.searchParams.set("client_id", CLIENT_ID);
      next.searchParams.set("redirect_uri", originOf(event) + "/callback");
      next.searchParams.set("response_type", "code");
      next.searchParams.set("scope", "identity friends");
      next.searchParams.set("code_challenge", found.challenge);
      next.searchParams.set("code_challenge_method", "S256");
      next.searchParams.set("state", found.state);
      return reply(302, "", "text/plain", { Location: next.toString() });
    }
    if (path === "/callback") {
      const code = String(query.code || "");
      const state = String(query.state || "");
      if (!/^[a-f0-9]{32}$/.test(state) || code.length < 8) return page(400, "<h1>NexStatus</h1><p>Sign-in was cancelled.</p>");
      await putJson("wait/" + state, { code, exp: Date.now() + TEN_MIN });
      return page(200, "<h1>NexStatus</h1><p>Signed in. You can close this page and return to the console.</p>");
    }
    if (path === "/poll") {
      const state = String(query.state || "");
      const found = await getJson("wait/" + state);
      if (!found) return reply(204, "", "text/plain");
      await s3("DELETE", "wait/" + state, "");
      return reply(200, JSON.stringify({ code: found.code }), "application/json");
    }
    const preset = String(query.c || "").toUpperCase().replace(/[^A-Z2-9]/g, "").slice(0, 6);
    return page(200, `<h1 style="font-weight:500">NexStatus</h1><p>Enter the code from the console.</p><form method=get action=/go><input name=code value="${preset}" maxlength=6 autocapitalize=characters autocomplete=off style="font-size:32px;letter-spacing:6px;text-align:center;width:7ch;padding:8px;border:0;border-radius:8px"><button style="font-size:20px;margin-left:8px;padding:10px 16px;border:0;border-radius:8px">Continue</button></form>`);
  } catch (error) {
    console.log(error);
    const detail = String(error.message || "failed").replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c])).slice(0, 180);
    return page(500, "<h1>NexStatus</h1><p>The sign-in site failed.</p><p>" + detail + "</p>");
  }
};
