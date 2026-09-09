import { Field } from '@base-ui/react/field'
import { cn } from '@/lib/cn'

export interface TextAreaProps {
  label: string
  value: string
  onValueChange: (value: string) => void
  placeholder?: string
  description?: string
  error?: string
  rows?: number
  disabled?: boolean
  /** Renders the value in the mono face. For keys, paths and other data. */
  mono?: boolean
  className?: string
}

/**
 * Multi-line text. Use this for anything that legitimately contains newlines,
 * such as a PEM key: a single-line input silently flattens them to spaces when
 * the value is pasted, which corrupts the content without telling anyone.
 */
export function TextArea({
  label,
  value,
  onValueChange,
  placeholder,
  description,
  error,
  rows = 6,
  disabled = false,
  mono = false,
  className,
}: TextAreaProps) {
  return (
    <Field.Root className={cn('flex flex-col gap-1.5', className)} disabled={disabled}>
      <Field.Label className="text-xs text-bone-dim">{label}</Field.Label>
      <Field.Control
        render={
          <textarea
            rows={rows}
            spellCheck={false}
            placeholder={placeholder}
            value={value}
            onChange={(event) => onValueChange(event.currentTarget.value)}
            className={cn(
              'w-full resize-y rounded-sm border border-line-strong bg-void px-3 py-2 text-sm text-bone',
              'placeholder:text-bone-faint',
              'transition-colors duration-150 ease-out',
              'hover:border-bone-faint focus:border-ember focus:outline-none',
              'disabled:cursor-not-allowed disabled:opacity-40',
              error && 'border-invader',
              mono && 'font-mono text-xs leading-relaxed',
            )}
          />
        }
      />
      {description && !error ? (
        <Field.Description className="text-xs text-bone-faint">{description}</Field.Description>
      ) : null}
      {error ? <p className="text-xs text-invader">{error}</p> : null}
    </Field.Root>
  )
}
