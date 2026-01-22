# app (UI + control)

Place your existing Node/React application in this folder.

This repo’s DSP engine exposes a Unix-domain control socket (see the main README) intended for a Node backend / UI controller.

## Expected structure (typical)
- `package.json`
- `src/` (React)
- optional: `server/` or similar (Node backend)

## Quick start (once you add the app)
From this folder:
- Install deps: `npm install` (or `pnpm install` / `yarn`)
- Run dev: `npm run dev` (or whatever your app uses)

If your UI expects the DSP engine running, start it from repo root:
- `./start_alsa.sh start-lowlat`
