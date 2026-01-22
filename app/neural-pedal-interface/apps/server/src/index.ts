import express from 'express';
import cors from 'cors';
import http from 'http';
import path from 'path';
import { promises as fs } from 'fs';
import { WebSocketServer, WebSocket } from 'ws';
import type {
  DeviceState,
  ServerMsg,
  ClientMsg,
  PresetFile,
  ModelLibrary,
  PedalComponent,
  AmpComponent,
  CabinetComponent,
} from '@ampbox/shared';
import modelsData from './models.json';

const PRESETS_DIR = path.join(process.cwd(), '../../presets');

// Model library
const models: ModelLibrary = modelsData as ModelLibrary;

// State - Initialize with empty signal chain
const state: DeviceState = {
  presetId: 'default',
  presetName: 'Default',
  pedals: [],
  amp: null,
  cabinet: null,
  updatedAt: Date.now(),
};

const app = express();
app.use(cors());
app.use(express.json());

// ---- Preset Management ----
async function loadPresetFile(presetId: string): Promise<PresetFile | null> {
  try {
    const filePath = path.join(PRESETS_DIR, `${presetId}.json`);
    const data = await fs.readFile(filePath, 'utf-8');
    return JSON.parse(data) as PresetFile;
  } catch (err) {
    console.error(`Failed to load preset ${presetId}:`, err);
    return null;
  }
}

async function savePresetFile(preset: PresetFile): Promise<boolean> {
  try {
    const filePath = path.join(PRESETS_DIR, `${preset.id}.json`);
    await fs.writeFile(filePath, JSON.stringify(preset, null, 2), 'utf-8');
    return true;
  } catch (err) {
    console.error(`Failed to save preset ${preset.id}:`, err);
    return false;
  }
}

async function listPresets(): Promise<Array<{ id: string; name: string }>> {
  try {
    const files = await fs.readdir(PRESETS_DIR);
    const presets: Array<{ id: string; name: string }> = [];

    for (const file of files) {
      if (file.endsWith('.json')) {
        const presetId = file.replace('.json', '');
        const preset = await loadPresetFile(presetId);
        if (preset) {
          presets.push({ id: preset.id, name: preset.name });
        }
      }
    }

    return presets;
  } catch (err) {
    console.error('Failed to list presets:', err);
    return [];
  }
}

function applyPreset(preset: PresetFile) {
  state.presetId = preset.id;
  state.presetName = preset.name;
  state.pedals = preset.pedals;
  state.amp = preset.amp;
  state.cabinet = preset.cabinet;
  state.updatedAt = Date.now();
}

// ---- REST API ----
app.get('/api/health', (_req, res) => res.json({ ok: true, t: Date.now() }));
app.get('/api/state', (_req, res) => res.json(state));

// Get model library
app.get('/api/models', (_req, res) => res.json(models));

// Get list of available presets
app.get('/api/presets', async (_req, res) => {
  const presets = await listPresets();
  res.json(presets);
});

// Load a specific preset
app.post('/api/preset/:id', async (req, res) => {
  const presetId = req.params.id;
  const preset = await loadPresetFile(presetId);

  if (!preset) {
    return res.status(404).json({ error: 'Preset not found' });
  }

  applyPreset(preset);
  broadcast({ type: 'state', state });

  res.json({ ok: true, preset: { id: preset.id, name: preset.name } });
});

// ---- Static UI hosting (for later) ----
// When you build the UI, you'll copy its dist/ into apps/server/public
const publicDir = path.join(process.cwd(), 'public');
app.use(express.static(publicDir));
// Catch-all route for SPA - serve index.html for any non-API routes
app.use((req, res, next) => {
  if (req.path.startsWith('/api') || req.path.startsWith('/ws')) {
    return next();
  }
  res.sendFile(path.join(publicDir, 'index.html'));
});

// ---- WS ----
const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: '/ws' });
const clients = new Set<WebSocket>();

function broadcast(msg: ServerMsg) {
  const payload = JSON.stringify(msg);
  for (const ws of clients) {
    if (ws.readyState === ws.OPEN) ws.send(payload);
  }
}

