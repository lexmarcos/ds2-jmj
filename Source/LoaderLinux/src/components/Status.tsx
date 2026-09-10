import type { ReactNode } from 'react'
import { cn } from '@/lib/cn'

/**
 * Phantom colours carry their meaning from the game: white is co-operation and
 * a healthy state, red is an invader and a failure, blue is information. Pick
 * the tone for what the state means, never for how it looks.
 */
export type StatusTone = 'ok' | 'pending' | 'error' | 'info' | 'idle'

const dot: Record<StatusTone, string> = {
  ok: 'bg-coop',
  pending: 'bg-ember',
  error: 'bg-invader',
  info: 'bg-sentinel',
  idle: 'bg-bone-faint',
}

const text: Record<StatusTone, string> = {
  ok: 'text-bone',
  pending: 'text-ember',
  error: 'text-invader',
  info: 'text-sentinel',
  idle: 'text-bone-dim',
}

export interface StatusProps {
  tone: StatusTone
  children: ReactNode
  className?: string
}

export function Status({ tone, children, className }: StatusProps) {
  return (
    <span className={cn('inline-flex items-center gap-2 text-xs', text[tone], className)}>
      <span aria-hidden className={cn('size-1.5 shrink-0 rounded-full', dot[tone])} />
      {children}
    </span>
  )
}
