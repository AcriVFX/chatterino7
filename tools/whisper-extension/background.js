// Chatterino9 Whispers: keeps each whisper you send on twitch.tv until
// Chatterino9 has it. Whispers go to the app through the browser's native
// messaging (the same local bridge Chatterino's own browser extension uses),
// never over the network.
"use strict";

const HOST = "com.chatterino.chatterino";
const RETRY_ALARM = "retry";
// Whispers kept while Chatterino is closed
const MAX_PENDING = 500;
const MAX_AGE_MS = 30 * 24 * 60 * 60 * 1000;

// storage: { pending: [{ id, recipientId, message, sentAt }], stats: {...} }

// one storage change at a time
let queue = Promise.resolve();
function locked(task) {
  const run = queue.then(task, task);
  queue = run.catch(() => {});
  return run;
}

async function load() {
  const { pending = [], stats = {} } = await chrome.storage.local.get([
    "pending",
    "stats",
  ]);
  return { pending, stats };
}

function newId() {
  return `${Date.now().toString(36)}-${crypto.randomUUID()}`;
}

function add(whisper) {
  return locked(async () => {
    const { pending, stats } = await load();
    const cutoff = Date.now() - MAX_AGE_MS;
    const kept = pending.filter((w) => w.sentAt >= cutoff);
    kept.push({ id: newId(), ...whisper });
    while (kept.length > MAX_PENDING) {
      kept.shift();
    }
    stats.captured = (stats.captured || 0) + 1;
    stats.lastCapturedAt = Date.now();
    await chrome.storage.local.set({ pending: kept, stats });
  });
}

// Passes whispers to Chatterino. Resolves to { delivered: Set<id>, dropped:
// Set<id>, error?: string }
function deliver(whispers) {
  return new Promise((resolve) => {
    const delivered = new Set();
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
      resolve({ delivered, dropped, error });
    };

    try {
      port = chrome.runtime.connectNative(HOST);
    } catch (e) {
      resolve({ delivered, dropped, error: String(e) });
      return;
    }

    port.onMessage.addListener((msg) => {
      if (!msg || msg.type !== "ack" || typeof msg.id !== "string") {
        return;
      }
      answered++;
      if (msg.ok) {
        delivered.add(msg.id);
      } else if (msg.reason === "too-long") {
        dropped.add(msg.id);
      } else {
        // the app isn't running, the rest won't get there either
        finish("Chatterino9 is not running");
        return;
      }
      if (answered >= whispers.length) {
        finish();
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
    timer = setTimeout(() => finish("Chatterino9 did not answer"), 8000);

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
    if (result.error || !left || result.delivered.size < batch.length) {
      return;
    }
  }
}

// Removes what got there, returns how many are left
function settle(result) {
  return locked(async () => {
    const { pending: now, stats } = await load();
    const left = now.filter(
      (w) => !result.delivered.has(w.id) && !result.dropped.has(w.id)
    );
    stats.delivered = (stats.delivered || 0) + result.delivered.size;
    if (result.delivered.size) {
      stats.lastDeliveredAt = Date.now();
    }
    stats.lastError = result.error || "";
    stats.lastTriedAt = Date.now();
    await chrome.storage.local.set({ pending: left, stats });
    if (left.length) {
      // try again every 2 minutes until Chatterino9 is open
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
  if (msg.type === "whisper-sent") {
    add({
      recipientId: String(msg.recipientId),
      message: String(msg.message),
      sentAt: Number(msg.sentAt) || Date.now(),
    })
      .then(flush)
      .catch(() => {});
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
