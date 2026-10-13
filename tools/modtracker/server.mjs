// Chatterino9 mod tracker: counts the moderation actions of one Twitch
// channel around the clock and serves them to the Mod activity window.
//
// - Signs in once with Twitch's device sign-in (read-only moderator scopes,
//   no password, no client secret) and renews its own login after that.
// - Listens to channel.moderate over EventSub WebSocket, so it only makes
//   outgoing connections.
// - Writes the same files the app keeps in Misc/ModActivity/<channel>/:
//   actions-YYYY-MM.tsv (msecs, kind, moderator, target, seconds) and
//   streams.tsv (id, start, lastSeen).
// - Counts how often each emote is used in the channel's chat for the Emote
//   stats window. It reads chat as an anonymous viewer (no login at all) and
//   writes emotes/YYYY-MM-DD.tsv in the same format as the app.
// - Serves them read-only over HTTP to requests carrying the API key. The
//   Twitch token is never sent anywhere except to Twitch.
//
// No dependencies besides Node itself.

import { createHash, timingSafeEqual } from "node:crypto";
import {
  appendFileSync,
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  renameSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { createServer } from "node:http";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const CLIENT_ID = requireEnv("TWITCH_CLIENT_ID");
const API_KEY = requireEnv("API_KEY");
const CHANNEL = (process.env.CHANNEL || "papaplatte").toLowerCase();
const DATA_DIR = process.env.DATA_DIR || "/data";
const PORT = Number(process.env.PORT || 7480);

// Read-only scopes channel.moderate v2 needs. None of them can ban, time out,
// chat or change anything.
const SCOPES = [
  "moderator:read:blocked_terms",
  "moderator:read:chat_settings",
  "moderator:read:unban_requests",
  "moderator:read:banned_users",
  "moderator:read:chat_messages",
  "moderator:read:warnings",
  "moderator:read:moderators",
  "moderator:read:vips",
];

const TOKEN_FILE = join(DATA_DIR, "token.json");
const CHANNEL_DIR = join(DATA_DIR, CHANNEL);
// Overridable only for testing against a fake Twitch
const ID_URL = process.env.TWITCH_ID_URL || "https://id.twitch.tv";
const API_URL = process.env.TWITCH_API_URL || "https://api.twitch.tv";
const EVENTSUB_URL =
  process.env.TWITCH_EVENTSUB_URL ||
  "wss://eventsub.wss.twitch.tv/ws?keepalive_timeout_seconds=30";
const EMOTE_DIR = join(CHANNEL_DIR, "emotes");
// Overridable only for testing against fake services
const IRC_URL = process.env.TWITCH_IRC_URL || "wss://irc-ws.chat.twitch.tv:443";
const SEVENTV_URL = process.env.SEVENTV_URL || "https://7tv.io";
const BTTV_URL = process.env.BTTV_URL || "https://api.betterttv.net";
const FFZ_URL = process.env.FFZ_URL || "https://api.frankerfacez.com";
// Emote lists are loaded again this often, counts written this often
const EMOTE_LIST_MSECS = 10 * 60 * 1000;
const EMOTE_FLUSH_MSECS = 30 * 1000;
// A live stream gets its "last seen" time written at most this often
const STREAM_WRITE_MSECS = 5 * 60 * 1000;
const STREAM_POLL_MSECS = 60 * 1000;

const state = {
  token: null,
  signInCode: null,
  broadcasterId: null,
  connected: false,
  lastEventAt: 0,
  startedAt: Date.now(),
};

function requireEnv(name) {
  const value = (process.env[name] || "").trim();
  if (!value) {
    console.error(`Missing ${name}`);
    process.exit(1);
  }
  return value;
}

function log(...args) {
  console.log(new Date().toISOString(), ...args);
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// ---------------------------------------------------------------- token ---

function loadToken() {
  try {
    return JSON.parse(readFileSync(TOKEN_FILE, "utf8"));
  } catch {
    return null;
  }
}

function saveToken(token) {
  const tmp = `${TOKEN_FILE}.tmp`;
  writeFileSync(tmp, JSON.stringify(token), { mode: 0o600 });
  renameSync(tmp, TOKEN_FILE);
}

async function postForm(url, fields) {
  const res = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams(fields),
  });
  let body = {};
  try {
    body = await res.json();
  } catch {
    // keep the empty body
  }
  return { ok: res.ok, status: res.status, body };
}

