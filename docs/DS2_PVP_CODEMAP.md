# DS2 PvP Codemap

Analise estatica do clone local `ds2-jmj`, feita para orientar um MVP privado de Dark Souls II: Scholar of the First Sin usando DSOS como base.

Escopo aplicado:

- Nao implementar patch neste passo.
- Nao adicionar codigo para servidores oficiais.
- Nao usar offsets magicos.
- Nao modificar saves retail.
- Manter o objetivo de servidor privado, sem matchmaking publico, sem Steam emulator e sem mistura intencional de jogadores vanilla com modded.

## Estado local observado

O worktree ja tinha alteracoes locais antes deste relatorio, principalmente na build/layout e no Loader:

- `Source/CMakeLists.txt`
- `Source/Injector/CMakeLists.txt`
- `Source/Loader/CMakeLists.txt`
- `Source/Loader/Directory.Build.props`
- `Source/Loader/Forms/MainForm.Designer.cs`
- `Source/Loader/Forms/MainForm.cs`
- `Source/Loader/Loader.csproj`
- `Source/Server/CMakeLists.txt`
- `Source/ThirdParty/steam/CMakeLists.txt`
- `Tools/generate_package_windows.bat`
- `Source/Loader/Forms/ImportServerDialog.Designer.cs`
- `Source/Loader/Forms/ImportServerDialog.cs`
- `global.json`

Este documento e a unica escrita feita por esta etapa.

## Como compilar

O README indica Visual Studio 2022, C++17, CMake e os scripts em `Tools/`. O CMake raiz inclui `Source/`, e `Tools/generate_vs2022.bat` gera a solucao em `intermediate/vs2022`.

Comandos exatos, a partir do repo:

```powershell
cd C:\Users\suel\projects\ds2-jmj
.\Tools\generate_vs2022.bat
.\Tools\Build\cmake\windows\bin\cmake.exe --build .\intermediate\vs2022 --config Release --target ALL_BUILD
```

Para targets separados:

```powershell
cd C:\Users\suel\projects\ds2-jmj
.\Tools\generate_vs2022.bat
.\Tools\Build\cmake\windows\bin\cmake.exe --build .\intermediate\vs2022 --config Release --target Server
.\Tools\Build\cmake\windows\bin\cmake.exe --build .\intermediate\vs2022 --config Release --target Loader
```

Prerequisitos observados no estado atual:

- Visual Studio 2022 com toolchain C++.
- .NET SDK `10.0.100` ou compativel, por causa de `global.json` e `Source/Loader/Loader.csproj`.
- O Loader usa Windows Forms e `net10.0-windows`.
- O projeto usa o CMake vendorizado em `Tools/Build/cmake/windows/bin/cmake.exe`.

Saidas esperadas no estado atual do checkout:

- `bin\x64_release\server\Server.exe`
- `bin\x64_release\server\WebUI\...`
- `bin\x64_release\loader\Loader.exe`
- `bin\x64_release\loader\Injector.dll`
- `bin\x64_release\loader\Injector.config`

Scripts relevantes:

- `Tools/generate_vs2022.bat`: gera projeto Visual Studio 2022.
- `Tools/generate_protobufs.bat`: regenera protobuf C++ para DS2, DS3 e Shared.
- `Tools/generate_package_windows.bat`: empacota `server` e `loader` a partir de `bin\x64_release`.

## Mapa de estrutura

### `Source/Server`

Servidor principal e servicos comuns:

- `Entry.cpp`: entrada do processo.
- `Config/RuntimeConfig.h` e `Config/RuntimeConfig.cpp`: config JSON, defaults e parametros de matching.
- `Config/BuildConfig.h`: constantes de build/runtime como app versions, timeouts e flags.
- `Server/Server.cpp`: inicializa config, database, Steam Game Server API, game implementation DS2/DS3, WebUI, login/auth/game services e advertisement.
- `Server/LoginService/*`: direciona cliente para auth service.
- `Server/AuthService/*`: autentica versao/app, Steam ticket, chave CWC e entrega token do game service.
- `Server/GameService/*`: aceita conexoes do jogo, cria `GameClient`, registra managers e despacha mensagens.
- `Server/Streams/*`: framing FRPG2/reliable UDP, fragments, heartbeat e serializacao de protobufs.
- `Server/Database/*`: persistencia de player, character, stats, messages e records.
- `Server/WebUIService/*`: WebUI e endpoints administrativos.

