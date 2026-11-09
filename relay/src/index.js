const CLIENT_ID = "nxc_rVab8rabbZ7j";

function page(text, extra) {
  return (
    "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">" +
    "<title>NexStatus</title>" +
    "<body style=\"font-family:sans-serif;background:#111;color:#fff;text-align:center;padding:48px\">" +
    "<p>" + text + "</p>" + (extra || "") + "</body>"
  );
}

function html(text, status, extra) {
  return new Response(page(text, extra), { status, headers: { "content-type": "text/html; charset=utf-8" } });
}

async function redis(env, command) {
  if (!env.UPSTASH_REDIS_REST_URL || !env.UPSTASH_REDIS_REST_TOKEN) {
    const err = new Error("missing redis");
    err.code = "NO_REDIS";
    throw err;
  }
  const res = await fetch(env.UPSTASH_REDIS_REST_URL, {
    method: "POST",
    headers: {
      Authorization: "Bearer " + env.UPSTASH_REDIS_REST_TOKEN,
      "Content-Type": "application/json",
    },
    body: JSON.stringify(command),
  });
  const data = await res.json();
  if (data.error) throw new Error(data.error);
  return data.result;
}

function authorizeUrl(origin, state, challenge) {
  const url = new URL("https://nextendo.network/oauth/authorize");
  url.searchParams.set("client_id", CLIENT_ID);
  url.searchParams.set("redirect_uri", origin + "/api/callback");
  url.searchParams.set("response_type", "code");
  url.searchParams.set("scope", "identity friends");
  url.searchParams.set("code_challenge", challenge);
  url.searchParams.set("code_challenge_method", "S256");
  url.searchParams.set("state", state);
  return url.toString();
}

async function pair(request, env, url) {
  const code = (url.searchParams.get("code") || "").toUpperCase();
  const state = url.searchParams.get("state") || "";
  const challenge = url.searchParams.get("challenge") || "";
  if (!/^[A-Z2-9]{6}$/.test(code) || !/^[a-f0-9]{32}$/.test(state) || !/^[A-Za-z0-9_-]{43,128}$/.test(challenge)) {
    return Response.json({ error: "bad pair" }, { status: 400 });
  }
  try {
    await redis(env, ["SET", "pc:" + code, state + " " + challenge, "EX", "180"]);
    return new Response(null, { status: 204 });
  } catch (err) {
    return Response.json({ error: err.code === "NO_REDIS" ? "no redis" : "redis" }, { status: 500 });
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const origin = url.origin;
    const path = url.pathname;

    if (path === "/" || path === "") {
      return html(
        "Enter the code from the Switch.",
        200,
        "<form action=\"/api/use\" method=\"get\">" +
          "<input name=\"code\" autofocus maxlength=\"6\" autocapitalize=\"characters\" autocomplete=\"off\" " +
          "style=\"font-size:32px;letter-spacing:6px;text-transform:uppercase;text-align:center;width:8ch\">" +
          "<button style=\"font-size:20px;margin-left:8px\">Continue</button></form>"
      );
    }

    if (path === "/api/go") {
      const state = url.searchParams.get("state") || "";
      const challenge = url.searchParams.get("challenge") || "";
      if (!/^[a-f0-9]{32}$/.test(state) || !/^[A-Za-z0-9_-]{43,128}$/.test(challenge)) {
        return new Response("Bad sign-in link.", { status: 400 });
      }
      return Response.redirect(authorizeUrl(origin, state, challenge), 302);
    }

    if (path === "/api/callback") {
      const state = url.searchParams.get("state") || "";
      const code = url.searchParams.get("code") || "";
      const error = url.searchParams.get("error") || "";
      if (error || !/^[a-f0-9]{32}$/.test(state) || !/^[A-Za-z0-9._~-]{8,512}$/.test(code)) {
        return html("Sign-in was not completed.", 400);
      }
      try {
        await redis(env, ["SET", "nx:" + state, code, "EX", "180"]);
      } catch (err) {
        return html(err.code === "NO_REDIS" ? "Relay storage is not configured." : "Could not save the sign-in code.", 500);
      }
      return html("Done. You can close this page and return to the Switch.", 200);
    }

    if (path === "/api/poll") {
      const state = url.searchParams.get("state") || "";
      if (!/^[a-f0-9]{32}$/.test(state)) return Response.json({ error: "bad state" }, { status: 400 });
      try {
        const code = await redis(env, ["GETDEL", "nx:" + state]);
        if (!code) return new Response(null, { status: 204 });
        return Response.json({ code });
      } catch (err) {
        return Response.json({ error: err.code === "NO_REDIS" ? "no redis" : "redis" }, { status: 500 });
      }
    }

    if (path === "/api/pair") return pair(request, env, url);

    if (path === "/api/use") {
      const code = (url.searchParams.get("code") || "").toUpperCase().replace(/[^A-Z0-9]/g, "");
      const again = "<p><a href=\"/\" style=\"color:#7eb6ff\">Try another code</a></p>";
      if (!/^[A-Z2-9]{6}$/.test(code)) return html("Enter the 6-character code from the Switch." + again, 400);
      try {
        const packed = await redis(env, ["GET", "pc:" + code]);
        const parts = String(packed || "").split(" ");
        if (parts.length !== 2) return html("That code is not active. Start sign-in on the Switch and try again." + again, 404);
        return Response.redirect(authorizeUrl(origin, parts[0], parts[1]), 302);
      } catch (err) {
        return html("Could not look up the code." + again, 500);
      }
    }

    return new Response("Not found.", { status: 404 });
  },
};
