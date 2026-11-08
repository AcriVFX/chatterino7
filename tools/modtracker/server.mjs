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
// - Serves them over HTTP to requests carrying the API key. The only writes
//   it takes are pausing, stopping or starting the mod action counting and
//   deleting the counted mod actions, from the Mod activity window, and
//   corrections to single mod actions from setup.ps1 edit (kept apart in
//   edits.tsv, the counted actions stay as they are). The Twitch token is
//   never sent anywhere except to Twitch.
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
  rmSync,
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
// Longest pause the app can ask for
const MAX_PAUSE_MSECS = 24 * 3600 * 1000;

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

async function storeGrant(body, renewal = false) {
  const prev = renewal ? state.token : null;
  const token = {
    accessToken: body.access_token,
    refreshToken: body.refresh_token,
    expiresAt: Date.now() + (body.expires_in ?? 3600) * 1000,
    login: prev?.login,
    userId: prev?.userId,
  };
  if (renewal) {
    // Twitch refresh tokens work once: save the new one before anything
    // else can fail, or a network blip here would force a new sign-in
    saveToken(token);
    state.token = token;
  }

  const validate = await fetch(`${ID_URL}/oauth2/validate`, {
    headers: { Authorization: `OAuth ${body.access_token}` },
  });
  if (!validate.ok) {
    if (renewal && token.userId) {
      // the account is known, the renewed login is good to use
      return token;
    }
    throw new Error(`token validation failed (${validate.status})`);
  }
  const info = await validate.json();
  token.expiresAt = Date.now() + info.expires_in * 1000;
  token.login = info.login;
  token.userId = info.user_id;
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
    await storeGrant(refresh.body, true);
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

// Werbung timeouts: "msecs, target" of each timeout about "Werbung" in
// werbung.tsv, apart from the action files so their format stays the same.
// Same rule as the app (src/util/ModActivity.hpp): the timed-out user wrote
// "werbung" (any case, also inside a word) in chat in the 2 minutes before
// the timeout. The mod's reason doesn't matter.

const WERBUNG_WINDOW_MSECS = 2 * 60 * 1000;
/// the timeout can arrive a little before the chat message it is about
const WERBUNG_SLACK_MSECS = 2000;
const WERBUNG_FILE = join(CHANNEL_DIR, "werbung.tsv");

export function mentionsWerbung(text) {
  return /werbung/i.test(String(text ?? ""));
}

/// login -> when they last wrote "werbung" in chat
const werbungSaid = new Map();

export function noteWerbung(login, msecs, text, said = werbungSaid) {
  if (!login || !mentionsWerbung(text)) {
    return;
  }
  said.delete(login);
  said.set(login, msecs);
  // oldest first: drop what is too old to matter
  for (const [name, at] of said) {
    if (said.size <= 5000 && at >= msecs - WERBUNG_WINDOW_MSECS * 2) {
      break;
    }
    said.delete(name);
  }
}

export function isWerbungTimeout(target, msecs, said = werbungSaid) {
  const at = said.get(String(target).toLowerCase());
  return (
    at !== undefined &&
    at >= msecs - WERBUNG_WINDOW_MSECS - WERBUNG_SLACK_MSECS &&
    at <= msecs + WERBUNG_SLACK_MSECS
  );
}

function recordWerbung(msecs, target) {
  mkdirSync(CHANNEL_DIR, { recursive: true });
  appendFileSync(WERBUNG_FILE, `${msecs}\t${target.toLowerCase()}\n`);
}

/// werbung.tsv lines newer than `since`
function werbungSince(since) {
  if (!existsSync(WERBUNG_FILE)) {
    return "";
  }
  let out = "";
  for (const line of readFileSync(WERBUNG_FILE, "utf8").split("\n")) {
    const msecs = Number(line.slice(0, line.indexOf("\t")));
    if (line && msecs > since) {
      out += `${line}\n`;
    }
  }
  return out;
}

// ---------------------------------------------------------------- edits ---
//
// Corrections made with `setup.ps1 edit`. The action files stay as they were
// counted; edits.tsv lists what to take away from them and what to add:
//   editedAt, "remove" or "add", msecs, kind, moderator, target, seconds
// A remove takes away the one action of that kind, mod and user closest to
// msecs, within 5 seconds (the edit list shows whole seconds, and the app's
// own copy of an action can be a little off). The app applies the same
// list to what it counted itself (src/util/ModActivity.cpp).

const EDITS_FILE = join(CHANNEL_DIR, "edits.tsv");
const SAME_ACTION_MSECS = 5000;
const EDIT_KINDS = [
  "timeout",
  "ban",
  "unban",
  "untimeout",
  "delete",
  "warn",
  "automod_approve",
  "automod_deny",
];
const EDIT_NAME = /^[a-z0-9_]{1,25}$/;
/// The day's list and the edited one can be big on a busy day
const EDITS_MAX_BYTES = 4 * 1024 * 1024;

/// One actions-*.tsv line as an object, null if it isn't one
function parseActionLine(line) {
  const parts = line.split("\t");
  if (parts.length !== 5 || !EDIT_KINDS.includes(parts[1]) || !parts[2]) {
    return null;
  }
  const msecs = Number(parts[0]);
  const seconds = Number(parts[4]);
  if (!Number.isFinite(msecs) || !Number.isFinite(seconds)) {
    return null;
  }
  return {
    msecs,
    kind: parts[1],
    moderator: parts[2],
    target: parts[3],
    seconds,
  };
}

export function parseEdits(text) {
  const edits = [];
  for (const line of String(text ?? "").split("\n")) {
    const [at, op, ...rest] = line.split("\t");
    const action = parseActionLine(rest.join("\t"));
    if (
      (op === "remove" || op === "add") &&
      action &&
      Number.isFinite(Number(at))
    ) {
      edits.push({ op, ...action });
    }
  }
  return edits;
}

function readEditsText() {
  try {
    return readFileSync(EDITS_FILE, "utf8");
  } catch {
    return "";
  }
}

/// `actions` sorted by time with `edits` applied: { kept, removed }, both
/// sorted by time
export function applyEdits(actions, edits) {
  const list = actions.map((a) => ({ ...a, removed: false }));
  list.sort((a, b) => a.msecs - b.msecs);
  for (const edit of edits) {
    if (edit.op === "add") {
      const at = list.findIndex((a) => a.msecs > edit.msecs);
      const added = { ...edit, removed: false, added: true };
      delete added.op;
      list.splice(at < 0 ? list.length : at, 0, added);
      continue;
    }
    let best = -1;
    for (let i = 0; i < list.length; i++) {
      const a = list[i];
      const off = Math.abs(a.msecs - edit.msecs);
      if (
        !a.removed &&
        off <= SAME_ACTION_MSECS &&
        a.kind === edit.kind &&
        a.moderator === edit.moderator &&
        a.target === edit.target &&
        (best < 0 || off < Math.abs(list[best].msecs - edit.msecs))
      ) {
        best = i;
      }
    }
    if (best >= 0) {
      list[best].removed = true;
    }
  }
  const strip = ({ removed, added, ...a }) => a;
  return {
    kept: list.filter((a) => !a.removed).map(strip),
    removed: list.filter((a) => a.removed).map(strip),
  };
}

// --- the day list shown in Notepad

/// Offset of `timeZone` from UTC at `msecs`, in msecs
function zoneOffset(msecs, timeZone) {
  const parts = Object.fromEntries(
    new Intl.DateTimeFormat("en-US", {
      timeZone,
      hourCycle: "h23",
      year: "numeric",
      month: "2-digit",
      day: "2-digit",
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
    })
      .formatToParts(new Date(msecs))
      .map((p) => [p.type, p.value])
  );
  const asUtc = Date.UTC(
    Number(parts.year),
    Number(parts.month) - 1,
    Number(parts.day),
    Number(parts.hour),
    Number(parts.minute),
    Number(parts.second)
  );
  return asUtc - (msecs - (msecs % 1000));
}

/// msecs of a wall clock time in `timeZone`
export function zonedMsecs(day, hour, minute, second, timeZone) {
  const [y, m, d] = day.split("-").map(Number);
  const wall = Date.UTC(y, m - 1, d, hour, minute, second);
  let msecs = wall - zoneOffset(wall, timeZone);
  msecs = wall - zoneOffset(msecs, timeZone);
  return msecs;
}

function clockOf(msecs, timeZone) {
  return new Intl.DateTimeFormat("en-GB", {
    timeZone,
    hourCycle: "h23",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  }).format(new Date(msecs));
}

export function isTimeZone(timeZone) {
  try {
    new Intl.DateTimeFormat("en-US", { timeZone });
    return /^[A-Za-z_]+(\/[A-Za-z0-9_+\-]+)*$/.test(timeZone);
  } catch {
    return false;
  }
}

export function isDay(day) {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(String(day))) {
    return false;
  }
  const date = new Date(`${day}T12:00:00Z`);
  return (
    !Number.isNaN(date.getTime()) && date.toISOString().slice(0, 10) === day
  );
}

