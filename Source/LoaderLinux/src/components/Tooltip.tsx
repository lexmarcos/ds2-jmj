import type { ReactElement, ReactNode } from 'react'
import { Tooltip as BaseTooltip } from '@base-ui/react/tooltip'

export interface TooltipProps {
  content: ReactNode
  children: ReactElement
}

/** Wrap the app once in TooltipProvider, then use Tooltip anywhere below it. */
export const TooltipProvider = BaseTooltip.Provider

export function Tooltip({ content, children }: TooltipProps) {
  return (
    <BaseTooltip.Root>
      <BaseTooltip.Trigger render={children} />
      <BaseTooltip.Portal>
        <BaseTooltip.Positioner sideOffset={6}>
          <BaseTooltip.Popup className="max-w-72 rounded-sm border border-line-strong bg-raised px-2.5 py-1.5 text-xs text-bone shadow-lg shadow-black/50">
            {content}
          </BaseTooltip.Popup>
        </BaseTooltip.Positioner>
      </BaseTooltip.Portal>
    </BaseTooltip.Root>
  )
}
