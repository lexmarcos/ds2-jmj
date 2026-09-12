# A API de rede do cliente tem nomes

O `DarkSoulsII.exe` carrega RTTI completo para o subsistema multijogador, com
as assinaturas dos métodos preservadas dentro dos namespaces das lambdas. Na
prática o binário se documenta sozinho — descoberto em 12/09, depois de meses
tratando essas funções como `FUN_1402a14c0`.

Isto vale para quem for continuar a engenharia reversa aqui: **antes de
rastrear qualquer coisa do multijogador, procure o nome**.

## Como ler os nomes

O label que o Ghidra guarda é só `vftable`; o nome da classe está no
*namespace* do símbolo, então é preciso pedir o nome qualificado
(`Symbol.getName(true)`). Dois scripts em `/home/suel/tools/scripts` fazem
isso:

```
analyzeHeadless ... -postScript LabelAt.java   <saida> 0x1410d61f8 ...
analyzeHeadless ... -postScript ClassList.java <saida> NetSvr
```

`ClassList.java` varre todos os símbolos e imprime os que terminam em
`::vftable`. Com `NetSvr` são 148, dos quais 84 são classes do próprio jogo e
o resto é encanamento de template.

As assinaturas aparecem porque um job criado dentro de um método vira uma
classe local, e o namespace preserva a declaração inteira:

    NetSvrBreakInInterface::GetBreakInTargetList(
        unsigned char, unsigned int, unsigned int, unsigned int,
        Frpg2Sv::MatchingParameter const&,
        Frpg2ClientLib::Frpg2Vector<Frpg2ClientLib::BreakInTargetData>*)
    NetSvrBreakInInterface::BreakIn(unsigned char, unsigned int, unsigned int, unsigned int)
    NetSvrSummonSignInterface::SummonSummonSign(unsigned int, Frpg2Sv::CellAddress const&, ...)

Tipos que aparecem e valem conhecer: `SignHandle`, `Frpg2Sv::CellAddress`,
`Frpg2Sv::MatchingParameter`, `Frpg2Sv::SignInfo`, `NetSvrJobResult`,
`App_BloodMessageData`, `NetSvrBreakInSessionInfo`.

## O padrão: Manager, Interface, Job

Cada subsistema aparece três vezes:

| camada | o que faz | exemplo |
| --- | --- | --- |
| `...Manager` | a fachada que o jogo chama | `NetSvrSummonSignManager` |
| `...Interface` | monta o pedido do protocolo | `NetSvrSummonSignInterface` |
| `...Job` | a tarefa que roda e envia | `NetSvrSummonSignSummonJob` |
| `...PushNotifyBuffer` | recebe o push do servidor | `NetSvrSummonSignPushNotifyBuffer` |

Foi assim que o caminho do summon ficou legível. O `FUN_1402a14c0` que a
varredura de breakpoints encontrou é **`NetSvrSummonSignManager::<invocar>`**:
o `rcx` capturado em execução (`0x7ffffe591000`) tem as vftables
`0x1410d61f8`/`0x1410d6270`, que são as do manager, e o `rdx` é ponteiro para
um `SignHandle` de 32 bits.

## Endereços das vftables, o que interessa aqui

    1410d61f8  NetSvrSummonSignManager
    1410d5db8  NetSvrSummonSignInterface
    1410d63a8  NetSvrSummonSignSummonJob
    1410d6380  NetSvrSummonSignPushNotifyBuffer
    1410d6028  NetSvrSummonSummonSignJob
    1410d5df8  NetSvrCreateSummonSignJob
    1410d5e68  NetSvrUpdateSummonSignJob
    1410d5ed8  NetSvrRemoveSummonSignJob
    1410d5f48  NetSvrRejectSummonSignJob
    1410d5fb8  NetSvrGetSummonSignListJob

    1410d2928  NetSvrBreakInManager
    1410d2680  NetSvrBreakInInterface
    1410d2728  NetSvrBreakInJob
    1410d2b18  NetSvrBreakInSummonJob
    1410d2af0  NetSvrBreakInPushNotifyBuffer
    1410d2798  NetSvrAllowBreakInJob
    1410d2808  NetSvrRejectBreakInJob
    1410d26b8  NetSvrGetBreakInTargetListJob

A lista completa das 84 sai em segundos com o `ClassList.java`; não vale a
pena congelá-la aqui, porque o script é mais confiável que uma cópia.