/// 600 -> "10m", 90 -> "90s", 0 -> "-"
export function lengthText(seconds) {
  if (!seconds) {
    return "-";
  }
  for (const [unit, size] of [
    ["w", 604800],
    ["d", 86400],
    ["h", 3600],
    ["m", 60],
  ]) {
    if (seconds % size === 0) {
      return `${seconds / size}${unit}`;
    }
  }
  return `${seconds}s`;
}

/// "10m", "1h30m", "600", "600s" -> seconds; null if it isn't a length
export function parseLength(text) {
  const t = String(text).toLowerCase();
  if (/^\d+$/.test(t)) {
    return Number(t);
  }
  if (!/^(\d+[wdhms])+$/.test(t)) {
    return null;
  }
  const size = { w: 604800, d: 86400, h: 3600, m: 60, s: 1 };
  let total = 0;
  for (const [, n, unit] of t.matchAll(/(\d+)([wdhms])/g)) {
    total += Number(n) * size[unit];
  }
  return total;
}

function pad(text, width) {
  return text.length >= width ? `${text} ` : text.padEnd(width);
}

function actionText(action, timeZone) {
  return (
    pad(clockOf(action.msecs, timeZone), 10) +
    pad(action.kind, 16) +
    pad(action.kind === "timeout" ? lengthText(action.seconds) : "-", 8) +
    pad(action.moderator, 17) +
    action.target
  );
}

