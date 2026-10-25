// Chatterino9 Whispers: passes what page.js saw on to the extension's
// background worker (page.js runs in twitch.tv's page and can't reach it).
(() => {
  "use strict";

  window.addEventListener("message", (event) => {
    const data = event.data;
    if (
      event.source !== window ||
      !data ||
      data.source !== "chatterino9-whispers" ||
      data.type !== "sent"
    ) {
      return;
    }
    if (
      typeof data.recipientId !== "string" ||
      !/^[0-9]{1,20}$/.test(data.recipientId) ||
      typeof data.message !== "string" ||
      !data.message.trim() ||
      data.message.length > 1000 ||
      typeof data.sentAt !== "number"
    ) {
      return;
    }
    try {
      chrome.runtime
        .sendMessage({
          type: "whisper-sent",
          recipientId: data.recipientId,
          message: data.message,
          sentAt: data.sentAt,
        })
        .catch(() => {});
    } catch {
      // the extension was reloaded: this page needs a refresh
    }
  });
})();
