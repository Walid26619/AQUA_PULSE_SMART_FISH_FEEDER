# AquaPulse Feeder Dashboard

Web app for the AquaPulse ESP32 fish feeder. Talks directly to the same
Firestore project the ESP32 firmware writes to, no separate backend needed.

## Running it

```
npm install
npm run dev
```

Then open the local address it prints, usually `http://localhost:5173`.

## Before it will actually work: check Firestore rules

Go to the Firebase console, Firestore Database, Rules tab. The ESP32
firmware assumes open read and write access:

```
rules_version = '2';
service cloud.firestore {
  match /databases/{database}/documents {
    match /{document=**} {
      allow read, write: if true;
    }
  }
}
```

If your rules look different (locked down, requiring auth), this app's reads
and writes will silently fail or throw permission errors in the browser
console. Either match the rule above for now, or add Firebase Auth to both
this app and lock rules down properly later, this app currently has no login
screen since it was built to match the open-access setup.

## What this app does

- Shows live feeder status: online/offline, hopper capacity, last feed time,
  any active fault.
- Manual feed button, sends a `feed` command the ESP32 picks up within a few
  seconds.
- Self test and reboot buttons.
- Schedule manager, add, enable/disable, or remove feeding times. The ESP32
  picks these up automatically and writes them into its own memory.
- System health grid, per-component status (LCD, RTC, hopper sensor, stepper,
  button, WiFi).
- Feed history log.

## Firestore schema this app expects

```
devices/feeder_01                    (root status document)
devices/feeder_01/commands/{id}      (feed / reboot / selftest requests)
devices/feeder_01/history/{id}       (completed feed events)
devices/feeder_01/schedules/{id}     (recurring feeding times)
```

This matches the schema the ESP32 firmware reads and writes. If you rename
the device, update `DEVICE_ID` in `src/firebaseConfig.ts` to match.

## Deploying

Firebase Hosting is the simplest option since you're already on Firebase:

```
npm run build
npm install -g firebase-tools
firebase login
firebase init hosting   (choose the dist folder as the public directory)
firebase deploy
```
