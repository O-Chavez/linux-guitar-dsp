type BypassToggleProps = {
  enabled: boolean;
  onToggle: (enabled: boolean) => void;
  disabled?: boolean;
};

export function BypassToggle({
  enabled,
  onToggle,
  disabled = false,
}: BypassToggleProps) {
  return (
    <button
      onClick={() => onToggle(!enabled)}
      disabled={disabled}
      className={`bypass-toggle ${enabled ? 'enabled' : 'bypassed'}`}
      aria-label={enabled ? 'Bypass effect' : 'Enable effect'}
      title={enabled ? 'Active' : 'Bypassed'}
    />
  );
}
