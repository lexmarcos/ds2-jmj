# DS2 Phantom Timer Patch

Este documento registra como o patch de tempo de sessao do phantom foi descoberto e implementado no injector.

## Objetivo

Evitar a saida automatica do phantom por tempo em Dark Souls II: Scholar of the First Sin, sem bloquear `RequestNotifyLeaveSession` e sem alterar o fluxo normal de saida por morte, boss, desconexao ou saida manual.

O patch final nao remove a mensagem de leave. Ele impede que o contador ativo de tempo chegue a zero.

## Resultado Final

A versao funcional validada foi:

```text
ds2-jmj injector pvp-timer v2026-05-07.1
```

O timer ativo fica em:

```text
R14 + 0xCFC
```

O hook escreve o valor configurado, por padrao:

```text
4000.0f
```

No console, a confirmacao esperada e:

```text
[DS2TimerParamPatch] patched active session timer old=... target=4000.000 trace_file=DS2_TimerParamPatch.log
```

No log `DS2_TimerParamPatch.log`, a confirmacao esperada e:

```text
event=DS2ActiveTimerPatch result=patched source=r14_plus_0xcfc
```

## Configuracao

O patch e controlado por estas entradas em `Injector.config`:

```json
"DS2TraceLeaveSession": false,
"DS2PreventPvpTimerLeave": false,
"DS2PatchPhantomTimers": true,
"DS2PhantomTimerSeconds": 4000
```

Tambem ha suporte a env vars:

```text
DS2_PATCH_PHANTOM_TIMERS=1
DS2_PHANTOM_TIMER_SECONDS=4000
```

`DS2PreventPvpTimerLeave` deve ficar desligado quando `DS2PatchPhantomTimers` estiver ligado. O patch correto e no timer ativo; bloquear leave direto foi mantido apenas como experimento antigo e nao deve ser usado para esta solucao.

## Arquivos Principais

Implementacao:

```text
Source/Injector/Hooks/DarkSouls2/DS2_PhantomTimerParamPatchHook.cpp
Source/Injector/Hooks/DarkSouls2/DS2_PhantomTimerParamPatchHook.h
```

Instalacao do hook:

```text
Source/Injector/Injector/Injector.cpp
```

Config runtime:

```text
Source/Injector/Config/RuntimeConfig.h
Source/Injector/Config/RuntimeConfig.cpp
Source/Loader/Config/InjectionConfig.cs
Source/Loader/Forms/MainForm.cs
```

Logs auxiliares:

```text
DS2_TimerParamPatch.log
DS2_LeaveSessionTrace.log
```

## Como Foi Descoberto

1. O trace de protobufs mostrou que a saida por tempo gera `RequestNotifyLeaveSession` depois de aproximadamente 763-766 segundos.

2. A callstack do leave por tempo sempre passava pelos mesmos pontos do jogo:

```text
game_rel=0x2c3c19
game_rel=0x2c36d7
game_rel=0x2c9844
```

3. Tentamos primeiro encontrar e patchar o param bruto de multiplayer, mas esse caminho foi descartado porque o bloco esperado nao apareceu no formato observado em memoria.

4. O mod/pesquisa externa confirmou que o timer existe como float, mas o bloco de param bruto nao apareceu no formato esperado em memoria.

5. Usamos Cheat Engine no processo `DarkSoulsII.exe`, ja dentro do mundo do host:

```text
Value Type: Float
Scan Type: Unknown initial value
Filtro repetido: Decreased value
Filtro final: faixa plausivel de 100 a 4000
```

O endereco encontrado foi:

```text
7FF447B49A9C
```

Esse valor diminuia de forma estavel, chegou a zero, disparou a saida da sessao e depois ficou `-1` no mundo local. Isso confirmou que era o contador ativo real.

6. No mesmo run, o log do injector no breakpoint `0x2c9844` tinha:

```text
R14 = 0x00007ff447b48da0
```

Somando o offset:

```text
0x00007ff447b48da0 + 0xCFC = 0x00007ff447b49a9c
```

Esse resultado bate exatamente com o endereco do Cheat Engine:

```text
7FF447B49A9C
```

Portanto, o offset funcional do timer ativo nesse hot path e:

```text
R14 + 0xCFC
```

## Como o Hook Funciona

O hook instala um breakpoint de software (`0xCC`) no offset:

```text
DarkSoulsII.exe + 0x2c9844
```

Antes de instalar, valida a assinatura esperada da instrucao. Se a assinatura nao bater, falha fechado e registra erro, sem patch parcial.

Quando o breakpoint dispara:

1. Restaura o byte original.
2. Usa o contexto de registradores do thread atual.
3. Calcula:

```text
timer_address = R14 + 0xCFC
```

4. Le o `float` em `timer_address`.
5. Valida que o valor e plausivel:

```text
finite
> 0.0
<= 100000.0
```

6. Se o timer estiver abaixo do alvo, escreve o alvo configurado:

```text
DS2PhantomTimerSeconds
```

7. Ativa trap flag para executar a instrucao original uma vez.
8. No `EXCEPTION_SINGLE_STEP`, reinstala o breakpoint.

Isso faz o contador ser resetado repetidamente para perto de `4000.0`, sem bloquear o envio final de leave quando ele for legitimo por outro motivo.

## Por Que Nao Bloquear Leave

Bloquear `RequestNotifyLeaveSession` ou `RequestNotifyLeaveGuestPlayer` diretamente causou instabilidade ou encerramento da sessao em testes anteriores. O jogo espera que a maquina de estado de rede avance. A solucao atual evita que o motivo "timer chegou a zero" aconteca, mas deixa o leave natural continuar quando houver morte, desconexao, saida manual ou outro evento legitimo.

## Como Validar

1. Compile Release:

```powershell
.\Tools\Build\cmake\windows\bin\cmake.exe --build intermediate\vs2022 --config Release --target Injector
```

2. Abra o Loader Release e inicie o jogo.

3. Confirme no console:

```text
Injector Version: ds2-jmj injector pvp-timer v2026-05-07.1 build=Release
```

4. Entre no mundo do host.

5. Confirme no console:

```text
[DS2TimerParamPatch] active timer patch installed offset=0x2c9844 target=4000.000 source=r14_plus_0xcfc
[DS2TimerParamPatch] patched active session timer old=... target=4000.000 trace_file=DS2_TimerParamPatch.log
```

6. Aguarde passar do limite antigo, cerca de:

```text
763-766 segundos
```

Se o phantom nao sair por tempo, o patch esta funcionando.

## Logs Esperados

`DS2_TimerParamPatch.log` deve conter linhas como:

```text
event=DS2ActiveTimerPatch result=installed source=r14_plus_0xcfc
event=DS2ActiveTimerPatch result=patched source=r14_plus_0xcfc old_seconds=... new_seconds=4000.000
```

`DS2_LeaveSessionTrace.log` ainda pode registrar leaves legitimos. O caso que o patch deve eliminar e:

```text
client_leave_reason_hint=timer_candidate
client_session_duration=~764
```

## Notas de Seguranca

- O patch fica desligado por padrao no runtime base e precisa de `DS2PatchPhantomTimers=true` ou `DS2_PATCH_PHANTOM_TIMERS=1`.
- O hook valida bytes antes de instalar.
- O state probe antigo foi removido; `DS2PatchPhantomTimers` e o unico hook de breakpoint mantido para essa pesquisa.
- O valor configurado precisa ser finito e maior que zero; o timer observado em memoria tambem precisa estar em uma faixa plausivel antes da escrita.
- O scanner antigo de param bruto foi removido do hook final. A solucao funcional validada e somente o contador ativo `R14+0xCFC`.
