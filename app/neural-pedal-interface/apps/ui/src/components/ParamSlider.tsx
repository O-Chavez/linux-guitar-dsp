import { type ChangeEvent } from 'react';

type ParamSliderProps = {
  label: string;
  value: number;
  min?: number;
  max?: number;
  step?: number;
  onChange: (value: number) => void;
  disabled?: boolean;
};

export function ParamSlider({
  label,
  value,
  min = 0,
  max = 1,
  step = 0.01,
  onChange,
  disabled = false,
}: ParamSliderProps) {
  const handleChange = (e: ChangeEvent<HTMLInputElement>) => {
    onChange(parseFloat(e.target.value));
  };

  // Format value display based on range
  const formatValue = (val: number) => {
    if (min < 0 && max <= 0) {
      // Threshold-style (negative values)
      return `${val.toFixed(0)} dB`;
    } else if (max > 1) {
      // Ratio-style
      return val.toFixed(1);
    } else {
      // Percentage-style (0-1)
      return `${Math.round(val * 100)}%`;
    }
  };

  return (
    <div className='param-slider'>
      <div className='param-label'>
        <span>{label}</span>
        <span className='param-value'>{formatValue(value)}</span>
      </div>
      <input
        type='range'
        min={min}
        max={max}
        step={step}
        value={value}
        onChange={handleChange}
        disabled={disabled}
        className='slider'
      />
    </div>
  );
}
