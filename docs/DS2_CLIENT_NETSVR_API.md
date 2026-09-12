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

## Por que isso importa agora

A revanche por red sign precisa que o cliente **do host** reemita o summon.
Com os nomes na mão, isso deixa de ser arqueologia:

    FUN_1402a14c0(NetSvrSummonSignManager*, SignHandle*)
      -> FUN_1402a41b0   resolve o handle pelo slot virtual +0x98 do manager
      -> FUN_1402a2ca0
      -> FUN_1402a5970   constrói o NetSvrSummonSignSummonJob

Um hook no injector guarda o ponteiro do manager (ele é estável dentro de uma
sessão de jogo) e chama essa função de novo. O que falta descobrir é como
obter o `SignHandle` **da placa nova** — o handle capturado morre com o toque,
e a placa recolocada é outro objeto. O caminho é o mesmo `GetSummonSignList` /
o slot que resolve handles no manager.
