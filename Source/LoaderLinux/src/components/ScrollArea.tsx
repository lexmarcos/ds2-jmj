import type { ReactNode } from 'react'
import { ScrollArea as BaseScrollArea } from '@base-ui/react/scroll-area'
import { cn } from '@/lib/cn'

export interface ScrollAreaProps {
  children: ReactNode
  className?: string
}

export function ScrollArea({ children, className }: ScrollAreaProps) {
  return (
    <BaseScrollArea.Root className={cn('relative min-h-0', className)}>
      <BaseScrollArea.Viewport className="h-full w-full overscroll-contain">
        <BaseScrollArea.Content>{children}</BaseScrollArea.Content>
      </BaseScrollArea.Viewport>
      <BaseScrollArea.Scrollbar
        orientation="vertical"
        className="flex w-2 justify-center py-1 opacity-0 transition-opacity duration-150 data-hovering:opacity-100 data-scrolling:opacity-100"
      >
        <BaseScrollArea.Thumb className="w-1 rounded-full bg-line-strong" />
      </BaseScrollArea.Scrollbar>
    </BaseScrollArea.Root>
  )
}
