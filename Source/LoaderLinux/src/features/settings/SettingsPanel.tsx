import { NumberField, Rule, ScrollArea, Switch, TextField } from '@/components'
import type { LoaderSettings } from '@/lib/api'

export interface SettingsPanelProps {
  settings: LoaderSettings
  onChange: (settings: LoaderSettings) => void
}

export function SettingsPanel({ settings, onChange }: SettingsPanelProps) {
  const patch = (partial: Partial<LoaderSettings>) => onChange({ ...settings, ...partial })

  return (
    <ScrollArea className="flex-1">
      <div className="flex flex-col gap-6 px-4 py-5">
        <Group title="Dark Souls II">
          <Switch
            label="Remover o limite de tempo da sessão PvP"
            description="O jogo encerra sessões PvP em cerca de 12 minutos. Isso reescreve o cronômetro na memória do cliente."
            checked={settings.patchPhantomTimers}
            onCheckedChange={(patchPhantomTimers) => patch({ patchPhantomTimers })}
          />
          <NumberField
            label="Duração da sessão"
            unit="segundos"
            value={settings.phantomTimerSeconds}
            onValueChange={(phantomTimerSeconds) => patch({ phantomTimerSeconds })}
            min={60}
            max={100000}
            step={100}
            disabled={!settings.patchPhantomTimers}
          />
        </Group>

        <Rule strong />

        <Group title="Saves">
          <Switch
            label="Usar arquivos de save separados"
            description="Mantém o save do servidor privado longe do save da versão oficial."
            checked={settings.separateSaves}
            onCheckedChange={(separateSaves) => patch({ separateSaves })}
          />
        </Group>

        <Rule strong />

        <Group title="Injector">
          <TextField
            label="Pasta do injector"
            mono
            value={settings.injectorDir ?? ''}
            onValueChange={(value) => patch({ injectorDir: value.trim() === '' ? null : value })}
            placeholder="/home/você/Downloads/injector"
            description="Onde estão Injector.dll e Injector.exe. Baixe-os do artefato do workflow Injector for Linux. Vazio usa a pasta do próprio loader."
          />
        </Group>

        <Rule strong />

        <Group title="Rede">
          <TextField
            label="Master server"
            mono
            value={settings.masterServerUrl}
            onValueChange={(masterServerUrl) => patch({ masterServerUrl })}
            description="Onde a lista pública de servidores é buscada."
          />
        </Group>
      </div>
    </ScrollArea>
  )
}

function Group({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className="flex flex-col gap-4">
      <h3 className="font-serif text-sm text-bone-dim">{title}</h3>
      {children}
    </div>
  )
}
