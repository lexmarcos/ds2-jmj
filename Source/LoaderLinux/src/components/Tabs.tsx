import type { ReactNode } from 'react'
import { Tabs as BaseTabs } from '@base-ui/react/tabs'
import { cn } from '@/lib/cn'

export interface TabItem<T extends string> {
  value: T
  label: string
}

export interface TabsProps<T extends string> {
  value: T
  onValueChange: (value: T) => void
  items: readonly TabItem<T>[]
  children: ReactNode
  className?: string
}

export function Tabs<T extends string>({
  value,
  onValueChange,
  items,
  children,
  className,
}: TabsProps<T>) {
  return (
    <BaseTabs.Root
      value={value}
      onValueChange={(next) => onValueChange(next as T)}
      className={cn('flex min-h-0 flex-col', className)}
    >
      <BaseTabs.List className="relative flex shrink-0 gap-1 border-b border-line px-4">
        {items.map((item) => (
          <BaseTabs.Tab
            key={item.value}
            value={item.value}
            className={cn(
              'cursor-pointer px-3 py-2.5 text-sm text-bone-dim',
              'transition-colors duration-150 ease-out',
              'hover:text-bone data-selected:text-bone',
            )}
          >
            {item.label}
          </BaseTabs.Tab>
        ))}
        <BaseTabs.Indicator className="absolute bottom-0 left-0 h-px w-(--active-tab-width) translate-x-(--active-tab-left) bg-ember transition-[translate,width] duration-200 ease-out" />
      </BaseTabs.List>
      {children}
    </BaseTabs.Root>
  )
}

export const TabPanel = BaseTabs.Panel
