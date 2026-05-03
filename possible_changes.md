# Possible Changes: DS2 Soul Memory Matchmaking

## Contexto

O DS2 usa Soul Memory como principal filtro de matchmaking. No servidor atual, esse filtro e aplicado server-side quando o cliente pede listas de sinais, alvos de invasao, visitantes de covenant ou quick match.

Os testes recentes confirmaram esse comportamento:

- jogador A com `soul_memory=3751200` nao via sinal vermelho de jogador B com `soul_memory=400`;
- quando B entrou com `soul_memory=3750400`, A passou a ver `response_sign_count=1` e conseguiu fazer summon via `RedSoapstone`.

Isso indica que a conexao privada, o import do servidor e o Red Soapstone funcionam. O bloqueio anterior era compatibilidade de Soul Memory.

## Como o Soul Memory e lido

O estado DS2 do jogador guarda o status enviado pelo jogo em `DS2_PlayerState`.

Arquivo:

- `Source/Server.DarkSouls2/Server/GameService/DS2_PlayerState.h`

Trecho relevante:

```cpp
virtual size_t GetSoulMemory() override
{
    auto Status = GetPlayerStatus().player_status();
    return Status.soul_memory();
}
```

Ou seja, o servidor nao calcula Soul Memory. Ele recebe o valor do cliente no status do personagem e usa esse numero nas regras de matchmaking.

## Como os tiers funcionam

Arquivo:

- `Source/Server/Config/RuntimeConfig.h`
- `Source/Server/Config/RuntimeConfig.cpp`

O tipo principal e:

```cpp
RuntimeConfigSoulMemoryMatchingParameters
```

Ele contem:

- `Tiers`: limites de cada tier de Soul Memory;
- `DisableSoulMemoryMatching`: desliga totalmente o filtro quando `true`;
- `TiersBelow`: quantos tiers abaixo podem entrar;
- `TiersAbove`: quantos tiers acima podem entrar;
- `TiersBelowWithPassword`: range abaixo quando ha password/name engraved ring;
- `TiersAboveWithPassword`: range acima quando ha password/name engraved ring.

A funcao central e:

```cpp
bool RuntimeConfigSoulMemoryMatchingParameters::CheckMatch(
    int HostSoulMemory,
    int ClientSoulMemory,
    bool UsingPassword
) const
{
    if (DisableSoulMemoryMatching)
    {
        return true;
    }

    int HostSoulTier = CalculateTier(HostSoulMemory);
    int ClientSoulTier = CalculateTier(ClientSoulMemory);

    int LowerLimit = HostSoulTier;
    int UpperLimit = HostSoulTier;

    if (UsingPassword)
    {
        LowerLimit -= TiersBelowWithPassword;
        UpperLimit += TiersAboveWithPassword;
    }
    else
    {
        LowerLimit -= TiersBelow;
        UpperLimit += TiersAbove;
    }

    return (ClientSoulTier >= LowerLimit && ClientSoulTier <= UpperLimit);
}
```

Portanto, para cada tentativa de matching:

1. Converte Soul Memory em tier.
2. Calcula uma janela de tiers permitidos.
3. Retorna `true` somente se o outro jogador esta dentro da janela.
4. Se `DisableSoulMemoryMatching=true`, pula tudo e aceita qualquer Soul Memory.

## Onde o filtro e aplicado

### Sinais: Red/White/Small White/Dragon

Arquivo:

- `Source/Server.DarkSouls2/Server/GameService/GameManagers/Signs/DS2_SignManager.cpp`

Funcao:

```cpp
DS2_SignManager::CanMatchWith(...)
```

Mapeamento atual:

- `SignType_RedSoapstone` usa `DS2_RedSoapstoneMatchingParameters`;
- `SignType_WhiteSoapstone` usa `DS2_SmallWhiteSoapstoneMatchingParameters`;
- `SignType_SmallWhiteSoapstone` usa `DS2_WhiteSoapstoneMatchingParameters`;
- `SignType_Dragon` usa `DS2_DragonEyeMatchingParameters`.

O filtro roda durante `RequestGetSignList`, antes do servidor devolver os sinais visiveis para o cliente.

Para PvP privado via sinal vermelho, o parametro mais importante e:

```json
"DS2_RedSoapstoneMatchingParameters": {
  "DisableSoulMemoryMatching": true
}
```

### Invasoes por orb

Arquivo:

- `Source/Server.DarkSouls2/Server/GameService/GameManagers/BreakIn/DS2_BreakInManager.cpp`

Funcao:

```cpp
DS2_BreakInManager::CanMatchWith(...)
```

Mapeamento:

- `BreakInType_RedEyeOrb` usa `DS2_RedEyeOrbMatchingParameters`;
- `BreakInType_BlueEyeOrb` usa `DS2_BlueEyeOrbMatchingParameters`.

Mesmo removendo Soul Memory, ainda existem outros filtros:

- `DisableInvasions`;
- se o alvo esta invadable;
- para Blue Eye Orb, o alvo precisa ter sinner points suficientes.

### Auto-summon de covenant

Arquivo:

- `Source/Server.DarkSouls2/Server/GameService/GameManagers/Visitor/DS2_VisitorManager.cpp`

Funcao:

```cpp
DS2_VisitorManager::CanMatchWith(...)
```

Mapeamento:

- `VisitorType_BlueSentinels` usa `DS2_BlueSentinelMatchingParameters`;
- `VisitorType_BellKeepers` usa `DS2_BellKeeperMatchingParameters`;
- `VisitorType_Rat` usa `DS2_RatMatchingParameters`.

