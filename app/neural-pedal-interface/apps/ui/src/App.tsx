import { useEffect, useRef, useState, useCallback } from 'react';
import type { DeviceState, ServerMsg, ClientMsg } from '@ampbox/shared';
import { DeviceView } from './components/DeviceView';
import { PresetPicker } from './components/PresetPicker';
import './App.css';

function App() {
  const [wsConnected, setWsConnected] = useState(false);
  const [deviceState, setDeviceState] = useState<DeviceState | null>(null);
  const wsRef = useRef<WebSocket | null>(null);
  const isShuttingDownRef = useRef(false);
  const reconnectTimeoutRef = useRef<number | null>(null);

  // Send a message to the WebSocket server
  const sendMessage = useCallback((msg: ClientMsg) => {
    if (!wsRef.current || wsRef.current.readyState !== WebSocket.OPEN) {
      console.warn('⚠️ WebSocket not connected');
      return;
    }
    wsRef.current.send(JSON.stringify(msg));
    console.log('📤 Sent message:', msg);
  }, []);

  // Handle component toggle (pedal/amp/cabinet enable/disable)
  const handleComponentToggle = useCallback(
    (componentId: string, enabled: boolean) => {
      sendMessage({ type: 'toggleComponent', componentId, enabled });
    },
    [sendMessage]
  );

  // Handle parameter change
  const handleParamChange = useCallback(
    (componentId: string, key: string, value: number) => {
      sendMessage({ type: 'setComponentParam', componentId, key, value });
    },
    [sendMessage]
  );

  // Handle preset selection
  const handlePresetSelect = useCallback(async (presetId: string) => {
    try {
      const response = await fetch(`/api/preset/${presetId}`, {
        method: 'POST',
      });
      if (!response.ok) {
        console.error('Failed to load preset');
      }
      // State update will come via WebSocket broadcast
    } catch (err) {
      console.error('Failed to load preset:', err);
    }
  }, []);

  useEffect(() => {
    isShuttingDownRef.current = false;

    const connect = () => {
      // Prevent duplicate sockets in dev + reconnect loops
      if (wsRef.current && wsRef.current.readyState !== WebSocket.CLOSED)
        return;

      const ws = new WebSocket(
        `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`
      );
      wsRef.current = ws;

      ws.onopen = () => {
        console.log('✅ WS connected');
        setWsConnected(true);
      };
      ws.onmessage = (ev) => {
        try {
          const msg = JSON.parse(ev.data) as ServerMsg;
          console.log('📩 WS message:', msg);

          if (msg.type === 'state') {
            setDeviceState(msg.state);
          } else if (msg.type === 'error') {
            console.error('Server error:', msg.message);
          }
        } catch (err) {
          console.error('Failed to parse WS message:', err);
        }
      };
      ws.onerror = (err) => console.error('❌ WS error:', err);

      ws.onclose = () => {
        setWsConnected(false);
        if (isShuttingDownRef.current) return; // <--- key line
        console.log('🔌 WS disconnected, reconnecting in 2s...');
        reconnectTimeoutRef.current = window.setTimeout(connect, 2000);
      };
    };

    connect();

    return () => {
      isShuttingDownRef.current = true;

      if (reconnectTimeoutRef.current) {
        clearTimeout(reconnectTimeoutRef.current);
        reconnectTimeoutRef.current = null;
      }

      wsRef.current?.close();
      wsRef.current = null;
    };
  }, []);

  return (
    <div className='app'>
      <header className='app-header'>
        <h1>🎸 Neural Pedal Interface</h1>
        <div className='connection-status'>
          {wsConnected ? '🟢 Connected' : '🔴 Disconnected'}
        </div>
      </header>

      <main className='app-main'>
        {deviceState ? (
          <>
            <PresetPicker
              currentPresetId={deviceState.presetId}
              onPresetSelect={handlePresetSelect}
              disabled={!wsConnected}
            />
            <DeviceView
              deviceState={deviceState}
              onComponentToggle={handleComponentToggle}
              onParamChange={handleParamChange}
              disabled={!wsConnected}
            />
          </>
        ) : (
          <div className='loading'>
            <p>Waiting for device state...</p>
          </div>
        )}
      </main>
    </div>
  );
}

export default App;
