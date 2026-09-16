# Arquitetura da viagem em conjunto

## Recomendação

Minha recomendação é **reconstruir a presença dos jogadores a cada viagem,
usando o carregamento e a entrada nativos**. Eu investigaria duas arquiteturas,
nesta ordem: reentrada coordenada completa; reconstrução apenas das cópias
remotas, mantendo o transporte atual.

Não considero demonstrado que a arquitetura atual seja irrecuperável. Está
demonstrado que **as guardas não estabeleceram um ciclo de vida seguro**. A
assinatura da corrupção ainda permite escrita fora dos limites, uso após
liberação e corrida entre tarefas. Dois bytes alterados num ponteiro não provam
que a instrução escritora tenha largura de dois bytes.

Este parecer foi feito a partir dos documentos do projeto, dos quatro hooks da
viagem e da conferência, em modo somente leitura no Ghidra, das funções de
entrada. Nenhuma viagem foi executada durante a análise.

Há três achados que pesam na decisão:

- A recarga destrói a presença dos jogadores, enquanto partes da sessão
  sobrevivem. A experiência anterior já mostrou host em `0x10` e convidado em
  `7`, sem interação entre eles. **Preservar a sessão não preserva nem
  reconstrói os personagens.** Ver [DS2_SEAMLESS_COOP.md](DS2_SEAMLESS_COOP.md),
  seção "Todo warp recarrega, e é isso que tira o fantasma".
- A entrada fornece mais que coordenadas: o host exporta um instantâneo, o
  convidado importa o mundo e recebe os dados necessários para registrar os
  jogadores remotos. Isso oferece um mecanismo existente para reconstrução.
  Ver [DS2_WORLD_STATE.md](DS2_WORLD_STATE.md), seção "De onde vem a cópia na
  entrada".
- A guarda do ciclo do mapa admite deixar a desmontagem incompleta. Executar a
  conclusão de uma tarefa após uma exceção preserva sua contabilidade; não
  desfaz as alterações parciais do trabalho. Portanto, o custo atual pode
  incluir recursos retidos e estruturas inconsistentes, além do quadro
  perdido. Ver `DS2_BackreadHook.cpp`, em `GuardedOwnerUpdate`.

## 1. Reentrada coordenada pelo carregamento nativo

Esta é a primeira escolha.

### Mecanismo

Separar a permanência na party da existência dos personagens naquele mapa.
Durante a cortina, a party permanece; as presenças antigas são retiradas e as
novas são construídas pelo jogo.

A sequência proposta seria:

1. A votação aprovada inicia uma transação identificada, com destino e
   participantes fixados.
2. Ambos suspendem ações e a aplicação de atualizações aos personagens que
   serão retirados. O transporte e as mensagens de controle continuam
   funcionando.
3. Cada máquina termina as tarefas pendentes e retira corretamente suas cópias
   remotas dos gerenciadores.
4. O host faz o carregamento nativo do destino, agora sem cópias antigas
   penduradas no mundo desmontado.
5. O host fornece um **novo** convite e instantâneo daquele mundo; o convidado
   carrega diretamente esse destino pela entrada nativa.
6. Ambos reconstruem as presenças e confirmam interação antes de liberar o
   controle.

As peças concretas são:

| Peça | Caminho existente |
| --- | --- |
| Viagem nativa do host | `FUN_1401843b0`, `FUN_140184830`, registro por `FUN_14044fe30` |
| Entrada do convidado | `FUN_1402c2a80`, pedido de motivo 4 e flag 1 |
| Instantâneo do host | `FUN_1402bf8f0` |
| Importação no convidado | `FUN_1402c2fa0`, quando está no estado 4 |
| Registro das presenças no convidado | estado 5, `FUN_1402c3c80` → `FUN_14051b0e0` |
| Finalização do host | sequência `0xd → 0xe → 0xf → 0x10`; handler de `0xf`: `FUN_1402c03e0` |

A conferência no Ghidra mostrou que a importação também preenche a lista de
jogadores consumida pelo estado 5. `FUN_14051b0e0` prepara uma entrada pendente
no gerenciador: seu retorno positivo **não significa que um personagem já
nasceu**.

A diferença para o que já falhou é substancial: não seria repetir o warp,
escrever `estado=2`, nem reapresentar sozinho o convite antigo. Seria executar
novamente o protocolo de materialização **nas duas pontas**, incluindo o
instantâneo, o registro dos remotos e as confirmações.

O convidado não passa pelo próprio mundo: durante a transição ele pode estar
sem mundo jogável, sob carregamento, mas o próximo mundo materializado precisa
ser o do host. É necessário preservar o registro original de retorno
`+0x1a0..+0x1c8` e a separação entre estado próprio e estado emprestado. Uma
nova entrada não pode registrar o mundo do host como se fosse a casa do
convidado.

### Por que respeita o ciclo de vida

Permite ao loader destruir e construir personagens e recursos na ordem que o
jogo espera. Elimina a necessidade de transportar o grafo vivo de animação,
modelo e física entre mapas.