### `Source/Server.DarkSouls2`

Implementacao especifica DS2:

- `Server/DS2_Game.cpp`: registra todos os game managers DS2.
- `Server/GameService/DS2_PlayerState.h`: estado por player DS2, incluindo `CurrentArea`, `CurrentOnlineActivityArea`, `VisitorPool`, `PlayerStatus` e `GetSoulMemory()`.
- `Server/GameService/GameManagers/Boot`: login inicial e push de upload config.
- `Server/GameService/GameManagers/PlayerData`: profile/session state enviado pelo jogo.
- `Server/GameService/GameManagers/Signs`: summon signs e soapstones.
- `Server/GameService/GameManagers/BreakIn`: invasoes por orbs.
- `Server/GameService/GameManagers/Visitor`: auto-summon / covenant visits.
- `Server/GameService/GameManagers/QuickMatch`: arena/quick match.
- `Server/GameService/GameManagers/Logging`: notificacoes de sessao, mortes e eventos.
- `Server/GameService/GameManagers/Misc`: `RequestSendMessageToPlayers`, total deaths e validacoes.
- `Server/Streams/DS2_Frpg2ReliableUdpMessageTypes.inc`: mapa de message ids DS2 para tipos protobuf.
- `Protobuf/Generated`: codigo gerado. Nao editar manualmente.

### `Source/Injector`

DLL injetada no processo do jogo:

- `Injector/Injector.cpp`: carrega `Injector.config`, detecta `DarkSoulsII.exe` ou `DarkSoulsIII.exe`, instala hooks.
- `Config/RuntimeConfig.h/.cpp`: config consumida pela DLL, incluindo `ServerPublicKey`.
- `Hooks/DarkSouls2/DS2_ReplaceServerAddressHook.cpp`: patch de hostname e public key para DS2.
- `Hooks/DarkSouls2/DS2_LogProtobufsHook.cpp`: hook de logging/decoding de protobufs para investigacao.
- `Hooks/Shared/ReplaceServerPortHook.cpp`: patch de porta por game type.
- `Hooks/Shared/ChangeSaveGameFilenameHook.cpp`: muda nome do save usado pelo DSOS.

### `Source/Loader`

WinForms launcher:

- `Forms/MainForm.cs`: lista servidores, detecta exe, seleciona DS2/DS3, escreve `Injector.config`, injeta `Injector.dll` ou patcha memoria conforme config.
- `Forms/ImportServerDialog.cs`: import manual de servidor no estado atual do checkout; normaliza public key com `\n`.
- `Config/ServerConfig.cs`: modelo de servidor exibido/importado.
- `Config/InjectionConfig.cs`: modelo escrito para `Injector.config`.
- `Config/BuildConfig.cs`: versoes suportadas dos EXEs, flags de injector e dados de patch.
- `Api/MasterServerApi.cs`: chamadas ao master server para listar servidores e buscar public key.
- `Utils/PatchingUtils.cs`: payload criptografado com hostname/public key para caminhos que nao usam injector.

### `Source/Shared`

Tipos e utilitarios compartilhados:

- `Game/GameType.h` e `Game/GameType.cpp`: enum `GameType` e parser textual.
- `Crypto/*`: RSA, CWC, TEA, hash.
- `Net/*`: sockets, HTTP, conexoes.
- `Platform/*`: filesystem/process/threading.
- `Utils/*`: logging, JSON, random, strings, time.

### `Source/MasterServer`

Master server NodeJS:

- `src/index.js`: app.
- `src/routes/api/v1/servers.js`: register/list/get public key.
- Usado quando `Advertise=true`; para MVP privado, `Advertise=false` evita depender dele.