async function storeGrant(body) {
  const validate = await fetch(`${ID_URL}/oauth2/validate`, {
    headers: { Authorization: `OAuth ${body.access_token}` },
  });
  if (!validate.ok) {
    throw new Error(`token validation failed (${validate.status})`);
  }
  const info = await validate.json();
  const token = {
    accessToken: body.access_token,
    refreshToken: body.refresh_token,
    expiresAt: Date.now() + info.expires_in * 1000,
    login: info.login,
    userId: info.user_id,
  };
  saveToken(token);
  state.token = token;
  return token;
}

/// Twitch device sign-in: prints a code to approve at twitch.tv/activate
async function deviceSignIn() {
  for (;;) {
    const start = await postForm(`${ID_URL}/oauth2/device`, {
      client_id: CLIENT_ID,
      scopes: SCOPES.join(" "),
    });
    if (!start.ok) {
      log(
        `Twitch refused the sign-in request (${start.status}): ${start.body.message ?? ""}`
      );
      log(
        "Check TWITCH_CLIENT_ID and that the Twitch app is a Public client. Retrying in 5 minutes."
      );
      await sleep(5 * 60 * 1000);
      continue;
    }
    const { device_code, user_code, verification_uri, expires_in } = start.body;
    let interval = Math.max(5, start.body.interval ?? 5);
    state.signInCode = user_code;
    log("==========================================================");
    log(`SIGN IN: open ${verification_uri}`);
    log(`and approve code ${user_code} with your mod account.`);
    log(`The code is valid for ${Math.round(expires_in / 60)} minutes.`);
    log("==========================================================");

    const deadline = Date.now() + expires_in * 1000;
    while (Date.now() < deadline) {
      await sleep(interval * 1000);
      const poll = await postForm(`${ID_URL}/oauth2/token`, {
        client_id: CLIENT_ID,
        scopes: SCOPES.join(" "),
        device_code,
        grant_type: "urn:ietf:params:oauth:grant-type:device_code",
      });
      if (poll.ok) {
        const token = await storeGrant(poll.body);
        state.signInCode = null;
        log(`Signed in as ${token.login}.`);
        return token;
      }
      const message = String(poll.body.message ?? "");
      if (message === "authorization_pending") {
        continue;
      }
      if (message === "slow_down") {
        interval += 5;
        continue;
      }
      log(`Sign-in stopped (${message || poll.status}). Getting a new code.`);
      break;
    }
  }
}

// Twitch refresh tokens of a Public app work only once: two renewals racing
// with the same one make the second fail and throw the login away. So every
// caller shares the renewal that is already running.
let renewal = null;

function refreshToken() {
  renewal ??= renewToken().finally(() => {
    renewal = null;
  });
  return renewal;
}

async function renewToken() {
  const used = state.token.refreshToken;
  const refresh = await postForm(`${ID_URL}/oauth2/token`, {
    client_id: CLIENT_ID,
    grant_type: "refresh_token",
    refresh_token: used,
  });
  if (refresh.ok) {
    await storeGrant(refresh.body);
    log("Twitch login renewed.");
    return;
  }
  if (refresh.status === 400 || refresh.status === 401) {
    if (state.token?.refreshToken !== used) {
      // renewed elsewhere in the meantime
      return;
    }
    // Revoked or expired (more than 30 days unused): start over
    log("Twitch login is no longer valid. A new sign-in is needed.");
    try {
      renameSync(TOKEN_FILE, `${TOKEN_FILE}.revoked`);
    } catch {
      // nothing to move
    }
    state.token = null;
    await signInOnce();
    return;
  }
  throw new Error(`token refresh failed (${refresh.status})`);
}

let signIn = null;

/// Only one device sign-in at a time, however many callers need it
function signInOnce() {
  signIn ??= deviceSignIn().finally(() => {
    signIn = null;
  });
  return signIn;
}

