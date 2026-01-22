import type {
  SignalComponent,
  PedalComponent,
  AmpComponent,
  CabinetComponent,
} from '@ampbox/shared';
import { ParamSlider } from './ParamSlider';
import { BypassToggle } from './BypassToggle';

type BlockCardProps = {
  component: SignalComponent;
  onToggle: (componentId: string, enabled: boolean) => void;
  onParamChange: (componentId: string, key: string, value: number) => void;
  disabled?: boolean;
};

// Helper to get parameter ranges based on param name
const getParamConfig = (key: string) => {
  const configs: Record<string, { min: number; max: number; step: number }> = {
    // Standard params
    input: { min: 0, max: 1, step: 0.01 },
    output: { min: 0, max: 1, step: 0.01 },
    mix: { min: 0, max: 1, step: 0.01 },

    // EQ
    low: { min: 0, max: 1, step: 0.01 },
    mid: { min: 0, max: 1, step: 0.01 },
    high: { min: 0, max: 1, step: 0.01 },

    // Pedal-specific
    drive: { min: 0, max: 1, step: 0.01 },
    tone: { min: 0, max: 1, step: 0.01 },

    // Amp-specific
    gain: { min: 0, max: 1, step: 0.01 },
    master: { min: 0, max: 1, step: 0.01 },
    presence: { min: 0, max: 1, step: 0.01 },
  };
  return configs[key] || { min: 0, max: 1, step: 0.01 };
};

// Helper to get emoji for component type
const getComponentEmoji = (component: SignalComponent) => {
  if (component.type === 'pedal') {
    const pedal = component as PedalComponent;
    const categoryEmoji: Record<string, string> = {
      overdrive: '🔥',
      distortion: '⚡',
      fuzz: '💥',
      boost: '📈',
      compressor: '🎚️',
      delay: '⏱️',
      reverb: '🌊',
      modulation: '〰️',
      eq: '�️',
    };
    return categoryEmoji[pedal.category] || '🎸';
  }
  if (component.type === 'amp') return '🎛️';
  if (component.type === 'cabinet') return '📢';
  return '🎸';
};

// Helper to format category for display
const formatCategory = (category: string) => {
  return category.charAt(0).toUpperCase() + category.slice(1);
};

export function BlockCard({
  component,
  onToggle,
  onParamChange,
  disabled = false,
}: BlockCardProps) {
  const emoji = getComponentEmoji(component);

  return (
    <div
      className={`block-card ${component.enabled ? 'enabled' : 'bypassed'}`}
      data-type={component.type}
      data-category={
        component.type === 'pedal' ? (component as PedalComponent).category : ''
      }
    >
      <div className='block-header'>
        <div className='block-header-info'>
          <h3 className='block-title'>
            {emoji} {component.model}
          </h3>
          {component.type === 'pedal' && (
            <p className='block-type'>
              {formatCategory((component as PedalComponent).category)}
            </p>
          )}
          {component.type === 'amp' && (
            <p className='block-type'>
              Amplifier{' '}
              {(component as AmpComponent).includesCabinet && '(w/ Cabinet)'}
            </p>
          )}
          {component.type === 'cabinet' && (
            <p className='block-type'>Cabinet</p>
          )}
        </div>
        <BypassToggle
          enabled={component.enabled}
          onToggle={(enabled) => onToggle(component.id, enabled)}
          disabled={disabled}
        />
      </div>

      <div className='block-params'>
        {Object.entries(component.params).map(([key, value]) => {
          if (value === undefined) return null;
          const config = getParamConfig(key);
          return (
            <ParamSlider
              key={key}
              label={key}
              value={value}
              min={config.min}
              max={config.max}
              step={config.step}
              onChange={(newValue) =>
                onParamChange(component.id, key, newValue)
              }
              disabled={disabled || !component.enabled}
            />
          );
        })}
      </div>
    </div>
  );
}