A hipótese ainda não demonstrada é que essa reconstrução possa acontecer
mantendo o vínculo de rede utilizável, sem acionar o retorno para casa. A
invocação original demonstra que as peças existem; não demonstra sua
reutilização em uma sessão já estabelecida.

### Experimento mais barato que a refuta

Primeiro, instrumentar uma invocação normal em Heide e sua saída legal, sem
viagem experimental: identificar registro, retirada, conclusão das tarefas e
associação entre peer e personagem.

Depois, fazer uma **reentrada coordenada no mesmo mapa, com o convidado vivo**.
O host deve participar da reconstrução; não repetir o experimento unilateral
antigo. A tentativa falha se:

- for necessário materializar o mundo próprio do convidado;
- os estados terminarem corretamente, mas os personagens não voltarem a
  interagir;
- não for possível retirar e recriar a presença sem perder o vínculo necessário
  à entrada.

Esse teste já exige duas contas. Sozinho só é possível verificar o loader e a
instrumentação.

### Critério positivo de aceitação

Registrar a retirada das presenças antigas, a construção das novas, o
instantâneo correto aplicado e movimento ou ações efetivamente reproduzidos
nos dois sentidos. Depois repetir atravessando mapas. Endereços diferentes não
são requisito: o alocador pode reutilizá-los; é preciso acompanhar gerações de
objetos.

### Riscos e pioras

Mais carregamento que os atuais aproximadamente 2,5 segundos por jogador;
timeouts nativos; mensagens atrasadas referentes a personagens antigos;
duplicação de registros; sobrescrita do retorno para casa; interferência com o
progresso compartilhado do M7.

A maior lacuna é a retirada segura da presença **sem saída da sessão**. Não há
uma primitiva pronta comprovada para isso. Esse é o primeiro trabalho de
engenharia reversa, não um detalhe a preencher depois.

## 2. Destruir e recriar apenas as cópias remotas

Esta é a segunda escolha.

### Mecanismo

Manter `StartTravel`, backread, foco e teleporte do jogador local, mas remover
temporariamente as cópias remotas **antes de qualquer máquina começar a
viajar**.

A transação seria: ambos retiram suas cópias, confirmam a conclusão das
tarefas, viajam, confirmam contato no destino e registram novamente os remotos
com dados atuais.

As estruturas relevantes são o `PlayerCtrl` remoto, identificado por
`chr+0x54 == 2`, seus componentes, o `CharacterManager` e o gerenciador
alcançado por `FUN_14051b0e0`. Além do caminho do estado 5, o Ghidra mostrou
`FUN_1402c8330` chamando esse registro a partir de `FUN_1402cf910`: outro ponto
concreto para investigar a recepção que entrega os dados do jogador.

Não bastaria chamar `FUN_1403f6300` para liberar o modelo. É necessário retirar
a presença pelo caminho proprietário: registros de rede, física, animação e
tarefas precisam concordar. Um destrutor isolado pode fabricar exatamente
outra referência pendurada.

O canal de controle existente usa Steam P2P no canal 7. Ele pode coordenar a
operação, mas continuar trocando esses anúncios não comprova que a replicação
nativa dos personagens voltou.

### Por que pode conviver com a viagem atual

Ataca a diferença mais forte entre os controles: sozinho, o jogador local
atravessa; em sessão, existe também uma cópia remota atravessando. A
arquitetura faria cada máquina transportar apenas seu jogador local e criaria
a representação do parceiro já no destino.

Isso é uma hipótese causal plausível, não uma generalização dos 14 trechos
solo. Subsistemas de sessão continuam ativos mesmo quando a cópia não existe.

### Experimento mais barato que a refuta

Em Heide, sem trocar de mapa, retirar e reconstruir **uma** cópia remota,
mantendo seu dono conectado. Confirmar que:

- a presença antiga foi realmente retirada;
- a nova recebe posição, animação e ações;
- o parceiro não é descartado após o prazo em que a tentativa antiga perdia
  comunicação.

Repetir na outra máquina. Se esse ciclo exigir sair para o próprio mundo,
deixar ponteiros pendurados ou produzir apenas um fantasma visual, a proposta
perde sua premissa.

Se passar, fazer um único trecho entre mapas com as cópias ausentes durante a
travessia. Corrupção mesmo nesse intervalo refuta a ideia de que remover apenas
as cópias basta.

### Critério positivo de aceitação

Uma presença por peer após cada reconstrução, interação bilateral, retirada
comprovada dos recursos antigos e nenhuma intervenção das guardas. O jogador
local deve manter HP, inventário, almas, papel e estado de mundo corretos.

### Riscos e pioras

Conserva a manipulação do streamer e suas limitações. A reconstrução parcial
pode ser mais difícil que deixar o loader reconstruir tudo. Exige tratamento
explícito das atualizações destinadas à presença ausente; simplesmente
acumular e reaplicar pacotes antigos é perigoso.

A vantagem potencial é preservar boa parte da velocidade atual. Por isso ela
fica como segunda opção e como experimento útil para a primeira.

## O contrato de viagem precisa mudar nas duas alternativas