Mesmo removendo Soul Memory, ainda existem filtros como `DisableInvasionAutoSummon`, `DisableCoopAutoSummon` e estado do visitor pool.

### Arena / Quick Match

Arquivo:

- `Source/Server.DarkSouls2/Server/GameService/GameManagers/QuickMatch/DS2_QuickMatchManager.cpp`

Funcao:

```cpp
DS2_QuickMatchManager::CanMatchWith(...)
```

Usa:

```cpp
Config.DS2_ArenaMatchingParameters.CheckMatch(...)
```

Mesmo removendo Soul Memory, o quick match ainda filtra por modo e mapa/cell.

## Maneira mais facil de abrir qualquer Soul Memory

Nao precisa alterar codigo para o primeiro MVP. A configuracao ja suporta isso.

No `config.json` do server, setar `DisableSoulMemoryMatching` como `true` nos blocos DS2 desejados.

Para o MVP focado em PvP com Red Soapstone entre amigos, a mudanca minima e:

```json
"DS2_RedSoapstoneMatchingParameters": {
  "DisableSoulMemoryMatching": true
}
```

Na pratica, o bloco ja contem tambem `Tiers`, `TiersAbove`, `TiersBelow`, etc. Basta mudar apenas:

```json
"DisableSoulMemoryMatching": true
```

dentro de `DS2_RedSoapstoneMatchingParameters`.

Com isso:

- qualquer Soul Memory pode ver sinais vermelhos;
- o filtro de area/cell continua existindo;
- o fluxo de summon continua igual;
- saves nao sao alterados;
- loader nao precisa mudar;
- nao ha offsets magicos.

## Config completa para abrir todos os tipos DS2

Se a meta for remover Soul Memory de todo matchmaking DS2 privado, mudar para `true` em todos estes blocos:

```json
"DS2_WhiteSoapstoneMatchingParameters": {
  "DisableSoulMemoryMatching": true
},
"DS2_SmallWhiteSoapstoneMatchingParameters": {
  "DisableSoulMemoryMatching": true
},
"DS2_RedSoapstoneMatchingParameters": {
  "DisableSoulMemoryMatching": true
},
"DS2_MirrorKnightMatchingParameters": {
  "DisableSoulMemoryMatching": true
},
"DS2_DragonEyeMatchingParameters": {
  "DisableSoulMemoryMatching": true
},
"DS2_RedEyeOrbMatchingParameters": {
  "DisableSoulMemoryMatching": true
},
"DS2_BlueEyeOrbMatchingParameters": {
  "DisableSoulMemoryMatching": true
},
"DS2_BellKeeperMatchingParameters": {
  "DisableSoulMemoryMatching": true
},
"DS2_RatMatchingParameters": {
  "DisableSoulMemoryMatching": true
},
"DS2_BlueSentinelMatchingParameters": {
  "DisableSoulMemoryMatching": true
},
"DS2_ArenaMatchingParameters": {
  "DisableSoulMemoryMatching": true
},
"DS2_MatchingAreaMatchingParameters": {
  "DisableSoulMemoryMatching": true
}
```

Observacao: no arquivo real, cada bloco contem os outros campos tambem. Nao substituir o bloco inteiro por esse exemplo; alterar apenas a propriedade `DisableSoulMemoryMatching`.

## Alternativa com feature flag global

Se quisermos uma experiencia mais limpa, podemos adicionar uma flag global no `RuntimeConfig`, por exemplo:

```json
"DS2OpenSoulMemoryMatchmaking": true
```

E entao fazer os pontos DS2 de matching retornarem `true` antes de chamar `CheckMatch`.

Vantagem:

- uma unica configuracao abre tudo para Soul Memory;
- reduz chance de esquecer algum bloco DS2;
- fica mais claro para o usuario final.

Desvantagem:

- exige alteracao de codigo e rebuild;
- precisa cuidado para nao abrir sistemas que nao queremos usar no MVP.

Para o MVP atual, a opcao config-only e mais segura.

## Riscos e limites

Remover Soul Memory nao remove todos os filtros. Ainda podem bloquear matching:

- area/cell diferentes;
- estado do jogador nao invadable;
- covenant/visitor pool incorreto;
- item usado no lado do cliente;
- modo/mapa do quick match;
- limite de sinais por area;
- `DisableInvasions`, `DisableCoop`, `DisableCoopAutoSummon`, `DisableInvasionAutoSummon`;
- comportamento client-side do DS2 que o servidor nao controla.

Tambem e importante manter isso apenas no servidor privado. Abrir Soul Memory muda o balanceamento esperado do jogo e pode permitir duelos entre personagens muito desiguais, mas nao altera saves e nao precisa mexer no cliente alem do loader/injector ja usado para apontar para o servidor privado.

## Plano recomendado

1. Para PvP privado via Red Soapstone, primeiro setar somente:

```json
"DS2_RedSoapstoneMatchingParameters": {
  "DisableSoulMemoryMatching": true
}
```

2. Repetir o teste com dois personagens de Soul Memory bem diferente.

3. Confirmar nos logs:

```text
ListSummonSigns response_sign_count=1 red_soapstone_count=1
SummonAttempt
SummonAccepted
JoinGuestPlayer
JoinSession
```

4. Se funcionar, decidir se abre tambem Dragon Eye, Red Eye Orb e Arena.

5. Se ficar chato manter varios blocos JSON, implementar a feature flag global `DS2OpenSoulMemoryMatchmaking`.