async function ensureFreshToken() {
  if (renewal) {
    await renewal;
  }
  if (!state.token) {
    state.token = loadToken();
  }
  if (!state.token) {
    await signInOnce();
  }
  if (state.token.expiresAt - Date.now() < 10 * 60 * 1000) {
    await refreshToken();
  }
}

async function helix(path, options = {}, retried = false) {
  await ensureFreshToken();
  const res = await fetch(`${API_URL}/helix${path}`, {
    ...options,
    headers: {
      "Client-Id": CLIENT_ID,
      Authorization: `Bearer ${state.token.accessToken}`,
      ...(options.body ? { "Content-Type": "application/json" } : {}),
    },
  });
  if (res.status === 401 && !retried) {
    await refreshToken();
    return helix(path, options, true);
  }
  return res;
}

// ---------------------------------------------------------------- files ---

function monthOf(msecs) {
  return new Date(msecs).toISOString().slice(0, 7);
}

function recordAction(msecs, kind, moderator, target, seconds = 0) {
  mkdirSync(CHANNEL_DIR, { recursive: true });
  const line = [
    msecs,
    kind,
    moderator.toLowerCase(),
    target.toLowerCase(),
    Math.max(0, seconds),
  ].join("\t");
  appendFileSync(
    join(CHANNEL_DIR, `actions-${monthOf(msecs)}.tsv`),
    `${line}\n`
  );
  state.lastEventAt = Date.now();
}

function writeStreamRow(id, start, lastSeen) {
  mkdirSync(CHANNEL_DIR, { recursive: true });
  appendFileSync(
    join(CHANNEL_DIR, "streams.tsv"),
    `${id}\t${start}\t${lastSeen}\n`
  );
}

/// Action lines newer than `since`, oldest month first
function actionsSince(since) {
  if (!existsSync(CHANNEL_DIR)) {
    return "";
  }
  // a month before `since`, in case events arrived slightly out of order
  const firstMonth = monthOf(Math.max(0, since - 24 * 3600 * 1000));
  const files = readdirSync(CHANNEL_DIR)
    .filter((name) => /^actions-\d{4}-\d{2}\.tsv$/.test(name))
    .filter((name) => name.slice(8, 15) >= firstMonth)
    .sort();
  let out = "";
  for (const name of files) {
    for (const line of readFileSync(join(CHANNEL_DIR, name), "utf8").split(
      "\n"
    )) {
      const msecs = Number(line.slice(0, line.indexOf("\t")));
      if (line && msecs > since) {
        out += `${line}\n`;
      }
    }
  }
  return out;
}

function readStreams() {
  try {
    return readFileSync(join(CHANNEL_DIR, "streams.tsv"), "utf8");
  } catch {
    return "";
  }
}

// ------------------------------------------------------------- eventsub ---

/// Twitch sends nanosecond timestamps; Date.parse wants at most milliseconds
function parseTime(text) {
  return Date.parse(String(text).replace(/(\.\d{3})\d+/, "$1"));
}

const KINDS = {
  timeout: "timeout",
  ban: "ban",
  unban: "unban",
  untimeout: "untimeout",
  delete: "delete",
  warn: "warn",
};

function onModerate(event, msecs) {
  // Shared chat actions come from other channels' mods (same as the app)
  if (event.source_broadcaster_user_id) {
    return;
  }
  const kind = KINDS[event.action];
  const detail = event[event.action];
  const moderator = event.moderator_user_login;
  if (!kind || !detail || !moderator || !detail.user_login) {
    return;
  }
  let seconds = 0;
  if (kind === "timeout" && detail.expires_at) {
    seconds = Math.round((parseTime(detail.expires_at) - msecs) / 1000);
  }
  recordAction(msecs, kind, moderator, detail.user_login, seconds);
}

const seenMessages = new Set();

function isDuplicate(id) {
  if (!id) {
    return false;
  }
  if (seenMessages.has(id)) {
    return true;
  }
  seenMessages.add(id);
  if (seenMessages.size > 2000) {
    seenMessages.delete(seenMessages.values().next().value);
  }
  return false;
}

