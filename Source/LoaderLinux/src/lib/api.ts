import { invoke } from '@tauri-apps/api/core'

export type GameType = 'DarkSouls2' | 'DarkSouls3'

/**
 * Mirrors `ServerEntry` in `crates/ds2os-core/src/master.rs`. Change the two
 * together. `gameType` is a plain string because the master server is free to
 * send anything.
 */
export interface ServerEntry {
  id: string
  name: string
  description: string
  hostname: string
  privateHostname: string
  ipAddress: string
  port: number
  publicKey: string
  playerCount: number
  passwordRequired: boolean
  gameType: string
  manualImport: boolean
}

/** What the loader found on this machine for one game. */
export interface GameDetection {
  appId: number
  gameType: GameType
  installDir: string | null
  prefixPath: string | null
  protonName: string | null
}

export interface LoaderSettings {
  masterServerUrl: string
  separateSaves: boolean
  patchPhantomTimers: boolean
  phantomTimerSeconds: number
  injectorDir: string | null
}

export interface LaunchPlan {
  /** Wrapper script line the user pastes into the game's Steam launch options. */
  launchOptions: string
  scriptPath: string
  injectorConfigPath: string
}

/**
 * Every backend call can fail for reasons the user needs to read (no Steam, no
 * prefix, master server down), so failures are values rather than exceptions.
 */
export type Result<T> = { ok: true; value: T } | { ok: false; error: string }

async function call<T>(command: string, args?: Record<string, unknown>): Promise<Result<T>> {
  try {
    return { ok: true, value: await invoke<T>(command, args) }
  } catch (error) {
    return { ok: false, error: error instanceof Error ? error.message : String(error) }
  }
}

export const api = {
  detectGame: (gameType: GameType) => call<GameDetection>('detect_game', { gameType }),
  listServers: () => call<ServerEntry[]>('list_servers'),
  importServer: (server: ServerEntry) => call<ServerEntry[]>('import_server', { server }),
  forgetServer: (serverId: string) => call<ServerEntry[]>('forget_server', { serverId }),
  loadSettings: () => call<LoaderSettings>('load_settings'),
  saveSettings: (settings: LoaderSettings) => call<void>('save_settings', { settings }),
  prepareLaunch: (serverId: string, password?: string) =>
    call<LaunchPlan>('prepare_launch', { serverId, password: password ?? null }),
}

/** A blank server, ready for the import form to fill in. */
export function emptyServer(): ServerEntry {
  return {
    id: '',
    name: '',
    description: '',
    hostname: '',
    privateHostname: '',
    ipAddress: '',
    port: 50050,
    publicKey: '',
    playerCount: 0,
    passwordRequired: false,
    gameType: 'DarkSouls2',
    manualImport: true,
  }
}
