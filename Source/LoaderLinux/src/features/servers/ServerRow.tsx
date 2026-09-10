import { Status } from '@/components'
import { cn } from '@/lib/cn'
import type { ServerEntry } from '@/lib/api'

export interface ServerRowProps {
  server: ServerEntry
  selected: boolean
  onSelect: (id: string) => void
}

/**
 * One server, drawn as a summon sign on the ground: quiet until the firelight
 * catches it. Selection is the only thing that lights the left edge.
 */
export function ServerRow({ server, selected, onSelect }: ServerRowProps) {
  return (
    <button
      type="button"
      aria-pressed={selected}
      onClick={() => onSelect(server.id)}
      className={cn(
        'group relative flex w-full items-start gap-4 px-4 py-2.5 text-left',
        'transition-colors duration-150 ease-out',
        selected ? 'bg-ember/12' : 'hover:bg-raised/50',
      )}
    >
      <span
        aria-hidden
        className={cn(
          'absolute inset-y-0 left-0 w-[3px] transition-colors duration-150 ease-out',
          selected ? 'bg-ember' : 'bg-transparent group-hover:bg-line-strong',
        )}
      />
      <span className="flex min-w-0 flex-1 flex-col">
        <span
          className={cn(
            'truncate font-serif text-md leading-snug',
            selected ? 'text-ember-bright' : 'text-bone',
          )}
        >
          {server.name}
        </span>
        {server.description ? (
          <span className="truncate font-serif text-sm leading-snug text-bone-dim">{server.description}</span>
        ) : null}
        <span className="data">
          {server.hostname}:{server.port}
        </span>
      </span>
      <span className="flex shrink-0 flex-col items-end gap-1 pt-0.5">
        <Status tone={server.playerCount > 0 ? 'ok' : 'idle'}>
          {server.playerCount === 1 ? '1 jogador' : `${server.playerCount} jogadores`}
        </Status>
        {server.passwordRequired ? (
          <Status tone="info">com senha</Status>
        ) : null}
        {server.manualImport ? <Status tone="idle">importado</Status> : null}
      </span>
    </button>
  )
}
