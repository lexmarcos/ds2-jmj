import { Switch as BaseSwitch } from '@base-ui/react/switch'
import { cn } from '@/lib/cn'

export interface SwitchProps {
  label: string
  checked: boolean
  onCheckedChange: (checked: boolean) => void
  description?: string
  disabled?: boolean
  className?: string
}

export function Switch({
  label,
  checked,
  onCheckedChange,
  description,
  disabled = false,
  className,
}: SwitchProps) {
  return (
    <label
      className={cn(
        'flex cursor-pointer items-start gap-3',
        disabled && 'cursor-not-allowed opacity-40',
        className,
      )}
    >
      <BaseSwitch.Root
        checked={checked}
        onCheckedChange={onCheckedChange}
        disabled={disabled}
        className={cn(
          'mt-0.5 h-5 w-9 shrink-0 rounded-full border p-0.5',
          'transition-colors duration-150 ease-out',
          checked ? 'border-ember bg-ember/25' : 'border-line-strong bg-void',
        )}
      >
        <BaseSwitch.Thumb
          className={cn(
            'block size-3.5 rounded-full',
            'transition-[translate,background-color] duration-150 ease-out',
            checked ? 'translate-x-4 bg-ember-bright' : 'translate-x-0 bg-bone-faint',
          )}
        />
      </BaseSwitch.Root>
      <span className="flex flex-col gap-0.5">
        <span className="text-sm text-bone">{label}</span>
        {description ? <span className="text-xs text-bone-faint">{description}</span> : null}
      </span>
    </label>
  )
}
