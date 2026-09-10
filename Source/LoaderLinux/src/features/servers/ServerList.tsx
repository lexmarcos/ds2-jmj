import { Button, Rule, ScrollArea } from '@/components'
import type { ServerEntry } from '@/lib/api'
import { ServerRow } from './ServerRow'

export interface ServerListProps {
  servers: ServerEntry[]
  selectedId: string | null
  onSelect: (id: string) => void
  onImport: () => void
  loading: boolean
  error: string | null
}

export function ServerList({
  servers,
  selectedId,
  onSelect,
  onImport,
  loading,
  error,
}: ServerListProps) {
  if (loading) {
    return <Placeholder>Procurando servidores.</Placeholder>
  }

  if (error) {
    return (
      <Placeholder tone="error" action={<Button onClick={onImport}>Importar servidor</Button>}>
        {error}
      </Placeholder>
    )
  }

  if (servers.length === 0) {
    return (
      <Placeholder action={<Button onClick={onImport}>Importar servidor</Button>}>
        Nenhum servidor à vista. Importe o seu com o endereço e a chave pública.
      </Placeholder>
    )
  }

  return (
    <ScrollArea className="flex-1">
      {servers.map((server, index) => (
        <div key={server.id}>
          {index > 0 ? <Rule /> : null}
          <ServerRow
            server={server}
            selected={server.id === selectedId}
            onSelect={onSelect}
          />
        </div>
      ))}
    </ScrollArea>
  )
}

function Placeholder({
  children,
  action,
  tone = 'idle',
}: {
  children: React.ReactNode
  action?: React.ReactNode
  tone?: 'idle' | 'error'
}) {
  return (
    <div className="flex flex-1 flex-col items-center justify-center gap-4 px-8 text-center">
      <p
        className={
          tone === 'error'
            ? 'max-w-80 font-serif text-sm text-invader'
            : 'max-w-80 font-serif text-sm text-bone-dim'
        }
      >
        {children}
      </p>
      {action}
    </div>
  )
}
