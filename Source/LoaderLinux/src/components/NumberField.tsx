import { NumberField as BaseNumberField } from '@base-ui/react/number-field'
import { cn } from '@/lib/cn'

export interface NumberFieldProps {
  label: string
  value: number
  onValueChange: (value: number) => void
  min?: number
  max?: number
  step?: number
  unit?: string
  description?: string
  disabled?: boolean
  className?: string
}

const stepper =
  'flex h-full w-7 items-center justify-center text-bone-dim ' +
  'transition-colors duration-150 ease-out ' +
  'hover:bg-raised hover:text-bone disabled:opacity-30'

export function NumberField({
  label,
  value,
  onValueChange,
  min,
  max,
  step = 1,
  unit,
  description,
  disabled = false,
  className,
}: NumberFieldProps) {
  return (
    <BaseNumberField.Root
      value={value}
      onValueChange={(next) => {
        if (next !== null) onValueChange(next)
      }}
      min={min}
      max={max}
      step={step}
      disabled={disabled}
      className={cn('flex flex-col gap-1.5', className)}
    >
      <div className="flex items-baseline gap-2">
        <label className="text-xs text-bone-dim">{label}</label>
        {unit ? <span className="text-xs text-bone-faint">{unit}</span> : null}
      </div>
      <BaseNumberField.Group
        className={cn(
          'flex h-9 w-40 items-stretch overflow-hidden rounded-sm border border-line-strong bg-void',
          'transition-colors duration-150 ease-out',
          'hover:border-bone-faint focus-within:border-ember',
          disabled && 'opacity-40',
        )}
      >
        <BaseNumberField.Decrement className={cn(stepper, 'border-r border-line')}>
          &minus;
        </BaseNumberField.Decrement>
        <BaseNumberField.Input className="w-full min-w-0 bg-transparent px-3 text-center font-mono text-sm text-bone focus:outline-none" />
        <BaseNumberField.Increment className={cn(stepper, 'border-l border-line')}>
          +
        </BaseNumberField.Increment>
      </BaseNumberField.Group>
      {description ? <p className="text-xs text-bone-faint">{description}</p> : null}
    </BaseNumberField.Root>
  )
}
