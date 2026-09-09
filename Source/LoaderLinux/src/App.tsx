import { useCallback, useEffect, useState } from 'react'
import {
  Button,
  Dialog,
  Rule,
  Status,
  TabPanel,
  Tabs,
  Tooltip,
  TooltipProvider,
} from '@/components'
import { ServerList } from '@/features/servers/ServerList'
import { SettingsPanel } from '@/features/settings/SettingsPanel'
import { api, type GameDetection, type LaunchPlan, type LoaderSettings, type ServerEntry } from '@/lib/api'

type Section = 'servers' | 'settings'

const SECTIONS = [
  { value: 'servers', label: 'Servidores' },
  { value: 'settings', label: 'Ajustes' },
] as const satisfies readonly { value: Section; label: string }[]

const DEFAULT_SETTINGS: LoaderSettings = {
  masterServerUrl: 'http://ds3os-master.timleonard.uk:50020',
  separateSaves: true,
  patchPhantomTimers: false,
  phantomTimerSeconds: 4000,
}

export function App() {
  const [section, setSection] = useState<Section>('servers')
  const [servers, setServers] = useState<ServerEntry[]>([])
  const [selectedId, setSelectedId] = useState<string | null>(null)
  const [settings, setSettings] = useState<LoaderSettings>(DEFAULT_SETTINGS)
  const [detection, setDetection] = useState<GameDetection | null>(null)
  const [loading, setLoading] = useState(true)
  const [listError, setListError] = useState<string | null>(null)
  const [plan, setPlan] = useState<LaunchPlan | null>(null)

  useEffect(() => {
    let cancelled = false

    void (async () => {
      const [serverResult, settingsResult, detectionResult] = await Promise.all([
        api.listServers(),
        api.loadSettings(),
        api.detectGame('DarkSouls2'),
      ])
      if (cancelled) return

      if (serverResult.ok) {
        setServers(serverResult.value)
        setListError(null)
      } else {
        setListError(serverResult.error)
      }
      if (settingsResult.ok) setSettings(settingsResult.value)
      if (detectionResult.ok) setDetection(detectionResult.value)
      setLoading(false)
    })()

    return () => {
      cancelled = true
    }
  }, [])

  const updateSettings = useCallback((next: LoaderSettings) => {
    setSettings(next)
    void api.saveSettings(next)
  }, [])

  const launch = useCallback(async () => {
    if (!selectedId) return
    const result = await api.prepareLaunch(selectedId)
    if (result.ok) setPlan(result.value)
    else setListError(result.error)
  }, [selectedId])

  const ready = detection?.installDir != null && detection.prefixPath != null

  return (
    <TooltipProvider>
      <div className="relative z-10 flex h-full flex-col">
        <TopBar detection={detection} />
        <Rule />

        <Tabs
          value={section}
          onValueChange={setSection}
          items={SECTIONS}
          className="min-h-0 flex-1"
        >
          <TabPanel value="servers" className="flex min-h-0 flex-1 flex-col">
            <ServerList
              servers={servers}
              selectedId={selectedId}
              onSelect={setSelectedId}
              onImport={() => setSection('settings')}
              loading={loading}
              error={listError}
            />
          </TabPanel>
          <TabPanel value="settings" className="flex min-h-0 flex-1 flex-col">
            <SettingsPanel settings={settings} onChange={updateSettings} />
          </TabPanel>
        </Tabs>

        <Rule />
        <LaunchBar detection={detection} ready={ready} canLaunch={selectedId !== null} onLaunch={launch} />
      </div>

      <Dialog
        open={plan !== null}
        onOpenChange={(open) => {
          if (!open) setPlan(null)
        }}
        title="Configure as opções de lançamento"
        description="A Steam precisa iniciar o jogo através deste wrapper para que o injector rode dentro do prefixo."
        actions={<Button onClick={() => setPlan(null)}>Fechar</Button>}
      >
        <div className="flex flex-col gap-4">
          <p className="font-serif text-sm text-bone-dim">
            Abra as propriedades do Dark Souls II na Steam e cole a linha abaixo em
            Opções de Lançamento.
          </p>
          <pre className="data overflow-x-auto rounded-sm border border-line-strong bg-void p-3 whitespace-pre">
            {plan?.launchOptions}
          </pre>
          <p className="text-xs text-bone-faint">
            Escrito em <span className="data">{plan?.scriptPath}</span>
          </p>
        </div>
      </Dialog>
    </TooltipProvider>
  )
}

function TopBar({ detection }: { detection: GameDetection | null }) {
  return (
    <header className="flex shrink-0 items-center justify-between px-4 py-3">
      <span className="font-serif text-md text-bone">ds2os</span>
      {detection === null ? (
        <Status tone="idle">procurando o jogo</Status>
      ) : detection.installDir === null ? (
        <Status tone="error">Dark Souls II não encontrado na Steam</Status>
      ) : detection.prefixPath === null ? (
        <Tooltip content="Rode o jogo uma vez pela Steam para o Proton criar o prefixo.">
          <span>
            <Status tone="pending">sem prefixo Proton</Status>
          </span>
        </Tooltip>
      ) : (
        <Status tone="ok">Dark Souls II pronto</Status>
      )}
    </header>
  )
}

function LaunchBar({
  detection,
  ready,
  canLaunch,
  onLaunch,
}: {
  detection: GameDetection | null
  ready: boolean
  canLaunch: boolean
  onLaunch: () => void
}) {
  return (
    <footer className="flex shrink-0 items-center justify-between gap-4 px-4 py-3">
      <span className="data min-w-0 truncate">
        {detection?.protonName ?? 'Proton não identificado'}
      </span>
      <Button variant="primary" disabled={!ready || !canLaunch} onClick={onLaunch}>
        Preparar lançamento
      </Button>
    </footer>
  )
}