let socket = null;
let watchdog = null;
let backoff = 1000;

async function subscribe(sessionId) {
  const res = await helix("/eventsub/subscriptions", {
    method: "POST",
    body: JSON.stringify({
      type: "channel.moderate",
      version: "2",
      condition: {
        broadcaster_user_id: state.broadcasterId,
        moderator_user_id: state.token.userId,
      },
      transport: { method: "websocket", session_id: sessionId },
    }),
  });
  if (!res.ok) {
    const text = await res.text();
    throw new Error(
      `subscribing to ${CHANNEL} failed (${res.status}): ${text.slice(0, 300)}`
    );
  }
  state.connected = true;
  backoff = 1000;
  log(`Counting mod actions in ${CHANNEL}.`);
}

function armWatchdog(seconds) {
  clearTimeout(watchdog);
  watchdog = setTimeout(
    () => {
      log("Twitch went quiet, reconnecting.");
      socket?.close();
    },
    (seconds + 10) * 1000
  );
}

function connect(url = EVENTSUB_URL, isReconnect = false) {
  const ws = new WebSocket(url);
  let keepalive = 30;

  ws.onmessage = async (msg) => {
    let data;
    try {
      data = JSON.parse(msg.data);
    } catch {
      return;
    }
    const type = data.metadata?.message_type;
    armWatchdog(keepalive);

    if (type === "session_welcome") {
      const old = socket;
      socket = ws;
      keepalive = data.payload.session.keepalive_timeout_seconds ?? 30;
      armWatchdog(keepalive);
      if (isReconnect) {
        // subscriptions move over to the new connection by themselves
        old?.close();
        state.connected = true;
        return;
      }
      try {
        await subscribe(data.payload.session.id);
      } catch (err) {
        log(String(err.message ?? err));
        ws.close();
      }
    } else if (type === "notification") {
      if (isDuplicate(data.metadata.message_id)) {
        return;
      }
      if (data.metadata.subscription_type === "channel.moderate") {
        onModerate(
          data.payload.event,
          parseTime(data.metadata.message_timestamp)
        );
      }
    } else if (type === "session_reconnect") {
      connect(data.payload.session.reconnect_url, true);
    } else if (type === "revocation") {
      log(
        `Twitch ended the subscription (${data.payload.subscription?.status}).`
      );
      state.connected = false;
      ws.close();
    }
  };

  ws.onclose = () => {
    if (socket !== ws && socket !== null) {
      // replaced by a reconnect
      return;
    }
    socket = null;
    state.connected = false;
    clearTimeout(watchdog);
    const wait = backoff;
    backoff = Math.min(backoff * 2, 60 * 1000);
    log(`Disconnected from Twitch, reconnecting in ${wait / 1000}s.`);
    setTimeout(() => connect(), wait);
  };

  ws.onerror = () => {
    // onclose follows and handles the reconnect
  };

  if (!isReconnect) {
    socket = ws;
  }
}

// -------------------------------------------------------------- streams ---

let live = null;

async function pollStream() {
  try {
    const res = await helix(`/streams?user_id=${state.broadcasterId}`);
    if (!res.ok) {
      return;
    }
    const stream = (await res.json()).data?.[0];
    const now = Date.now();
    if (stream) {
      if (!live || live.id !== stream.id) {
        live = {
          id: stream.id,
          start: Date.parse(stream.started_at),
          written: 0,
        };
      }
      if (now - live.written >= STREAM_WRITE_MSECS) {
        live.written = now;
        writeStreamRow(live.id, live.start, now);
      }
    } else if (live) {
      writeStreamRow(live.id, live.start, now);
      live = null;
    }
  } catch (err) {
    log(`Stream check failed: ${err.message ?? err}`);
  }
}

// --------------------------------------------------------------- emotes ---
//
// Every chat message counts once per emote in it, with who sent it, in one
// bucket per UTC hour. Files: emotes/YYYY-MM-DD.tsv with lines
// "HH, source, name, messages, chatters" (tab separated, chatters comma
// separated); source "*" with name "*" counts every chat message. Same
// format and same emote lookup as the app (src/util/EmoteStats.cpp).