## O registro de placas do cliente, e como chegar nele

O `NetSvrSummonSignManager` não guarda as placas; quem guarda é o lado de
jogo. `FUN_1402a41b0` pega o registro assim:

```asm
1402a41c1:  call   0x1402128d0        ; devolve o registro
1402a41c6:  mov    (%r14),%r8d        ; o SignHandle
1402a41ce:  mov    %rax,%rcx
1402a41d6:  mov    (%rax),%r8         ; a vftable
1402a41dc:  call   *0x98(%r8)         ; registro->slot 0x98(handle) -> placa
```

E `FUN_1402128d0` é uma cadeia de ponteiros a partir do global do jogo,
`DAT_1416148f0`, verificada na memória viva:

| passo | classe (RTTI) | vftable |
| --- | --- | --- |
| `*(global) + 0x90` | `SignManager` | `0x1410cb668` |
| `... + 0x68` | `SignSetCtrlManager` | `0x1410cb4a0` |
| `... + 0x20` | `SummonSignSetCtrl` | `0x1410cb698` |
| `... + 0x28` | `SummonSignSetCtrl`, segunda base | `0x1410cb6e8` |

É a segunda base que responde ao slot `+0x98`. Ela é `FUN_140213460`:

```c
void FUN_140213460(longlong this, undefined4 *handle)
{
    uint h = *handle;
    if (FUN_14020e6f0(*(this - 0x10), &h) == 0) {   // procura na coleção A
        FUN_14020e6f0(*(this - 8), &h);             // e depois na B
    }
}
```

**Duas coleções**, em `this-0x10` e `this-0x8`, com `FUN_14020e6f0` fazendo a
busca por handle. Os slots vizinhos `+0xa0` e `+0xa8` chamam o mesmo
`FUN_14020f660(this - 0x28, x, 2|3, ...)` com discriminadores diferentes, o que
cheira a "por tipo de placa".

### O formato do contêiner, e a previsão que fechou o modelo

`FUN_14020e6f0(colecao, &handle)` é a busca, e ela revela a interface:

```c
count = colecao->vftable[0x18]();                 // quantos
for (i = 0; i < count; i++) {
    item = colecao->vftable[0x10](colecao, i);    // o i-esimo
    if (item[5] < 0 && item[0] == *handle) return item;
}
```

As duas coleções são `TSignSet<SummonSignParam>`, e a leitura na memória viva
dá o resto:

| campo | conteúdo |
| --- | --- |
| `+0x14` | capacidade (20 na coleção medida) |
| `+0x18` | **quantidade** — subiu de 5 para 6 no instante em que uma placa nova chegou |
| `+0x30` | ponteiro do armazenamento |
| `+0x38` | quantidade, de novo |

E cada item ocupa **0x88 bytes**:

| campo | conteúdo |
| --- | --- |
| `+0x00` | o `SignHandle` |
| `+0x04`, `+0x08`, `+0x0c` | x, y, z do mundo, em float |
| `+0x14` | bit alto ligado = item válido (é o `item[5] < 0` da busca) |

O que fecha o argumento: com uma placa recém-colocada, o item 5 do
armazenamento trazia handle `0x80000055` e posição (6.15, -18.5, 209.1) — a
fogueira de Heide onde ela tinha sido colocada. **Previ que o summon usaria
`0x80000055` e o breakpoint capturou exatamente isso.** Placas de invocação
carregam a tag `0x80000000`; a outra coleção tem itens com `0xc0000000`.

## Com isso, a revanche tem desenho completo

Nada mais falta descobrir para escrever o hook do lado do host:

1. andar do global até o `SummonSignSetCtrl` (quatro ponteiros, acima);
2. percorrer a coleção com a interface `count`/`at`, pegando o item cuja
   posição bate com a do duelo anterior — ou, mais simples, o único item com a
   tag `0x80000000` quando só existe um par no servidor;
3. chamar `FUN_1402a14c0(NetSvrSummonSignManager*, &handle)`.

O ponteiro do manager é estável e já foi capturado (`0x7ffffe591000` em duas
sessões diferentes, inclusive depois de fechar o jogo), mas o hook deve
resolvê-lo pelo caminho próprio em vez de fixar o endereço.

O que ainda não foi lido é **qual campo do item identifica o dono** da placa.
Para dois jogadores isso não é necessário; para mais de dois, é.
