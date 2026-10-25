// Chatterino9 Whispers: runs inside twitch.tv's own page, so it sees the
// request Twitch makes when you send a whisper (on the site, in mod view, in
// the stream manager or with /w in web chat).
//
// It only reads the body of a whisper send (who it goes to and the text) and
// whether Twitch accepted it. It never reads request headers, cookies or
// storage, so your Twitch login never passes through it.
(() => {
  "use strict";

  const SOURCE = "chatterino9-whispers";

  function isGql(url) {
    try {
      return new URL(url, location.href).hostname === "gql.twitch.tv";
    } catch {
      return false;
    }
  }

  // Whisper sends in a GraphQL request body: [{ index, recipientId, message }]
  // (Twitch can batch several operations into one request)
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
    ops.forEach((op, index) => {
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
        message.trim()
      ) {
        found.push({ index, recipientId, message });
      }
    });
    return found;
  }

  // Whether Twitch's answer says the whisper at `index` went through.
  // An answer in a shape we don't know counts as sent (the HTTP status was ok).
  function accepted(json, index) {
    const result = Array.isArray(json) ? json[index] : json;
    if (!result || typeof result !== "object") {
      return true;
    }
    if (Array.isArray(result.errors) && result.errors.length) {
      return false;
    }
    const data = result.data;
    if (data && typeof data === "object" && "sendWhisper" in data) {
      const sent = data.sendWhisper;
      return !!sent && !sent.error;
    }
    return true;
  }

  function report(whispers, sentAt, json) {
    for (const w of whispers) {
      if (json !== undefined && !accepted(json, w.index)) {
        continue;
      }
      window.postMessage(
        {
          source: SOURCE,
          type: "sent",
          recipientId: w.recipientId,
          message: w.message,
          sentAt,
        },
        location.origin
      );
    }
  }

  function watchResponse(responsePromise, bodyPromise, sentAt) {
    responsePromise.then(
      (response) => {
        if (!response || !response.ok) {
          return;
        }
        // clone now, before Twitch reads the body
        let copy;
        try {
          copy = response.clone();
        } catch {
          return;
        }
        bodyPromise
          .then((body) => {
            const whispers = findWhispers(body);
            if (!whispers.length) {
              return;
            }
            return copy.json().then(
              (json) => report(whispers, sentAt, json),
              () => report(whispers, sentAt, undefined)
            );
          })
          .catch(() => {});
      },
      () => {}
    );
  }

  // fetch
  const originalFetch = window.fetch;
  if (typeof originalFetch === "function") {
    window.fetch = function (input, init) {
      let bodyPromise = null;
      try {
        const url =
          typeof input === "string"
            ? input
            : input instanceof URL
              ? input.href
              : input && input.url;
        if (url && isGql(url)) {
          const body = init && init.body;
          if (typeof body === "string") {
            // known right away: skip requests that aren't whispers
            if (findWhispers(body).length) {
              bodyPromise = Promise.resolve(body);
            }
          } else if (body instanceof Blob) {
            bodyPromise = body.text();
          } else if (!body && input instanceof Request) {
            // read a copy before fetch uses up the original
            bodyPromise = input.clone().text();
          }
        }
      } catch {
        bodyPromise = null;
      }
      const result = originalFetch.apply(this, arguments);
      if (bodyPromise) {
        watchResponse(result, bodyPromise, Date.now());
      }
      return result;
    };
  }

  // XMLHttpRequest, in case Twitch uses it for some sends
  const xhrUrls = new WeakMap();
  const xhrProto = XMLHttpRequest.prototype;
  const originalOpen = xhrProto.open;
  const originalSend = xhrProto.send;
  xhrProto.open = function (method, url) {
    try {
      xhrUrls.set(this, String(url));
    } catch {
      // ignore
    }
    return originalOpen.apply(this, arguments);
  };
  xhrProto.send = function (body) {
    try {
      const url = xhrUrls.get(this);
      const whispers =
        url && isGql(url) && typeof body === "string" ? findWhispers(body) : [];
      if (whispers.length) {
        const sentAt = Date.now();
        this.addEventListener("load", () => {
          if (this.status < 200 || this.status >= 300) {
            return;
          }
          let json;
          try {
            if (this.responseType === "json") {
              json = this.response;
            } else if (
              this.responseType === "" ||
              this.responseType === "text"
            ) {
              json = JSON.parse(this.responseText);
            }
          } catch {
            json = undefined;
          }
          report(whispers, sentAt, json);
        });
      }
    } catch {
      // never get in the way of Twitch's own request
    }
    return originalSend.apply(this, arguments);
  };
})();