const chat = {
  connected: false,
  lastMessageAt: 0,
  roomId: null,
  listsLoadedAt: 0,
};

/// Emote names by set, looked up in this order like chat does
let emoteLists = {
  ffzChannel: new Set(),
  bttvChannel: new Set(),
  seventvChannel: new Set(),
  ffzGlobal: new Set(),
  bttvGlobal: new Set(),
  seventvGlobal: new Set(),
};

const LIST_SOURCES = [
  ["ffzChannel", "ffz"],
  ["bttvChannel", "bttv"],
  ["seventvChannel", "7tv"],
  ["ffzGlobal", "ffz"],
  ["bttvGlobal", "bttv"],
  ["seventvGlobal", "7tv"],
];

export function classifyWord(word, lists = emoteLists) {
  for (const [list, source] of LIST_SOURCES) {
    if (lists[list].has(word)) {
      return source;
    }
  }
  return null;
}

/// null when the service has nothing for this channel (404)
async function getJson(url) {
  const res = await fetch(url, { signal: AbortSignal.timeout(20000) });
  if (res.status === 404) {
    return null;
  }
  if (!res.ok) {
    throw new Error(`${url} answered ${res.status}`);
  }
  return res.json();
}

function ffzNames(body, setIds = null) {
  const names = new Set();
  for (const [id, set] of Object.entries(body?.sets ?? {})) {
    if (setIds && !setIds.map(String).includes(String(id))) {
      continue;
    }
    for (const emote of set.emoticons ?? []) {
      names.add(emote.name);
    }
  }
  return names;
}

const LOADERS = {
  seventvChannel: async (id) =>
    new Set(
      (
        (await getJson(`${SEVENTV_URL}/v3/users/twitch/${id}`))?.emote_set
          ?.emotes ?? []
      ).map((e) => e.name)
    ),
  seventvGlobal: async () =>
    new Set(
      (
        (await getJson(`${SEVENTV_URL}/v3/emote-sets/global`))?.emotes ?? []
      ).map((e) => e.name)
    ),
  bttvChannel: async (id) => {
    const body = await getJson(`${BTTV_URL}/3/cached/users/twitch/${id}`);
    return new Set(
      [...(body?.channelEmotes ?? []), ...(body?.sharedEmotes ?? [])].map(
        (e) => e.code
      )
    );
  },
  bttvGlobal: async () =>
    new Set(
      ((await getJson(`${BTTV_URL}/3/cached/emotes/global`)) ?? []).map(
        (e) => e.code
      )
    ),
  ffzChannel: async (id) =>
    ffzNames(await getJson(`${FFZ_URL}/v1/room/id/${id}`)),
  ffzGlobal: async () => {
    const body = await getJson(`${FFZ_URL}/v1/set/global`);
    return ffzNames(body, body?.default_sets ?? []);
  },
};

/// Loads every emote list; a list that fails keeps its last version
async function loadEmoteLists(roomId) {
  const next = { ...emoteLists };
  const failed = [];
  await Promise.all(
    Object.entries(LOADERS).map(async ([name, load]) => {
      try {
        next[name] = await load(roomId);
      } catch (err) {
        failed.push(`${name} (${err.message ?? err})`);
      }
    })
  );
  emoteLists = next;
  chat.listsLoadedAt = Date.now();
  if (failed.length) {
    log(`Some emote lists did not load: ${failed.join(", ")}`);
  }
}

/// Parses one IRC line into tags, prefix, command, params and trailing text
export function parseIrc(line) {
  let rest = line;
  const tags = {};
  if (rest.startsWith("@")) {
    const space = rest.indexOf(" ");
    for (const pair of rest.slice(1, space).split(";")) {
      const eq = pair.indexOf("=");
      tags[eq < 0 ? pair : pair.slice(0, eq)] =
        eq < 0 ? "" : pair.slice(eq + 1);
    }
    rest = rest.slice(space + 1);
  }
  let prefix = "";
  if (rest.startsWith(":")) {
    const space = rest.indexOf(" ");
    prefix = rest.slice(1, space);
    rest = rest.slice(space + 1);
  }
  let trailing = null;
  const colon = rest.indexOf(" :");
  if (colon >= 0) {
    trailing = rest.slice(colon + 2);
    rest = rest.slice(0, colon);
  }
  const [command, ...params] = rest.split(" ");
  return { tags, prefix, command, params, trailing };
}

