import { Select as BaseSelect } from '@base-ui/react/select'
import { cn } from '@/lib/cn'

export interface SelectOption<T extends string> {
  value: T
  label: string
}

export interface SelectProps<T extends string> {
  label: string
  value: T
  onValueChange: (value: T) => void
  options: readonly SelectOption<T>[]
  disabled?: boolean
  className?: string
}

export function Select<T extends string>({
  label,
  value,
  onValueChange,
  options,
  disabled = false,
  className,
}: SelectProps<T>) {
  return (
    <div className={cn('flex flex-col gap-1.5', className)}>
      <BaseSelect.Root
        value={value}
        onValueChange={(next) => onValueChange(next as T)}
        disabled={disabled}
      >
        <BaseSelect.Label className="text-xs text-bone-dim">{label}</BaseSelect.Label>
        <BaseSelect.Trigger
          className={cn(
            'flex h-9 w-full items-center justify-between gap-2 rounded-sm',
            'border border-line-strong bg-void px-3 text-sm text-bone',
            'transition-colors duration-150 ease-out',
            'hover:border-bone-faint focus:border-ember focus:outline-none',
            'disabled:cursor-not-allowed disabled:opacity-40',
          )}
        >
          <BaseSelect.Value />
          <BaseSelect.Icon className="text-bone-faint">&#9662;</BaseSelect.Icon>
        </BaseSelect.Trigger>
        <BaseSelect.Portal>
          <BaseSelect.Positioner sideOffset={4} alignItemWithTrigger={false}>
            <BaseSelect.Popup className="min-w-(--anchor-width) overflow-hidden rounded-md border border-line-strong bg-raised py-1 shadow-lg shadow-black/50">
              <BaseSelect.List>
                {options.map((option) => (
                  <BaseSelect.Item
                    key={option.value}
                    value={option.value}
                    className={cn(
                      'flex cursor-pointer items-center gap-2 px-3 py-1.5 text-sm text-bone-dim',
                      'data-highlighted:bg-panel data-highlighted:text-bone',
                      'data-selected:text-ember',
                    )}
                  >
                    <BaseSelect.ItemIndicator className="w-2 text-ember">
                      &bull;
                    </BaseSelect.ItemIndicator>
                    <BaseSelect.ItemText>{option.label}</BaseSelect.ItemText>
                  </BaseSelect.Item>
                ))}
              </BaseSelect.List>
            </BaseSelect.Popup>
          </BaseSelect.Positioner>
        </BaseSelect.Portal>
      </BaseSelect.Root>
    </div>
  )
}