### `Protobuf`

Contratos protobuf originais:

- `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto`: mensagens DS2 de login, status, summon, invasion, visitor, quick match, logging.
- `Protobuf/DarkSouls2/DS2_Frpg2PlayerData.proto`: dados de player/status DS2, incluindo `phantom_leave_at` e `soul_memory`.
- `Protobuf/DarkSouls3/*`: DS3.
- `Protobuf/Shared/*`: mensagens compartilhadas.

### `Tools`

- `generate_vs2022.bat`: geracao CMake/VS.
- `generate_protobufs.bat`: protoc para DS2/DS3/shared.
- `generate_package_windows.bat`: pacote final Windows.
- `Build/build/*.cmake`: output dirs, flags C++ e configuracao VS.
- `Utilities/*`: scripts auxiliares de protobuf/Cheat Engine/logging.

## Pontos Dark Souls 2

Selecao de jogo:

- `Source/Shared/Game/GameType.h:16`: `GameType::DarkSouls2`.
- `Source/Shared/Game/GameType.h:24`: string `"DarkSouls2"`.
- `Source/Shared/Game/GameType.cpp:15`: `ParseGameType`.
- `Source/Server/Server/Server.cpp:125`: parse de `Config.GameType`.
- `Source/Server/Server/Server.cpp:153`: cria `DS2_Game`.
- `Source/Injector/Injector/Injector.cpp:113`: detecta modulo `DarkSoulsII.exe`.
- `Source/Loader/Forms/MainForm.cs:32`: enum WinForms inclui `DarkSouls2`.
- `Source/Loader/Forms/MainForm.cs:278`: caminho Steam previsto para `Dark Souls II Scholar of the First Sin`.

SOTFS:

- `README.md:9`: DSOS declara suporte a Dark Souls 2 SOTFS e 3.
- `README.md:41`: tabela de features DS2 SOTFS.
- `Source/Loader/Forms/MainForm.cs:278`: nome Steam `Dark Souls II Scholar of the First Sin`.
- `Source/Loader/Config/BuildConfig.cs:98` e `:111`: versoes DS2 Steam suportadas.

Soapstones e summon:

