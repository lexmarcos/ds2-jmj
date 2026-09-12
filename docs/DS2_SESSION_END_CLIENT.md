# Como o cliente encerra uma sessão

O caminho, dentro de `DarkSoulsII.exe`, entre "o fantasma morreu" e
`RequestNotifyLeaveSession` chegando ao servidor. Levantado em 12/09 com
breakpoints em execução, porque o grafo estático de chamadas não leva a lugar
nenhum aqui: todas essas funções são chamadas por tabela ou virtualmente, e
`getCallingFunctions` volta vazio para todas elas.

Versão 1.03 Calibrations 2.02, base `0x140000000`.

## Os envios, achados pelos ids do protocolo

Procurando o id de cada mensagem como imediato (`FindImmediate.java`):

| função | imediato | mensagem |
| --- | --- | --- |
| `FUN_1406aad80` | `MOV EDX,0x3f1` | `RequestNotifyDeath` |
| `FUN_1406ab230` | `MOV EDX,0x3ea` | `RequestNotifyJoinSession` |
| `FUN_1406ab650` | `MOV EDX,0x3eb` | `RequestNotifyLeaveSession` |
| `FUN_1406a2610` | `MOV EDX,0x398` | `RequestSummonSign` |
| `FUN_1406a24f0` | `MOV EDX,0x396` | `RequestRemoveSign` |
| `FUN_1406a1170`, `FUN_1406a1de0` | `MOV EDX,0x394` | `RequestCreateSign` |
| `FUN_1406a0910` | `MOV EDX,0x39b` | `PushRequestSummonSign` |
| `FUN_1406a6300` | `MOV EDX,0x3d2` | `RequestGetBreakInTargetList` |
| `FUN_1406a6fb0` | `MOV EDX,0x3d3` | `RequestBreakInTarget` |

Todos vizinhos, em `0x1406a....`: é a camada de rede, não a jogabilidade.

## A morte, e depois a saída

Um fantasma morrendo no mundo do host, com os dois breakpoints armados no
cliente **do fantasma** (`DS2_Trace.req`):

    bp 6aad80     RequestNotifyDeath
    bp 6ab650     RequestNotifyLeaveSession

O que saiu:

    alcancado +0x6aad80 de=+0x2ab3de  rdx=0x009d5170 r8=0x00ffffff
      pilha: +0x2ab3de +0x203bb4 +0x26b047 +0x18fc2d +0x18f690 +0x470b9 +0x46e44

    alcancado +0x6ab650 de=+0x293153  rdx=1 r8=5
      pilha: +0x293153 +0x2900f8 +0x25c400 +0x2bbd33 +0x2c3c19 +0x2581ff

Os doze segundos entre as duas já estavam medidos em
[DS2_LEAVE_SESSION_BY_KILL.md](DS2_LEAVE_SESSION_BY_KILL.md); aqui eles
aparecem como dois pontos distintos do código, não como um atraso de rede.

## A cadeia da saída, de baixo para cima

    FUN_1406ab650                    envia RequestNotifyLeaveSession
      ^ FUN_140293110  (+0x43)       chama slot virtual +0x30 do serviço de rede
      ^ FUN_1402900b0  (+0x48)       despachante de eventos de sessão
      ^ FUN_14025c3d0  (+0x30)
      ^ FUN_1402bbcf0  (+0x43)       embrulha o código do evento e despacha
      ^ FUN_1402c3900  (+0x319)      a rotina que encerra a sessão

### O despachante decide pelo código do evento

```c
void FUN_1402900b0(longlong param_1, undefined1 *param_2, int *param_3)
{
    uVar1 = FUN_1402aacb0(*param_2);
    if (*param_3 == 5) {
        FUN_140292fa0(..., param_3[1], uVar1, param_2[0x18] == 2, param_3[2]);
    }
    else if (*param_3 == 6) {
        FUN_140293110(..., param_3[1]);      // ← a saída
    }
    ...
}
```

`*param_3` é o código do evento. **6 é sair da sessão**; 5 é o outro lado do
par. O resto da função repassa o mesmo evento para quatro assinantes
opcionais, guardados em `param_1 + 0x78`, `+0xa0`, `+0xa8` e `+0xb0`, cada um
atrás de um bit da máscara em `param_2[0x18]`.

### Quem levanta o evento

```c
LAB_1402c3bb7:                                    // dentro de FUN_1402c3900
    uVar7 = FUN_14028f270(local_58, *(undefined4 *)(param_1 + 0x198));
    FUN_1402900b0(*(undefined8 *)(param_1 + 0x110), param_1 + 0xd8, uVar7);
    FUN_1402bbcf0(param_1, param_1 + 0xd8, *(undefined4 *)(param_1 + 0x198));
    *(undefined4 *)(param_1 + 0xf8) = 9;
```

Campos do objeto de sessão que aparecem aqui:

| campo | o que é |
| --- | --- |
| `+0x198` | o código do evento, 6 no caso da morte |
| `+0xf8` | o estado, escrito com 9 ao encerrar |
| `+0xd8` | o registro da sessão passado adiante |
| `+0x110` | o despachante |

Quem escreve `+0x198` decide **por que** a sessão termina, e é aí que mora a
diferença entre morrer, o temporizador estourar e usar um item de saída. Esse
ponto ainda não foi localizado.

## Por que isso importa

Duas funcionalidades pedidas dependem deste caminho:

- **Revanche por red sign.** Se a sessão não terminasse na morte, não haveria o
  que reconectar. Ver [DS2_REMATCH_AFTER_DEATH.md](DS2_REMATCH_AFTER_DEATH.md)
  para a conclusão de que o servidor sozinho não consegue refazer a sessão.
- **Co-op seamless.** O objetivo é que a morte devolva o jogador à última
  fogueira **dentro da mesma sessão**, em vez de mandá-lo para o próprio
  mundo.

Um aviso que já estava registrado e continua valendo: o
`RequestNotifyLeaveSession` **não** pode ser bloqueado às cegas, porque ele
também faz a limpeza legítima de uma morte. Não mandar a mensagem não impede o
cliente de ejetar o jogador — quem faz isso é o mesmo evento, localmente. O
alvo certo é o código do evento, não o envio.

## O que ainda falta

- Onde `+0x198` recebe 6, e o que mais pode escrever ali.
- O que são os códigos 5 e 6 exatamente (5 aparece no par da saída).
- Se `FUN_1402c3900` é chamada uma vez por sessão ou por quadro.
