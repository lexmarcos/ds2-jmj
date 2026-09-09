import { cn } from '@/lib/cn'

export interface RuleProps {
  className?: string
  /** Marks a rule that separates sections rather than sibling rows. */
  strong?: boolean
}

/**
 * A hairline. Structure in this interface is carried by rules rather than by
 * cards, so these do real work and should not be added for decoration.
 */
export function Rule({ className, strong = false }: RuleProps) {
  return (
    <hr
      role="separator"
      className={cn('h-px w-full border-0', strong ? 'bg-line-strong' : 'bg-line', className)}
    />
  )
}