- `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto:576`: `SignType_WhiteSoapstone`.
- `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto:577`: `SignType_SmallWhiteSoapstone`.
- `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto:578`: `SignType_RedSoapstone`.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/Signs/DS2_SignManager.cpp:116`: matching por tipo de sign.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/Signs/DS2_SignManager.cpp:150`: lista signs.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/Signs/DS2_SignManager.cpp:226`: cria sign.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/Signs/DS2_SignManager.cpp:358`: request para summonar sign.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/Signs/DS2_SignManager.cpp:463`: rejeicao de summon.

Invasion / BreakIn:

- `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto:694`: enum `BreakInType`.
- `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto:695`: `BreakInType_RedEyeOrb`.
- `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto:696`: `BreakInType_BlueEyeOrb`.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/BreakIn/DS2_BreakInManager.cpp:54`: matching de invasion target.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/BreakIn/DS2_BreakInManager.cpp:95`: lista targets de invasao.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/BreakIn/DS2_BreakInManager.cpp:159`: request de invasao.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/BreakIn/DS2_BreakInManager.cpp:232`: rejeicao de invasao.

Visitor / auto-summon:

- `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto:804`: enum `VisitorType`.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/Visitor/DS2_VisitorManager.cpp:51`: matching de visitor.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/Visitor/DS2_VisitorManager.cpp:89`: lista visitors.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/Visitor/DS2_VisitorManager.cpp:130`: request visit.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/Visitor/DS2_VisitorManager.cpp:242`: rejeicao visit.

QuickMatch / arena:

- `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto:987`: enum `QuickMatchGameMode`.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/QuickMatch/DS2_QuickMatchManager.cpp:86`: matching de arena.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/QuickMatch/DS2_QuickMatchManager.cpp:150`: busca matches.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/QuickMatch/DS2_QuickMatchManager.cpp:185`: registra match.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/QuickMatch/DS2_QuickMatchManager.cpp:277`: join quick match.
- `Source/Server.DarkSouls2/Server/GameService/GameManagers/QuickMatch/DS2_QuickMatchManager.cpp:344`: reject quick match.

Soul Memory e MatchingParameters:

- `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto:451`: `MatchingParameter`.
- `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto:466`: `soul_memory`.
- `Protobuf/DarkSouls2/DS2_Frpg2PlayerData.proto:143`: `PlayerStatus.soul_memory`.
- `Source/Server.DarkSouls2/Server/GameService/DS2_PlayerState.h`: `GetSoulMemory()`.
- `Source/Server/Config/RuntimeConfig.h:59`: `RuntimeConfigSoulMemoryMatchingParameters`.
- `Source/Server/Config/RuntimeConfig.cpp:185`: calcula tier de soul memory.
- `Source/Server/Config/RuntimeConfig.cpp:203`: valida match por tier.
- `Source/Server/Config/RuntimeConfig.h:398` ate `:480`: parametros DS2 por modo.

## RuntimeConfig

Arquivos:

- `Source/Server/Config/RuntimeConfig.h`
- `Source/Server/Config/RuntimeConfig.cpp`

Serializacao JSON:

- `RuntimeConfig::Save`: `Source/Server/Config/RuntimeConfig.cpp:15`.
- `RuntimeConfig::Load`: `Source/Server/Config/RuntimeConfig.cpp:29`.
- `RuntimeConfig::Serialize`: `Source/Server/Config/RuntimeConfig.cpp:268`.
- Macros `SERIALIZE_VAR` e `SERIALIZE_STRUCT_VAR` controlam defaults e leitura/escrita.

Defaults e campos globais relevantes:

- `GameType = "DarkSouls3"` em `RuntimeConfig.h:139`.
- `ServerHostname` em `RuntimeConfig.h:152`.
- `MasterServerIp` e `MasterServerPort` em `RuntimeConfig.h:160` e `:163`.
- `Advertise = true` em `RuntimeConfig.h:170`.
- `AdvertiseHearbeatTime = 30.0f` em `RuntimeConfig.h:174`.
- `Password` em `RuntimeConfig.h:183`.
- `ModsWhitelist`, `ModsBlacklist`, `ModsRequiredList` em `RuntimeConfig.h:187`, `:191`, `:195`.
- `WebUIServerPort`, `WebUIServerUsername`, `WebUIServerPassword` em `RuntimeConfig.h:207`, `:213`, `:216`.
- `DisableInvasions`, `DisableCoop`, `DisableInvasionAutoSummon`, `DisableCoopAutoSummon` em `RuntimeConfig.h:290`, `:293`, `:305`, `:308`.
- `PlayerStatusUploadInterval`, `PlayerStatusUploadSendDelay` em `RuntimeConfig.h:316`, `:322`.

Parametros DS2 especificos:

- `DS2_WhiteSoapstoneMatchingParameters`
- `DS2_SmallWhiteSoapstoneMatchingParameters`
- `DS2_RedSoapstoneMatchingParameters`
- `DS2_MirrorKnightMatchingParameters`
- `DS2_DragonEyeMatchingParameters`
- `DS2_RedEyeOrbMatchingParameters`
- `DS2_BlueEyeOrbMatchingParameters`
- `DS2_BellKeeperMatchingParameters`
- `DS2_RatMatchingParameters`
- `DS2_BlueSentinelMatchingParameters`
- `DS2_ArenaMatchingParameters`
- `DS2_MatchingAreaMatchingParameters`

Esses campos sao serializados em `RuntimeConfig.cpp:333` ate `:344`.

Nao encontrei campo de config com semantica clara de duracao de phantom, PvP session timeout ou red phantom lifetime. Os timeouts encontrados sao de conexao/autenticacao/processo, nao de duracao de phantom.

## Servicos e handlers de rede

### Login/Auth/Game services

Fluxo comum:

1. Loader inicia o jogo com `Injector.dll` e `Injector.config`.
2. Injector troca hostname, porta e public key usados pelo jogo.
3. O jogo conecta no `LoginService`.
4. `LoginService` devolve endereco do `AuthService`.
5. `AuthService` valida versao/app, Steam ticket e gera token.
6. `GameService` aceita conexao autenticada.
7. `GameClient::HandleMessage` despacha cada protobuf para os managers DS2 registrados por `DS2_Game`.

Arquivos e pontos:

- `Source/Server/Server/LoginService/LoginClient.cpp:36`: timeout do login client.
- `Source/Server/Server/AuthService/AuthClient.cpp:44`: poll/auth timeout.
- `Source/Server/Server/GameService/GameService.cpp:42`: init e registro de managers.
- `Source/Server/Server/GameService/GameService.cpp:98`: poll de managers e clients.
- `Source/Server/Server/GameService/GameService.cpp:226`: cria auth token.
- `Source/Server/Server/GameService/GameService.cpp:237`: refresh auth token.
- `Source/Server/Server/GameService/GameClient.cpp:41`: poll do cliente de jogo.
- `Source/Server/Server/GameService/GameClient.cpp:95`: `CLIENT_TIMEOUT`.
- `Source/Server/Server/GameService/GameClient.cpp:107`: dispatch para managers.
- `Source/Server/Config/BuildConfig.h:71`: `CLIENT_TIMEOUT = 120.0`.
- `Source/Server/Config/BuildConfig.h:74`: `AUTH_TICKET_TIMEOUT = 30.0`.

### Heartbeat

- Reliable UDP heartbeat: `Source/Server/Server/Streams/Frpg2ReliableUdpPacketStream.h:59`, `Source/Server/Server/Streams/Frpg2ReliableUdpPacketStream.cpp:781`, `:807`.
- Master server heartbeat/ad: `Source/Server/Server/Server.cpp:416` e `:446`, usando `AdvertiseHearbeatTime`.
- Keepalive de atividade do servidor local: `Source/Server/Server/Server.cpp:510` ate `:530`.
- `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto:1142` tem `ServerPing`, mas nao encontrei handler DS2 mapeado para isso em `DS2_Frpg2ReliableUdpMessageTypes.inc`.

### Profile/session state

- `DS2_BootManager::Handle_RequestWaitForUserLogin`: `Source/Server.DarkSouls2/Server/GameService/GameManagers/Boot/DS2_BootManager.cpp:46`.
- `PlayerInfoUploadConfigPushMessage`: configurado pelo Boot manager, com delays vindos do RuntimeConfig.
- `DS2_PlayerDataManager::Handle_RequestUpdatePlayerStatus`: `Source/Server.DarkSouls2/Server/GameService/GameManagers/PlayerData/DS2_PlayerDataManager.cpp:125`.
- `RequestUpdatePlayerStatus`: `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto:132`.
- `PlayerStatus.phantom_leave_at`: `Protobuf/DarkSouls2/DS2_Frpg2PlayerData.proto:132`.
- `PlayerStatus.soul_memory`: `Protobuf/DarkSouls2/DS2_Frpg2PlayerData.proto:143`.

`DS2_PlayerDataManager` guarda status enviado pelo cliente e deriva area atual, online activity area, invadable state, soul level, soul memory e visitor pool. Isso e dado de estado para matching, nao parece controlar a duracao da sessao multiplayer.

### Summon sign

- Mapeamento de mensagens: `Source/Server.DarkSouls2/Server/Streams/DS2_Frpg2ReliableUdpMessageTypes.inc:62` ate `:73`.
- `DS2_SignManager::Handle_RequestGetSignList`: filtra signs por area e matching.
- `DS2_SignManager::Handle_RequestCreateSign`: cria cache de sign.
- `DS2_SignManager::Handle_RequestSummonSign`: envia `PushRequestSummonSign` ao dono do sign e marca `BeingSummonedByPlayerId`.
- `DS2_SignManager::Handle_RequestRejectSign`: notifica rejeicao e libera estado.
- `DS2_SignManager::RemoveSignAndNotifyAware`: remove sign e notifica clientes que estavam aware.

### Invasion request

- Mapeamento de mensagens: `Source/Server.DarkSouls2/Server/Streams/DS2_Frpg2ReliableUdpMessageTypes.inc:123` ate `:131`.
- `DS2_BreakInManager::Handle_RequestGetBreakInTargetList`: lista targets.
- `DS2_BreakInManager::CanMatchWith`: aplica `DisableInvasions`, `IsInvadable`, sinner points para BlueEyeOrb e Soul Memory matching.
- `DS2_BreakInManager::Handle_RequestBreakInTarget`: envia `PushRequestBreakInTarget` ao host.
- `DS2_BreakInManager::Handle_RequestRejectBreakInTarget`: repassa rejeicao ao invader.

### Visitor / covenant request

- Mapeamento de mensagens: `Source/Server.DarkSouls2/Server/Streams/DS2_Frpg2ReliableUdpMessageTypes.inc:136` ate `:142`.
- `DS2_VisitorManager::CanMatchWith`: usa `DisableInvasionAutoSummon`, `DisableCoopAutoSummon`, visitor pool e Soul Memory matching.
- `DS2_VisitorManager::Handle_RequestVisit`: envia `PushRequestVisit`.
- `DS2_VisitorManager::Handle_RequestRejectVisit`: repassa rejeicao.

### Quick match / arena

- Mapeamento de mensagens: `Source/Server.DarkSouls2/Server/Streams/DS2_Frpg2ReliableUdpMessageTypes.inc:163` ate `:173`.
- `DS2_QuickMatchManager::Handle_RequestRegisterQuickMatch`: host registra arena.
- `DS2_QuickMatchManager::Handle_RequestSearchQuickMatch`: cliente busca arena.
- `DS2_QuickMatchManager::Handle_RequestJoinQuickMatch`: envia `PushRequestJoinQuickMatch` ao host.
- `DS2_QuickMatchManager::Handle_RequestRejectQuickMatch`: repassa rejeicao.

### Disconnect e session lifecycle

- Mensagens protobuf: `Protobuf/DarkSouls2/DS2_Frpg2RequestMessage.proto:345` ate `:421`.
- Mapeamento DS2: `Source/Server.DarkSouls2/Server/Streams/DS2_Frpg2ReliableUdpMessageTypes.inc:87` ate `:93`.
- `DS2_LoggingManager::Handle_RequestNotifyDisconnectSession`: `Source/Server.DarkSouls2/Server/GameService/GameManagers/Logging/DS2_LoggingManager.cpp:129`.
- `DS2_LoggingManager::Handle_RequestNotifyJoinSession`: `Source/Server.DarkSouls2/Server/GameService/GameManagers/Logging/DS2_LoggingManager.cpp:161`.
- `DS2_LoggingManager::Handle_RequestNotifyKillPlayer`: `Source/Server.DarkSouls2/Server/GameService/GameManagers/Logging/DS2_LoggingManager.cpp:210`.
- `DS2_LoggingManager::Handle_RequestNotifyLeaveGuestPlayer`: `Source/Server.DarkSouls2/Server/GameService/GameManagers/Logging/DS2_LoggingManager.cpp:226`.
- `DS2_LoggingManager::Handle_RequestNotifyLeaveSession`: `Source/Server.DarkSouls2/Server/GameService/GameManagers/Logging/DS2_LoggingManager.cpp:242`.

Esses handlers hoje respondem vazio e registram estatisticas/logs pontuais. Eles parecem bons pontos para observar ciclo de vida, mas nao vi logica que force ou prorrogue tempo de phantom.

## Call graph aproximado

```text
Loader MainForm
  -> seleciona ServerConfig
  -> escreve Injector.config
  -> inicia DarkSoulsII.exe suspenso
  -> injeta Injector.dll