/// One line of the edited list as an action, or the reason it isn't one
export function parseListLine(line, day, timeZone) {
  const words = line.trim().split(/\s+/);
  if (words.length !== 5) {
    return {
      error:
        "needs 5 parts: time, action, length, mod and user (- for no length)",
    };
  }
  const [time, kindWord, lengthWord, modWord, userWord] = words;
  const clock = /^(\d{1,2}):(\d{2})(?::(\d{2}))?$/.exec(time);
  if (
    !clock ||
    Number(clock[1]) > 23 ||
    Number(clock[2]) > 59 ||
    Number(clock[3] ?? 0) > 59
  ) {
    return { error: `"${time}" is not a time like 20:17:40` };
  }
  const kind = kindWord.toLowerCase();
  if (!EDIT_KINDS.includes(kind)) {
    return { error: `"${kindWord}" is not one of: ${EDIT_KINDS.join(" ")}` };
  }
  let seconds = 0;
  if (kind === "timeout") {
    seconds = parseLength(lengthWord);
    if (!seconds) {
      return { error: `a timeout needs a length like 30s, 10m, 1h or 1d` };
    }
    if (seconds > 14 * 86400) {
      return { error: "a timeout is at most 2 weeks (2w)" };
    }
  }
  const moderator = modWord.replace(/^@/, "").toLowerCase();
  const target = userWord.replace(/^@/, "").toLowerCase();
  if (!EDIT_NAME.test(moderator)) {
    return { error: `"${modWord}" is not a Twitch name` };
  }
  if (!EDIT_NAME.test(target)) {
    return { error: `"${userWord}" is not a Twitch name` };
  }
  const msecs = zonedMsecs(
    day,
    Number(clock[1]),
    Number(clock[2]),
    Number(clock[3] ?? 0),
    timeZone
  );
  return { action: { msecs, kind, moderator, target, seconds } };
}

