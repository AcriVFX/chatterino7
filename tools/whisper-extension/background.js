// Chatterino9 Whispers: notices each whisper you send on twitch.tv and keeps
// it until Chatterino9 has saved it.
//
// - The send is seen from outside the page (webRequest on gql.twitch.tv,
//   request body only, never headers), so scripts in a twitch.tv page can't
//   make up whispers. page.js only reports Twitch's refusals.
// - Whispers go to the app through the browser's native messaging (its own
//   host, com.chatterino9.whispers), never over the network. The app answers
//   once a whisper is saved; until then it stays here and is sent again.
"use strict";

const HOST = "com.chatterino9.whispers";
const RETRY_ALARM = "retry";
// Whispers kept while Chatterino9 is closed
const MAX_PENDING = 500;
const MAX_AGE_MS = 30 * 24 * 60 * 60 * 1000;
// Far more than anyone types, far less than a flood
const MAX_PER_MINUTE = 20;
const MAX_PER_HOUR = 200;
// Sites whose whisper sends count
const INITIATORS = new Set([
  "https://www.twitch.tv",
  "https://dashboard.twitch.tv",
]);
// How long to wait for page.js to say Twitch refused a send
const REFUSAL_WAIT_MS = 3000;

// storage: { pending: [{ id, recipientId, message, sentAt }],
//            recent: [msecs of recent captures], stats: {...} }

// one storage change at a time
let queue = Promise.resolve();
function locked(task) {
  const run = queue.then(task, task);
  queue = run.catch(() => {});
  return run;
}

async function load() {
  const {
    pending = [],
    recent = [],
    stats = {},
  } = await chrome.storage.local.get(["pending", "recent", "stats"]);
  return { pending, recent, stats };
}

function newId() {
  return `${Date.now().toString(36)}-${crypto.randomUUID()}`;
}

// Same as in page.js: whisper sends in a GraphQL request body
function findWhispers(bodyText) {
  if (typeof bodyText !== "string" || !/whisper/i.test(bodyText)) {
    return [];
  }
  let parsed;
  try {
    parsed = JSON.parse(bodyText);
  } catch {
    return [];
  }
  const ops = Array.isArray(parsed) ? parsed : [parsed];
  const found = [];
  ops.forEach((op) => {
    if (!op || typeof op !== "object") {
      return;
    }
    const named =
      /whisper/i.test(op.operationName || "") ||
      /sendWhisper/.test(op.query || "");
    const input = op.variables && op.variables.input;
    if (!named || !input || typeof input !== "object") {
      return;
    }
    const recipientId = String(
      input.recipientUserID ?? input.recipientUserId ?? ""
    );
    const message = input.message;
    if (
      /^[0-9]{1,20}$/.test(recipientId) &&
      typeof message === "string" &&
      message.trim() &&
      message.length <= 1000
    ) {
      found.push({ recipientId, message });
    }
  });
  return found;
}

function add(whisper) {
  return locked(async () => {
    const { pending, recent, stats } = await load();
    const now = Date.now();
    const lastHour = recent.filter((t) => t > now - 60 * 60 * 1000);
    const lastMinute = lastHour.filter((t) => t > now - 60 * 1000);
    if (
      lastMinute.length >= MAX_PER_MINUTE ||
      lastHour.length >= MAX_PER_HOUR
    ) {
      stats.skipped = (stats.skipped || 0) + 1;
      stats.lastError = "Too many whispers at once, some were skipped";
      await chrome.storage.local.set({ recent: lastHour, stats });
      return;
    }
    lastHour.push(now);
    const kept = pending.filter((w) => w.sentAt >= now - MAX_AGE_MS);
    kept.push({ id: newId(), ...whisper });
    while (kept.length > MAX_PENDING) {
      kept.shift();
    }
    stats.captured = (stats.captured || 0) + 1;
    stats.lastCapturedAt = now;
    await chrome.storage.local.set({ pending: kept, recent: lastHour, stats });
  });
}

// Twitch's refusals reported by page.js: [{ recipientId, message, at }]
let refusals = [];

function takeRefusal(whisper) {
  const now = Date.now();
  refusals = refusals.filter((r) => r.at > now - 30000);
  const i = refusals.findIndex(
    (r) =>
      r.recipientId === whisper.recipientId && r.message === whisper.message
  );
  if (i < 0) {
    return false;
  }
  refusals.splice(i, 1);
  return true;
}

// requestId -> { whispers, sentAt }
const sends = new Map();

function requestText(requestBody) {
  if (!requestBody || !Array.isArray(requestBody.raw)) {
    return "";
  }
  const decoder = new TextDecoder();
  let text = "";
  for (const part of requestBody.raw) {
    if (part.bytes) {
      text += decoder.decode(part.bytes, { stream: true });
    }
  }
  return text + decoder.decode();
}

chrome.webRequest.onBeforeRequest.addListener(
  (details) => {
    if (details.method !== "POST" || !INITIATORS.has(details.initiator)) {
      return;
    }
    const whispers = findWhispers(requestText(details.requestBody));
    if (whispers.length) {
      sends.set(details.requestId, { whispers, sentAt: Date.now() });
    }
  },
  { urls: ["https://gql.twitch.tv/*"] },
  ["requestBody"]
);

