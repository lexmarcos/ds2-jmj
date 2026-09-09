import { invoke } from '@tauri-apps/api/core'

export type GameType = 'DarkSouls2' | 'DarkSouls3'

export interface ServerEntry {
  id: string
  name: string
  description: string
  hostname: string
  port: number
  playerCount: number
  gameType: GameType
  passwordRequired: boolean
  manualImport: boolean
}

/** What the loader found on this machine for one game. */
export interface GameDetection {
  appId: number
  gameType: GameType
  /** Steam library path holding the game, if it is installed. */
  installDir: string | null
  /** Proton prefix (compatdata/<appid>/pfx), if Steam has created one. */
  prefixPath: string | null
  /** Proton build Steam is configured to use, if it can be determined. */
  protonName: string | null
}

export interface LoaderSettings {
  masterServerUrl: string
  separateSaves: boolean
  patchPhantomTimers: boolean
  phantomTimerSeconds: number
}

export interface LaunchPlan {
  /** Wrapper script the user pastes into the game's Steam launch options. */
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
    return { ok: true, value: (await invoke<T>(command, args)) }
  } catch (error) {
    return { ok: false, error: error instanceof Error ? error.message : String(error) }
  }
}

export const api = {
  detectGame: (gameType: GameType) => call<GameDetection>('detect_game', { gameType }),
  listServers: () => call<ServerEntry[]>('list_servers'),
  loadSettings: () => call<LoaderSettings>('load_settings'),
  saveSettings: (settings: LoaderSettings) => call<void>('save_settings', { settings }),
  prepareLaunch: (serverId: string) => call<LaunchPlan>('prepare_launch', { serverId }),
}