/// Actions (kept and removed by edits) between `from` and `to`
function editedActions(from, to) {
  const raw = actionsSince(from - SAME_ACTION_MSECS - 1)
    .split("\n")
    .map(parseActionLine)
    .filter((a) => a && a.msecs <= to + SAME_ACTION_MSECS);
  const edits = parseEdits(readEditsText()).filter(
    (e) =>
      e.msecs >= from - 2 * SAME_ACTION_MSECS &&
      e.msecs <= to + 2 * SAME_ACTION_MSECS
  );
  const { kept, removed } = applyEdits(raw, edits);
  const inDay = (a) => a.msecs >= from && a.msecs < to;
  return { kept: kept.filter(inDay), removed: removed.filter(inDay) };
}

function dayRange(day, timeZone) {
  const from = zonedMsecs(day, 0, 0, 0, timeZone);
  const next = new Date(`${day}T12:00:00Z`);
  next.setUTCDate(next.getUTCDate() + 1);
  const to = zonedMsecs(next.toISOString().slice(0, 10), 0, 0, 0, timeZone);
  return { from, to };
}

function dayTitle(day) {
  const weekday = new Date(`${day}T12:00:00Z`).toLocaleDateString("en-US", {
    weekday: "long",
    timeZone: "UTC",
  });
  return `${weekday} ${day}`;
}

/// The text `setup.ps1 edit` opens in Notepad
export function editList(day, timeZone, name = "", actions = null) {
  const { from, to } = dayRange(day, timeZone);
  let { kept, removed } = actions ?? editedActions(from, to);
  const who = String(name).replace(/^@/, "").toLowerCase();
  if (who) {
    const match = (a) => a.moderator === who || a.target === who;
    kept = kept.filter(match);
    removed = removed.filter(match);
  }
  const lines = [
    `# ${CHANNEL} mod actions, ${dayTitle(day)} (${timeZone === "Europe/Berlin" ? "German" : timeZone} time), ${kept.length} ${kept.length === 1 ? "action" : "actions"}${who ? ` with ${who}` : ""}`,
    "# Delete a line to remove that action from the counts.",
    "# Change a line to fix it: the mod, the action, the length or the user.",
    "# Write a new line to add an action the tracker missed.",
    `# Actions: ${EDIT_KINDS.join(" ")}`,
    '# Length is for timeouts (30s, 10m, 1h, 1d), "-" for everything else.',
    "# Save (Ctrl+S) when you're done and press Enter in PowerShell. Lines starting with # don't count.",
    "#",
    `# ${pad("time", 8)}${pad("action", 16)}${pad("length", 8)}${pad("mod", 17)}user`,
    ...kept.map((a) => actionText(a, timeZone)),
  ];
  if (removed.length) {
    lines.push(
      "",
      "#",
      "# Removed earlier on this day (delete the # in front of one to bring it back):",
      ...removed.map((a) => `# ${actionText(a, timeZone)}`)
    );
  }
  return `${lines.join("\n")}\n`;
}

/// The lines of an edit list that are actions, as normalized text -> how
/// often, plus the lines that can't be read
function listLines(text, day, timeZone) {
  const counts = new Map();
  const actions = new Map();
  const errors = [];
  String(text ?? "")
    .replace(/^﻿/, "")
    .split(/\r?\n/)
    .forEach((line, i) => {
      if (!line.trim() || line.trim().startsWith("#")) {
        return;
      }
      const parsed = parseListLine(line, day, timeZone);
      if (parsed.error) {
        errors.push({ line: i + 1, text: line.trim(), problem: parsed.error });
        return;
      }
      const key = actionText(parsed.action, timeZone);
      counts.set(key, (counts.get(key) ?? 0) + 1);
      actions.set(key, parsed.action);
    });
  return { counts, actions, errors };
}