chrome.webRequest.onCompleted.addListener(
  (details) => {
    const send = sends.get(details.requestId);
    if (!send) {
      return;
    }
    sends.delete(details.requestId);
    if (details.statusCode < 200 || details.statusCode >= 300) {
      return;
    }
    setTimeout(() => {
      const accepted = send.whispers.filter((w) => !takeRefusal(w));
      Promise.all(
        accepted.map((w) =>
          add({
            recipientId: w.recipientId,
            message: w.message,
            sentAt: send.sentAt,
          })
        )
      )
        .then(() => (accepted.length ? flush() : undefined))
        .catch(() => {});
    }, REFUSAL_WAIT_MS);
  },
  { urls: ["https://gql.twitch.tv/*"] }
);

chrome.webRequest.onErrorOccurred.addListener(
  (details) => {
    sends.delete(details.requestId);
  },
  { urls: ["https://gql.twitch.tv/*"] }
);

// Passes whispers to Chatterino9. Resolves to { saved: Set<id>, dropped:
// Set<id>, error?: string }
function deliver(whispers) {
  return new Promise((resolve) => {
    const saved = new Set();
    const dropped = new Set();
    let answered = 0;
    let finished = false;
    let port;
    let timer;

    const finish = (error) => {
      if (finished) {
        return;
      }
      finished = true;
      clearTimeout(timer);
      try {
        port.disconnect();
      } catch {
        // already closed
      }
      resolve({ saved, dropped, error });
    };

    try {
      port = chrome.runtime.connectNative(HOST);
    } catch (e) {
      resolve({ saved, dropped, error: String(e) });
      return;
    }

    port.onMessage.addListener((msg) => {
      if (!msg || msg.type !== "ack" || typeof msg.id !== "string") {
        return;
      }
      answered++;
      if (msg.ok) {
        saved.add(msg.id);
      } else if (msg.reason === "too-long" || msg.reason === "rejected") {
        // the app will never take it
        dropped.add(msg.id);
      } else if (msg.reason === "not-running") {
        // the rest won't get there either
        finish("Chatterino9 is not running");
        return;
      }
      // "pending": not saved yet, sent again later
      if (answered >= whispers.length) {
        finish(
          saved.size + dropped.size < answered
            ? "Chatterino9 hasn't saved some yet"
            : undefined
        );
      }
    });
    port.onDisconnect.addListener(() => {
      const error = chrome.runtime.lastError;
      finish(
        error
          ? error.message
          : answered < whispers.length
            ? "Chatterino9 closed the connection"
            : undefined
      );
    });
    timer = setTimeout(() => finish("Chatterino9 did not answer"), 9000);

    for (const w of whispers) {
      port.postMessage({
        action: "chatterino9-whisper-sent",
        id: w.id,
        recipientId: w.recipientId,
        message: w.message,
        sentAt: w.sentAt,
      });
    }
  });
}

let flushing = null;
function flush() {
  if (!flushing) {
    flushing = flushOnce().finally(() => {
      flushing = null;
    });
  }
  return flushing;
}

// Whispers passed on per connection, well below the app's queue size
const BATCH = 50;

async function flushOnce() {
  for (;;) {
    const { pending } = await load();
    if (!pending.length) {
      await chrome.alarms.clear(RETRY_ALARM);
      return;
    }
    const batch = pending.slice(0, BATCH);
    const result = await deliver(batch);
    const left = await settle(result);
    if (result.error || !left || result.saved.size < batch.length) {
      return;
    }
  }
}

// Removes what the app saved or will never take, returns how many are left
function settle(result) {
  return locked(async () => {
    const { pending: now, stats } = await load();
    const left = now.filter(
      (w) => !result.saved.has(w.id) && !result.dropped.has(w.id)
    );
    stats.delivered = (stats.delivered || 0) + result.saved.size;
    if (result.saved.size) {
      stats.lastDeliveredAt = Date.now();
    }
    stats.lastError = result.error || "";
    stats.lastTriedAt = Date.now();
    await chrome.storage.local.set({ pending: left, stats });
    if (left.length) {
      // try again every 2 minutes until Chatterino9 has them
      await chrome.alarms.create(RETRY_ALARM, { periodInMinutes: 2 });
    } else {
      await chrome.alarms.clear(RETRY_ALARM);
    }
    return left.length;
  });
}

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (sender.id !== chrome.runtime.id || !msg) {
    return;
  }
  if (msg.type === "whisper-rejected") {
    refusals.push({
      recipientId: String(msg.recipientId),
      message: String(msg.message),
      at: Date.now(),
    });
    if (refusals.length > 50) {
      refusals.shift();
    }
    return;
  }
  if (msg.type === "status") {
    load().then(({ pending, stats }) =>
      sendResponse({ pending: pending.length, stats })
    );
    return true;
  }
  if (msg.type === "send-now") {
    flush()
      .then(load)
      .then(({ pending, stats }) =>
        sendResponse({ pending: pending.length, stats })
      );
    return true;
  }
});

chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === RETRY_ALARM) {
    flush();
  }
});

chrome.runtime.onStartup.addListener(() => {
  flush();
});
