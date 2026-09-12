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

## O campo da razão, e seu vocabulário

`+0x198` não é um booleano de "saiu": é o **motivo**, e o jogo tem um
vocabulário para ele. Procurando as escritas imediatas nesse deslocamento
(sintaxe AT&T, `movl $0x?,0x198(%reg)`, faixa 0x140200000–0x140400000):

    1402bd018   movl $0xd,0x198(%rdi)
    1402bd622   movl $0x2,0x198(%rdi)
    1402bd781   movl $0x1,0x198(%rdi)
    1402bdcda   movl $0x1,0x198(%rbx)
    1402be5a2   movl $0x4,0x198(%rsi)
    1402be7a9   movl $0x4,0x198(%rsi)
    1402beeb2   movl $0x7,0x198(%rdi)
    1402bf4f7   movl $0x6,0x198(%r14)      <-- o único que escreve 6
    1402bf63a   movl $0x1,0x198(%r14)
    1402c04bb   movl $0x1,0x198(%rdi)
    1403f3914   movl $0x1,0x198(%rbx)

Um único sítio escreve 6, dentro de `FUN_1402bf440`, e ele tem a forma de um
**padrão**, não de uma decisão:

```c
if (*(int *)(param_1 + 0x198) == 0) {
    *(undefined4 *)(param_1 + 0x198) = 6;     // o outro ramo põe 1
}
if (*(int *)(param_1 + 0x150) != 0) {
    FUN_1402ce5f0(*(undefined4 *)(param_1 + 0x198), param_1 + 0xb0);
}
```

Quem escreveu primeiro ganha: a razão só é preenchida se ainda estiver zerada.
Então quem sabe *por que* a sessão acabou escreve antes, e `FUN_1402bf440` só
completa o que ficou em branco. Trocar o 6 por outro valor aqui muda o rótulo,
não o comportamento.

Estes dois têm chamador estático, ao contrário do resto do caminho:

    FUN_1402bddb0  ->  FUN_1402bf440     (a rotina que põe a razão)
    FUN_1402c3630  ->  FUN_1402c3900     (a rotina que encerra)

## A máquina de estados da sessão

`FUN_1402c3630` é o passo por quadro do objeto de sessão. Ele despacha pelo
estado guardado em `+0xf8`:

| estado | handler | o que é |
| --- | --- | --- |
| 0 | `FUN_1402c37a0` | |
| 1 | `FUN_1402c4450` | |
| 4 | inline | conta tempo e, ao estourar, chama o slot virtual `+0x30` com motivo `0xf` |
| 5 | `FUN_1402c3c80` | |
| 6 | `FUN_1402c45b0` | |
| 7 | `FUN_1402c3830` | **em sessão** — é aqui que a saída é decidida |
| 8 | `FUN_1402c3900` | demole: despacha o evento, manda o `LeaveSession`, põe o estado em 9 |
| 10 | `FUN_1402c4240` | |
| 0xb | inline | estado := 0xc |

Uma única instrução no binário inteiro escreve 8 nesse campo —
`1402c386a`, dentro do handler do estado 7 — e a condição dela é de uma
simplicidade que surpreende:

```c
void FUN_1402c3830(longlong *param_1, float param_2)
{
    ...
    if (*(int *)((longlong)param_1 + 0x1cc) != 0) {
        *(undefined4 *)(param_1 + 0x1f) = 8;      // +0xf8 := 8, encerrar
    }
    ...
}
```

`+0x1cc` não é um booleano: é **o motivo pelo qual alguém pediu o fim**, e
`FUN_1402c2f20` é a porta por onde o pedido entra:

```c
void FUN_1402c2f20(longlong *param_1, undefined4 param_2)   // slot virtual +0x30
{
    if ((**(code **)(*param_1 + 0xa8))() != '\0') {
        *(undefined4 *)((longlong)param_1 + 0x1cc) = param_2;
        ...
    }
}
```

Os motivos que o binário passa para esse slot, achados por padrão de chamada:
`0xe`, `0xf`, `0x10`, `0x12`, `0x13`, `0x18`. São de uma escala diferente da
do `+0x198`, então em algum ponto um é traduzido no outro.

## O que a morte faz, medido

Breakpoint em `FUN_1402c2f20` no cliente do fantasma, invasão por orbe montada,
fantasma morto por queda:

    alcancado +0x2c2f20 de=+0x2c9246 rdx=2 r8=0x1410c0050
      pilha: +0x2c9246 +0x190989 +0x18f773 +0x470b9 ...

