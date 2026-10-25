"use strict";

function when(msecs) {
  if (!msecs) {
    return "never";
  }
  return new Date(msecs).toLocaleString([], {
    day: "2-digit",
    month: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
  });
}

function show(status) {
  if (!status) {
    return;
  }
  const stats = status.stats || {};
  document.getElementById("captured").textContent = stats.captured || 0;
  document.getElementById("delivered").textContent = stats.delivered || 0;
  document.getElementById("pending").textContent = status.pending;
  document.getElementById("last").textContent = when(stats.lastCapturedAt);
  document.getElementById("problem").textContent =
    status.pending && stats.lastError ? stats.lastError : "";
}

chrome.runtime.sendMessage({ type: "status" }).then(show);

document.getElementById("send").addEventListener("click", () => {
  const button = document.getElementById("send");
  button.disabled = true;
  chrome.runtime
    .sendMessage({ type: "send-now" })
    .then(show)
    .finally(() => {
      button.disabled = false;
    });
});
