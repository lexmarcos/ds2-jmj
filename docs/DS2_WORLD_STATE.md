# Estado de mundo numa sessão (M4)

O M4 pede que portas, alavancas, elevadores, atalhos, illusory walls e Pharros
abertos pelo host apareçam abertos para quem entrou, e que isso **não** vá para
o mundo do convidado. Antes de construir qualquer coisa, a pergunta é o que o
jogo já faz. Este arquivo é o que foi medido em 15/09.

## O jogo já sincroniza event flags por P2P

Os pacotes P2P são registrados em `FUN_14051e3d0`, e o de tipo `0x20` é
`NetP2pPacketEventFlag` (vftable `0x1410fb688`, 7 slots). Os vizinhos: `0x2a`
AiFlag, `0x2b` AiData, `0x2c` AiGroup, `0x31` SpEffect.

| função | papel |
| --- | --- |
| `FUN_140474a60(mgr, flag, valor)` | **o setter do jogo**. Em sessão, um cliente que não é o host (`FUN_1405135f0`) só grava as flags que `FUN_14025cdb0` aceita; se a flag mudou e há sessão, `FUN_14051e6b0` a manda |
| `FUN_14051e6b0` | envia 8 bytes (`uint32 flag`, `byte valor`) pelo slot `+0x18` do pacote, `FUN_14025cec0` → `NetSvr` slot `+0x78`, tipo `0x20` |
| `FUN_14025ce10` | recebe: 8 bytes; se quem mandou não é o host, passa pelo mesmo filtro; grava com `FUN_1404750b0` |
| `FUN_1404750b0(mgr, flag, valor)` | grava o bit e chama os ouvintes (`FUN_140184ff0`) quando ele muda |
| `FUN_14025cdb0(flag)` | o filtro: recusa `flag < 1e9` (e o bloco `1e9..`) cujo `(flag/10000) % 100 > 2`, ou seja, as **globais**; aceita as de mapa (`1310xxxxx` em Heide) |

Ou seja, em sessão **o host manda qualquer flag**, e o convidado só consegue
mudar flags de mapa.

## Onde as flags moram

`mgr = *(*(*0x1416148f0 + 0x70) + 0x20)` (`EventFlagManager`, vftable
`0x1410eff58`). Em `mgr + 0x20` há uma tabela de 31 baldes indexada por
`((flag / 10000) * 0x89) % 0x1f`; cada nó tem `+0x00` ponteiro para os bytes,
`+0x08` o tamanho, `+0x0c` a categoria (`flag / 10000`) e `+0x10` o próximo. O
bit é `flag % 10000`, do mais alto para o mais baixo dentro de cada byte.

Pela harness: `chain a 16148f0 70,20 312` lê o gerenciador; os nós e os bytes
saem com `abs`. Em Heide só existem cinco categorias carregadas: `10` e `20`
(1250 bytes cada, as globais) e `13100`–`13102` (25 bytes cada, o mapa). Samuel
e Chico tinham exatamente os mesmos bits (34 em `10`, 18 em `20`, 2 em
`13100`), então comparar os dois não diz nada sozinho.

`FUN_1404744b0` esvazia a tabela (troca de mapa) e grava em `mgr + 0x118` se o
cliente está em sessão como não-host.

## O que foi medido

Com os dois em Heide, `up --seamless --party`, traço em `25ce10` e `25cec0`
(`deref rdx 8`) nas duas instalações:

1. **Entrada: nenhum pacote de flag.** Nem o host mandou nem o convidado
   recebeu um `0x20` ao formar a sessão.
2. **O convidado recebe as flags de mapa do host ao entrar.** Com a sessão
   desfeita, o bit `131000199` (sem uso conhecido) foi ligado **só na memória
   do host**. Formada a sessão, o convidado tinha o bit. Como nenhum `0x20`
   passou, a cópia na entrada vem de outro caminho, ainda não encontrado.
3. **O convidado não leva a flag para casa.** Depois de `session end`, no
   próprio mundo, o bit estava desligado no Chico e ligado no Samuel (e foi
   desligado de volta na memória do Samuel em seguida).
4. **As globais vêm junto.** O mesmo teste com o último bit da categoria `10`
   (`109999`): ligado só no host, presente no convidado na sessão, ausente no
   convidado em casa, desligado de volta no host.

5. **É troca, não mescla.** O inverso: `109999` ligado só no convidado, em
   casa. Na sessão o convidado tinha o bit **desligado**, como o host; de volta
   em casa, ligado de novo (e desligado na memória do Chico em seguida).

O convidado **carrega as flags do host** ao entrar, no lugar das próprias, e
recupera as próprias ao voltar.

Isso é o comportamento que o design pede para portas e alavancas, **se** elas
forem flags de mapa: o convidado vê o mundo do host, e o mundo dele não muda.

## O que ainda não se sabe

- **Se portas, alavancas, elevadores e illusory walls são flags de mapa.**
  `MapObjStateActComponent` (vftable `0x1410c6d78`) pode guardar estado por
  objeto fora do `EventFlagManager`. Nenhum mecanismo real foi acionado ainda.
- **Uma mudança feita durante a sessão.** O caminho de envio existe
  (`FUN_140474a60` → `FUN_14051e6b0`), mas nenhuma flag mudou nas sessões
  medidas, então não há hit de `25cec0` para mostrar.
- **O que o convidado vê de uma alavanca que ele mesmo puxa** no mundo do host:
  uma flag de mapa passa pelo filtro, mas o objeto pode ter outra trava.
- **De onde vem a cópia na entrada.** Não é o `0x20`.
- **O lado oposto do design.** "O que vocês fizerem juntos é salvo para todos"
  (um chefe morto na sessão fica morto no mundo do convidado) é exatamente o
  que o jogo **não** faz: o mundo do convidado volta como estava. Isso é
  trabalho de um marco posterior, não do M4.
