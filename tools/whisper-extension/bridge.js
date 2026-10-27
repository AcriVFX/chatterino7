// Chatterino9 Whispers: passes Twitch's refusals that page.js saw on to the
// extension's background worker (page.js runs in twitch.tv's page and can't
// reach it). Refusals can only keep a whisper out, never add one.
(() => {
  "use strict";

  window.addEventListener("message", (event) => {
    const data = event.data;
    if (
      event.source !== window ||
      !data ||
      data.source !== "chatterino9-whispers" ||
      data.type !== "rejected"
    ) {
      return;
    }
    if (
      typeof data.recipientId !== "string" ||
      !/^[0-9]{1,20}$/.test(data.recipientId) ||
      typeof data.message !== "string" ||
      data.message.length > 1000
    ) {
      return;
    }
    try {
      chrome.runtime
        .sendMessage({
          type: "whisper-rejected",
          recipientId: data.recipientId,
          message: data.message,
        })
        .catch(() => {});
    } catch {
      // the extension was reloaded: this page needs a refresh
    }
  });
})();