/// [source, name] of every emote in `text`, Twitch emotes from the emotes
/// tag ("id:start-end,start-end/id:..." in code points), the others by name
export function emoteUses(text, emotesTag, lists = emoteLists) {
  const uses = [];
  const twitchNames = new Set();
  if (emotesTag) {
    const points = Array.from(text);
    for (const part of emotesTag.split("/")) {
      const ranges = part.split(":")[1];
      if (!ranges) {
        continue;
      }
      for (const range of ranges.split(",")) {
        const [start, end] = range.split("-").map(Number);
        const name = points.slice(start, end + 1).join("");
        if (name && !twitchNames.has(name)) {
          twitchNames.add(name);
          uses.push(["twitch", name]);
        }
      }
    }
  }
  for (const word of text.split(" ")) {
    if (!word || twitchNames.has(word)) {
      continue;
    }
    const source = classifyWord(word, lists);
    if (source) {
      uses.push([source, word]);
    }
  }
  return uses;
}

/// day name -> { buckets: Map(key -> {count, users}), dirty }
const emoteDays = new Map();
const seenChatIds = new Set();

function dayName(msecs) {
  return new Date(msecs).toISOString().slice(0, 10);
}

function readEmoteDay(day) {
  const buckets = new Map();
  let text = "";
  try {
    text = readFileSync(join(EMOTE_DIR, `${day}.tsv`), "utf8");
  } catch {
    return buckets;
  }
  for (const line of text.split("\n")) {
    const parts = line.split("\t");
    if (parts.length !== 5) {
      continue;
    }
    const count = Number(parts[3]);
    if (!Number.isFinite(count)) {
      continue;
    }
    buckets.set(parts.slice(0, 3).join("\t"), {
      count,
      users: new Set(parts[4] ? parts[4].split(",") : []),
    });
  }
  return buckets;
}

export function countMessage(msecs, login, uses) {
  const day = dayName(msecs);
  let data = emoteDays.get(day);
  if (!data) {
    // continue the day saved before a restart
    data = { buckets: readEmoteDay(day), dirty: false };
    emoteDays.set(day, data);
  }
  const hour = String(new Date(msecs).getUTCHours()).padStart(2, "0");
  const add = (key, user) => {
    let bucket = data.buckets.get(key);
    if (!bucket) {
      bucket = { count: 0, users: new Set() };
      data.buckets.set(key, bucket);
    }
    bucket.count++;
    if (user) {
      bucket.users.add(user);
    }
  };
  add(`${hour}\t*\t*`, null);
  const seen = new Set();
  for (const [source, name] of uses) {
    const key = `${hour}\t${source}\t${name}`;
    if (!seen.has(key)) {
      seen.add(key);
      add(key, login);
    }
  }
  data.dirty = true;
}

export function flushEmotes() {
  mkdirSync(EMOTE_DIR, { recursive: true });
  for (const [day, data] of emoteDays) {
    if (!data.dirty) {
      continue;
    }
    let out = "";
    for (const [key, bucket] of data.buckets) {
      out += `${key}\t${bucket.count}\t${[...bucket.users].join(",")}\n`;
    }
    const file = join(EMOTE_DIR, `${day}.tsv`);
    writeFileSync(`${file}.tmp`, out);
    renameSync(`${file}.tmp`, file);
    data.dirty = false;
  }
  // keep today and yesterday in memory
  const days = [...emoteDays.keys()].sort();
  for (const day of days.slice(0, -2)) {
    if (!emoteDays.get(day).dirty) {
      emoteDays.delete(day);
    }
  }
}