**O motivo da morte é 2.** Subindo a pilha:

    FUN_1402c9220(manager, motivo)     percorre a lista de sessões (+0x48..+0x50)
                                       e chama o slot +0xe8 de cada uma
    FUN_140190950(obj, motivo)         "manda todas as sessões terminarem"
    FUN_14018f760(tarefa)              invocador de tarefa: +0x18 é o ponteiro de
                                       função, +0x20 e +0x28 os argumentos capturados

Ou seja, a morte **enfileira** a saída como tarefa; quando ela roda, a decisão
já foi tomada e não está mais na pilha.

## A tabela que decide se um tipo de morte desfaz a sessão

O chamador direto de `FUN_140190950` é um salto guardado por uma consulta a
uma tabela:

```asm
14018ffcf:  test   %rdx,%rdx              ; sem motivo, volta
14018ffd2:  je     0x140190006
14018ffd4:  movzbl 0xe0(%rcx),%r8d        ; o "tipo", byte do objeto
14018ffdf:  cmp    $0x14,%r8b             ; 20 tipos
14018ffe3:  cmovb  %r8d,%r9d
14018ffe7:  lea    0xf30062(%rip),%r8     ; a tabela: 0x1410c0050
14018fff2:  add    %rax,%rax
14018fff5:  cmpb   $0x0,0x1(%r8,%rax,8)   ; entrada[tipo].byte1
14018fffb:  je     0x140190920            ; zero: NÃO encerra as sessões
140190001:  jmp    0x140190950            ; diferente de zero: encerra todas
```

Entradas de 16 bytes, lidas da memória viva em `mod 10c0050 140`:

    +0x000  00 00 00 00  07 17 00 00     tipo 0  -> byte1 = 0, não encerra
    +0x010  02 01 01 02  03 02 01 02     tipo 1  -> byte1 = 1, encerra
    +0x020  02 01 02 02  03 02 00 02     tipo 2  -> encerra
    +0x030  02 01 01 03  03 02 01 02     tipo 3  -> encerra
    +0x040  02 01 02 03  03 02 00 02     tipo 4  -> encerra
    +0x050  02 02 01 07  01 03 00 06     tipo 5  -> encerra
    +0x060  01 02 01 08  01 05 00 07     tipo 6  -> encerra
    +0x070  01 01 01 04  01 07 00 03     tipo 7  -> encerra
    +0x080  01 02 01 04  01 07 00 04     tipo 8  -> encerra

**É aqui que o co-op seamless pode nascer.** Zerar o byte `+1` da entrada certa
faz a morte daquele tipo deixar de desmanchar as sessões — e é um patch de
*dado*, não de `.text`, da mesma família do `DS2_TimerParamPatch`.

### Tentar zerar a tabela inteira não entrega o co-op, e ensina por quê

Primeira tentativa, em 12/09: zerar o byte `+1` das dezessete entradas que o
tinham diferente de zero, **só no cliente do fantasma**, com
`pokemod 10c00?1 00`. Todas aceitaram (`antes=01`/`03`, `ok`).

Depois disso, uma invasão montada e o fantasma jogado do penhasco:

- a sessão **continuou de pé** — a barra de vida do host seguiu na tela do
  fantasma, e o servidor continuou recebendo posição dele;
- mas o fantasma **não morreu**. Ficou caindo no vazio por mais de dois
  minutos, sem tela de morte, sem renascer, sem voltar para o próprio mundo. O
  processo seguia rodando a 80% de CPU e a imagem seguia mudando, então não era
  travamento: era um fluxo de morte que nunca completa;
- restaurar os bytes não desfez o estado em que o personagem já estava.

A leitura: esse byte não é "mantenha a sessão". Ele faz parte do caminho da
**morte**, e tirá-lo deixa o jogador num limbo. O co-op seamless precisa de
mais do que suprimir o fim da sessão — precisa redirecionar o renascimento
para a fogueira dentro do mundo do host. Suprimir sem redirecionar é
exatamente o erro que o CLAUDE.md descreve como "patch no valor em vez de na
origem".

Falta ainda saber **qual índice** corresponde a cada papel. O tipo vem de
`rcx+0xe0`; capturei o `rcx` num breakpoint em `FUN_140190950`
(`rcx=0x7fffe8158ac0`, `rdx=2`), mas a sonda rodou um minuto depois e leu
memória já reciclada — o objeto é transitório. Para ler o byte certo, a sonda
precisa acontecer no mesmo instante do breakpoint, o que o tracer ainda não
sabe fazer.

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

- Quem escreve `+0x198` **antes** de `FUN_1402bf440`, que é quem realmente
  decide o motivo. O sítio do 6 é só o padrão.
- O que são os códigos 5 e 6 exatamente (5 aparece no par da saída).
- Se `FUN_1402c3900` é chamada uma vez por sessão ou por quadro.