Injector.dll
  -> Injector::Init
  -> ParseGameType(ServerGameType)
  -> DS2_ReplaceServerAddressHook::Install
      -> PatchHostname
      -> PatchKey
  -> ReplaceServerPortHook::Install
  -> ChangeSaveGameFilenameHook::Install

DarkSoulsII.exe
  -> conecta LoginService
  -> conecta AuthService
  -> conecta GameService

Server::Init
  -> RuntimeConfig::Load/Save
  -> ParseGameType(Config.GameType)
  -> DS2_Game()
  -> LoginService::Init
  -> AuthService::Init
  -> GameService::Init
      -> DS2_Game::RegisterGameManagers
          -> DS2_BootManager
          -> DS2_PlayerDataManager
          -> DS2_GhostManager
          -> DS2_BloodMessageManager
          -> DS2_BloodstainManager
          -> DS2_BreakInManager
          -> DS2_LoggingManager
          -> DS2_MiscManager
          -> DS2_VisitorManager
          -> DS2_RankingManager
          -> DS2_MirrorKnightManager
          -> DS2_SignManager
          -> DS2_QuickMatchManager

GameService::Poll
  -> each manager Poll
  -> accept NetConnection
  -> GameClient::Poll
      -> MessageStream::Poll
      -> GameClient::HandleMessage
          -> manager.OnMessageRecieved
              -> DS2_*Manager::Handle_*