function describe(a, timeZone) {
  const length = a.kind === "timeout" ? `  ${lengthText(a.seconds)}` : "";
  return `${clockOf(a.msecs, timeZone)}  ${a.kind}${length}  ${a.moderator} on ${a.target}`;
}

/// What changed between the list as it was opened (`before`) and as it was
/// saved (`after`). Changes `save` to edits.tsv when asked to and nothing is
/// wrong. Returns { changes, errors, saved }.
export function editDay(
  { day, timeZone, before, after, save = false },
  current = null,
  now = Date.now()
) {
  const old = listLines(before, day, timeZone);
  const fresh = listLines(after, day, timeZone);
  const errors = fresh.errors;

  // the counted actions behind the lines that were opened, to remove the
  // exact one even when the list shows whole seconds
  const { from, to } = dayRange(day, timeZone);
  const counted = new Map();
  for (const a of (current ?? editedActions(from, to)).kept) {
    const key = actionText(a, timeZone);
    if (!counted.has(key)) {
      counted.set(key, []);
    }
    counted.get(key).push(a);
  }

  const removes = [];
  const adds = [];
  for (const [key, n] of old.counts) {
    for (let i = fresh.counts.get(key) ?? 0; i < n; i++) {
      removes.push(counted.get(key)?.shift() ?? old.actions.get(key));
    }
  }
  for (const [key, n] of fresh.counts) {
    for (let i = old.counts.get(key) ?? 0; i < n; i++) {
      const a = fresh.actions.get(key);
      if (a.msecs < from || a.msecs >= to) {
        errors.push({
          line: 0,
          text: key.trim(),
          problem: `the time is not on ${day}`,
        });
        continue;
      }
      adds.push(a);
    }
  }

  // a removed and an added line at the same second with the same mod or
  // user read as one changed action
  const changes = [];
  const unpaired = [...adds];
  for (const r of removes) {
    const i = unpaired.findIndex(
      (a) =>
        Math.floor(a.msecs / 1000) === Math.floor(r.msecs / 1000) &&
        (a.target === r.target || a.moderator === r.moderator)
    );
    if (i < 0) {
      changes.push({ op: "remove", text: describe(r, timeZone) });
      continue;
    }
    const a = unpaired.splice(i, 1)[0];
    const what = [];
    if (a.kind !== r.kind) what.push(`action ${r.kind} -> ${a.kind}`);
    if (
      a.seconds !== r.seconds &&
      (a.kind === "timeout" || r.kind === "timeout")
    )
      what.push(`length ${lengthText(r.seconds)} -> ${lengthText(a.seconds)}`);
    if (a.moderator !== r.moderator)
      what.push(`mod ${r.moderator} -> ${a.moderator}`);
    if (a.target !== r.target) what.push(`user ${r.target} -> ${a.target}`);
    changes.push({
      op: "change",
      text: `${clockOf(r.msecs, timeZone)}  ${r.kind} on ${r.target}: ${what.join(", ")}`,
    });
  }
  for (const a of unpaired) {
    changes.push({ op: "add", text: describe(a, timeZone) });
  }

  let saved = 0;
  if (save && !errors.length && (removes.length || adds.length)) {
    const line = (op, a) =>
      [
        now,
        op,
        a.msecs,
        a.kind,
        a.moderator,
        a.target,
        Math.max(0, a.seconds),
      ].join("\t");
    const text =
      [
        ...removes.map((a) => line("remove", a)),
        ...adds.map((a) => line("add", a)),
      ].join("\n") + "\n";
    mkdirSync(CHANNEL_DIR, { recursive: true });
    appendFileSync(EDITS_FILE, text);
    saved = changes.length;
    log(
      `Saved ${removes.length} removed and ${adds.length} added mod actions from setup.ps1 edit (${day}).`
    );
  }
  return { changes, errors, saved };
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

// ------------------------------------------------------------- tracking ---
//
// Whether mod actions are counted, set from the Mod activity window:
// "on", "paused" until `until` (msecs, then on again by itself) or
// "stopped" until it is started again. `since` is when that state began.
// Emotes and streams are always counted.

const TRACKING_FILE = join(CHANNEL_DIR, "tracking.json");
const TRACKING_STATES = new Set(["on", "paused", "stopped"]);

function loadTracking() {
  try {
    const saved = JSON.parse(readFileSync(TRACKING_FILE, "utf8"));
    if (TRACKING_STATES.has(saved.state)) {
      return {
        state: saved.state,
        until: Number(saved.until) || 0,
        since: Number(saved.since) || 0,
      };
    }
  } catch {
    // never set: counting
  }
  return { state: "on", until: 0, since: 0 };
}

let tracking = null;

function saveTracking() {
  mkdirSync(CHANNEL_DIR, { recursive: true });
  const tmp = `${TRACKING_FILE}.tmp`;
  writeFileSync(tmp, JSON.stringify(tracking));
  renameSync(tmp, TRACKING_FILE);
}

/// The current tracking state; a pause that ran out turns into "on"
export function currentTracking(now = Date.now()) {
  tracking ??= loadTracking();
  if (tracking.state === "paused" && tracking.until <= now) {
    tracking = { state: "on", until: 0, since: tracking.until };
    saveTracking();
  }
  return tracking;
}

/// Sets the tracking state from a request body. Returns an error text or
/// null.
export function setTracking(body, now = Date.now()) {
  const state = body?.state;
  if (!TRACKING_STATES.has(state)) {
    return "state must be on, paused or stopped";
  }
  let until = 0;
  if (state === "paused") {
    until = Number(body.until);
    if (!Number.isFinite(until) || until <= now) {
      return "until must be in the future";
    }
    until = Math.min(Math.round(until), now + MAX_PAUSE_MSECS);
  }
  tracking = { state, until, since: now };
  saveTracking();
  log(
    state === "paused"
      ? `Mod action counting paused until ${new Date(until).toISOString()}.`
      : `Mod action counting ${state === "on" ? "started" : "stopped"}.`
  );
  return null;
}

/// Deletes every counted mod action and stream of the channel. Returns how
/// many actions were deleted.
export function deleteCounts() {
  let actions = 0;
  if (existsSync(CHANNEL_DIR)) {
    for (const name of readdirSync(CHANNEL_DIR)) {
      if (/^actions-\d{4}-\d{2}\.tsv$/.test(name)) {
        const path = join(CHANNEL_DIR, name);
        actions += readFileSync(path, "utf8")
          .split("\n")
          .filter(Boolean).length;
        rmSync(path, { force: true });
      }
    }
    rmSync(join(CHANNEL_DIR, "streams.tsv"), { force: true });
    rmSync(WERBUNG_FILE, { force: true });
    rmSync(EDITS_FILE, { force: true });
  }
  // the running stream gets its row again on the next check
  if (live) {
    live.written = 0;
  }
  log(`Deleted ${actions} counted mod actions and the stream list.`);
  return actions;
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
  if (event.source_broadcaster_user_id || currentTracking().state !== "on") {
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
  if (kind === "timeout" && isWerbungTimeout(detail.user_login, msecs)) {
    recordWerbung(msecs, detail.user_login);
  }
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

let reconnectTimer = null;

function scheduleReconnect() {
  if (reconnectTimer) {
    // one reconnect at a time, or two sessions would count everything twice
    return;
  }
  const wait = backoff;
  backoff = Math.min(backoff * 2, 60 * 1000);
  log(`Disconnected from Twitch, reconnecting in ${wait / 1000}s.`);
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connect();
  }, wait);
}

/// Closes a connection without waiting for its close event: on a dead link
/// Node's WebSocket can stay in CLOSING and never fire onclose.
function dropSocket(ws) {
  ws.onmessage = null;
  ws.onclose = null;
  ws.onerror = null;
  try {
    ws.close();
  } catch {
    // already closed
  }
  if (socket === ws) {
    socket = null;
    state.connected = false;
    clearTimeout(watchdog);
    scheduleReconnect();
  }
}

function armWatchdog(seconds) {
  clearTimeout(watchdog);
  watchdog = setTimeout(
    () => {
      log("Twitch went quiet, reconnecting.");
      if (socket) {
        dropSocket(socket);
      }
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
        dropSocket(ws);
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
      dropSocket(ws);
    }
  };

  ws.onclose = () => {
    if (socket !== ws) {
      // replaced by a reconnect, or a reconnect attempt that failed
      if (socket === null) {
        scheduleReconnect();
      }
      return;
    }
    socket = null;
    state.connected = false;
    clearTimeout(watchdog);
    scheduleReconnect();
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
// separated as "login:messages"); source "*" with name "*" counts every chat
// message, source "any" with name "+" every message with an emote. Same
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

/// day name -> { buckets: Map(key -> {count, users: Map(login -> count)}),
/// dirty }. A count of 0 is "at least one" (files from before chatters were
/// counted).
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
    const users = new Map();
    for (const entry of parts[4] ? parts[4].split(",") : []) {
      const [user, n] = entry.split(":");
      if (user) {
        users.set(user, Math.max(users.get(user) ?? 0, Number(n) || 0));
      }
    }
    buckets.set(parts.slice(0, 3).join("\t"), { count, users });
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
      bucket = { count: 0, users: new Map() };
      data.buckets.set(key, bucket);
    }
    bucket.count++;
    if (user) {
      bucket.users.set(user, (bucket.users.get(user) ?? 0) + 1);
    }
  };
  add(`${hour}\t*\t*`, login);
  const seen = new Set();
  for (const [source, name] of uses) {
    const key = `${hour}\t${source}\t${name}`;
    if (!seen.has(key)) {
      seen.add(key);
      add(key, login);
    }
  }
  // messages with at least one emote. Not source "*": apps from before it
  // was counted add every "*" line to the chat messages.
  if (seen.size > 0) {
    add(`${hour}\tany\t+`, login);
  }
  data.dirty = true;
}

