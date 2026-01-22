import { useState, useEffect } from 'react';

type Preset = {
  id: string;
  name: string;
};

type PresetPickerProps = {
  currentPresetId: string | null;
  onPresetSelect: (presetId: string) => void;
  disabled?: boolean;
};

export function PresetPicker({
  currentPresetId,
  onPresetSelect,
  disabled = false,
}: PresetPickerProps) {
  const [presets, setPresets] = useState<Preset[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    fetch('/api/presets')
      .then((res) => res.json())
      .then((data) => {
        setPresets(data);
        setLoading(false);
      })
      .catch((err) => {
        console.error('Failed to load presets:', err);
        setError('Failed to load presets');
        setLoading(false);
      });
  }, []);

  if (loading) {
    return <div className='preset-picker loading'>Loading presets...</div>;
  }

  if (error) {
    return <div className='preset-picker error'>{error}</div>;
  }

  return (
    <div className='preset-picker'>
      <h2 className='preset-picker-title'>Presets</h2>
      <div className='preset-list'>
        {presets.map((preset) => (
          <button
            key={preset.id}
            className={`preset-button ${
              preset.id === currentPresetId ? 'active' : ''
            }`}
            onClick={() => onPresetSelect(preset.id)}
            disabled={disabled}
          >
            <span className='preset-button-name'>{preset.name}</span>
            {preset.id === currentPresetId && (
              <span className='preset-button-indicator'>●</span>
            )}
          </button>
        ))}
      </div>
    </div>
  );
}