function onChatMessage(message) {
  const { tags, prefix, command } = message;
  let text = message.trailing;
  if (text === null) {
    return;
  }
  const id = tags.id;
  if (id) {
    if (seenChatIds.has(id)) {
      return;
    }
    seenChatIds.add(id);
    if (seenChatIds.size > 5000) {
      seenChatIds.delete(seenChatIds.values().next().value);
    }
  }
  // shared chat: messages from the other channels
  if (tags["source-room-id"] && tags["source-room-id"] !== tags["room-id"]) {
    return;
  }
  if (text.startsWith("\u0001ACTION ") && text.endsWith("\u0001")) {
    text = text.slice(8, -1);
  }
  const login = (
    command === "PRIVMSG" ? prefix.split("!")[0] : tags.login || ""
  ).toLowerCase();
  const sent = Number(tags["tmi-sent-ts"]);
  const msecs = Number.isFinite(sent) && sent > 0 ? sent : Date.now();
  countMessage(msecs, login, emoteUses(text, tags.emotes));
  chat.lastMessageAt = Date.now();
}

let chatSocket = null;
let chatBackoff = 1000;
let chatWatchdog = null;

function armChatWatchdog(ws) {
  clearTimeout(chatWatchdog);
  // Twitch pings about every 5 minutes
  chatWatchdog = setTimeout(
    () => {
      log("Chat went quiet, reconnecting.");
      ws.close();
    },
    7 * 60 * 1000
  );
}

function connectChat() {
  const ws = new WebSocket(IRC_URL);
  chatSocket = ws;

  ws.onopen = () => {
    ws.send("CAP REQ :twitch.tv/tags twitch.tv/commands");
    // anonymous: read-only, no account
    ws.send("PASS SCHMOOPIIE");
    ws.send(`NICK justinfan${10000 + Math.floor(Math.random() * 80000)}`);
    ws.send(`JOIN #${CHANNEL}`);
    armChatWatchdog(ws);
  };

  ws.onmessage = (msg) => {
    armChatWatchdog(ws);
    for (const line of String(msg.data).split("\r\n")) {
      if (!line) {
        continue;
      }
      const message = parseIrc(line);
      switch (message.command) {
        case "PING":
          ws.send(`PONG :${message.trailing ?? "tmi.twitch.tv"}`);
          break;
        case "RECONNECT":
          ws.close();
          break;
        case "ROOMSTATE": {
          const roomId = message.tags["room-id"];
          if (!chat.connected) {
            log(`Counting emotes in ${CHANNEL}.`);
          }
          chat.connected = true;
          chatBackoff = 1000;
          if (roomId && roomId !== chat.roomId) {
            chat.roomId = roomId;
            loadEmoteLists(roomId).catch((err) =>
              log(`Emote lists failed: ${err.message ?? err}`)
            );
          }
          break;
        }
        case "PRIVMSG":
        case "USERNOTICE":
          if (message.params[0] === `#${CHANNEL}`) {
            onChatMessage(message);
          }
          break;
        default:
          break;
      }
    }
  };

  ws.onclose = () => {
    if (chatSocket !== ws) {
      return;
    }
    chatSocket = null;
    chat.connected = false;
    clearTimeout(chatWatchdog);
    const wait = chatBackoff;
    chatBackoff = Math.min(chatBackoff * 2, 60 * 1000);
    log(`Disconnected from chat, reconnecting in ${wait / 1000}s.`);
    setTimeout(connectChat, wait);
  };

  ws.onerror = () => {
    // onclose follows and handles the reconnect
  };
}

function emoteDayList() {
  flushEmotes();
  let out = "";
  for (const name of readdirSync(EMOTE_DIR).sort()) {
    if (/^\d{4}-\d{2}-\d{2}\.tsv$/.test(name)) {
      const mtime = Math.floor(statSync(join(EMOTE_DIR, name)).mtimeMs);
      out += `${name.slice(0, 10)}\t${mtime}\n`;
    }
  }
  return out;
}

// ----------------------------------------------------------------- http ---

const KEY_HASH = createHash("sha256").update(API_KEY).digest();

function authorized(req) {
  const given = String(req.headers["x-modtracker-key"] ?? "");
  return timingSafeEqual(createHash("sha256").update(given).digest(), KEY_HASH);
}