/// A day file as apps from before chatters were counted read it
export function stripEmoteCounts(text) {
  let out = "";
  for (const line of text.split("\n")) {
    const parts = line.split("\t");
    if (parts.length !== 5 || parts[1] === "any") {
      continue;
    }
    parts[4] = parts[4]
      ? parts[4]
          .split(",")
          .map((entry) => entry.split(":")[0])
          .join(",")
      : "";
    out += `${parts.join("\t")}\n`;
  }
  return out;
}

export function flushEmotes() {
  mkdirSync(EMOTE_DIR, { recursive: true });
  for (const [day, data] of emoteDays) {
    if (!data.dirty) {
      continue;
    }
    let out = "";
    for (const [key, bucket] of data.buckets) {
      const users = [...bucket.users].map(([user, n]) =>
        n > 0 ? `${user}:${n}` : user
      );
      out += `${key}\t${bucket.count}\t${users.join(",")}\n`;
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
  noteWerbung(login, msecs, text);
  chat.lastMessageAt = Date.now();
}

let chatSocket = null;
let chatBackoff = 1000;
let chatWatchdog = null;

/// Like dropSocket: don't wait for onclose, it may never come on a dead link
function dropChatSocket(ws) {
  ws.onopen = null;
  ws.onmessage = null;
  ws.onclose = null;
  ws.onerror = null;
  try {
    ws.close();
  } catch {
    // already closed
  }
  if (chatSocket === ws) {
    chatSocket = null;
    chat.connected = false;
    clearTimeout(chatWatchdog);
    const wait = chatBackoff;
    chatBackoff = Math.min(chatBackoff * 2, 60 * 1000);
    log(`Disconnected from chat, reconnecting in ${wait / 1000}s.`);
    setTimeout(connectChat, wait);
  }
}

function armChatWatchdog(ws) {
  clearTimeout(chatWatchdog);
  // Twitch pings about every 5 minutes
  chatWatchdog = setTimeout(
    () => {
      log("Chat went quiet, reconnecting.");
      dropChatSocket(ws);
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
          dropChatSocket(ws);
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
    if (chatSocket === ws) {
      dropChatSocket(ws);
    }
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

function readBody(req, maxBytes = 4096) {
  return new Promise((resolve) => {
    let body = "";
    req.setEncoding("utf8");
    req.on("data", (chunk) => {
      body += chunk;
      if (body.length > maxBytes) {
        req.destroy();
        resolve(null);
      }
    });
    req.on("end", () => {
      try {
        resolve(JSON.parse(body || "{}"));
      } catch {
        resolve(null);
      }
    });
    req.on("error", () => resolve(null));
  });
}

function trackingJson() {
  return { ...currentTracking() };
}

async function handlePost(req, res, url) {
  const edits = url.pathname === `/v1/${CHANNEL}/edits`;
  const body = await readBody(req, edits ? EDITS_MAX_BYTES : 4096);
  if (body === null) {
    return send(res, 400, "Bad request\n");
  }
  if (edits) {
    // from setup.ps1 edit: the day's list as it was opened and as it was
    // saved in Notepad
    const timeZone = String(body.timeZone ?? "");
    if (!isDay(body.day) || !isTimeZone(timeZone)) {
      return send(
        res,
        400,
        "day must be YYYY-MM-DD and timeZone a time zone\n"
      );
    }
    if (typeof body.before !== "string" || typeof body.after !== "string") {
      return send(res, 400, "before and after must be the list texts\n");
    }
    const result = editDay({
      day: body.day,
      timeZone,
      before: body.before,
      after: body.after,
      save: body.save === true,
    });
    return send(res, 200, JSON.stringify(result), "application/json");
  }
  if (url.pathname === `/v1/${CHANNEL}/tracking`) {
    const error = setTracking(body);
    if (error) {
      return send(res, 400, `${error}\n`);
    }
    return send(res, 200, JSON.stringify(trackingJson()), "application/json");
  }
  if (url.pathname === `/v1/${CHANNEL}/delete`) {
    // the app sends the channel name the user typed to confirm
    if (body.confirm !== CHANNEL) {
      return send(res, 400, "confirm must be the channel name\n");
    }
    const actions = deleteCounts();
    return send(res, 200, JSON.stringify({ actions }), "application/json");
  }
  return send(res, 404, "Not found\n");
}

const server = createServer((req, res) => {
  if (!authorized(req) || (req.method !== "GET" && req.method !== "POST")) {
    return send(res, 404, "Not found\n");
  }
  const url = new URL(req.url, "http://localhost");
  if (req.method === "POST") {
    return handlePost(req, res, url).catch((err) => {
      log(`Request failed: ${err.message ?? err}`);
      send(res, 500, "Failed\n");
    });
  }
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
        tracking: trackingJson(),
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
  if (url.pathname === `/v1/${CHANNEL}/werbung`) {
    const since = Number(url.searchParams.get("since") ?? 0);
    return send(res, 200, werbungSince(Number.isFinite(since) ? since : 0));
  }
  if (url.pathname === `/v1/${CHANNEL}/edits`) {
    return send(res, 200, readEditsText());
  }
  if (url.pathname === `/v1/${CHANNEL}/edit-list`) {
    const day = url.searchParams.get("day") ?? "";
    const timeZone = url.searchParams.get("tz") ?? "";
    if (!isDay(day) || !isTimeZone(timeZone)) {
      return send(res, 400, "day must be YYYY-MM-DD and tz a time zone\n");
    }
    return send(
      res,
      200,
      editList(day, timeZone, url.searchParams.get("name") ?? "")
    );
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
    let text;
    try {
      text = readFileSync(join(EMOTE_DIR, `${day}.tsv`), "utf8");
    } catch {
      return send(res, 404, "Not found\n");
    }
    // apps from before chatters were counted (they don't send v=2) read
    // "login:3" as a name: send them only the logins
    if (url.searchParams.get("v") !== "2") {
      text = stripEmoteCounts(text);
    }
    return send(res, 200, text);
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
