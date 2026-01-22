import type { DeviceState } from '@ampbox/shared';
import { BlockCard } from './BlockCard';

type DeviceViewProps = {
  deviceState: DeviceState;
  onComponentToggle: (componentId: string, enabled: boolean) => void;
  onParamChange: (componentId: string, key: string, value: number) => void;
  disabled?: boolean;
};

export function DeviceView({
  deviceState,
  onComponentToggle,
  onParamChange,
  disabled = false,
}: DeviceViewProps) {
  return (
    <div className='device-view'>
      <div className='preset-header'>
        <h1 className='preset-name'>{deviceState.presetName || 'No Preset'}</h1>
        <p className='preset-id'>ID: {deviceState.presetId || 'none'}</p>
      </div>

      <div className='signal-chain-container'>
        {/* Signal flow indicator */}
        <div className='signal-flow-label'>Signal Chain →</div>

        {/* Pedals Section */}
        {deviceState.pedals.length > 0 && (
          <div className='pedals-section'>
            <h2 className='section-title'>Pedals</h2>
            <div className='components-grid'>
              {deviceState.pedals
                .sort((a, b) => a.position - b.position)
                .map((pedal) => (
                  <BlockCard
                    key={pedal.id}
                    component={pedal}
                    onToggle={onComponentToggle}
                    onParamChange={onParamChange}
                    disabled={disabled}
                  />
                ))}
            </div>
          </div>
        )}

        {/* Amp Section */}
        {deviceState.amp && (
          <div className='amp-section'>
            <h2 className='section-title'>Amplifier</h2>
            <div className='components-grid'>
              <BlockCard
                component={deviceState.amp}
                onToggle={onComponentToggle}
                onParamChange={onParamChange}
                disabled={disabled}
              />
            </div>
          </div>
        )}

        {/* Cabinet Section */}
        {deviceState.cabinet && (
          <div className='cabinet-section'>
            <h2 className='section-title'>Cabinet</h2>
            <div className='components-grid'>
              <BlockCard
                component={deviceState.cabinet}
                onToggle={onComponentToggle}
                onParamChange={onParamChange}
                disabled={disabled}
              />
            </div>
          </div>
        )}

        {/* Empty state */}
        {deviceState.pedals.length === 0 &&
          !deviceState.amp &&
          !deviceState.cabinet && (
            <div className='empty-state'>
              <p>No components in signal chain</p>
              <p className='empty-hint'>
                Add pedals, amp, or cabinet to get started
              </p>
            </div>
          )}
      </div>
    </div>
  );
}