Hoje o host considera assentado `!Moving()`, mas a própria interface define
isso como "chegou **ou desistiu**". Depois do timeout, o host pode inclusive
chamar os convidados mesmo sem ter chegado. Essa ambiguidade participa da
orquestração, além de prejudicar os testes. Ver `DS2_BonfireInSessionHook.cpp`,
na lógica de `s_call.Active`.

A conclusão implícita deve ser substituída por resultados distintos:
preparando, carregando, assentando, chegou e falhou. A conclusão do grupo exige
recibos de todos os participantes para a mesma viagem.

A confirmação de chegada deve juntar:

- contato físico atual de cada jogador, convertido em mapa de destino;
- proximidade da fogueira e quadros de física avançando;
- presença remota funcional em ambas as máquinas;
- mundo autoritativo do host aplicado.

`ContinueSettle` compara `CurrentMap()`, obtido do streamer, e imprime o
contato como diagnóstico. Para o teste proposto, é necessário comparar também
o contato diretamente; a frase do log não deve virar uma assertion física
independente. Ver `DS2_DeathInterceptHook.cpp`, em `ContinueSettle`.

## Quando preservar a arquitetura atual

Vale fazer a captura da escrita antes de investir numa reconstrução extensa.
Ela pode revelar um erro localizado e mudar o ranking.

Há um obstáculo documentado: Dr0–Dr7 já foram aceitos sem disparar neste
ambiente. `DS2_INVESTIGATION_TOOLS.md` atribui isso ao caminho
Wine/wineserver/`ptrace` sob Yama. Portanto, "nunca tentado nesta corrupção"
não equivale a "instrumentação disponível".

O procedimento seria:

1. **Controle positivo fora de sessão:** observar uma escrita deliberada em
   memória pertencente ao injector, incluindo uma thread diferente da que arma
   o breakpoint. Exigir exceção, endereço e thread corretos.
2. Identificar uma **instância saudável equivalente** do campo que costuma
   corromper. Por exemplo, a vftable embutida em
   `MapModelComponent+0x50`, após confirmar componente e proprietário.
3. Armar a vigia antes da viagem, cobrindo as threads que podem escrever e
   registrando lacunas de cobertura. Para `SetThreadContext`, suspender a
   thread ao modificar seu contexto; usar tamanho e alinhamento adequados.
4. Capturar registradores, pilha, bytes e geração do objeto num buffer
   limitado, sem parar longamente a sessão para inspeção.
5. Correlacionar o acerto com destruição ou liberação do objeto e do heap. Uma
   escrita legítima num bloco já liberado aponta para um consumidor obsoleto;
   uma escrita indevida num objeto ainda vivo aponta para o escritor.

Armar no endereço **depois** da corrupção não recupera a escrita anterior.
Tampouco se deve transportar o endereço bruto entre boots.

Se o controle de hardware falhar, não vale gastar uma sessão esperando
silêncio. A vigia de página existente é alternativa para janelas curtas, com
seu custo e suas lacunas; não uma prova equivalente automática.

A arquitetura atual merece ser mantida se essa captura identificar um defeito
delimitado, sua correção estabelecer a ordem correta de vida dos recursos e os
testes comprovarem retirada completa dos mapas. Se mostrar dependências antigas
espalhadas por vários subsistemas, isso reforça reconstruir a presença.

## Critério final e custo dos experimentos

As guardas podem permanecer como proteção durante os testes, mas **qualquer
acionamento conta como falha arquitetural**, mesmo que a viagem chegue. É
necessário ler contadores completos: várias mensagens de falha são limitadas
em quantidade, então o log pode ficar silencioso enquanto o contador continua
crescendo.

Deve-se exigir uma sequência inicialmente curta e depois pelo menos os mesmos
40 trechos reais da referência, com:

- chegada física dos dois em cada trecho;
- movimento e ações replicados nos dois sentidos após a chegada;
- passagem pelo descarregamento efetivo da origem, além dos 30 segundos de
  retenção;
- recursos antigos retirados e ocupação estabilizada nos retornos ao mesmo
  destino;
- nenhum ponto de penalidade acrescentado;
- saída legal final, confirmando que o convidado recupera seu próprio mundo e
  preserva o personagem.

Inverter host e convidado e exercitar a votação completa também fazem parte da
aprovação. Dois jogadores aprovados não autorizam conclusão sobre três.

Os primeiros ensaios podem ser observacionais e sem custo esperado de
desconexão. Os experimentos de reconstrução não têm essa garantia: devem usar
cenário com baseline das duas contas e restauração verificada. Isso evita
conservar a penalidade no save original; não transforma um crash em teste sem
penalidade.

O começo recomendado é o controle do watchpoint e o ciclo "retirar/recriar uma
presença no mesmo mapa". São os dois experimentos que mais reduzem a incerteza:
o primeiro distingue um defeito localizado de um problema estrutural; o
segundo testa a capacidade central das duas arquiteturas propostas. A direção
de implementação preferida continua sendo a reentrada coordenada pelo loader
nativo.
