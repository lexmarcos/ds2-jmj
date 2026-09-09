import type { ReactNode } from 'react'
import { cn } from '@/lib/cn'

export interface PanelProps {
  children: ReactNode
  className?: string
  /** Lifts the surface for content that sits on top of another panel. */
  raised?: boolean
}

export function Panel({ children, className, raised = false }: PanelProps) {
  return (
    <section
      className={cn(
        'relative rounded-md border border-line',
        raised ? 'bg-raised' : 'bg-panel',
        className,
      )}
    >
      {children}
    </section>
  )
}

export interface PanelHeadingProps {
  children: ReactNode
  /** Right-hand slot for counts, status or a single action. */
  aside?: ReactNode
}

export function PanelHeading({ children, aside }: PanelHeadingProps) {
  return (
    <header className="flex items-baseline justify-between gap-4 border-b border-line px-4 py-3">
      <h2 className="font-serif text-md font-medium text-bone">{children}</h2>
      {aside ? <div className="shrink-0 text-xs text-bone-dim">{aside}</div> : null}
    </header>
  )
}
