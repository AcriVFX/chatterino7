# Chatterino9 Whispers (Edge extension)

Twitch has no API for whispers you send, so whispers sent on twitch.tv never
reach Chatterino. This extension catches them in the browser and hands them to
Chatterino9, which saves them in the Whispers window like whispers sent from
the app.

## How it works

- `page.js` runs inside twitch.tv's page and watches for the request Twitch
  makes when a whisper is sent (`SendWhisper` on gql.twitch.tv). It reads only
  that request's body (recipient user id and text) and whether Twitch accepted
  it. It never reads headers, cookies or storage, so the Twitch login never
  passes through it.
- `bridge.js` passes the whisper to `background.js`, which keeps it in the
  extension's storage until Chatterino9 confirms it got it.
- Delivery uses the browser's native messaging: Edge starts `chatterino.exe`
  as the `com.chatterino.chatterino` host (the same bridge Chatterino's own
  browser extension uses), which passes the whisper to the running app. If the
  app is closed, the whisper waits and is retried every 2 minutes (up to 500
  whispers, 30 days).
- The app looks up the recipient's login by id (Helix) and saves the whisper
  with the time it was sent. The same whisper is never saved twice.

Nothing goes over the network except Twitch's own request, which the page makes
anyway.

The extension's ID is fixed by the `key` in `manifest.json`:
`bcgnaflbiogmdloflolimldoepjipehp`. Chatterino9 allows that ID in its native
messaging manifest and also registers the manifest for Edge.

## Install (Edge)

1. Put this folder somewhere it can stay, e.g.
   `%APPDATA%\Chatterino2\Misc\WhisperExtension`.
2. Open `edge://extensions`, turn on **Developer mode** (bottom left).
3. Click **Load unpacked** and pick the folder.
4. Reload open twitch.tv tabs.

The toolbar button shows how many whispers were caught and whether any are
still waiting for Chatterino9.

## Limits

- Only whispers sent from this browser on this PC (not the phone app).
- A Twitch site change can break it. The toolbar button then shows
  "Whispers caught on twitch.tv" not going up after sending one.
- Whispers go to the account that is selected in Chatterino9.
