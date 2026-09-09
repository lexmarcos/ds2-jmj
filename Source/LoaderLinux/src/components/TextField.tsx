import { Field } from '@base-ui/react/field'
import { Input } from '@base-ui/react/input'
import { cn } from '@/lib/cn'

export interface TextFieldProps {
  label: string
  value: string
  onValueChange: (value: string) => void
  placeholder?: string
  description?: string
  error?: string
  disabled?: boolean
  /** Renders the value in the mono face. For paths, hosts and addresses. */
  mono?: boolean
  className?: string
}

export function TextField({
  label,
  value,
  onValueChange,
  placeholder,
  description,
  error,
  disabled = false,
  mono = false,
  className,
}: TextFieldProps) {
  return (
    <Field.Root className={cn('flex flex-col gap-1.5', className)} disabled={disabled}>
      <Field.Label className="text-xs text-bone-dim">{label}</Field.Label>
      <Input
        value={value}
        placeholder={placeholder}
        onChange={(event) => onValueChange(event.currentTarget.value)}
        className={cn(
          'h-9 w-full rounded-sm border border-line-strong bg-void px-3 text-sm text-bone',
          'placeholder:text-bone-faint',
          'transition-colors duration-150 ease-out',
          'hover:border-bone-faint focus:border-ember focus:outline-none',
          'disabled:cursor-not-allowed disabled:opacity-40',
          error && 'border-invader',
          mono && 'font-mono text-xs',
        )}
      />
      {description && !error ? (
        <Field.Description className="text-xs text-bone-faint">{description}</Field.Description>
      ) : null}
      {error ? <p className="text-xs text-invader">{error}</p> : null}
    </Field.Root>
  )
}
