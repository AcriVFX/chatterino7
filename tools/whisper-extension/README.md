# Chatterino9 Whispers (Edge extension)

Twitch has no API for whispers you send, so whispers sent on twitch.tv never
reach Chatterino. This extension catches them in the browser and hands them to
Chatterino9, which saves them in the Whispers window like whispers sent from
the app.

## How it works

- `background.js` notices whisper sends with `webRequest` (host access to
  gql.twitch.tv and the two sites below, which webRequest needs): a POST from
  www.twitch.tv or dashboard.twitch.tv to gql.twitch.tv whose body is a
  `SendWhisper` operation. It reads only the request body (recipient user id
  and text), never headers, cookies or storage, so the Twitch login never
  passes through it. Scripts in a twitch.tv page can't make up whispers.
- `page.js` runs inside twitch.tv's page only to see Twitch's answer. When
  Twitch refuses a whisper (e.g. you're blocked), it tells the extension
  through `bridge.js`, and that whisper isn't saved. It can only keep a
  whisper out, never add one.
- At most 20 whispers a minute and 200 an hour are kept.
- Delivery uses the browser's native messaging with its own host,
  `com.chatterino9.whispers` (Chatterino9 registers it for Edge and Chrome on
  every start). Edge starts `chatterino.exe` as the host, which only accepts
  these whispers from this extension and passes them to the running app.
- The app looks up the recipient's login by id (Helix, at most 20 lookups a
  minute) and saves the whisper with the time it was sent. It notes each saved
  whisper in `Misc/Whispers/browser-receipts.tsv`; the host answers the
  extension only once a whisper is in there. Anything not saved yet (app
  closed or crashed, lookup failed) stays in the extension and is sent again
  every 2 minutes (up to 500 whispers, 30 days). The same whisper is never
  saved twice.

Nothing goes over the network except Twitch's own request, which the page makes
anyway.

The extension's ID is fixed by the `key` in `manifest.json`:
`bcgnaflbiogmdloflolimldoepjipehp`.

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