```

Fluxos PvP principais:

```text
Coop/red sign:
RequestCreateSign -> DS2_SignManager cache
RequestGetSignList -> DS2_SignManager filters by area + soul memory
RequestSummonSign -> PushRequestSummonSign to sign owner
RequestRejectSign / RequestRemoveSign -> cleanup or reject push

Orb invasion:
RequestGetBreakInTargetList -> DS2_BreakInManager filters targets
RequestBreakInTarget -> PushRequestBreakInTarget to host
RequestRejectBreakInTarget -> reject push to invader

Covenant/auto summon:
RequestGetVisitorList -> DS2_VisitorManager filters visitors
RequestVisit -> PushRequestVisit to target
RequestRejectVisit -> reject push to initiator

Arena:
RequestRegisterQuickMatch -> DS2_QuickMatchManager live cache
RequestSearchQuickMatch -> filtered quick matches
RequestJoinQuickMatch -> PushRequestJoinQuickMatch to host
RequestRejectQuickMatch -> reject push to joiner

Lifecycle:
RequestNotifyJoinSession / JoinGuestPlayer / KillPlayer / LeaveGuestPlayer / LeaveSession / DisconnectSession
  -> DS2_LoggingManager
  -> empty response + limited statistics
```

## Hipoteses sobre timer de red phantom

1. Nao parece existir configuracao server-side direta para duracao de red phantom.
   - `RuntimeConfig` tem matching, upload intervals e flags de disable, mas nao duracao de phantom.
   - `BuildConfig::CLIENT_TIMEOUT` e timeout de inatividade de conexao, nao timer de PvP.

2. `phantom_leave_at` provavelmente e dado client-side reportado ao servidor.
   - Campo em `DS2_Frpg2PlayerData.proto:132`.
   - O servidor recebe `RequestUpdatePlayerStatus` e guarda/mescla `PlayerStatus`.
   - Nao encontrei uso server-side desse campo para expulsar ou manter phantom.

3. O servidor atua principalmente como broker de matching e rendezvous.
   - Sign, BreakIn, Visitor e QuickMatch enviam push para conectar jogadores.
   - Depois do aceite/entrada no mundo, a duracao parece ser governada pelo cliente/jogo.

4. `DS2_LoggingManager` e o melhor ponto inicial de observacao.
   - Ele ve `JoinSession`, `JoinGuestPlayer`, `KillPlayer`, `LeaveGuestPlayer`, `LeaveSession` e `DisconnectSession`.
   - Antes de qualquer patch de comportamento, vale logar os campos e correlacionar com red soapstone, red eye orb, dragon eye e arena.

5. Evitar kick durante PvP provavelmente exigiria primeiro derivar um estado "em PvP".
   - Candidatos: eventos brokered (`RequestSummonSign`, `RequestBreakInTarget`, `RequestVisit`, `RequestJoinQuickMatch`) + lifecycle (`JoinSession`, `KillPlayer`, `LeaveSession`).
   - Sem essa correlacao, qualquer mudanca de timeout pode afetar desconexoes legitimas ou deixar sessoes presas.

## Server-side versus client-side

Provavelmente server-side neste codigo:

- Autenticacao Steam ticket.
- Escolha de game type DS2/DS3.
- Chave publica do servidor e endpoint que o cliente usa.
- Lista de servidores via master, quando `Advertise=true`.
- Matching por area, online activity area, password e Soul Memory.
- Cache de summon signs, break-in targets, visitors e quick matches.
- Persistencia de profiles, characters, messages, stats e records.
- WebUI/admin.

Provavelmente client-side ou dependente do binario do jogo:

- Duracao real da presenca do phantom no mundo.
- Timer visual/estado interno de red phantom.
- Regras finais de desconexao do phantom depois que a sessao multiplayer ja foi estabelecida.
- Interpretacao completa de `phantom_leave_at`.
- UI/gameplay de summon, invasion e arena.
- Formato retail de save; DSOS so troca nome do save via hook e nao deve editar save retail.

## Riscos

- DS2 esta marcado no README como experimental; varias mensagens/protobufs tem comentarios de "guessing" ou "needs validation".
- Nao ha evidencia de uma config server-side simples para tempo de phantom.
- Mexer em timer de phantom pode exigir instrumentacao client-side; isso deve ser feito sem offsets magicos e com validacao por logs/protobufs.
- `ModsWhitelist`, `ModsBlacklist` e `ModsRequiredList` sao serializados/anunciados, mas a analise estatica nao encontrou enforcement forte no fluxo de conexao DS2. Para impedir vanilla com modded, sera necessario validar politica de mods no Loader/server antes do launch/conexao.
- `Advertise=true` e default no `RuntimeConfig`; para o MVP privado deve ficar `false`.
- `AUTH_ENABLED=true` em `BuildConfig`, coerente com nao usar Steam emulator.
- `DISCONNECT_ON_UNHANDLED_MESSAGE=false`; mensagens desconhecidas podem ser ignoradas, o que ajuda compatibilidade mas mascara lacunas de protocolo.
- O Injector e o Loader podem ser sinalizados por antivirus por injecao DLL, mesmo sem comportamento malicioso.
- O estado local atual ja contem alteracoes de build para `bin\x64_release\server` e `bin\x64_release\loader`, alem do Import Server.
- Em `Source/Server.DarkSouls2/Server/DS2_Game.cpp` ha um include com token extra observado no checkout: `DS2_SignManager.h"0`; vale revisar em etapa de higiene se a build reclamar.

## Proximos experimentos

1. Rodar local/ZeroTier com `Advertise=false`, `GameType=DarkSouls2`, porta de login `50050`, auth `50000`, game `50010`.
2. Capturar logs para quatro fluxos: red soapstone, red eye orb, dragon eye e arena.
3. Adicionar somente logging observacional em `DS2_LoggingManager` e nos handlers de request/push PvP, sem mudar comportamento.
4. Confirmar se `phantom_leave_at` muda em `RequestUpdatePlayerStatus` antes, durante e depois do PvP.
5. Correlacionar `RequestNotifyJoinSession`, `RequestNotifyJoinGuestPlayer`, `RequestNotifyKillPlayer`, `RequestNotifyLeaveGuestPlayer`, `RequestNotifyLeaveSession` e `RequestNotifyDisconnectSession`.
6. Definir um estado server-side minimo `InPvpSession` somente depois da correlacao de logs.
7. Se o objetivo for "nao kickar durante PvP", testar primeiro uma regra de deferral baseada em estado observado, nao em offsets.
8. Validar politica anti-mix vanilla/modded: manifest/hash no Loader, config de mods no server ou handshake proprio privado.
9. Manter qualquer experimento restrito ao endpoint privado/importado e nao adicionar caminho para servidor oficial.