function send(res, status, body, type = "text/plain; charset=utf-8") {
  res.writeHead(status, { "Content-Type": type, "Cache-Control": "no-store" });
  res.end(body);
}

const server = createServer((req, res) => {
  if (req.method !== "GET" || !authorized(req)) {
    return send(res, 404, "Not found\n");
  }
  const url = new URL(req.url, "http://localhost");
  if (url.pathname === "/v1/status") {
    return send(
      res,
      200,
      JSON.stringify({
        channel: CHANNEL,
        signedIn: Boolean(state.token),
        needsSignIn: Boolean(state.signInCode),
        connected: state.connected,
        lastEventAt: state.lastEventAt,
        serverTime: Date.now(),
        emotes: {
          connected: chat.connected,
          lastMessageAt: chat.lastMessageAt,
          listsLoadedAt: chat.listsLoadedAt,
        },
      }),
      "application/json"
    );
  }
  if (url.pathname === `/v1/${CHANNEL}/actions`) {
    const since = Number(url.searchParams.get("since") ?? 0);
    return send(res, 200, actionsSince(Number.isFinite(since) ? since : 0));
  }
  if (url.pathname === `/v1/${CHANNEL}/streams`) {
    return send(res, 200, readStreams());
  }
  if (url.pathname === `/v1/${CHANNEL}/emotes/days`) {
    return send(res, 200, emoteDayList());
  }
  if (url.pathname === `/v1/${CHANNEL}/emotes/day`) {
    const day = url.searchParams.get("d") ?? "";
    if (!/^\d{4}-\d{2}-\d{2}$/.test(day)) {
      return send(res, 404, "Not found\n");
    }
    flushEmotes();
    try {
      return send(res, 200, readFileSync(join(EMOTE_DIR, `${day}.tsv`)));
    } catch {
      return send(res, 404, "Not found\n");
    }
  }
  return send(res, 404, "Not found\n");
});

// ----------------------------------------------------------------- main ---

async function main() {
  mkdirSync(CHANNEL_DIR, { recursive: true });
  mkdirSync(EMOTE_DIR, { recursive: true });
  server.listen(PORT, () => log(`Serving on port ${PORT}.`));

  // emote counting needs no Twitch login, so it runs even while the mod
  // actions wait for a sign-in
  connectChat();
  setInterval(() => {
    try {
      flushEmotes();
    } catch (err) {
      log(`Saving emote counts failed: ${err.message ?? err}`);
    }
  }, EMOTE_FLUSH_MSECS);
  setInterval(() => {
    if (chat.roomId) {
      loadEmoteLists(chat.roomId).catch((err) =>
        log(`Emote lists failed: ${err.message ?? err}`)
      );
    }
  }, EMOTE_LIST_MSECS);

  await ensureFreshToken();
  log(`Logged in as ${state.token.login}.`);

  for (;;) {
    try {
      const res = await helix(`/users?login=${encodeURIComponent(CHANNEL)}`);
      const user = res.ok ? (await res.json()).data?.[0] : null;
      if (user) {
        state.broadcasterId = user.id;
        break;
      }
      log(
        `Could not look up ${CHANNEL} (${res.status}), retrying in a minute.`
      );
    } catch (err) {
      log(`Could not reach Twitch: ${err.message ?? err}`);
    }
    await sleep(60 * 1000);
  }

  connect();
  await pollStream();
  setInterval(pollStream, STREAM_POLL_MSECS);
  // renew the login well before it runs out, even when Twitch is quiet
  setInterval(
    () => ensureFreshToken().catch((err) => log(String(err.message ?? err))),
    5 * 60 * 1000
  );
}

// Imported by the tests without starting anything
if (
  process.argv[1] &&
  import.meta.url === pathToFileURL(process.argv[1]).href
) {
  process.on("unhandledRejection", (err) =>
    log(`Error: ${err?.message ?? err}`)
  );
  process.on("SIGTERM", () => {
    try {
      flushEmotes();
    } catch {
      // nothing more to save
    }
    process.exit(0);
  });
  main().catch((err) => {
    log(`Fatal: ${err.message ?? err}`);
    process.exit(1);
  });
}
