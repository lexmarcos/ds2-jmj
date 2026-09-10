import { useState } from 'react'
import { Button, Dialog, NumberField, Select, TextArea, TextField } from '@/components'
import { emptyServer, type GameType, type ServerEntry } from '@/lib/api'

const GAMES = [
  { value: 'DarkSouls2', label: 'Dark Souls II' },
  { value: 'DarkSouls3', label: 'Dark Souls III' },
] as const satisfies readonly { value: GameType; label: string }[]

export interface ImportServerDialogProps {
  open: boolean
  onOpenChange: (open: boolean) => void
  onImport: (server: ServerEntry) => Promise<string | null>
}

/**
 * For servers the master server does not list: the user has the address and the
 * public key, but no config file to point at.
 */
export function ImportServerDialog({ open, onOpenChange, onImport }: ImportServerDialogProps) {
  const [draft, setDraft] = useState<ServerEntry>(emptyServer)
  const [error, setError] = useState<string | null>(null)
  const [saving, setSaving] = useState(false)

  const patch = (partial: Partial<ServerEntry>) => setDraft({ ...draft, ...partial })

  const submit = async () => {
    setSaving(true)
    const failure = await onImport({
      ...draft,
      privateHostname: draft.hostname,
      ipAddress: draft.hostname,
    })
    setSaving(false)

    if (failure) {
      setError(failure)
      return
    }
    setDraft(emptyServer())
    setError(null)
    onOpenChange(false)
  }

  return (
    <Dialog
      open={open}
      onOpenChange={onOpenChange}
      title="Importar servidor"
      description="Use isso para um servidor que não aparece na lista pública."
      actions={
        <>
          <Button variant="quiet" onClick={() => onOpenChange(false)}>
            Cancelar
          </Button>
          <Button variant="primary" onClick={submit} disabled={saving}>
            {saving ? 'Salvando' : 'Salvar servidor'}
          </Button>
        </>
      }
    >
      <div className="flex flex-col gap-4">
        <TextField
          label="Nome"
          value={draft.name}
          onValueChange={(name) => patch({ name })}
          placeholder="Servidor do grupo"
        />
        <TextField
          label="Endereço"
          mono
          value={draft.hostname}
          onValueChange={(hostname) => patch({ hostname })}
          placeholder="192.168.0.10"
        />
        <div className="flex gap-4">
          <NumberField
            label="Porta"
            value={draft.port}
            onValueChange={(port) => patch({ port })}
            min={1}
            max={65535}
          />
          <Select
            label="Jogo"
            className="flex-1"
            value={draft.gameType as GameType}
            onValueChange={(gameType) => patch({ gameType })}
            options={GAMES}
          />
        </div>
        <TextArea
          label="Chave pública"
          mono
          rows={7}
          value={draft.publicKey}
          onValueChange={(publicKey) => patch({ publicKey })}
          placeholder="-----BEGIN RSA PUBLIC KEY-----"
          description="Cole o conteúdo do arquivo public.key do servidor, com as quebras de linha."
          {...(error ? { error } : {})}
        />
      </div>
    </Dialog>
  )
}