wss.on('connection', (ws) => {
  clients.add(ws);
  ws.send(JSON.stringify({ type: 'state', state } satisfies ServerMsg));

  ws.on('message', async (data) => {
    try {
      const msg = JSON.parse(data.toString()) as ClientMsg;

      if (msg.type === 'hello') return;

      // ---- Pedal Management ----
      if (msg.type === 'addPedal') {
        const model = models.pedals.find((p) => p.id === msg.modelId);
        if (!model) {
          return ws.send(
            JSON.stringify({
              type: 'error',
              message: 'Pedal model not found',
            } satisfies ServerMsg)
          );
        }

        const newPedal: PedalComponent = {
          id: `pedal-${Date.now()}`,
          type: 'pedal',
          model: model.name,
          category: model.category,
          position: state.pedals.length,
          enabled: true,
          params: { ...model.defaultParams },
        };

        state.pedals.push(newPedal);
        state.updatedAt = Date.now();
        broadcast({ type: 'state', state });
        return;
      }

      if (msg.type === 'removePedal') {
        const index = state.pedals.findIndex((p) => p.id === msg.pedalId);
        if (index === -1) {
          return ws.send(
            JSON.stringify({
              type: 'error',
              message: 'Pedal not found',
            } satisfies ServerMsg)
          );
        }

        state.pedals.splice(index, 1);
        // Update positions
        state.pedals.forEach((p, i) => (p.position = i));
        state.updatedAt = Date.now();
        broadcast({ type: 'state', state });
        return;
      }

      if (msg.type === 'reorderPedal') {
        const pedal = state.pedals.find((p) => p.id === msg.pedalId);
        if (!pedal) {
          return ws.send(
            JSON.stringify({
              type: 'error',
              message: 'Pedal not found',
            } satisfies ServerMsg)
          );
        }

        const oldIndex = pedal.position;
        const newIndex = msg.newPosition;

        if (newIndex < 0 || newIndex >= state.pedals.length) {
          return ws.send(
            JSON.stringify({
              type: 'error',
              message: 'Invalid position',
            } satisfies ServerMsg)
          );
        }

        // Remove from old position
        state.pedals.splice(oldIndex, 1);
        // Insert at new position
        state.pedals.splice(newIndex, 0, pedal);
        // Update all positions
        state.pedals.forEach((p, i) => (p.position = i));

        state.updatedAt = Date.now();
        broadcast({ type: 'state', state });
        return;
      }

      // ---- Component Control (Pedal/Amp/Cabinet params) ----
      if (msg.type === 'setComponentParam') {
        const { componentId, key, value } = msg;

        // Try pedal
        const pedal = state.pedals.find((p) => p.id === componentId);
        if (pedal) {
          (pedal.params as any)[key] = value;
          state.updatedAt = Date.now();
          broadcast({ type: 'state', state });
          return;
        }

        // Try amp
        if (state.amp && state.amp.id === componentId) {
          (state.amp.params as any)[key] = value;
          state.updatedAt = Date.now();
          broadcast({ type: 'state', state });
          return;
        }

        // Try cabinet
        if (state.cabinet && state.cabinet.id === componentId) {
          (state.cabinet.params as any)[key] = value;
          state.updatedAt = Date.now();
          broadcast({ type: 'state', state });
          return;
        }

        return ws.send(
          JSON.stringify({
            type: 'error',
            message: 'Component not found',
          } satisfies ServerMsg)
        );
      }

      if (msg.type === 'toggleComponent') {
        const { componentId, enabled } = msg;

        // Try pedal
        const pedal = state.pedals.find((p) => p.id === componentId);
        if (pedal) {
          pedal.enabled = enabled;
          state.updatedAt = Date.now();
          broadcast({ type: 'state', state });
          return;
        }

        // Try amp
        if (state.amp && state.amp.id === componentId) {
          state.amp.enabled = enabled;
          state.updatedAt = Date.now();
          broadcast({ type: 'state', state });
          return;
        }

        // Try cabinet
        if (state.cabinet && state.cabinet.id === componentId) {
          state.cabinet.enabled = enabled;
          state.updatedAt = Date.now();
          broadcast({ type: 'state', state });
          return;
        }

        return ws.send(
          JSON.stringify({
            type: 'error',
            message: 'Component not found',
          } satisfies ServerMsg)
        );
      }

      // ---- Amp Management ----
      if (msg.type === 'setAmp') {
        const model = models.amps.find((a) => a.id === msg.modelId);
        if (!model) {
          return ws.send(
            JSON.stringify({
              type: 'error',
              message: 'Amp model not found',
            } satisfies ServerMsg)
          );
        }

        state.amp = {
          id: 'amp',
          type: 'amp',
          model: model.name,
          enabled: true,
          includesCabinet: model.includesCabinet,
          params: { ...model.defaultParams },
        };

        state.updatedAt = Date.now();
        broadcast({ type: 'state', state });
        return;
      }

      if (msg.type === 'removeAmp') {
        state.amp = null;
        state.updatedAt = Date.now();
        broadcast({ type: 'state', state });
        return;
      }

      // ---- Cabinet Management ----
      if (msg.type === 'setCabinet') {
        const model = models.cabinets.find((c) => c.id === msg.modelId);
        if (!model) {
          return ws.send(
            JSON.stringify({
              type: 'error',
              message: 'Cabinet model not found',
            } satisfies ServerMsg)
          );
        }

        state.cabinet = {
          id: 'cabinet',
          type: 'cabinet',
          model: model.name,
          enabled: true,
          params: { ...model.defaultParams },
        };

        state.updatedAt = Date.now();
        broadcast({ type: 'state', state });
        return;
      }

      if (msg.type === 'removeCabinet') {
        state.cabinet = null;
        state.updatedAt = Date.now();
        broadcast({ type: 'state', state });
        return;
      }

      // ---- Preset Management ----
      if (msg.type === 'savePreset') {
        const preset: PresetFile = {
          id: msg.presetId,
          name: msg.presetName,
          pedals: state.pedals,
          amp: state.amp,
          cabinet: state.cabinet,
        };

        const success = await savePresetFile(preset);
        if (!success) {
          return ws.send(
            JSON.stringify({
              type: 'error',
              message: 'Failed to save preset',
            } satisfies ServerMsg)
          );
        }

        state.presetId = preset.id;
        state.presetName = preset.name;
        state.updatedAt = Date.now();
        broadcast({ type: 'state', state });
        return;
      }

      if (msg.type === 'loadPreset') {
        const preset = await loadPresetFile(msg.presetId);
        if (!preset) {
          return ws.send(
            JSON.stringify({
              type: 'error',
              message: 'Preset not found',
            } satisfies ServerMsg)
          );
        }

        applyPreset(preset);
        broadcast({ type: 'state', state });
        return;
      }
    } catch {
      ws.send(
        JSON.stringify({
          type: 'error',
          message: 'Invalid message',
        } satisfies ServerMsg)
      );
    }
  });

  ws.on('close', () => clients.delete(ws));
});

const PORT = Number(process.env.PORT ?? 3000);
server.listen(PORT, '0.0.0.0', () => {
  console.log(`API: http://localhost:${PORT}/api/health`);
  console.log(`WS:  ws://localhost:${PORT}/ws`);
});
