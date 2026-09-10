import { Button as BaseButton } from '@base-ui/react/button'
import { cn } from '@/lib/cn'

export type ButtonVariant = 'primary' | 'default' | 'quiet' | 'danger'
export type ButtonSize = 'sm' | 'md'

export interface ButtonProps extends BaseButton.Props {
  variant?: ButtonVariant
  size?: ButtonSize
}

const base =
  'inline-flex items-center justify-center gap-2 rounded-sm border font-sans font-medium ' +
  'transition-colors duration-150 ease-out select-none ' +
  'disabled:cursor-not-allowed disabled:opacity-40'

const sizes: Record<ButtonSize, string> = {
  sm: 'h-7 px-3 text-xs',
  md: 'h-9 px-4 text-sm',
}

const variants: Record<ButtonVariant, string> = {
  // The launch action, and nothing else. Only one ember on screen at a time.
  primary:
    'border-ember/70 bg-ember/15 text-ember-bright ' +
    'hover:bg-ember/25 hover:border-ember active:bg-ember/30',
  default:
    'border-line-strong bg-raised text-bone ' +
    'hover:border-bone-faint hover:bg-raised/80 active:bg-panel',
  quiet:
    'border-transparent bg-transparent text-bone-dim ' +
    'hover:text-bone hover:bg-raised/60',
  danger:
    'border-invader/60 bg-invader/10 text-invader ' +
    'hover:bg-invader/20 hover:border-invader active:bg-invader/25',
}

export function Button({
  variant = 'default',
  size = 'md',
  className,
  ...props
}: ButtonProps) {
  return (
    <BaseButton
      {...props}
      className={cn(base, sizes[size], variants[variant], typeof className === 'string' ? className : undefined)}
    />
  )
}
