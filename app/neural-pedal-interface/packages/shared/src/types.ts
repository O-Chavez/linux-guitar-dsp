/**
 * Core device state types
 */

// Component types in the signal chain
export type ComponentType = 'pedal' | 'amp' | 'cabinet';

// Pedal categories
export type PedalCategory =
  | 'overdrive'
  | 'distortion'
  | 'fuzz'
  | 'boost'
  | 'compressor'
  | 'delay'
  | 'reverb'
  | 'modulation'
  | 'eq';

// Base parameters all components share
export type BaseParams = {
  input?: number; // Input gain (0-1)
  output?: number; // Output level (0-1)
};

// EQ parameters
export type EQParams = {
  low?: number; // Bass (0-1)
  mid?: number; // Mids (0-1)
  high?: number; // Treble (0-1)
};

// Pedal-specific parameters
export type PedalParams = BaseParams &
  EQParams & {
    drive?: number; // Drive/gain amount
    tone?: number; // Tone control
    mix?: number; // Wet/dry mix
    [key: string]: number | undefined;
  };

// Amp-specific parameters
export type AmpParams = BaseParams &
  EQParams & {
    gain?: number; // Preamp gain
    master?: number; // Master volume
    presence?: number; // Presence control
    [key: string]: number | undefined;
  };

// Cabinet-specific parameters
export type CabinetParams = {
  output?: number; // Output level
  mix?: number; // Wet/dry mix for parallel cabs
  [key: string]: number | undefined;
};

// Pedal component
export type PedalComponent = {
  id: string;
  type: 'pedal';
  category: PedalCategory;
  model: string; // e.g., "TS808", "Klon Centaur"
  enabled: boolean;
  params: PedalParams;
  position: number; // Position in pedal chain (0, 1, 2...)
};

// Amp component
export type AmpComponent = {
  id: string;
  type: 'amp';
  model: string; // e.g., "Deluxe Reverb", "Marshall JCM800"
  enabled: boolean;
  params: AmpParams;
  includesCabinet: boolean; // Some amp captures include cabinet
};

// Cabinet component
export type CabinetComponent = {
  id: string;
  type: 'cabinet';
  model: string; // IR file name
  enabled: boolean;
  params: CabinetParams;
};

// Union type for any component
export type SignalComponent = PedalComponent | AmpComponent | CabinetComponent;

// Device state representing the full signal chain
export type DeviceState = {
  presetId: string;
  presetName: string;
  pedals: PedalComponent[]; // Ordered array of pedals
  amp: AmpComponent | null; // Single amp
  cabinet: CabinetComponent | null; // Single cabinet
  updatedAt: number;
};

/**
 * Available models/captures library
 */
export type ModelLibrary = {
  pedals: Array<{
    id: string;
    name: string;
    category: PedalCategory;
    defaultParams: PedalParams;
  }>;
  amps: Array<{
    id: string;
    name: string;
    includesCabinet: boolean;
    defaultParams: AmpParams;
  }>;
  cabinets: Array<{
    id: string;
    name: string;
    irFile: string;
    defaultParams: CabinetParams;
  }>;
};

/**
 * WebSocket message types
 */
export type ServerMsg =
  | { type: 'state'; state: DeviceState }
  | { type: 'error'; message: string }
  | { type: 'library'; library: ModelLibrary };

export type ClientMsg =
  | { type: 'hello'; clientId: string }
  // Component management
  | { type: 'addPedal'; modelId: string; position?: number }
  | { type: 'removePedal'; pedalId: string }
  | { type: 'reorderPedal'; pedalId: string; newPosition: number }
  | { type: 'setAmp'; modelId: string }
  | { type: 'removeAmp' }
  | { type: 'setCabinet'; modelId: string }
  | { type: 'removeCabinet' }
  // Parameter changes
  | {
      type: 'setComponentParam';
      componentId: string;
      key: string;
      value: number;
    }
  | { type: 'toggleComponent'; componentId: string; enabled: boolean }
  // Preset management
  | { type: 'loadPreset'; presetId: string }
  | { type: 'savePreset'; presetId: string; presetName: string }
  | { type: 'deletePreset'; presetId: string };

/**
 * Preset file format
 */
export type PresetFile = {
  id: string;
  name: string;
  pedals: PedalComponent[];
  amp: AmpComponent | null;
  cabinet: CabinetComponent | null;
};
