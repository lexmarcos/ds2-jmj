import type { ReactNode } from 'react'
import { Dialog as BaseDialog } from '@base-ui/react/dialog'

export interface DialogProps {
  open: boolean
  onOpenChange: (open: boolean) => void
  title: string
  description?: string
  children: ReactNode
  /** Buttons for the bottom bar. Put the confirming action last. */
  actions?: ReactNode
}

export function Dialog({
  open,
  onOpenChange,
  title,
  description,
  children,
  actions,
}: DialogProps) {
  return (
    <BaseDialog.Root open={open} onOpenChange={onOpenChange}>
      <BaseDialog.Portal>
        <BaseDialog.Backdrop className="fixed inset-0 z-40 bg-void/80" />
        <BaseDialog.Popup className="fixed top-1/2 left-1/2 z-50 flex max-h-[85vh] w-[min(34rem,92vw)] -translate-x-1/2 -translate-y-1/2 flex-col rounded-md border border-line-strong bg-panel shadow-2xl shadow-black/60">
          <header className="border-b border-line px-5 py-4">
            <BaseDialog.Title className="font-serif text-md font-medium text-bone">
              {title}
            </BaseDialog.Title>
            {description ? (
              <BaseDialog.Description className="mt-1 text-xs text-bone-dim">
                {description}
              </BaseDialog.Description>
            ) : null}
          </header>
          <div className="flex-1 overflow-y-auto px-5 py-4">{children}</div>
          {actions ? (
            <footer className="flex justify-end gap-2 border-t border-line px-5 py-3">
              {actions}
            </footer>
          ) : null}
        </BaseDialog.Popup>
      </BaseDialog.Portal>
    </BaseDialog.Root>
  )
}

export const DialogClose = BaseDialog.Close
